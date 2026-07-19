#include "mutation_engine.hpp"
#include "runtime_values.hpp"
#include "runtime_tables.hpp"
#include "runtime_memory.hpp"

#include <climits>
#include <cmath>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#endif

namespace bored::signalscope {

namespace {

uint64_t makeBitMask(uint8_t bit_length) {
    if (bit_length == 0U) {
        return 0U;
    }
    if (bit_length >= 64U) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return (1ULL << bit_length) - 1ULL;
}

uint64_t convertPhysicalToRaw(const SignalMutation& mutation, float physical_value) {
    if (mutation.length == 0U || mutation.length > 64U) {
        return 0U;
    }

    const double factor = (mutation.factor == 0.0F) ? 1.0 : static_cast<double>(mutation.factor);
    const double offset = static_cast<double>(mutation.offset);
    int64_t raw_signed = static_cast<int64_t>(std::llround((static_cast<double>(physical_value) - offset) / factor));

    int64_t min_value = 0;
    int64_t max_value = 0;
    if (mutation.is_signed) {
        if (mutation.length >= 64U) {
            min_value = INT64_MIN;
            max_value = INT64_MAX;
        } else {
            max_value = (1LL << (mutation.length - 1U)) - 1LL;
            min_value = -(1LL << (mutation.length - 1U));
        }
    } else {
        min_value = 0;
        if (mutation.length >= 63U) {
            max_value = INT64_MAX;
        } else {
            max_value = static_cast<int64_t>((1ULL << mutation.length) - 1ULL);
        }
    }

    if (raw_signed < min_value) {
        raw_signed = min_value;
    }
    if (raw_signed > max_value) {
        raw_signed = max_value;
    }

    const uint64_t mask = makeBitMask(mutation.length);
    return static_cast<uint64_t>(raw_signed) & mask;
}

uint32_t initialRuntimeValue(const RuleStageRequest& rule) {
    if (rule.kind == RuleKind::BIT_RANGE && rule.dynamic_value) {
        return static_cast<uint32_t>(rule.replace_value);
    }
    if (rule.kind == RuleKind::COUNTER) {
        return rule.counter_initial;
    }
    if (rule.kind == RuleKind::SEQUENCE8) {
        return rule.sequence_initial_index;
    }
    return 0U;
}

void yieldCommitter() {
#if defined(ARDUINO_ARCH_ESP32)
    taskYIELD();
#else
    std::this_thread::yield();
#endif
}

class MutationReadScope {
public:
    explicit MutationReadScope(std::atomic<uint32_t>& readers) : readers_(readers) {}

    ~MutationReadScope() {
        readers_.fetch_sub(1U, std::memory_order_seq_cst);
    }

    MutationReadScope(const MutationReadScope&) = delete;
    MutationReadScope& operator=(const MutationReadScope&) = delete;

private:
    std::atomic<uint32_t>& readers_;
};

}  // namespace

void MutationEngine::init() {
    if (staged_ == nullptr) staged_ = static_cast<StagedRule*>(allocateRuntimeMemory(sizeof(StagedRule) * kMaxRules));
    if (committed_shadow_ == nullptr) {
        committed_shadow_ = static_cast<StagedRule*>(allocateRuntimeMemory(sizeof(StagedRule) * kMaxRules));
    }
    if (tables_[0] == nullptr) tables_[0] = static_cast<ActiveRuleTable*>(allocateRuntimeMemory(sizeof(ActiveRuleTable)));
    if (tables_[1] == nullptr) tables_[1] = static_cast<ActiveRuleTable*>(allocateRuntimeMemory(sizeof(ActiveRuleTable)));
    if (staged_ == nullptr || committed_shadow_ == nullptr || tables_[0] == nullptr || tables_[1] == nullptr) {
        active_table_.store(nullptr, std::memory_order_release);
        return;
    }
    staged_count_ = 0U;
    committed_count_ = 0U;
    next_sequence_ = 1U;

    for (size_t i = 0; i < kMaxRules; ++i) {
        staged_[i] = {};
        committed_shadow_[i] = {};
        runtime_state_[i].current_value.store(0U, std::memory_order_relaxed);
        runtime_state_[i].enabled.store(0U, std::memory_order_relaxed);
    }

    clearActiveTable(*tables_[0]);
    clearActiveTable(*tables_[1]);
    active_table_index_ = 0U;
    active_table_.store(tables_[active_table_index_], std::memory_order_release);
    active_count_.store(0U, std::memory_order_relaxed);
    rule_epoch_.store(1U, std::memory_order_release);
    mutation_readers_.store(0U, std::memory_order_relaxed);
    mutation_reads_enabled_.store(1U, std::memory_order_release);
}

void MutationEngine::setRuntimeValueRegistry(const RuntimeValueRegistry* registry) {
    runtime_values_ = registry;
}

void MutationEngine::setRuntimeTableRegistry(const RuntimeTableRegistry* registry) {
    runtime_tables_ = registry;
}

bool MutationEngine::stageRule(const RuleStageRequest& request, uint16_t* out_rule_id) {
    if (staged_ == nullptr || committed_shadow_ == nullptr) return false;
    RuleStageRequest normalized = request;
    if (!normalizeRule(normalized)) {
        return false;
    }

    uint16_t slot = findStagedIdentity(staged_, static_cast<uint16_t>(kMaxRules), normalized);
    const bool new_slot = slot == kInvalidRuleId;
    if (slot == kInvalidRuleId) {
        slot = allocateRuleSlot();
        if (slot == kInvalidRuleId) {
            return false;
        }
    }

    // A staged-rule handle must never share an epoch with the active rule that
    // previously occupied this reusable slot. This also invalidates handles
    // issued before an in-place staged replacement.
    advanceRuleEpoch();
    if (new_slot) {
        staged_[slot].in_use = true;
        staged_[slot].rule_id = slot;
        staged_[slot].sequence = allocateSequence();
        ++staged_count_;
    }

    staged_[slot].request = normalized;
    staged_[slot].pending_runtime_value = initialRuntimeValue(normalized);
    staged_[slot].pending_enabled = normalized.enabled;
    staged_[slot].publish_runtime_on_commit = true;

    if (out_rule_id != nullptr) {
        *out_rule_id = slot;
    }
    return true;
}

bool MutationEngine::stageMutation(const SignalMutation& mutation) {
    RuleStageRequest request{};
    request.kind = RuleKind::BIT_RANGE;
    request.can_id = mutation.can_id;
    request.direction = mutation.direction;
    request.start_bit = mutation.start_bit;
    request.bit_length = mutation.length;
    request.little_endian = mutation.little_endian;
    request.dynamic_value = false;
    request.enabled = mutation.enabled;

    switch (mutation.operation) {
    case MutationOperation::REPLACE:
        request.replace_value = convertPhysicalToRaw(mutation, mutation.op_value1);
        break;
    case MutationOperation::PASS_THROUGH:
        request.replace_value = 0U;
        request.enabled = false;
        break;
    default:
        // Non-deterministic arithmetic ops are intentionally rejected by the runtime engine.
        return false;
    }

    return stageRule(request, nullptr);
}

void MutationEngine::clearStaging() {
    // Slots are reused from zero. Invalidate every handle issued before the
    // staging set was discarded, even when the active table remains unchanged.
    advanceRuleEpoch();
    if (staged_ == nullptr) {
        staged_count_ = 0U;
        return;
    }
    for (size_t i = 0; i < kMaxRules; ++i) {
        staged_[i] = {};
    }
    staged_count_ = 0U;
}

void MutationEngine::revertStagingToActive() {
    clearStaging();
    for (uint16_t i = 0; i < committed_count_; ++i) {
        const StagedRule& src = committed_shadow_[i];
        if (!src.in_use || src.rule_id == kInvalidRuleId || src.rule_id >= kMaxRules) {
            continue;
        }
        staged_[src.rule_id] = src;
        staged_[src.rule_id].in_use = true;
        ++staged_count_;
    }
}

bool MutationEngine::applyCommit() {
    if (staged_ == nullptr || committed_shadow_ == nullptr) {
        return false;
    }
    ActiveRuleTable* next_table = inactiveTable();
    if (next_table == nullptr) {
        return false;
    }
    clearActiveTable(*next_table);

    uint16_t ordered_rule_ids[kMaxRules];
    uint16_t ordered_count = 0U;

    for (uint16_t slot = 0; slot < kMaxRules; ++slot) {
        if (!staged_[slot].in_use) {
            continue;
        }

        uint16_t insert_at = ordered_count;
        while (insert_at > 0U && staged_[ordered_rule_ids[insert_at - 1U]].sequence > staged_[slot].sequence) {
            ordered_rule_ids[insert_at] = ordered_rule_ids[insert_at - 1U];
            --insert_at;
        }
        ordered_rule_ids[insert_at] = slot;
        ++ordered_count;
    }

    for (uint16_t i = 0; i < ordered_count; ++i) {
        const StagedRule& staged_rule = staged_[ordered_rule_ids[i]];
        RuleGroup* group = ensureGroup(*next_table, staged_rule.request.can_id, staged_rule.request.direction);
        if (group == nullptr) {
            return false;
        }
        ++group->rule_count;
    }

    uint16_t start = 0U;
    for (uint16_t group_index = 0; group_index < next_table->group_count; ++group_index) {
        RuleGroup& group = next_table->groups[group_index];
        const uint16_t count = group.rule_count;
        group.first_rule = start;
        group.rule_count = 0U;
        start = static_cast<uint16_t>(start + count);
    }

    uint16_t write_cursor[kMaxRules];
    for (uint16_t i = 0; i < next_table->group_count; ++i) {
        write_cursor[i] = next_table->groups[i].first_rule;
    }

    uint16_t priority = 0U;
    constexpr RuleKind kCompileOrder[] = {
        RuleKind::BIT_RANGE,
        RuleKind::RAW_MASK,
        RuleKind::COUNTER,
        RuleKind::SEQUENCE8,
        RuleKind::CHECKSUM_XOR,
        RuleKind::CHECKSUM_CRC8_AUTOSAR,
    };
    for (RuleKind wanted_kind : kCompileOrder) {

        for (uint16_t order_index = 0; order_index < ordered_count; ++order_index) {
            const uint16_t rule_id = ordered_rule_ids[order_index];
            const StagedRule& staged_rule = staged_[rule_id];
            if (staged_rule.request.kind != wanted_kind) {
                continue;
            }

            RuleGroup* group = ensureGroup(*next_table, staged_rule.request.can_id, staged_rule.request.direction);
            if (group == nullptr) {
                return false;
            }

            const uint16_t group_idx = static_cast<uint16_t>(group - next_table->groups);
            const uint16_t write_index = write_cursor[group_idx];
            if (write_index >= kMaxRules) {
                return false;
            }

            CompiledRule compiled{};
            if (!compileRule(staged_rule, priority, compiled)) {
                return false;
            }

            next_table->rules[write_index] = compiled;
            ++write_cursor[group_idx];
            ++group->rule_count;
            ++priority;
        }
    }

    next_table->rule_count = priority;

    // Compilation above is the only fallible part of a commit. Keep candidate
    // runtime values isolated until that work is complete so a rejected rule
    // package cannot alter state still referenced by the active table. New
    // mutation readers pass frames through while the committer drains existing
    // readers and publishes the runtime/table pair as one quiescent transition.
    pauseMutationReads();
    for (uint16_t i = 0; i < kMaxRules; ++i) {
        StagedRule& staged_rule = staged_[i];
        if (!staged_rule.in_use) {
            runtime_state_[i].current_value.store(0U, std::memory_order_release);
            runtime_state_[i].enabled.store(0U, std::memory_order_release);
            continue;
        }
        if (staged_rule.publish_runtime_on_commit) {
            runtime_state_[i].current_value.store(
                staged_rule.pending_runtime_value,
                std::memory_order_release);
            runtime_state_[i].enabled.store(
                staged_rule.pending_enabled ? 1U : 0U,
                std::memory_order_release);
            staged_rule.publish_runtime_on_commit = false;
        }
    }

    for (uint16_t i = 0; i < kMaxRules; ++i) {
        committed_shadow_[i] = {};
    }
    committed_count_ = 0U;
    for (uint16_t i = 0; i < ordered_count; ++i) {
        const StagedRule& src = staged_[ordered_rule_ids[i]];
        committed_shadow_[committed_count_++] = src;
    }

    swapActiveTable();
    advanceRuleEpoch();
    resumeMutationReads();
    return true;
}

size_t MutationEngine::stagingCount() const {
    return staged_count_.load(std::memory_order_acquire);
}

size_t MutationEngine::activeCount() const {
    return active_count_.load(std::memory_order_acquire);
}

uint32_t MutationEngine::ruleEpoch() const {
    return rule_epoch_.load(std::memory_order_acquire);
}

bool MutationEngine::hasRulesForFrame(uint32_t can_id, Direction direction) const {
    if (!beginMutationRead()) {
        return false;
    }
    MutationReadScope read_scope(mutation_readers_);
    const ActiveRuleTable* table = activeTable();
    if (table == nullptr || table->rule_count == 0U) {
        return false;
    }

    const RuleGroup* group = findGroup(*table, can_id, direction);
    if (group == nullptr || group->rule_count == 0U) {
        return false;
    }

    const uint16_t begin = group->first_rule;
    const uint16_t end = static_cast<uint16_t>(group->first_rule + group->rule_count);
    for (uint16_t i = begin; i < end; ++i) {
        const uint16_t rule_id = table->rules[i].rule_id;
        if (rule_id < kMaxRules &&
            runtime_state_[rule_id].enabled.load(std::memory_order_relaxed) != 0U) {
            return true;
        }
    }

    return false;
}

size_t MutationEngine::applyFrameMutations(CanFrame& frame) const {
    if (!beginMutationRead()) {
        return 0U;
    }
    MutationReadScope read_scope(mutation_readers_);
    const ActiveRuleTable* table = activeTable();
    if (table == nullptr || table->rule_count == 0U) {
        return 0U;
    }

    const RuleGroup* group = findGroup(*table, frame.id, frame.direction);
    if (group == nullptr || group->rule_count == 0U) {
        return 0U;
    }

    size_t applied = 0U;
    const uint16_t begin = group->first_rule;
    const uint16_t end = static_cast<uint16_t>(group->first_rule + group->rule_count);
    for (uint16_t i = begin; i < end; ++i) {
        const CompiledRule& rule = table->rules[i];
        if (rule.rule_id >= kMaxRules) {
            continue;
        }

        const bool enabled = runtime_state_[rule.rule_id].enabled.load(std::memory_order_relaxed) != 0U;
        if (!enabled) {
            continue;
        }
        uint8_t selector = 0U;
        if (!selectorAllows(rule, selector)) {
            continue;
        }

        if (rule.source.kind == RuleKind::COUNTER) {
            applyCounterRule(rule, frame);
        } else if (rule.source.kind == RuleKind::SEQUENCE8) {
            applySequenceRule(rule, frame);
        } else if (rule.source.kind == RuleKind::CHECKSUM_XOR) {
            applyXorChecksumRule(rule, frame);
        } else if (rule.source.kind == RuleKind::CHECKSUM_CRC8_AUTOSAR) {
            applyAutosarChecksumRule(rule, frame);
        } else if (rule.source.dynamic_value && rule.source.kind == RuleKind::BIT_RANGE) {
            applyDynamicRule(rule, frame);
        } else {
            applyStaticRule(rule, frame);
        }

        ++applied;
    }

    return applied;
}

int32_t MutationEngine::registerDynamicSignalRule(
    uint32_t can_id,
    Direction direction,
    uint16_t start_bit,
    uint8_t bit_length,
    bool little_endian,
    uint32_t initial_value,
    bool enabled) {

    RuleStageRequest request{};
    request.kind = RuleKind::BIT_RANGE;
    request.can_id = can_id;
    request.direction = direction;
    request.enabled = enabled;
    request.start_bit = start_bit;
    request.bit_length = bit_length;
    request.little_endian = little_endian;
    request.dynamic_value = true;
    request.replace_value = initial_value;

    uint16_t rule_id = kInvalidRuleId;
    if (!stageRule(request, &rule_id)) {
        return -1;
    }
    if (!applyCommit()) {
        return -1;
    }
    return static_cast<int32_t>(rule_id);
}

bool MutationEngine::setRuleValue(uint16_t rule_id, uint32_t value, uint32_t expected_epoch) {
    if (rule_id >= kMaxRules || !beginMutationRead()) {
        return false;
    }
    MutationReadScope read_scope(mutation_readers_);
    if (expected_epoch != ruleEpoch() || !ruleExistsForControl(rule_id)) return false;
    if (staged_ != nullptr && staged_[rule_id].in_use && staged_[rule_id].publish_runtime_on_commit) {
        staged_[rule_id].pending_runtime_value = value;
        return true;
    }
    runtime_state_[rule_id].current_value.store(value, std::memory_order_release);
    return true;
}

bool MutationEngine::enableRule(uint16_t rule_id, bool enabled, uint32_t expected_epoch) {
    if (rule_id >= kMaxRules || !beginMutationRead()) {
        return false;
    }
    MutationReadScope read_scope(mutation_readers_);
    if (expected_epoch != ruleEpoch() || !ruleExistsForControl(rule_id)) return false;

    const bool pending_commit = staged_ != nullptr && staged_[rule_id].in_use &&
        staged_[rule_id].publish_runtime_on_commit;
    if (staged_ != nullptr && staged_[rule_id].in_use) {
        staged_[rule_id].request.enabled = enabled;
        staged_[rule_id].pending_enabled = enabled;
    }

    if (pending_commit) {
        return true;
    }

    if (committed_shadow_ != nullptr) {
        for (uint16_t i = 0; i < committed_count_; ++i) {
            if (committed_shadow_[i].rule_id == rule_id) {
                committed_shadow_[i].request.enabled = enabled;
                committed_shadow_[i].pending_enabled = enabled;
            }
        }
    }

    runtime_state_[rule_id].enabled.store(enabled ? 1U : 0U, std::memory_order_release);
    return true;
}

void MutationEngine::clearRules() {
    clearStaging();
    static_cast<void>(applyCommit());
}

size_t MutationEngine::listRules(RuleListEntry* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0U;
    }

    if (!beginMutationRead()) {
        return 0U;
    }
    MutationReadScope read_scope(mutation_readers_);
    const ActiveRuleTable* table = activeTable();
    if (table == nullptr) {
        return 0U;
    }

    const size_t count = (table->rule_count < capacity) ? table->rule_count : capacity;
    for (size_t i = 0; i < count; ++i) {
        const CompiledRule& src = table->rules[i];
        RuleListEntry& dst = out_entries[i];
        dst.rule_id = src.rule_id;
        dst.priority = src.priority;
        dst.epoch = ruleEpoch();
        dst.request = src.source;
        dst.active = runtime_state_[src.rule_id].enabled.load(std::memory_order_acquire) != 0U;
    }
    return count;
}

size_t MutationEngine::listStagedRules(RuleListEntry* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U || staged_ == nullptr) {
        return 0U;
    }

    // Staging slots are reusable runtime handles, so slot number is not rule
    // order. Reproduce the same bounded sequence ordering used by applyCommit
    // without allocating or exposing the private staging array.
    uint16_t ordered_rule_ids[kMaxRules] = {};
    uint16_t ordered_count = 0U;
    for (uint16_t slot = 0U; slot < kMaxRules; ++slot) {
        if (!staged_[slot].in_use) continue;

        uint16_t insert_at = ordered_count;
        while (insert_at > 0U &&
               staged_[ordered_rule_ids[insert_at - 1U]].sequence > staged_[slot].sequence) {
            ordered_rule_ids[insert_at] = ordered_rule_ids[insert_at - 1U];
            --insert_at;
        }
        ordered_rule_ids[insert_at] = slot;
        ++ordered_count;
    }

    const size_t count = ordered_count < capacity ? ordered_count : capacity;
    const uint32_t epoch = ruleEpoch();
    for (size_t i = 0U; i < count; ++i) {
        const StagedRule& src = staged_[ordered_rule_ids[i]];
        RuleListEntry& dst = out_entries[i];
        dst = {};
        dst.rule_id = src.rule_id;
        dst.priority = static_cast<uint16_t>(i);
        dst.epoch = epoch;
        dst.request = src.request;

        // Candidate enable state is separate from whether a rule is already
        // applied. Preserve the existing RuleListEntry field for compatibility
        // while making request.enabled authoritative for candidate clients.
        dst.request.enabled = src.pending_enabled;
        dst.active = src.pending_enabled;

        // Dynamic bit-range controls may be adjusted after staging. Surface
        // the value that would be committed, or the live value after commit,
        // instead of the package's now-stale initial value.
        if (src.request.kind == RuleKind::BIT_RANGE && src.request.dynamic_value) {
            dst.request.replace_value = src.publish_runtime_on_commit
                ? src.pending_runtime_value
                : runtime_state_[src.rule_id].current_value.load(std::memory_order_acquire);
        }
    }
    return count;
}

uint32_t MutationEngine::keyHash(uint32_t can_id, Direction direction) {
    const uint32_t seed = can_id ^ (static_cast<uint32_t>(direction) * 0x9E3779B9U);
    return seed ^ (seed >> 16U);
}

uint16_t MutationEngine::nextMotorolaBit(uint16_t current_bit) {
    if ((current_bit % 8U) == 0U) {
        return static_cast<uint16_t>(current_bit + 15U);
    }
    return static_cast<uint16_t>(current_bit - 1U);
}

bool MutationEngine::normalizeRule(RuleStageRequest& rule) {
    // Standard and extended CAN identifiers share this engine. Reject values
    // outside the 29-bit wire format before a controller driver can mask them.
    if (rule.can_id > 0x1FFFFFFFU) return false;
    if (rule.kind == RuleKind::BIT_RANGE || rule.kind == RuleKind::COUNTER || rule.kind == RuleKind::SEQUENCE8) {
        if (rule.bit_length < 1U || rule.bit_length > 64U) {
            return false;
        }
        if (rule.start_bit > 63U) {
            return false;
        }
        if ((rule.dynamic_value || rule.kind == RuleKind::COUNTER) && rule.bit_length > 32U) {
            return false;
        }
        if (rule.kind == RuleKind::SEQUENCE8 &&
            (rule.sequence_count == 0U || rule.sequence_count > 16U || rule.bit_length > 8U ||
             rule.sequence_initial_index >= rule.sequence_count)) {
            return false;
        }
        if (rule.start_bit + rule.bit_length > 64U && rule.little_endian) {
            return false;
        }

        uint8_t test_mask[8] = {0};
        uint8_t test_value[8] = {0};
        return buildBitMaskAndValue(rule.start_bit, rule.bit_length, rule.little_endian, rule.replace_value, test_mask, test_value);
    }

    if (rule.kind == RuleKind::RAW_MASK) {
        return true;
    }

    if (rule.kind == RuleKind::CHECKSUM_XOR) {
        return rule.checksum_target_byte < 8U && rule.checksum_start_byte < 8U &&
            rule.checksum_end_byte < 8U && rule.checksum_start_byte <= rule.checksum_end_byte;
    }

    if (rule.kind == RuleKind::CHECKSUM_CRC8_AUTOSAR) {
        return rule.checksum_target_byte < 8U && rule.checksum_counter_byte < 8U &&
            rule.checksum_start_byte < 8U && rule.checksum_end_byte < 8U &&
            rule.checksum_start_byte <= rule.checksum_end_byte && rule.sequence_count == 16U;
    }

    return false;
}

void MutationEngine::clearActiveTable(ActiveRuleTable& table) {
    table.rule_count = 0U;
    table.group_count = 0U;
    std::memset(table.bucket_head, 0xFF, sizeof(table.bucket_head));
}

bool MutationEngine::buildBitMaskAndValue(
    uint16_t start_bit,
    uint8_t bit_length,
    bool little_endian,
    uint64_t raw_value,
    uint8_t out_mask[8],
    uint8_t out_value[8]) {

    std::memset(out_mask, 0, 8U);
    std::memset(out_value, 0, 8U);

    if (bit_length < 1U || bit_length > 64U || start_bit > 63U) {
        return false;
    }

    if (little_endian) {
        for (uint8_t i = 0; i < bit_length; ++i) {
            const uint16_t bit_index = static_cast<uint16_t>(start_bit + i);
            if (bit_index > 63U) {
                return false;
            }
            const uint8_t byte_idx = static_cast<uint8_t>(bit_index / 8U);
            const uint8_t bit_in_byte = static_cast<uint8_t>(bit_index % 8U);
            const uint8_t bit_mask = static_cast<uint8_t>(1U << bit_in_byte);
            out_mask[byte_idx] |= bit_mask;

            if (((raw_value >> i) & 1ULL) != 0ULL) {
                out_value[byte_idx] |= bit_mask;
            }
        }
        return true;
    }

    uint16_t bit_index = start_bit;
    for (uint8_t i = 0; i < bit_length; ++i) {
        if (bit_index > 63U) {
            return false;
        }

        const uint8_t byte_idx = static_cast<uint8_t>(bit_index / 8U);
        const uint8_t bit_in_byte = static_cast<uint8_t>(bit_index % 8U);
        const uint8_t bit_mask = static_cast<uint8_t>(1U << bit_in_byte);
        out_mask[byte_idx] |= bit_mask;

        const uint8_t raw_bit = static_cast<uint8_t>((bit_length - 1U) - i);
        if (((raw_value >> raw_bit) & 1ULL) != 0ULL) {
            out_value[byte_idx] |= bit_mask;
        }

        bit_index = nextMotorolaBit(bit_index);
    }
    return true;
}

bool MutationEngine::buildDynamicBitPositions(
    uint16_t start_bit,
    uint8_t bit_length,
    bool little_endian,
    uint8_t out_positions[32]) {

    if (bit_length < 1U || bit_length > 32U || start_bit > 63U) {
        return false;
    }

    if (little_endian) {
        for (uint8_t bit = 0; bit < bit_length; ++bit) {
            const uint16_t frame_bit = static_cast<uint16_t>(start_bit + bit);
            if (frame_bit > 63U) {
                return false;
            }
            out_positions[bit] = static_cast<uint8_t>(frame_bit);
        }
        return true;
    }

    uint16_t frame_bit = start_bit;
    for (uint8_t i = 0; i < bit_length; ++i) {
        if (frame_bit > 63U) {
            return false;
        }

        const uint8_t raw_bit = static_cast<uint8_t>((bit_length - 1U) - i);
        out_positions[raw_bit] = static_cast<uint8_t>(frame_bit);
        frame_bit = nextMotorolaBit(frame_bit);
    }
    return true;
}

uint16_t MutationEngine::findStagedIdentity(
    const StagedRule* staged,
    uint16_t count,
    const RuleStageRequest& request) {
    for (uint16_t i = 0; i < count; ++i) {
        const StagedRule& candidate = staged[i];
        if (!candidate.in_use) {
            continue;
        }

        const RuleStageRequest& existing = candidate.request;
        if (existing.kind != request.kind ||
            existing.can_id != request.can_id ||
            existing.direction != request.direction) {
            continue;
        }

        if (request.kind == RuleKind::BIT_RANGE || request.kind == RuleKind::COUNTER ||
            request.kind == RuleKind::SEQUENCE8) {
            if (existing.start_bit == request.start_bit &&
                existing.bit_length == request.bit_length &&
                existing.little_endian == request.little_endian &&
                existing.dynamic_value == request.dynamic_value) {
                return i;
            }
            continue;
        }

        if (request.kind == RuleKind::CHECKSUM_XOR || request.kind == RuleKind::CHECKSUM_CRC8_AUTOSAR) {
            if (existing.checksum_target_byte == request.checksum_target_byte) return i;
            continue;
        }

        // One RAW_MASK rule per (can_id, direction) identity in staging.
        return i;
    }
    return kInvalidRuleId;
}

const MutationEngine::ActiveRuleTable* MutationEngine::activeTable() const {
    return active_table_.load(std::memory_order_acquire);
}

MutationEngine::ActiveRuleTable* MutationEngine::inactiveTable() {
    const uint8_t next_idx = (active_table_index_ == 0U) ? 1U : 0U;
    return tables_[next_idx];
}

void MutationEngine::swapActiveTable() {
    active_table_index_ = (active_table_index_ == 0U) ? 1U : 0U;
    active_table_.store(tables_[active_table_index_], std::memory_order_release);
    active_count_.store(tables_[active_table_index_]->rule_count, std::memory_order_release);
}

const MutationEngine::RuleGroup* MutationEngine::findGroup(
    const ActiveRuleTable& table,
    uint32_t can_id,
    Direction direction) const {

    const uint32_t bucket = keyHash(can_id, direction) % kBucketCount;
    int16_t idx = table.bucket_head[bucket];
    while (idx >= 0) {
        const RuleGroup& group = table.groups[idx];
        if (group.can_id == can_id && group.direction == direction) {
            return &group;
        }
        idx = group.next;
    }

    return nullptr;
}

MutationEngine::RuleGroup* MutationEngine::ensureGroup(ActiveRuleTable& table, uint32_t can_id, Direction direction) {
    const uint32_t bucket = keyHash(can_id, direction) % kBucketCount;
    int16_t idx = table.bucket_head[bucket];
    while (idx >= 0) {
        RuleGroup& existing = table.groups[idx];
        if (existing.can_id == can_id && existing.direction == direction) {
            return &existing;
        }
        idx = existing.next;
    }

    if (table.group_count >= kMaxRules) {
        return nullptr;
    }

    RuleGroup& group = table.groups[table.group_count];
    group.can_id = can_id;
    group.direction = direction;
    group.first_rule = 0U;
    group.rule_count = 0U;
    group.next = table.bucket_head[bucket];
    table.bucket_head[bucket] = static_cast<int16_t>(table.group_count);
    ++table.group_count;
    return &group;
}

bool MutationEngine::compileRule(const StagedRule& staged_rule, uint16_t priority, CompiledRule& out_rule) {
    out_rule = {};
    out_rule.rule_id = staged_rule.rule_id;
    out_rule.priority = priority;
    out_rule.source = staged_rule.request;
    std::memset(out_rule.clear_mask, 0xFF, sizeof(out_rule.clear_mask));
    std::memset(out_rule.static_set_bits, 0, sizeof(out_rule.static_set_bits));
    out_rule.dynamic_bit_count = 0U;
    out_rule.runtime_value_index = -1;
    out_rule.runtime_table_index = -1;
    out_rule.override_active_index = -1;
    out_rule.override_value_index = -1;
    out_rule.selector_index = -1;
    std::memset(out_rule.dynamic_bit_positions, 0, sizeof(out_rule.dynamic_bit_positions));

    if (staged_rule.request.selector_source[0] != '\0') {
        if (runtime_values_ == nullptr) return false;
        const int32_t index = runtime_values_->find(staged_rule.request.selector_source);
        if (index < 0 || index > 0x7FFF) return false;
        out_rule.selector_index = static_cast<int16_t>(index);
    }

    if (staged_rule.request.kind == RuleKind::RAW_MASK) {
        for (uint8_t i = 0; i < 8U; ++i) {
            out_rule.clear_mask[i] = static_cast<uint8_t>(~staged_rule.request.mask[i]);
            out_rule.static_set_bits[i] = static_cast<uint8_t>(staged_rule.request.value[i] & staged_rule.request.mask[i]);
        }
        return true;
    }

    if (staged_rule.request.kind == RuleKind::CHECKSUM_XOR ||
        staged_rule.request.kind == RuleKind::CHECKSUM_CRC8_AUTOSAR) return true;

    uint8_t bit_mask[8] = {0};
    uint8_t bit_value[8] = {0};
    const bool runtime_bits = staged_rule.request.dynamic_value || staged_rule.request.kind == RuleKind::COUNTER ||
        staged_rule.request.kind == RuleKind::SEQUENCE8;
    const uint64_t static_value = runtime_bits ? 0ULL : staged_rule.request.replace_value;
    if (!buildBitMaskAndValue(
            staged_rule.request.start_bit,
            staged_rule.request.bit_length,
            staged_rule.request.little_endian,
            static_value,
            bit_mask,
            bit_value)) {
        return false;
    }

    for (uint8_t i = 0; i < 8U; ++i) {
        out_rule.clear_mask[i] = static_cast<uint8_t>(~bit_mask[i]);
        out_rule.static_set_bits[i] = static_cast<uint8_t>(bit_value[i] & bit_mask[i]);
    }

    if (runtime_bits) {
        if (staged_rule.request.value_source[0] != '\0') {
            if (runtime_values_ == nullptr) return false;
            const int32_t index = runtime_values_->find(staged_rule.request.value_source);
            if (index < 0 || index > 0x7FFF) return false;
            out_rule.runtime_value_index = static_cast<int16_t>(index);
        }
        if (staged_rule.request.lookup_table[0] != '\0') {
            if (runtime_tables_ == nullptr) return false;
            const int32_t index = runtime_tables_->find(staged_rule.request.lookup_table);
            if (index < 0 || index > 0x7FFF) return false;
            out_rule.runtime_table_index = static_cast<int16_t>(index);
        }
        if (staged_rule.request.override_active_source[0] != '\0' ||
            staged_rule.request.override_value_source[0] != '\0') {
            if (runtime_values_ == nullptr || staged_rule.request.override_active_source[0] == '\0' ||
                staged_rule.request.override_value_source[0] == '\0') return false;
            const int32_t active_index = runtime_values_->find(staged_rule.request.override_active_source);
            const int32_t value_index = runtime_values_->find(staged_rule.request.override_value_source);
            if (active_index < 0 || value_index < 0 || active_index > 0x7FFF || value_index > 0x7FFF) return false;
            out_rule.override_active_index = static_cast<int16_t>(active_index);
            out_rule.override_value_index = static_cast<int16_t>(value_index);
        }
        out_rule.dynamic_bit_count = staged_rule.request.bit_length;
        if (!buildDynamicBitPositions(
            staged_rule.request.start_bit,
            staged_rule.request.bit_length,
            staged_rule.request.little_endian,
            out_rule.dynamic_bit_positions)) return false;
    }

    return true;
}

void MutationEngine::applyCounterRule(const CompiledRule& rule, CanFrame& frame) const {
    applyDynamicRule(rule, frame);
    const uint32_t current = runtime_state_[rule.rule_id].current_value.load(std::memory_order_relaxed);
    const uint32_t next = current >= rule.source.counter_wrap_after
        ? rule.source.counter_wrap_to
        : current + rule.source.counter_step;
    runtime_state_[rule.rule_id].current_value.store(next, std::memory_order_relaxed);
}

void MutationEngine::applySequenceRule(const CompiledRule& rule, CanFrame& frame) const {
    const uint32_t index = runtime_state_[rule.rule_id].current_value.load(std::memory_order_relaxed);
    const uint8_t safe_index = static_cast<uint8_t>(index % rule.source.sequence_count);
    runtime_state_[rule.rule_id].current_value.store(
        static_cast<uint32_t>((safe_index + 1U) % rule.source.sequence_count),
        std::memory_order_relaxed);
    // applyDynamicRule reads current_value, so temporarily publish the selected
    // byte and then restore the next sequence index after applying it.
    runtime_state_[rule.rule_id].current_value.store(rule.source.sequence_values[safe_index], std::memory_order_relaxed);
    applyDynamicRule(rule, frame);
    runtime_state_[rule.rule_id].current_value.store(
        static_cast<uint32_t>((safe_index + 1U) % rule.source.sequence_count),
        std::memory_order_relaxed);
}

void MutationEngine::applyXorChecksumRule(const CompiledRule& rule, CanFrame& frame) {
    uint8_t checksum = rule.source.checksum_seed;
    for (uint8_t i = rule.source.checksum_start_byte; i <= rule.source.checksum_end_byte; ++i) {
        checksum = static_cast<uint8_t>(checksum ^ frame.data[i]);
    }
    frame.data[rule.source.checksum_target_byte] = checksum;
}

void MutationEngine::applyAutosarChecksumRule(const CompiledRule& rule, CanFrame& frame) {
    // Counter-indexed AUTOSAR-style E2E profile: CRC-8/AUTOSAR (poly 0x2F,
    // init/xorout 0xFF) over the protected payload followed by the selected
    // S-PDU Data ID. The rule package supplies the 16-entry Data-ID sequence.
    uint8_t crc = 0xFFU;
    const auto update = [&crc](uint8_t value) {
        crc = static_cast<uint8_t>(crc ^ value);
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x80U) != 0U
                ? static_cast<uint8_t>((crc << 1U) ^ 0x2FU)
                : static_cast<uint8_t>(crc << 1U);
        }
    };

    for (uint8_t i = rule.source.checksum_start_byte; i <= rule.source.checksum_end_byte; ++i) {
        update(frame.data[i]);
    }
    const uint8_t counter = static_cast<uint8_t>(frame.data[rule.source.checksum_counter_byte] & 0x0FU);
    update(rule.source.sequence_values[counter]);
    frame.data[rule.source.checksum_target_byte] = static_cast<uint8_t>(crc ^ 0xFFU);
}

void MutationEngine::applyStaticRule(const CompiledRule& rule, CanFrame& frame) {
    applyMask(frame.data, rule.clear_mask, rule.static_set_bits);
}

bool MutationEngine::selectorAllows(const CompiledRule& rule, uint8_t& selector) const {
    selector = 0U;
    if (rule.selector_index < 0) return true;
    if (runtime_values_ == nullptr) return false;
    float raw = 0.0F;
    if (!runtime_values_->read(static_cast<uint16_t>(rule.selector_index), raw) || raw < 0.0F || raw > 15.0F) {
        return false;
    }
    selector = static_cast<uint8_t>(raw + 0.5F);
    return (rule.source.selector_active_mask & static_cast<uint16_t>(1U << selector)) != 0U;
}

void MutationEngine::applyDynamicRule(const CompiledRule& rule, CanFrame& frame) const {
    for (uint8_t i = 0; i < 8U; ++i) {
        frame.data[i] = static_cast<uint8_t>(frame.data[i] & rule.clear_mask[i]);
    }

    uint32_t value = runtime_state_[rule.rule_id].current_value.load(std::memory_order_relaxed);
    uint8_t selector = 0U;
    if (!selectorAllows(rule, selector)) return;
    if (rule.runtime_value_index >= 0 && runtime_values_ != nullptr) {
        float source = 0.0F;
        if (runtime_values_->read(static_cast<uint16_t>(rule.runtime_value_index), source)) {
            float override_active = 0.0F;
            float override_value = 0.0F;
            const bool use_override = rule.override_active_index >= 0 && rule.override_value_index >= 0 &&
                runtime_values_->read(static_cast<uint16_t>(rule.override_active_index), override_active) &&
                runtime_values_->read(static_cast<uint16_t>(rule.override_value_index), override_value) &&
                override_active >= 0.5F;
            const bool selector_direct = rule.selector_index >= 0 &&
                (rule.source.selector_direct_mask & static_cast<uint16_t>(1U << selector)) != 0U;
            const float output_scale = rule.source.selector_maps_output
                ? rule.source.selector_output_scale[selector]
                : rule.source.output_scale;
            const uint32_t full_output = rule.source.selector_maps_output
                ? rule.source.selector_full_output[selector]
                : rule.source.full_output;
            if (selector_direct) {
                value = rule.source.selector_direct_output[selector];
            } else if (use_override) {
                float transformed = override_value * output_scale + rule.source.output_offset;
                if (transformed < 0.0F) transformed = 0.0F;
                value = static_cast<uint32_t>(transformed + (rule.source.truncate_output ? 0.0F : 0.5F));
            } else if (rule.source.zero_override && source <= rule.source.zero_threshold) {
                value = rule.source.zero_output;
            } else if (rule.source.full_override && source >= rule.source.full_threshold) {
                value = full_output;
            } else {
                float affine = (source * rule.source.source_gain) + rule.source.source_offset;
                uint32_t table_output = 0U;
                if (rule.runtime_table_index >= 0 && runtime_tables_ != nullptr &&
                    runtime_tables_->lookupFirstAtLeast(
                        static_cast<uint16_t>(rule.runtime_table_index), source, table_output)) {
                    affine = static_cast<float>(table_output);
                }
                if (rule.source.truncate_affine) affine = std::floor(affine);
                float transformed = affine * output_scale + rule.source.output_offset;
                if (transformed < 0.0F) transformed = 0.0F;
                const uint64_t max_value = (rule.dynamic_bit_count >= 32U)
                    ? 0xFFFFFFFFULL
                    : ((1ULL << rule.dynamic_bit_count) - 1ULL);
                if (transformed > static_cast<float>(max_value)) transformed = static_cast<float>(max_value);
                value = static_cast<uint32_t>(transformed + (rule.source.truncate_output ? 0.0F : 0.5F));
            }
        }
    }
    for (uint8_t bit = 0; bit < rule.dynamic_bit_count; ++bit) {
        if ((value & (1UL << bit)) == 0UL) {
            continue;
        }
        const uint8_t frame_bit = rule.dynamic_bit_positions[bit];
        const uint8_t byte_idx = static_cast<uint8_t>(frame_bit / 8U);
        const uint8_t bit_idx = static_cast<uint8_t>(frame_bit % 8U);
        frame.data[byte_idx] = static_cast<uint8_t>(frame.data[byte_idx] | static_cast<uint8_t>(1U << bit_idx));
    }
}

void MutationEngine::applyMask(uint8_t frame_data[8], const uint8_t clear_mask[8], const uint8_t set_bits[8]) {
    for (uint8_t i = 0; i < 8U; ++i) {
        frame_data[i] = static_cast<uint8_t>((frame_data[i] & clear_mask[i]) | set_bits[i]);
    }
}

uint16_t MutationEngine::allocateRuleSlot() {
    for (uint16_t i = 0; i < kMaxRules; ++i) {
        if (!staged_[i].in_use) {
            return i;
        }
    }
    return kInvalidRuleId;
}

uint16_t MutationEngine::allocateSequence() {
    const uint16_t value = static_cast<uint16_t>(next_sequence_ & 0xFFFFU);
    ++next_sequence_;
    if (next_sequence_ == 0U) {
        next_sequence_ = 1U;
    }
    return value;
}

void MutationEngine::advanceRuleEpoch() {
    uint32_t next_epoch = rule_epoch_.load(std::memory_order_relaxed) + 1U;
    if (next_epoch == 0U) next_epoch = 1U;
    rule_epoch_.store(next_epoch, std::memory_order_release);
}

bool MutationEngine::ruleExistsForControl(uint16_t rule_id) const {
    if (rule_id >= kMaxRules) return false;
    if (staged_ != nullptr && staged_[rule_id].in_use) return true;
    if (committed_shadow_ == nullptr) return false;
    for (uint16_t i = 0U; i < committed_count_; ++i) {
        if (committed_shadow_[i].in_use && committed_shadow_[i].rule_id == rule_id) return true;
    }
    return false;
}

bool MutationEngine::beginMutationRead() const {
    if (mutation_reads_enabled_.load(std::memory_order_seq_cst) == 0U) {
        return false;
    }
    mutation_readers_.fetch_add(1U, std::memory_order_seq_cst);
    if (mutation_reads_enabled_.load(std::memory_order_seq_cst) != 0U) {
        return true;
    }
    mutation_readers_.fetch_sub(1U, std::memory_order_seq_cst);
    return false;
}

void MutationEngine::pauseMutationReads() {
    mutation_reads_enabled_.store(0U, std::memory_order_seq_cst);
    while (mutation_readers_.load(std::memory_order_seq_cst) != 0U) {
        yieldCommitter();
    }
}

void MutationEngine::resumeMutationReads() {
    mutation_reads_enabled_.store(1U, std::memory_order_seq_cst);
}

}  // namespace bored::signalscope
