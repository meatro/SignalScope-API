#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope { class RuntimeValueRegistry; }
namespace bored::signalscope { class RuntimeTableRegistry; }

namespace bored::signalscope {

enum class RuleKind : uint8_t {
    BIT_RANGE = 0,
    RAW_MASK = 1,
    COUNTER = 2,
    CHECKSUM_XOR = 3,
    SEQUENCE8 = 4,
    CHECKSUM_CRC8_AUTOSAR = 5,
};

struct RuleStageRequest {
    RuleKind kind = RuleKind::BIT_RANGE;
    uint32_t can_id = 0;
    Direction direction = Direction::A_TO_B;
    bool enabled = true;

    // Bit-range mode
    uint16_t start_bit = 0;
    uint8_t bit_length = 1;
    bool little_endian = true;
    bool dynamic_value = false;
    uint64_t replace_value = 0;
    char value_source[32] = {0};
    char lookup_table[32] = {0};
    char override_active_source[32] = {0};
    char override_value_source[32] = {0};
    float source_gain = 1.0F;
    float source_offset = 0.0F;
    float output_scale = 1.0F;
    float output_offset = 0.0F;
    bool truncate_affine = false;
    bool truncate_output = false;
    bool zero_override = false;
    float zero_threshold = 0.0F;
    uint32_t zero_output = 0U;
    bool full_override = false;
    float full_threshold = 100.0F;
    uint32_t full_output = 0U;
    char selector_source[32] = {0};
    // True only for SOURCE_SELECT_INT. BIND_ACTIVE also installs a selector,
    // but uses it solely as an enable gate and must retain output_scale.
    bool selector_maps_output = false;
    uint16_t selector_active_mask = 0U;
    uint16_t selector_direct_mask = 0U;
    float selector_output_scale[16] = {0.0F};
    uint32_t selector_full_output[16] = {0U};
    uint32_t selector_direct_output[16] = {0U};

    // Raw mask mode
    uint8_t mask[8] = {0};
    uint8_t value[8] = {0};

    // Counter and checksum post-processing modes.
    uint32_t counter_initial = 0U;
    uint32_t counter_step = 1U;
    uint32_t counter_wrap_after = 0xFFFFFFFFU;
    uint32_t counter_wrap_to = 0U;
    uint8_t checksum_target_byte = 0U;
    uint8_t checksum_start_byte = 0U;
    uint8_t checksum_end_byte = 7U;
    uint8_t checksum_seed = 0U;
    uint8_t checksum_counter_byte = 1U;
    uint8_t sequence_values[16] = {0};
    uint8_t sequence_count = 0U;
    uint8_t sequence_initial_index = 0U;
};

struct RuleListEntry {
    uint16_t rule_id = 0;
    uint16_t priority = 0;
    uint32_t epoch = 0;
    RuleStageRequest request{};
    bool active = false;
};

class MutationEngine {
public:
    static constexpr size_t kMaxRules = 96;

    void init();
    void setRuntimeValueRegistry(const RuntimeValueRegistry* registry);
    void setRuntimeTableRegistry(const RuntimeTableRegistry* registry);

    bool stageRule(const RuleStageRequest& request, uint16_t* out_rule_id = nullptr);
    bool stageMutation(const SignalMutation& mutation);  // Legacy adapter
    void clearStaging();
    void revertStagingToActive();
    bool applyCommit();

    size_t stagingCount() const;
    size_t activeCount() const;
    uint32_t ruleEpoch() const;

    bool hasRulesForFrame(uint32_t can_id, Direction direction) const;
    size_t applyFrameMutations(CanFrame& frame) const;

    int32_t registerDynamicSignalRule(
        uint32_t can_id,
        Direction direction,
        uint16_t start_bit,
        uint8_t bit_length,
        bool little_endian,
        uint32_t initial_value,
        bool enabled);

    // Rule IDs are reusable runtime slots. Callers must present the epoch that
    // accompanied the ID so a delayed request cannot control a different rule
    // after a package/DBC generation swap.
    bool setRuleValue(uint16_t rule_id, uint32_t value, uint32_t expected_epoch);
    bool enableRule(uint16_t rule_id, bool enabled, uint32_t expected_epoch);

    void clearRules();
    size_t listRules(RuleListEntry* out_entries, size_t capacity) const;
    // Snapshot the complete candidate table that the next applyCommit() would
    // compile. The caller must serialize this read with stage/clear/revert
    // writers; the HTTP layer does so with its application lock.
    size_t listStagedRules(RuleListEntry* out_entries, size_t capacity) const;

private:
    static constexpr size_t kBucketCount = 128;
    static constexpr uint16_t kInvalidRuleId = 0xFFFFU;

    struct RuntimeRuleState {
        alignas(4) std::atomic<uint32_t> current_value{0};
        std::atomic<uint8_t> enabled{0};
    };

    struct StagedRule {
        bool in_use = false;
        uint16_t rule_id = kInvalidRuleId;
        uint32_t sequence = 0;
        uint32_t pending_runtime_value = 0U;
        bool pending_enabled = false;
        bool publish_runtime_on_commit = false;
        RuleStageRequest request{};
    };

    struct CompiledRule {
        uint16_t rule_id = kInvalidRuleId;
        uint16_t priority = 0;
        RuleStageRequest source{};

        uint8_t clear_mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t static_set_bits[8] = {0};

        uint8_t dynamic_bit_count = 0;
        uint8_t dynamic_bit_positions[32] = {0};
        int16_t runtime_value_index = -1;
        int16_t runtime_table_index = -1;
        int16_t override_active_index = -1;
        int16_t override_value_index = -1;
        int16_t selector_index = -1;
    };

    struct RuleGroup {
        uint32_t can_id = 0;
        Direction direction = Direction::A_TO_B;
        uint16_t first_rule = 0;
        uint16_t rule_count = 0;
        int16_t next = -1;
    };

    struct ActiveRuleTable {
        uint16_t rule_count = 0;
        uint16_t group_count = 0;
        CompiledRule rules[kMaxRules];
        RuleGroup groups[kMaxRules];
        int16_t bucket_head[kBucketCount];
    };

    static uint32_t keyHash(uint32_t can_id, Direction direction);
    static uint16_t nextMotorolaBit(uint16_t current_bit);
    static bool normalizeRule(RuleStageRequest& rule);
    static void clearActiveTable(ActiveRuleTable& table);
    static bool buildBitMaskAndValue(
        uint16_t start_bit,
        uint8_t bit_length,
        bool little_endian,
        uint64_t raw_value,
        uint8_t out_mask[8],
        uint8_t out_value[8]);
    static bool buildDynamicBitPositions(
        uint16_t start_bit,
        uint8_t bit_length,
        bool little_endian,
        uint8_t out_positions[32]);
    static uint16_t findStagedIdentity(
        const StagedRule* staged,
        uint16_t count,
        const RuleStageRequest& request);

    const ActiveRuleTable* activeTable() const;
    ActiveRuleTable* inactiveTable();
    void swapActiveTable();
    const RuleGroup* findGroup(const ActiveRuleTable& table, uint32_t can_id, Direction direction) const;
    RuleGroup* ensureGroup(ActiveRuleTable& table, uint32_t can_id, Direction direction);

    bool compileRule(const StagedRule& staged_rule, uint16_t priority, CompiledRule& out_rule);
    static void applyStaticRule(const CompiledRule& rule, CanFrame& frame);
    void applyDynamicRule(const CompiledRule& rule, CanFrame& frame) const;
    void applyCounterRule(const CompiledRule& rule, CanFrame& frame) const;
    void applySequenceRule(const CompiledRule& rule, CanFrame& frame) const;
    bool selectorAllows(const CompiledRule& rule, uint8_t& selector) const;
    static void applyXorChecksumRule(const CompiledRule& rule, CanFrame& frame);
    static void applyAutosarChecksumRule(const CompiledRule& rule, CanFrame& frame);
    static void applyMask(uint8_t frame_data[8], const uint8_t clear_mask[8], const uint8_t set_bits[8]);

    uint16_t allocateRuleSlot();
    uint16_t allocateSequence();
    void advanceRuleEpoch();
    bool ruleExistsForControl(uint16_t rule_id) const;
    bool beginMutationRead() const;
    void pauseMutationReads();
    void resumeMutationReads();

    StagedRule* staged_ = nullptr;
    // Status polling runs on the UI task while an application can rebuild the
    // candidate table. Rule contents remain application-lock protected; the
    // lightweight count is atomic so status reads are race-free.
    std::atomic<uint16_t> staged_count_{0U};

    StagedRule* committed_shadow_ = nullptr;
    uint16_t committed_count_ = 0;

    mutable RuntimeRuleState runtime_state_[kMaxRules];

    ActiveRuleTable* tables_[2] = {nullptr, nullptr};
    std::atomic<const ActiveRuleTable*> active_table_{nullptr};
    mutable std::atomic<uint32_t> mutation_readers_{0U};
    std::atomic<uint32_t> mutation_reads_enabled_{0U};
    uint8_t active_table_index_ = 0;
    std::atomic<uint16_t> active_count_{0};
    std::atomic<uint32_t> rule_epoch_{1U};
    const RuntimeValueRegistry* runtime_values_ = nullptr;
    const RuntimeTableRegistry* runtime_tables_ = nullptr;

    uint32_t next_sequence_ = 1U;
};

}  // namespace bored::signalscope
