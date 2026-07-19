#include "rule_package.hpp"

#include <cstdlib>
#include <cstring>

#include "mutation_engine.hpp"

namespace bored::signalscope {
namespace {

char* trim(char* value) {
    while (*value == ' ' || *value == '\t' || *value == '\r') ++value;
    char* end = value + std::strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = '\0';
    return value;
}

size_t splitCsv(char* line, char** fields, size_t capacity) {
    size_t count = 0U;
    char* context = nullptr;
    char* token = strtok_r(line, ",", &context);
    while (token != nullptr && count < capacity) {
        fields[count++] = trim(token);
        token = strtok_r(nullptr, ",", &context);
    }
    return count;
}

bool parseCanId(const char* text, uint32_t& out_can_id) {
    if (text == nullptr || text[0] == '\0' || text[0] == '-') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || value > 0x1FFFFFFFULL) return false;
    out_can_id = static_cast<uint32_t>(value);
    return true;
}

bool parseDirection(const char* text, Direction& out_direction) {
    if (std::strcmp(text, "A_TO_B") == 0) {
        out_direction = Direction::A_TO_B;
        return true;
    }
    if (std::strcmp(text, "B_TO_A") == 0) {
        out_direction = Direction::B_TO_A;
        return true;
    }
    return false;
}

bool parseCommon(char** field, RuleStageRequest& rule) {
    rule.kind = RuleKind::BIT_RANGE;
    if (!parseCanId(field[1], rule.can_id) || !parseDirection(field[2], rule.direction)) return false;
    rule.start_bit = static_cast<uint16_t>(std::strtoul(field[3], nullptr, 0));
    rule.bit_length = static_cast<uint8_t>(std::strtoul(field[4], nullptr, 0));
    rule.little_endian = std::strtoul(field[5], nullptr, 0) != 0U;
    rule.enabled = true;
    return rule.bit_length > 0U;
}

class StagingFailureGuard {
public:
    explicit StagingFailureGuard(MutationEngine& engine) : engine_(engine) {
        engine_.clearStaging();
    }

    ~StagingFailureGuard() {
        if (!released_) {
            engine_.clearStaging();
        }
    }

    void release() {
        released_ = true;
    }

private:
    MutationEngine& engine_;
    bool released_ = false;
};

}  // namespace

bool RulePackageLoader::loadCsv(char* text, size_t length, MutationEngine& engine, size_t* loaded_count) {
    if (loaded_count != nullptr) *loaded_count = 0U;
    StagingFailureGuard staging_guard(engine);
    if (text == nullptr || length == 0U) return false;
    size_t loaded = 0U;
    char bound_source[32] = {0};
    char bound_table[32] = {0};
    char override_source[32] = {0};
    char override_active[32] = {0};
    char override_value[32] = {0};
    char active_selector_source[32] = {0};
    uint16_t active_selector_mask = 0U;
    char* cursor = text;
    char* end = text + length;
    while (cursor < end && *cursor != '\0') {
        char* newline = static_cast<char*>(std::memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        if (newline != nullptr) *newline = '\0';
        char* line = trim(cursor);
        if (*line != '\0' && *line != '#') {
            char* fields[24] = {};
            const size_t count = splitCsv(line, fields, 24U);
            RuleStageRequest rule{};
            bool stage_rule = true;
            if (count >= 3U && std::strcmp(fields[0], "BIND_TABLE") == 0) {
                std::strncpy(bound_source, fields[1], sizeof(bound_source) - 1U);
                std::strncpy(bound_table, fields[2], sizeof(bound_table) - 1U);
                stage_rule = false;
            } else if (count >= 4U && std::strcmp(fields[0], "BIND_OVERRIDE") == 0) {
                std::strncpy(override_source, fields[1], sizeof(override_source) - 1U);
                std::strncpy(override_active, fields[2], sizeof(override_active) - 1U);
                std::strncpy(override_value, fields[3], sizeof(override_value) - 1U);
                stage_rule = false;
            } else if (count >= 3U && std::strcmp(fields[0], "BIND_ACTIVE") == 0) {
                std::strncpy(active_selector_source, fields[1], sizeof(active_selector_source) - 1U);
                char* selector_context = nullptr;
                char* selector = strtok_r(fields[2], "|", &selector_context);
                while (selector != nullptr) {
                    const unsigned long value = std::strtoul(selector, nullptr, 0);
                    if (value > 15UL) return false;
                    active_selector_mask |= static_cast<uint16_t>(1U << value);
                    selector = strtok_r(nullptr, "|", &selector_context);
                }
                if (active_selector_mask == 0U) return false;
                stage_rule = false;
            } else if (count >= 7U && std::strcmp(fields[0], "STATIC") == 0) {
                if (!parseCommon(fields, rule)) return false;
                rule.replace_value = std::strtoull(fields[6], nullptr, 0);
            } else if (count >= 15U && std::strcmp(fields[0], "SOURCE_INT") == 0) {
                if (!parseCommon(fields, rule)) return false;
                rule.dynamic_value = true;
                std::strncpy(rule.value_source, fields[6], sizeof(rule.value_source) - 1U);
                if (bound_source[0] != '\0' && std::strcmp(bound_source, rule.value_source) == 0) {
                    std::strncpy(rule.lookup_table, bound_table, sizeof(rule.lookup_table) - 1U);
                }
                if (override_source[0] != '\0' && std::strcmp(override_source, rule.value_source) == 0) {
                    std::strncpy(rule.override_active_source, override_active, sizeof(rule.override_active_source) - 1U);
                    std::strncpy(rule.override_value_source, override_value, sizeof(rule.override_value_source) - 1U);
                }
                rule.source_gain = std::strtof(fields[7], nullptr);
                rule.source_offset = std::strtof(fields[8], nullptr);
                rule.output_scale = std::strtof(fields[9], nullptr);
                rule.output_offset = std::strtof(fields[10], nullptr);
                rule.truncate_affine = true;
                rule.truncate_output = true;
                rule.zero_override = true;
                rule.zero_threshold = std::strtof(fields[11], nullptr);
                rule.zero_output = static_cast<uint32_t>(std::strtoul(fields[12], nullptr, 0));
                rule.full_override = true;
                rule.full_threshold = std::strtof(fields[13], nullptr);
                rule.full_output = static_cast<uint32_t>(std::strtoul(fields[14], nullptr, 0));
            } else if (count >= 15U && std::strcmp(fields[0], "SOURCE_SELECT_INT") == 0) {
                if (!parseCommon(fields, rule)) return false;
                rule.dynamic_value = true;
                rule.selector_maps_output = true;
                std::strncpy(rule.value_source, fields[6], sizeof(rule.value_source) - 1U);
                if (bound_source[0] != '\0' && std::strcmp(bound_source, rule.value_source) == 0) {
                    std::strncpy(rule.lookup_table, bound_table, sizeof(rule.lookup_table) - 1U);
                }
                if (override_source[0] != '\0' && std::strcmp(override_source, rule.value_source) == 0) {
                    std::strncpy(rule.override_active_source, override_active, sizeof(rule.override_active_source) - 1U);
                    std::strncpy(rule.override_value_source, override_value, sizeof(rule.override_value_source) - 1U);
                }
                rule.source_gain = std::strtof(fields[7], nullptr);
                rule.source_offset = std::strtof(fields[8], nullptr);
                rule.output_offset = std::strtof(fields[9], nullptr);
                rule.truncate_affine = true;
                rule.truncate_output = true;
                rule.zero_override = true;
                rule.zero_threshold = std::strtof(fields[10], nullptr);
                rule.zero_output = static_cast<uint32_t>(std::strtoul(fields[11], nullptr, 0));
                rule.full_override = true;
                rule.full_threshold = std::strtof(fields[12], nullptr);
                std::strncpy(rule.selector_source, fields[13], sizeof(rule.selector_source) - 1U);
                char* entry_context = nullptr;
                char* entry = strtok_r(fields[14], "|", &entry_context);
                while (entry != nullptr) {
                    char* part_context = nullptr;
                    char* selector_text = strtok_r(entry, ":", &part_context);
                    char* type = strtok_r(nullptr, ":", &part_context);
                    char* first = strtok_r(nullptr, ":", &part_context);
                    const unsigned long selector = selector_text == nullptr ? 16UL : std::strtoul(selector_text, nullptr, 0);
                    if (selector > 15UL || type == nullptr || first == nullptr) return false;
                    const uint16_t bit = static_cast<uint16_t>(1U << selector);
                    rule.selector_active_mask |= bit;
                    if (std::strcmp(type, "D") == 0) {
                        rule.selector_direct_mask |= bit;
                        rule.selector_direct_output[selector] = static_cast<uint32_t>(std::strtoul(first, nullptr, 0));
                    } else if (std::strcmp(type, "S") == 0) {
                        char* second = strtok_r(nullptr, ":", &part_context);
                        if (second == nullptr) return false;
                        rule.selector_output_scale[selector] = std::strtof(first, nullptr);
                        rule.selector_full_output[selector] = static_cast<uint32_t>(std::strtoul(second, nullptr, 0));
                    } else {
                        return false;
                    }
                    entry = strtok_r(nullptr, "|", &entry_context);
                }
            } else if (count >= 10U && std::strcmp(fields[0], "COUNTER") == 0) {
                if (!parseCommon(fields, rule)) return false;
                rule.kind = RuleKind::COUNTER;
                rule.counter_initial = static_cast<uint32_t>(std::strtoul(fields[6], nullptr, 0));
                rule.counter_step = static_cast<uint32_t>(std::strtoul(fields[7], nullptr, 0));
                rule.counter_wrap_after = static_cast<uint32_t>(std::strtoul(fields[8], nullptr, 0));
                rule.counter_wrap_to = static_cast<uint32_t>(std::strtoul(fields[9], nullptr, 0));
            } else if (count >= 8U && std::strcmp(fields[0], "CHECKSUM_XOR") == 0) {
                rule.kind = RuleKind::CHECKSUM_XOR;
                if (!parseCanId(fields[1], rule.can_id) ||
                    !parseDirection(fields[2], rule.direction)) return false;
                rule.checksum_target_byte = static_cast<uint8_t>(std::strtoul(fields[3], nullptr, 0));
                rule.checksum_start_byte = static_cast<uint8_t>(std::strtoul(fields[4], nullptr, 0));
                rule.checksum_end_byte = static_cast<uint8_t>(std::strtoul(fields[5], nullptr, 0));
                rule.checksum_seed = static_cast<uint8_t>(std::strtoul(fields[6], nullptr, 0));
                rule.enabled = std::strtoul(fields[7], nullptr, 0) != 0U;
            } else if (count >= 9U && std::strcmp(fields[0], "CHECKSUM_CRC8_AUTOSAR") == 0) {
                rule.kind = RuleKind::CHECKSUM_CRC8_AUTOSAR;
                if (!parseCanId(fields[1], rule.can_id) ||
                    !parseDirection(fields[2], rule.direction)) return false;
                rule.checksum_target_byte = static_cast<uint8_t>(std::strtoul(fields[3], nullptr, 0));
                rule.checksum_counter_byte = static_cast<uint8_t>(std::strtoul(fields[4], nullptr, 0));
                rule.checksum_start_byte = static_cast<uint8_t>(std::strtoul(fields[5], nullptr, 0));
                rule.checksum_end_byte = static_cast<uint8_t>(std::strtoul(fields[6], nullptr, 0));
                char* sequence_context = nullptr;
                char* value = strtok_r(fields[7], "|", &sequence_context);
                while (value != nullptr && rule.sequence_count < 16U) {
                    rule.sequence_values[rule.sequence_count++] =
                        static_cast<uint8_t>(std::strtoul(value, nullptr, 0));
                    value = strtok_r(nullptr, "|", &sequence_context);
                }
                if (value != nullptr || rule.sequence_count != 16U) return false;
                rule.enabled = std::strtoul(fields[8], nullptr, 0) != 0U;
            } else if (count >= 8U && std::strcmp(fields[0], "SEQUENCE8") == 0) {
                if (!parseCommon(fields, rule)) return false;
                rule.kind = RuleKind::SEQUENCE8;
                rule.sequence_initial_index = static_cast<uint8_t>(std::strtoul(fields[7], nullptr, 0));
                char* sequence_context = nullptr;
                char* value = strtok_r(fields[6], "|", &sequence_context);
                while (value != nullptr && rule.sequence_count < 16U) {
                    rule.sequence_values[rule.sequence_count++] =
                        static_cast<uint8_t>(std::strtoul(value, nullptr, 0));
                    value = strtok_r(nullptr, "|", &sequence_context);
                }
            } else {
                return false;
            }
            if (stage_rule && rule.selector_source[0] == '\0' && active_selector_source[0] != '\0') {
                std::strncpy(rule.selector_source, active_selector_source, sizeof(rule.selector_source) - 1U);
                rule.selector_active_mask = active_selector_mask;
            }
            if (stage_rule && !engine.stageRule(rule, nullptr)) {
                return false;
            }
            if (stage_rule) ++loaded;
        }
        if (newline == nullptr) break;
        cursor = newline + 1;
    }
    if (loaded == 0U || !engine.applyCommit()) {
        return false;
    }
    if (loaded_count != nullptr) *loaded_count = loaded;
    staging_guard.release();
    return true;
}

}  // namespace bored::signalscope
