#include "rule_package.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mutation_engine.hpp"

namespace bored::signalscope {
namespace {

constexpr size_t kMaxFields = 24U;
constexpr size_t kNameCapacity = 32U;
constexpr size_t kMaxRuleLineBytes = 1024U;

char* trim(char* value) {
    while (*value == ' ' || *value == '\t' || *value == '\r') ++value;
    char* end = value + std::strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = '\0';
    return value;
}

// Unlike strtok_r(), this preserves empty fields so a missing value cannot
// shift the remainder of a row into a valid-looking schema.
bool splitDelimited(char* text, char delimiter, char** fields, size_t capacity, size_t& count) {
    count = 0U;
    if (text == nullptr || capacity == 0U) return false;

    char* token = text;
    while (true) {
        if (count >= capacity) return false;
        char* separator = std::strchr(token, delimiter);
        if (separator != nullptr) *separator = '\0';
        fields[count++] = trim(token);
        if (separator == nullptr) return true;
        token = separator + 1;
    }
}

bool parseUnsigned(const char* text, uint64_t maximum, uint64_t& out_value) {
    if (text == nullptr || text[0] == '\0' || text[0] == '-' || text[0] == '+') return false;
    const bool hexadecimal = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const char* digits = hexadecimal ? text + 2 : text;
    if (*digits == '\0') return false;
    for (const char* cursor = digits; *cursor != '\0'; ++cursor) {
        const bool decimal_digit = *cursor >= '0' && *cursor <= '9';
        const bool hex_digit = decimal_digit || (*cursor >= 'a' && *cursor <= 'f') ||
            (*cursor >= 'A' && *cursor <= 'F');
        if (!(hexadecimal ? hex_digit : decimal_digit)) return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(digits, &end, hexadecimal ? 16 : 10);
    if (errno == ERANGE || end == digits || *end != '\0' || value > maximum) return false;
    out_value = static_cast<uint64_t>(value);
    return true;
}

bool parseU8(const char* text, uint8_t maximum, uint8_t& out_value) {
    uint64_t value = 0U;
    if (!parseUnsigned(text, maximum, value)) return false;
    out_value = static_cast<uint8_t>(value);
    return true;
}

bool parseU16(const char* text, uint16_t maximum, uint16_t& out_value) {
    uint64_t value = 0U;
    if (!parseUnsigned(text, maximum, value)) return false;
    out_value = static_cast<uint16_t>(value);
    return true;
}

bool parseU32(const char* text, uint32_t maximum, uint32_t& out_value) {
    uint64_t value = 0U;
    if (!parseUnsigned(text, maximum, value)) return false;
    out_value = static_cast<uint32_t>(value);
    return true;
}

bool parseU64(const char* text, uint64_t& out_value) {
    return parseUnsigned(text, UINT64_MAX, out_value);
}

bool valueFitsBits(uint64_t value, uint8_t bit_length) {
    return bit_length == 64U || value < (1ULL << bit_length);
}

bool parseBoolean(const char* text, bool& out_value) {
    uint8_t value = 0U;
    if (!parseU8(text, 1U, value)) return false;
    out_value = value == 1U;
    return true;
}

bool parseFiniteFloat(const char* text, float& out_value) {
    if (text == nullptr || text[0] == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !std::isfinite(value)) return false;
    out_value = value;
    return true;
}

bool parseName(const char* text, char* destination, size_t capacity) {
    if (text == nullptr || destination == nullptr || capacity == 0U) return false;
    const size_t length = std::strlen(text);
    if (length == 0U || length >= capacity) return false;
    const auto asciiLetter = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = static_cast<unsigned char>(text[i]);
        const bool valid_first = asciiLetter(value) || value == '_';
        const bool valid_rest = valid_first || (value >= '0' && value <= '9') ||
            value == '.' || value == '-';
        if (!(i == 0U ? valid_first : valid_rest)) return false;
    }
    std::memcpy(destination, text, length + 1U);
    return true;
}

bool parseCanId(const char* text, uint32_t& out_can_id) {
    return parseU32(text, 0x1FFFFFFFU, out_can_id);
}

bool parseDirection(const char* text, Direction& out_direction) {
    if (text == nullptr) return false;
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
    if (!parseCanId(field[1], rule.can_id) || !parseDirection(field[2], rule.direction) ||
        !parseU16(field[3], 63U, rule.start_bit) ||
        !parseU8(field[4], 64U, rule.bit_length) || rule.bit_length == 0U ||
        !parseBoolean(field[5], rule.little_endian)) {
        return false;
    }
    rule.enabled = true;
    return true;
}

bool parseByteList(
    char* text,
    uint8_t maximum,
    uint8_t* values,
    size_t capacity,
    size_t& count) {
    char* fields[16] = {};
    if (capacity > 16U || !splitDelimited(text, '|', fields, capacity, count)) return false;
    for (size_t i = 0U; i < count; ++i) {
        if (!parseU8(fields[i], maximum, values[i])) return false;
    }
    return true;
}

bool parseActiveSelectorList(char* text, uint16_t& out_mask) {
    char* fields[16] = {};
    size_t count = 0U;
    if (!splitDelimited(text, '|', fields, 16U, count)) return false;
    uint16_t mask = 0U;
    for (size_t i = 0U; i < count; ++i) {
        uint8_t selector = 0U;
        if (!parseU8(fields[i], 15U, selector)) return false;
        const uint16_t bit = static_cast<uint16_t>(1U << selector);
        if ((mask & bit) != 0U) return false;
        mask |= bit;
    }
    if (mask == 0U) return false;
    out_mask = mask;
    return true;
}

bool parseSelectorEntries(char* text, RuleStageRequest& rule) {
    char* entries[16] = {};
    size_t entry_count = 0U;
    if (!splitDelimited(text, '|', entries, 16U, entry_count)) return false;

    for (size_t i = 0U; i < entry_count; ++i) {
        char* parts[5] = {};
        size_t part_count = 0U;
        if (!splitDelimited(entries[i], ':', parts, 5U, part_count)) return false;

        uint8_t selector = 0U;
        if (!parseU8(parts[0], 15U, selector)) return false;
        const uint16_t bit = static_cast<uint16_t>(1U << selector);
        if ((rule.selector_active_mask & bit) != 0U) return false;

        if (part_count == 3U && std::strcmp(parts[1], "D") == 0) {
            uint32_t output = 0U;
            if (!parseU32(parts[2], UINT32_MAX, output)) return false;
            rule.selector_direct_mask |= bit;
            rule.selector_direct_output[selector] = output;
        } else if (part_count == 4U && std::strcmp(parts[1], "S") == 0) {
            float scale = 0.0F;
            uint32_t full_output = 0U;
            if (!parseFiniteFloat(parts[2], scale) ||
                !parseU32(parts[3], UINT32_MAX, full_output)) {
                return false;
            }
            rule.selector_output_scale[selector] = scale;
            rule.selector_full_output[selector] = full_output;
        } else {
            return false;
        }
        rule.selector_active_mask |= bit;
    }
    return rule.selector_active_mask != 0U;
}

class StagingFailureGuard {
public:
    explicit StagingFailureGuard(MutationEngine& engine) : engine_(engine) {
        engine_.clearStaging();
    }

    ~StagingFailureGuard() {
        if (!released_) {
            // Parsing uses the candidate table as bounded scratch space. On
            // failure, restore the active mirror so a rejected package cannot
            // leave an empty candidate that a later Apply would publish.
            engine_.revertStagingToActive();
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
    if (text == nullptr || length == 0U || std::memchr(text, '\0', length) != nullptr) return false;

    size_t loaded = 0U;
    char bound_source[kNameCapacity] = {0};
    char bound_table[kNameCapacity] = {0};
    char override_source[kNameCapacity] = {0};
    char override_active[kNameCapacity] = {0};
    char override_value[kNameCapacity] = {0};
    char active_selector_source[kNameCapacity] = {0};
    uint16_t active_selector_mask = 0U;
    char final_line[kMaxRuleLineBytes + 1U] = {0};
    char* cursor = text;
    char* end = text + length;
    while (cursor < end && *cursor != '\0') {
        char* newline = static_cast<char*>(std::memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const size_t line_length = newline == nullptr
            ? static_cast<size_t>(end - cursor)
            : static_cast<size_t>(newline - cursor);
        if (line_length > kMaxRuleLineBytes) return false;
        char* line_storage = cursor;
        if (newline != nullptr) {
            *newline = '\0';
        } else {
            // The public API accepts exactly `length` writable bytes; callers
            // do not owe us an extra sentinel byte at text[length]. Copy only
            // the final bounded row before trim()/tokenization use strlen().
            std::memcpy(final_line, cursor, line_length);
            final_line[line_length] = '\0';
            line_storage = final_line;
        }
        char* line = trim(line_storage);
        if (*line != '\0' && *line != '#') {
            char* fields[kMaxFields] = {};
            size_t count = 0U;
            if (!splitDelimited(line, ',', fields, kMaxFields, count)) return false;

            RuleStageRequest rule{};
            bool stage_rule = true;
            if (std::strcmp(fields[0], "BIND_TABLE") == 0) {
                if (count != 3U ||
                    !parseName(fields[1], bound_source, sizeof(bound_source)) ||
                    !parseName(fields[2], bound_table, sizeof(bound_table))) {
                    return false;
                }
                stage_rule = false;
            } else if (std::strcmp(fields[0], "BIND_OVERRIDE") == 0) {
                if (count != 4U ||
                    !parseName(fields[1], override_source, sizeof(override_source)) ||
                    !parseName(fields[2], override_active, sizeof(override_active)) ||
                    !parseName(fields[3], override_value, sizeof(override_value))) {
                    return false;
                }
                stage_rule = false;
            } else if (std::strcmp(fields[0], "BIND_ACTIVE") == 0) {
                char next_source[kNameCapacity] = {0};
                uint16_t next_mask = 0U;
                if (count != 3U ||
                    !parseName(fields[1], next_source, sizeof(next_source)) ||
                    !parseActiveSelectorList(fields[2], next_mask)) {
                    return false;
                }
                // A later BIND_ACTIVE replaces the earlier gate; it must not
                // accidentally union its states with the previous directive.
                std::memcpy(active_selector_source, next_source, sizeof(active_selector_source));
                active_selector_mask = next_mask;
                stage_rule = false;
            } else if (std::strcmp(fields[0], "STATIC") == 0) {
                if (count != 7U || !parseCommon(fields, rule) ||
                    !parseU64(fields[6], rule.replace_value) ||
                    !valueFitsBits(rule.replace_value, rule.bit_length)) {
                    return false;
                }
            } else if (std::strcmp(fields[0], "SOURCE_INT") == 0) {
                if (count != 15U || !parseCommon(fields, rule)) return false;
                rule.dynamic_value = true;
                if (rule.bit_length > 32U ||
                    !parseName(fields[6], rule.value_source, sizeof(rule.value_source)) ||
                    !parseFiniteFloat(fields[7], rule.source_gain) ||
                    !parseFiniteFloat(fields[8], rule.source_offset) ||
                    !parseFiniteFloat(fields[9], rule.output_scale) ||
                    !parseFiniteFloat(fields[10], rule.output_offset) ||
                    !parseFiniteFloat(fields[11], rule.zero_threshold) ||
                    !parseU32(fields[12], UINT32_MAX, rule.zero_output) ||
                    !parseFiniteFloat(fields[13], rule.full_threshold) ||
                    !parseU32(fields[14], UINT32_MAX, rule.full_output)) {
                    return false;
                }
                if (bound_source[0] != '\0' && std::strcmp(bound_source, rule.value_source) == 0) {
                    std::memcpy(rule.lookup_table, bound_table, sizeof(rule.lookup_table));
                }
                if (override_source[0] != '\0' && std::strcmp(override_source, rule.value_source) == 0) {
                    std::memcpy(rule.override_active_source, override_active, sizeof(rule.override_active_source));
                    std::memcpy(rule.override_value_source, override_value, sizeof(rule.override_value_source));
                }
                rule.truncate_affine = true;
                rule.truncate_output = true;
                rule.zero_override = true;
                rule.full_override = true;
            } else if (std::strcmp(fields[0], "SOURCE_SELECT_INT") == 0) {
                if (count != 15U || !parseCommon(fields, rule)) return false;
                rule.dynamic_value = true;
                rule.selector_maps_output = true;
                if (rule.bit_length > 32U ||
                    !parseName(fields[6], rule.value_source, sizeof(rule.value_source)) ||
                    !parseFiniteFloat(fields[7], rule.source_gain) ||
                    !parseFiniteFloat(fields[8], rule.source_offset) ||
                    !parseFiniteFloat(fields[9], rule.output_offset) ||
                    !parseFiniteFloat(fields[10], rule.zero_threshold) ||
                    !parseU32(fields[11], UINT32_MAX, rule.zero_output) ||
                    !parseFiniteFloat(fields[12], rule.full_threshold) ||
                    !parseName(fields[13], rule.selector_source, sizeof(rule.selector_source)) ||
                    !parseSelectorEntries(fields[14], rule)) {
                    return false;
                }
                if (bound_source[0] != '\0' && std::strcmp(bound_source, rule.value_source) == 0) {
                    std::memcpy(rule.lookup_table, bound_table, sizeof(rule.lookup_table));
                }
                if (override_source[0] != '\0' && std::strcmp(override_source, rule.value_source) == 0) {
                    std::memcpy(rule.override_active_source, override_active, sizeof(rule.override_active_source));
                    std::memcpy(rule.override_value_source, override_value, sizeof(rule.override_value_source));
                }
                rule.truncate_affine = true;
                rule.truncate_output = true;
                rule.zero_override = true;
                rule.full_override = true;
            } else if (std::strcmp(fields[0], "COUNTER") == 0) {
                if (count != 10U || !parseCommon(fields, rule)) return false;
                rule.kind = RuleKind::COUNTER;
                if (rule.bit_length > 32U) return false;
                const uint32_t field_maximum = rule.bit_length == 32U
                    ? UINT32_MAX
                    : static_cast<uint32_t>((1ULL << rule.bit_length) - 1ULL);
                if (!parseU32(fields[6], field_maximum, rule.counter_initial) ||
                    !parseU32(fields[7], UINT32_MAX, rule.counter_step) ||
                    !parseU32(fields[8], field_maximum, rule.counter_wrap_after) ||
                    !parseU32(fields[9], field_maximum, rule.counter_wrap_to)) {
                    return false;
                }
            } else if (std::strcmp(fields[0], "CHECKSUM_XOR") == 0) {
                if (count != 8U) return false;
                rule.kind = RuleKind::CHECKSUM_XOR;
                if (!parseCanId(fields[1], rule.can_id) ||
                    !parseDirection(fields[2], rule.direction) ||
                    !parseU8(fields[3], 7U, rule.checksum_target_byte) ||
                    !parseU8(fields[4], 7U, rule.checksum_start_byte) ||
                    !parseU8(fields[5], 7U, rule.checksum_end_byte) ||
                    rule.checksum_start_byte > rule.checksum_end_byte ||
                    !parseU8(fields[6], UINT8_MAX, rule.checksum_seed) ||
                    !parseBoolean(fields[7], rule.enabled)) {
                    return false;
                }
            } else if (std::strcmp(fields[0], "CHECKSUM_CRC8_AUTOSAR") == 0) {
                if (count != 9U) return false;
                rule.kind = RuleKind::CHECKSUM_CRC8_AUTOSAR;
                size_t sequence_count = 0U;
                if (!parseCanId(fields[1], rule.can_id) ||
                    !parseDirection(fields[2], rule.direction) ||
                    !parseU8(fields[3], 7U, rule.checksum_target_byte) ||
                    !parseU8(fields[4], 7U, rule.checksum_counter_byte) ||
                    !parseU8(fields[5], 7U, rule.checksum_start_byte) ||
                    !parseU8(fields[6], 7U, rule.checksum_end_byte) ||
                    rule.checksum_start_byte > rule.checksum_end_byte ||
                    !parseByteList(fields[7], UINT8_MAX, rule.sequence_values, 16U, sequence_count) ||
                    sequence_count != 16U ||
                    !parseBoolean(fields[8], rule.enabled)) {
                    return false;
                }
                rule.sequence_count = static_cast<uint8_t>(sequence_count);
            } else if (std::strcmp(fields[0], "SEQUENCE8") == 0) {
                if (count != 8U || !parseCommon(fields, rule)) return false;
                rule.kind = RuleKind::SEQUENCE8;
                if (rule.bit_length > 8U) return false;
                size_t sequence_count = 0U;
                const uint8_t field_maximum = static_cast<uint8_t>((1U << rule.bit_length) - 1U);
                if (!parseByteList(fields[6], field_maximum, rule.sequence_values, 16U, sequence_count) ||
                    !parseU8(fields[7], 15U, rule.sequence_initial_index) ||
                    rule.sequence_initial_index >= sequence_count) {
                    return false;
                }
                rule.sequence_count = static_cast<uint8_t>(sequence_count);
            } else {
                return false;
            }

            if (stage_rule && rule.selector_source[0] == '\0' && active_selector_source[0] != '\0') {
                std::memcpy(rule.selector_source, active_selector_source, sizeof(rule.selector_source));
                rule.selector_active_mask = active_selector_mask;
            }
            if (stage_rule && !engine.stageRule(rule, nullptr)) return false;
            if (stage_rule) ++loaded;
        }
        if (newline == nullptr) break;
        cursor = newline + 1;
    }
    if (loaded == 0U || !engine.applyCommit()) return false;
    if (loaded_count != nullptr) *loaded_count = loaded;
    staging_guard.release();
    return true;
}

}  // namespace bored::signalscope
