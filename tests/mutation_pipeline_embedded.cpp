#include <Arduino.h>

#include <cstring>
#include <limits>

#include "core/mutation_engine.hpp"
#include "core/dbc_parser.hpp"
#include "core/rule_package.hpp"
#include "core/runtime_tables.hpp"
#include "core/runtime_values.hpp"
#include "core/signal_codec.hpp"

using bored::signalscope::CanFrame;
using bored::signalscope::Direction;
using bored::signalscope::MutationEngine;
using bored::signalscope::RulePackageLoader;
using bored::signalscope::RuleKind;
using bored::signalscope::RuleListEntry;
using bored::signalscope::RuleStageRequest;
using bored::signalscope::RuntimeTableRegistry;
using bored::signalscope::RuntimeValueRegistry;

namespace {

MutationEngine engine;
RuntimeValueRegistry values;
RuntimeTableRegistry tables;
uint32_t failures = 0U;

void require(bool condition, const char* label) {
    if (condition) return;
    ++failures;
    Serial.printf("FAIL %s\n", label);
}

CanFrame frame(uint32_t id) {
    CanFrame result{};
    result.id = id;
    result.dlc = 8U;
    result.direction = Direction::A_TO_B;
    return result;
}

void publish(float command_value, float override_active, float override_value) {
    require(values.publish("commandValue", command_value) >= 0, "publish commandValue");
    require(values.publish("overrideActive", override_active) >= 0, "publish overrideActive");
    require(values.publish("overrideValue", override_value) >= 0, "publish overrideValue");
    require(values.publish("profileCode", 2.0F) >= 0, "publish profileCode");
}

void testRuntimeOverride() {
    constexpr uint8_t override_values[] = {0U, 25U, 50U, 75U, 100U};
    constexpr uint8_t encoded[] = {0U, 63U, 127U, 190U, 254U};
    for (size_t i = 0U; i < sizeof(override_values); ++i) {
        publish(100.0F, 1.0F, static_cast<float>(override_values[i]));
        CanFrame value = frame(0x280U);
        require(engine.applyFrameMutations(value) == 1U, "runtime override rule applied");
        require(value.data[1] == encoded[i], "runtime override encoded");
    }

    publish(100.0F, 1.0F, 999.0F);
    CanFrame clamped = frame(0x280U);
    require(engine.applyFrameMutations(clamped) == 1U, "oversized runtime override rule applied");
    require(clamped.data[1] == 0xFFU, "runtime override clamps to the target field width");

    publish(50.0F, 0.0F, 0.0F);
    require(values.publish("profileCode", 0.0F) >= 0, "publish inactive profileCode");
    CanFrame gated = frame(0x280U);
    gated.data[1] = 0xA5U;
    static_cast<void>(engine.applyFrameMutations(gated));
    require(gated.data[1] == 0xA5U, "inactive selector leaves the target field untouched");
    require(values.publish("profileCode", 2.0F) >= 0, "restore active profileCode");
}

void testFallbackCurveAndTable() {
    constexpr uint8_t command_samples[] = {0U, 25U, 50U, 75U, 100U};
    constexpr uint8_t fallback_encoded[] = {0U, 81U, 114U, 144U, 254U};
    for (size_t i = 0U; i < sizeof(command_samples); ++i) {
        publish(static_cast<float>(command_samples[i]), 0.0F, 0.0F);
        CanFrame value = frame(0x280U);
        require(engine.applyFrameMutations(value) == 1U, "fallback rule applied");
        require(value.data[1] == fallback_encoded[i], "fallback correction encoded");
    }

    float transfer_curve[101] = {};
    for (size_t i = 0U; i < 101U; ++i) transfer_curve[i] = static_cast<float>(i);
    require(tables.publish("transferCurve", transfer_curve, 101U, true) >= 0,
            "publish valid transfer table");
    publish(50.0F, 0.0F, 0.0F);
    CanFrame table_value = frame(0x280U);
    require(engine.applyFrameMutations(table_value) == 1U, "table rule applied");
    require(table_value.data[1] == 127U, "table lookup controls encoded output");
}

void testCounterBeforeChecksum() {
    publish(50.0F, 0.0F, 0.0F);
    CanFrame first = frame(0x2A0U);
    for (uint8_t i = 0U; i < 6U; ++i) first.data[i] = static_cast<uint8_t>(i + 1U);
    require(engine.applyFrameMutations(first) == 2U, "counter and checksum applied first frame");
    require(first.data[6] == 0x00U, "counter begins at zero");
    require(first.data[7] == 0x07U, "checksum sees first counter value");

    CanFrame second = frame(0x2A0U);
    for (uint8_t i = 0U; i < 6U; ++i) second.data[i] = static_cast<uint8_t>(i + 1U);
    require(engine.applyFrameMutations(second) == 2U, "counter and checksum applied second frame");
    require(second.data[6] == 0x10U, "counter advances by 0x10");
    require(second.data[7] == 0x17U, "checksum sees advanced counter value");
}

void expectActiveSourceRule(const char* label) {
    publish(50.0F, 0.0F, 0.0F);
    CanFrame value = frame(0x280U);
    const size_t applied = engine.applyFrameMutations(value);
    const bool valid = applied == 1U && value.data[1] == 127U;
    require(valid, label);
    if (!valid) {
        Serial.printf(
            "  source applied=%u encoded=%u\n",
            static_cast<unsigned>(applied),
            static_cast<unsigned>(value.data[1]));
    }
}

void expectActiveCounter(uint8_t expected, const char* label) {
    CanFrame value = frame(0x2A0U);
    for (uint8_t i = 0U; i < 6U; ++i) value.data[i] = static_cast<uint8_t>(i + 1U);
    const size_t applied = engine.applyFrameMutations(value);
    const uint8_t expected_checksum = static_cast<uint8_t>(0x07U ^ expected);
    const bool valid = applied == 2U && value.data[6] == expected && value.data[7] == expected_checksum;
    require(valid, label);
    if (!valid) {
        Serial.printf(
            "  counter applied=%u value=0x%02X checksum=0x%02X\n",
            static_cast<unsigned>(applied),
            static_cast<unsigned>(value.data[6]),
            static_cast<unsigned>(value.data[7]));
    }
}

void expectDisabledCounter(const char* label) {
    CanFrame value = frame(0x2A0U);
    for (uint8_t i = 0U; i < 6U; ++i) value.data[i] = static_cast<uint8_t>(i + 1U);
    value.data[6] = 0x5AU;
    const size_t applied = engine.applyFrameMutations(value);
    const bool valid = applied == 1U && value.data[6] == 0x5AU && value.data[7] == 0x5DU;
    require(valid, label);
    if (!valid) {
        Serial.printf(
            "  disabled counter applied=%u value=0x%02X checksum=0x%02X\n",
            static_cast<unsigned>(applied),
            static_cast<unsigned>(value.data[6]),
            static_cast<unsigned>(value.data[7]));
    }
}

void testStagedCandidateListing() {
    engine.revertStagingToActive();

    RuleListEntry active_before[8] = {};
    const size_t active_before_count = engine.listRules(active_before, 8U);
    require(active_before_count == 4U, "candidate test snapshots active table");

    RuleStageRequest candidate{};
    candidate.kind = RuleKind::BIT_RANGE;
    candidate.can_id = 0x390U;
    candidate.direction = Direction::A_TO_B;
    candidate.enabled = false;
    candidate.start_bit = 0U;
    candidate.bit_length = 8U;
    candidate.little_endian = true;
    candidate.dynamic_value = true;
    candidate.replace_value = 0x12U;

    uint16_t candidate_id = 0xFFFFU;
    require(engine.stageRule(candidate, &candidate_id), "stage dynamic candidate rule");
    const uint32_t candidate_epoch = engine.ruleEpoch();
    require(engine.setRuleValue(candidate_id, 0x5AU, candidate_epoch),
            "update pending dynamic candidate value");
    require(engine.enableRule(candidate_id, true, candidate_epoch),
            "enable pending dynamic candidate");

    RuleListEntry staged[8] = {};
    const size_t staged_count = engine.listStagedRules(staged, 8U);
    require(staged_count == 5U, "candidate list includes staged addition");
    for (size_t i = 0U; i < staged_count; ++i) {
        require(staged[i].priority == i, "candidate list preserves sequence order");
    }
    const RuleListEntry& pending = staged[staged_count - 1U];
    require(pending.rule_id == candidate_id && pending.request.can_id == 0x390U,
            "candidate list keeps the staged addition last");
    require(pending.request.enabled && pending.active,
            "candidate list exposes pending enabled state");
    require(pending.request.dynamic_value && pending.request.replace_value == 0x5AU,
            "candidate list exposes pending dynamic value");

    RuleListEntry active_after[8] = {};
    const size_t active_after_count = engine.listRules(active_after, 8U);
    require(active_after_count == active_before_count,
            "candidate edits do not change active rule count before apply");
    for (size_t i = 0U; i < active_after_count; ++i) {
        require(active_after[i].rule_id == active_before[i].rule_id,
                "candidate edits preserve active rule order before apply");
    }
    CanFrame untouched = frame(0x390U);
    require(engine.applyFrameMutations(untouched) == 0U && untouched.data[0] == 0U,
            "candidate rule cannot mutate frames before apply");

    engine.revertStagingToActive();
    require(engine.stagingCount() == active_before_count,
            "candidate test restores the committed table to staging");
}
void testFailedPackagesAreTransactional() {
    // Force a candidate into slot zero while the active table still references
    // that slot. Even empty-input rejection must discard it without disabling
    // the active SOURCE_INT rule.
    engine.clearStaging();
    RuleStageRequest disabled_candidate{};
    disabled_candidate.kind = RuleKind::CHECKSUM_XOR;
    disabled_candidate.can_id = 0x555U;
    disabled_candidate.direction = Direction::A_TO_B;
    disabled_candidate.checksum_target_byte = 7U;
    disabled_candidate.checksum_start_byte = 0U;
    disabled_candidate.checksum_end_byte = 6U;
    disabled_candidate.enabled = false;
    require(engine.stageRule(disabled_candidate), "stage candidate before empty package failure");
    size_t loaded = 99U;
    require(!RulePackageLoader::loadCsv(nullptr, 0U, engine, &loaded), "reject empty rule package");
    require(loaded == 0U, "empty package resets loaded count");
    require(engine.stagingCount() == 4U, "empty package failure restores active candidate mirror");
    require(!engine.stagingDirty(), "empty package failure leaves no misleading draft changes");
    require(engine.activeCount() == 4U, "empty package failure preserves active table");
    expectActiveSourceRule("empty package failure preserves active enabled state");
    expectActiveCounter(0x20U, "empty package failure preserves active counter state");

    // Both valid rows occupy live rule IDs before the malformed row rejects
    // the replacement. Neither their enabled flag nor counter initial value
    // may leak into the old table.
    char parse_failure[] =
        "CHECKSUM_XOR,0x555,A_TO_B,7,0,6,0x00,0\n"
        "COUNTER,0x2A0,A_TO_B,48,8,1,0xA0,0x10,0xF0,0x00\n"
        "BIND_ACTIVE,profileCode,16\n";
    loaded = 99U;
    require(
        !RulePackageLoader::loadCsv(parse_failure, sizeof(parse_failure) - 1U, engine, &loaded),
        "reject malformed replacement package");
    require(loaded == 0U, "parse failure resets loaded count");
    require(engine.stagingCount() == 4U, "parse failure restores active candidate mirror");
    require(!engine.stagingDirty(), "parse failure leaves no misleading draft changes");
    require(engine.activeCount() == 4U, "parse failure preserves active table");
    expectActiveSourceRule("parse failure preserves active enabled state");
    expectActiveCounter(0x30U, "parse failure preserves active counter state");

    // A syntactically valid dynamic rule can still fail during compilation if
    // its runtime source is absent. This is the final fallible commit phase.
    char compile_failure[] =
        "CHECKSUM_XOR,0x555,A_TO_B,7,0,6,0x00,0\n"
        "SOURCE_INT,0x333,A_TO_B,8,8,1,missingRuntime,1,0,1,0,0,0,100,255\n";
    loaded = 99U;
    require(
        !RulePackageLoader::loadCsv(compile_failure, sizeof(compile_failure) - 1U, engine, &loaded),
        "reject replacement with missing runtime source");
    require(loaded == 0U, "compile failure resets loaded count");
    require(engine.stagingCount() == 4U, "compile failure restores active candidate mirror");
    require(!engine.stagingDirty(), "compile failure leaves no misleading draft changes");
    require(engine.activeCount() == 4U, "compile failure preserves active table");
    expectActiveSourceRule("compile failure preserves active enabled state");
    expectActiveCounter(0x40U, "compile failure preserves active counter state");
}

void expectRejectedPackage(char* package, size_t length, const char* label) {
    size_t loaded = 99U;
    require(!RulePackageLoader::loadCsv(package, length, engine, &loaded), label);
    require(loaded == 0U, "strict package rejection resets loaded count");
}

void testStrictRulePackageGrammar() {
    char extra_field[] =
        "STATIC,0x321,A_TO_B,0,8,1,130\n"
        "CHECKSUM_XOR,0x321,A_TO_B,7,0,6,0,1,ignored\n";
    expectRejectedPackage(extra_field, sizeof(extra_field) - 1U,
                          "reject extra package fields transactionally");

    char partial_integer[] = "STATIC,0x321,A_TO_B,0,8junk,1,130\n";
    expectRejectedPackage(partial_integer, sizeof(partial_integer) - 1U,
                          "reject partially parsed integer token");

    char signed_integer[] = "STATIC,+801,A_TO_B,0,8,1,130\n";
    expectRejectedPackage(signed_integer, sizeof(signed_integer) - 1U,
                          "reject signed unsigned-integer token");

    char missing_field[] = "STATIC,0x321,A_TO_B,0,,1,130\n";
    expectRejectedPackage(missing_field, sizeof(missing_field) - 1U,
                          "reject empty required field");

    char invalid_direction[] = "STATIC,0x321,SIDEWAYS,0,8,1,130\n";
    expectRejectedPackage(invalid_direction, sizeof(invalid_direction) - 1U,
                          "reject invalid direction");

    char invalid_boolean[] = "CHECKSUM_XOR,0x321,A_TO_B,7,0,6,0,2\n";
    expectRejectedPackage(invalid_boolean, sizeof(invalid_boolean) - 1U,
                          "reject non-boolean enabled token");

    char invalid_endian[] = "STATIC,0x321,A_TO_B,0,8,3,130\n";
    expectRejectedPackage(invalid_endian, sizeof(invalid_endian) - 1U,
                          "reject non-boolean byte-order token");

    char oversized_start[] = "STATIC,0x321,A_TO_B,65536,8,1,130\n";
    expectRejectedPackage(oversized_start, sizeof(oversized_start) - 1U,
                          "reject start bit before narrowing cast");

    char oversized_counter[] =
        "COUNTER,0x321,A_TO_B,0,8,1,4294967296,1,255,0\n";
    expectRejectedPackage(oversized_counter, sizeof(oversized_counter) - 1U,
                          "reject counter value before narrowing cast");

    char counter_outside_field[] =
        "COUNTER,0x321,A_TO_B,0,4,1,16,1,15,0\n";
    expectRejectedPackage(counter_outside_field, sizeof(counter_outside_field) - 1U,
                          "reject counter state outside target field");

    char static_outside_field[] = "STATIC,0x321,A_TO_B,0,4,1,16\n";
    expectRejectedPackage(static_outside_field, sizeof(static_outside_field) - 1U,
                          "reject static value outside target field");

    RuleStageRequest direct_outside_field{};
    direct_outside_field.kind = RuleKind::BIT_RANGE;
    direct_outside_field.can_id = 0x321U;
    direct_outside_field.direction = Direction::A_TO_B;
    direct_outside_field.start_bit = 0U;
    direct_outside_field.bit_length = 4U;
    direct_outside_field.little_endian = true;
    direct_outside_field.replace_value = 16U;
    require(!engine.stageRule(direct_outside_field),
            "reject direct-stage value outside target field");

    char invalid_checksum_byte[] = "CHECKSUM_XOR,0x321,A_TO_B,263,0,6,0,1\n";
    expectRejectedPackage(invalid_checksum_byte, sizeof(invalid_checksum_byte) - 1U,
                          "reject checksum byte before narrowing cast");

    char non_finite_source[] =
        "SOURCE_INT,0x321,A_TO_B,0,8,1,commandValue,nan,0,1,0,0,0,100,255\n";
    expectRejectedPackage(non_finite_source, sizeof(non_finite_source) - 1U,
                          "reject non-finite dynamic coefficient");

    char partial_float[] =
        "SOURCE_INT,0x321,A_TO_B,0,8,1,commandValue,1.5junk,0,1,0,0,0,100,255\n";
    expectRejectedPackage(partial_float, sizeof(partial_float) - 1U,
                          "reject partially parsed float token");

    char invalid_name[] =
        "SOURCE_INT,0x321,A_TO_B,0,8,1,bad\"source,1,0,1,0,0,0,100,255\n";
    expectRejectedPackage(invalid_name, sizeof(invalid_name) - 1U,
                          "reject quoted runtime source name");

    char non_portable_name[] =
        "SOURCE_INT,0x321,A_TO_B,0,8,1,bad source,1,0,1,0,0,0,100,255\n";
    expectRejectedPackage(non_portable_name, sizeof(non_portable_name) - 1U,
                          "reject whitespace in runtime source name");

    char oversized_sequence_byte[] =
        "SEQUENCE8,0x321,A_TO_B,0,8,1,0|256,0\n";
    expectRejectedPackage(oversized_sequence_byte, sizeof(oversized_sequence_byte) - 1U,
                          "reject sequence byte before narrowing cast");

    char sequence_outside_field[] =
        "SEQUENCE8,0x321,A_TO_B,0,4,1,0|16,0\n";
    expectRejectedPackage(sequence_outside_field, sizeof(sequence_outside_field) - 1U,
                          "reject sequence value outside target field");

    char invalid_sequence_index[] =
        "SEQUENCE8,0x321,A_TO_B,0,8,1,1|2,2\n";
    expectRejectedPackage(invalid_sequence_index, sizeof(invalid_sequence_index) - 1U,
                          "reject sequence index outside its values");

    char short_crc_table[] =
        "CHECKSUM_CRC8_AUTOSAR,0x321,A_TO_B,0,1,1,7,"
        "0|1|2|3|4|5|6|7|8|9|10|11|12|13|14,1\n";
    expectRejectedPackage(short_crc_table, sizeof(short_crc_table) - 1U,
                          "reject incomplete AUTOSAR data-ID table");

    char duplicate_active_state[] =
        "BIND_ACTIVE,profileCode,1|1\n"
        "STATIC,0x321,A_TO_B,0,8,1,130\n";
    expectRejectedPackage(duplicate_active_state, sizeof(duplicate_active_state) - 1U,
                          "reject duplicate active-selector state");

    char malformed_selector_entry[] =
        "SOURCE_SELECT_INT,0x321,A_TO_B,0,8,1,commandValue,1,0,0,0,0,100,"
        "profileCode,1:S:1:100:extra\n";
    expectRejectedPackage(malformed_selector_entry, sizeof(malformed_selector_entry) - 1U,
                          "reject selector entry with extra parts");

    char duplicate_selector_entry[] =
        "SOURCE_SELECT_INT,0x321,A_TO_B,0,8,1,commandValue,1,0,0,0,0,100,"
        "profileCode,1:D:10|1:S:1:100\n";
    expectRejectedPackage(duplicate_selector_entry, sizeof(duplicate_selector_entry) - 1U,
                          "reject duplicate selector entry");

    require(engine.activeCount() == 4U, "strict rejections preserve active package");
    require(engine.stagingCount() == 4U, "strict rejections restore active candidate mirror");
    require(!engine.stagingDirty(), "strict rejections leave no draft behind");
    expectActiveSourceRule("strict rejections preserve active source rule");
}

void testLatestActiveBindingReplacesEarlierStates() {
    char rules[] =
        "BIND_ACTIVE,profileCode,1|2\n"
        "BIND_ACTIVE,profileCode,3\n"
        "STATIC,0x5A0,A_TO_B,0,8,1,0xA5\n";
    size_t loaded = 0U;
    require(RulePackageLoader::loadCsv(rules, sizeof(rules) - 1U, engine, &loaded),
            "load package with replacement active binding");
    require(loaded == 1U, "active bind directives do not count as rules");

    require(values.publish("profileCode", 1.0F) >= 0, "publish superseded active state");
    CanFrame superseded = frame(0x5A0U);
    superseded.data[0] = 0x11U;
    require(engine.applyFrameMutations(superseded) == 0U && superseded.data[0] == 0x11U,
            "latest BIND_ACTIVE replaces earlier state mask");

    require(values.publish("profileCode", 3.0F) >= 0, "publish replacement active state");
    CanFrame selected = frame(0x5A0U);
    require(engine.applyFrameMutations(selected) == 1U && selected.data[0] == 0xA5U,
            "latest BIND_ACTIVE state enables later rule");
}

void testCompleteStrictPackageLoads() {
    char rules[] =
        "BIND_TABLE,commandValue,transferCurve\n"
        "BIND_OVERRIDE,commandValue,overrideActive,overrideValue\n"
        "BIND_ACTIVE,profileCode,2\n"
        "STATIC,08,A_TO_B,0,64,1,0xFEDCBA9876543210\n"
        "SOURCE_INT,0x501,A_TO_B,0,8,1,commandValue,1,0,1,0,0,0,100,255\n"
        "SOURCE_SELECT_INT,0x502,A_TO_B,0,8,1,commandValue,1,0,0,0,0,100,"
        "profileCode,0:D:0|1:S:1:100|2:S:2:200\n"
        "COUNTER,0x503,A_TO_B,0,4,1,0,1,15,0\n"
        "CHECKSUM_XOR,0x503,A_TO_B,7,0,6,0x5A,1\n"
        "SEQUENCE8,0x504,A_TO_B,8,8,1,0x10|0x20|0x30,1\n"
        "CHECKSUM_CRC8_AUTOSAR,0x505,A_TO_B,0,1,1,7,"
        "0xA0|0xA1|0xA2|0xA3|0xA4|0xA5|0xA6|0xA7|"
        "0xA8|0xA9|0xAA|0xAB|0xAC|0xAD|0xAE|0xAF,1\n";
    size_t loaded = 0U;
    require(RulePackageLoader::loadCsv(rules, sizeof(rules) - 1U, engine, &loaded),
            "strict parser accepts every supported package row type");
    require(loaded == 7U && engine.activeCount() == 7U,
            "three bindings plus seven mutations load with correct count");
}

void testSequenceRuntimeStateRemainsIndex() {
    require(values.publish("profileCode", 2.0F) >= 0, "enable sequence fixture");

    RuleListEntry before[8] = {};
    const size_t before_count = engine.listRules(before, 8U);
    uint16_t sequence_rule_id = 0xFFFFU;
    for (size_t i = 0U; i < before_count; ++i) {
        if (before[i].request.kind == RuleKind::SEQUENCE8) {
            sequence_rule_id = before[i].rule_id;
            require(before[i].request.sequence_initial_index == 1U,
                    "sequence listing begins at configured index");
            break;
        }
    }
    require(sequence_rule_id != 0xFFFFU, "find active sequence fixture");

    CanFrame first = frame(0x504U);
    require(engine.applyFrameMutations(first) == 1U && first.data[1] == 0x20U,
            "sequence emits value selected by current index");

    RuleListEntry after[8] = {};
    const size_t after_count = engine.listRules(after, 8U);
    for (size_t i = 0U; i < after_count; ++i) {
        if (after[i].rule_id == sequence_rule_id) {
            require(after[i].request.sequence_initial_index == 2U,
                    "sequence runtime listing remains the next index");
        }
    }

    const uint32_t epoch = engine.ruleEpoch();
    require(engine.setActiveRuleValue(sequence_rule_id, 0U, epoch),
            "set sequence runtime index explicitly");
    CanFrame reset = frame(0x504U);
    require(engine.applyFrameMutations(reset) == 1U && reset.data[1] == 0x10U,
            "sequence API index selects the expected next value");
}

void testDynamicRuntimeRejectsNonFiniteAndClamps32Bit() {
    char rules[] =
        "BIND_OVERRIDE,commandValue,overrideActive,overrideValue\n"
        "BIND_ACTIVE,profileCode,2\n"
        "SOURCE_INT,0x5B0,A_TO_B,0,32,1,commandValue,1,0,1e20,0,-1,0,1e30,0\n";
    size_t loaded = 0U;
    require(RulePackageLoader::loadCsv(rules, sizeof(rules) - 1U, engine, &loaded),
            "load 32-bit dynamic hardening fixture");
    require(loaded == 1U, "dynamic hardening fixture contains one mutation");

    publish(100.0F, 0.0F, 0.0F);
    CanFrame clamped = frame(0x5B0U);
    require(engine.applyFrameMutations(clamped) == 1U,
            "oversized 32-bit dynamic transform applies");
    require(clamped.data[0] == 0xFFU && clamped.data[1] == 0xFFU &&
            clamped.data[2] == 0xFFU && clamped.data[3] == 0xFFU,
            "oversized 32-bit dynamic transform clamps without wraparound");

    constexpr uint8_t original[] = {0x11U, 0x22U, 0x33U, 0x44U};
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();

    require(values.publish("commandValue", not_a_number) >= 0, "publish NaN dynamic source");
    require(values.publish("overrideActive", 0.0F) >= 0, "disable override for NaN source");
    CanFrame nan_source = frame(0x5B0U);
    std::memcpy(nan_source.data, original, sizeof(original));
    static_cast<void>(engine.applyFrameMutations(nan_source));
    require(std::memcmp(nan_source.data, original, sizeof(original)) == 0,
            "NaN dynamic source leaves target bits untouched");

    require(values.publish("commandValue", 10.0F) >= 0, "restore finite dynamic source");
    require(values.publish("overrideActive", 1.0F) >= 0, "enable NaN override");
    require(values.publish("overrideValue", not_a_number) >= 0, "publish NaN override value");
    CanFrame nan_override = frame(0x5B0U);
    std::memcpy(nan_override.data, original, sizeof(original));
    static_cast<void>(engine.applyFrameMutations(nan_override));
    require(std::memcmp(nan_override.data, original, sizeof(original)) == 0,
            "NaN override value leaves target bits untouched");

    require(values.publish("overrideActive", 0.0F) >= 0, "restore finite override state");
    require(values.publish("overrideValue", 0.0F) >= 0, "restore finite override value");
    require(values.publish("profileCode", not_a_number) >= 0, "publish NaN selector");
    CanFrame nan_selector = frame(0x5B0U);
    std::memcpy(nan_selector.data, original, sizeof(original));
    require(engine.applyFrameMutations(nan_selector) == 0U &&
            std::memcmp(nan_selector.data, original, sizeof(original)) == 0,
            "NaN selector disables rule without clearing target bits");

    publish(0.0F, 0.0F, 0.0F);
}

void testStagedRuntimePublishesOnlyOnCommit() {
    engine.revertStagingToActive();
    require(engine.stagingCount() == 4U, "restore active rules to staging");

    RuleStageRequest replacement{};
    replacement.kind = RuleKind::COUNTER;
    replacement.can_id = 0x2A0U;
    replacement.direction = Direction::A_TO_B;
    replacement.enabled = false;
    replacement.start_bit = 48U;
    replacement.bit_length = 8U;
    replacement.little_endian = true;
    replacement.counter_initial = 0xA0U;
    replacement.counter_step = 0x10U;
    replacement.counter_wrap_after = 0xF0U;
    replacement.counter_wrap_to = 0x00U;

    uint16_t counter_rule_id = 0xFFFFU;
    require(engine.stageRule(replacement, &counter_rule_id), "stage disabled counter replacement");
    expectActiveCounter(0x50U, "staging does not disable or reset active counter");
    require(engine.applyCommit(), "commit disabled counter replacement");
    expectDisabledCounter("commit publishes staged disabled state");

    replacement.enabled = true;
    require(engine.stageRule(replacement, &counter_rule_id), "stage enabled counter replacement");
    expectDisabledCounter("staging does not re-enable active counter");
    const uint32_t pending_epoch = engine.ruleEpoch();
    require(engine.setRuleValue(counter_rule_id, 0xB0U, pending_epoch),
            "update pending counter value");
    RuleListEntry pending_rules[8] = {};
    const size_t pending_count = engine.listStagedRules(pending_rules, 8U);
    bool pending_counter_found = false;
    for (size_t i = 0U; i < pending_count; ++i) {
        if (pending_rules[i].rule_id == counter_rule_id) {
            pending_counter_found = pending_rules[i].request.counter_initial == 0xB0U;
            break;
        }
    }
    require(pending_counter_found, "candidate listing exposes pending counter state");
    expectDisabledCounter("pending value update does not re-enable active counter");
    require(engine.enableRule(counter_rule_id, true, pending_epoch), "enable pending counter");
    expectDisabledCounter("pending enable remains isolated from active counter");
    require(engine.applyCommit(), "commit enabled counter replacement");
    expectActiveCounter(0xB0U, "commit publishes latest pending counter value and enabled state");

    // Once a table is committed, the ordinary controls are intentionally live.
    // Authoring UIs use the staged-only controls so their Draft view remains a
    // real candidate and never changes forwarded frames before Apply.
    const uint32_t draft_epoch = engine.ruleEpoch();
    require(engine.setStagedRuleValue(counter_rule_id, 0xD0U, draft_epoch),
            "edit committed counter value in candidate only");
    RuleListEntry draft_rules[8] = {};
    const size_t draft_count = engine.listStagedRules(draft_rules, 8U);
    bool draft_counter_found = false;
    for (size_t i = 0U; i < draft_count; ++i) {
        if (draft_rules[i].rule_id == counter_rule_id) {
            draft_counter_found = draft_rules[i].request.counter_initial == 0xD0U;
            break;
        }
    }
    require(draft_counter_found, "candidate listing exposes staged-only counter state");
    require(engine.setStagedRuleEnabled(counter_rule_id, false, draft_epoch),
            "disable committed counter in candidate only");
    expectActiveCounter(0xC0U, "post-commit candidate edits leave live counter running");
    require(engine.applyCommit(), "apply post-commit candidate controls");
    expectDisabledCounter("post-commit candidate controls publish only on apply");
}

void testRuleHandleEpochRejectsSlotReuse() {
    const uint32_t old_epoch = engine.ruleEpoch();
    engine.clearRules();

    RuleStageRequest replacement{};
    replacement.kind = RuleKind::COUNTER;
    replacement.can_id = 0x444U;
    replacement.direction = Direction::A_TO_B;
    replacement.enabled = false;
    replacement.start_bit = 0U;
    replacement.bit_length = 8U;
    replacement.little_endian = true;
    replacement.counter_initial = 0xA0U;
    replacement.counter_step = 1U;
    replacement.counter_wrap_after = 0xFEU;
    replacement.counter_wrap_to = 0U;

    RuleStageRequest abandoned = replacement;
    abandoned.can_id = 0x443U;
    uint16_t abandoned_rule_id = 0xFFFFU;
    require(engine.stageRule(abandoned, &abandoned_rule_id), "stage abandoned pending rule");
    const uint32_t abandoned_epoch = engine.ruleEpoch();
    engine.clearStaging();

    uint16_t reused_rule_id = 0xFFFFU;
    require(engine.stageRule(replacement, &reused_rule_id), "stage rule into reusable slot");
    require(reused_rule_id == 0U, "empty table reuses first runtime slot");
    const uint32_t staged_epoch = engine.ruleEpoch();
    require(staged_epoch != old_epoch, "staging slot reuse advances handle epoch");
    require(staged_epoch != abandoned_epoch, "staging reset advances pending handle epoch");
    require(!engine.setRuleValue(reused_rule_id, 0xC0U, abandoned_epoch),
            "abandoned pending value cannot alter a reused staged slot");
    require(!engine.enableRule(reused_rule_id, true, abandoned_epoch),
            "abandoned pending enable cannot alter a reused staged slot");
    require(engine.applyCommit(), "commit reused runtime slot");
    const uint32_t active_epoch = engine.ruleEpoch();
    require(active_epoch != staged_epoch, "commit advances handle epoch");

    require(!engine.enableRule(reused_rule_id, true, old_epoch),
            "stale enable cannot activate a reused rule slot");
    require(!engine.setRuleValue(reused_rule_id, 0xB0U, old_epoch),
            "stale value cannot alter a reused rule slot");
    require(!engine.enableRule(95U, true, active_epoch),
            "current epoch still rejects nonexistent rule slot");

    CanFrame untouched = frame(0x444U);
    require(engine.applyFrameMutations(untouched) == 0U && untouched.data[0] == 0U,
            "stale controls leave replacement disabled");
    require(engine.setRuleValue(reused_rule_id, 0xB0U, active_epoch),
            "current handle updates replacement value");
    require(engine.enableRule(reused_rule_id, true, active_epoch),
            "current handle enables replacement");
    CanFrame controlled = frame(0x444U);
    require(engine.applyFrameMutations(controlled) == 1U && controlled.data[0] == 0xB0U,
            "current handle controls the intended replacement rule");
}

void testClearRulesCommitsEmptyTable() {
    engine.clearRules();
    require(engine.stagingCount() == 0U, "clear rules leaves staging empty");
    require(engine.activeCount() == 0U, "clear rules commits empty active table");
    CanFrame value = frame(0x2A0U);
    require(engine.applyFrameMutations(value) == 0U, "clear rules leaves frame unmodified");
}

void testAutosarDataIdProfile() {
    // Synthetic vectors independently exercise every counter nibble and every
    // entry in the 16-byte Data-ID sequence. The protected payload is bytes
    // 1..7 and the selected Data ID is appended after those bytes.
    struct AutosarVector {
        uint8_t protected_payload[7];
        uint8_t expected_checksum;
    };
    constexpr AutosarVector vectors[] = {
        {{0x00U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U, 0x70U}, 0x01U},
        {{0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U}, 0xE3U},
        {{0x22U, 0x24U, 0x36U, 0x48U, 0x5AU, 0x6CU, 0x7EU}, 0xEAU},
        {{0x33U, 0x26U, 0x39U, 0x4CU, 0x5FU, 0x72U, 0x85U}, 0xB5U},
        {{0x44U, 0x28U, 0x3CU, 0x50U, 0x64U, 0x78U, 0x8CU}, 0xCDU},
        {{0x55U, 0x2AU, 0x3FU, 0x54U, 0x69U, 0x7EU, 0x93U}, 0xB4U},
        {{0x66U, 0x2CU, 0x42U, 0x58U, 0x6EU, 0x84U, 0x9AU}, 0x2CU},
        {{0x77U, 0x2EU, 0x45U, 0x5CU, 0x73U, 0x8AU, 0xA1U}, 0xB3U},
        {{0x88U, 0x30U, 0x48U, 0x60U, 0x78U, 0x90U, 0xA8U}, 0x8DU},
        {{0x99U, 0x32U, 0x4BU, 0x64U, 0x7DU, 0x96U, 0xAFU}, 0x6FU},
        {{0xAAU, 0x34U, 0x4EU, 0x68U, 0x82U, 0x9CU, 0xB6U}, 0xEFU},
        {{0xBBU, 0x36U, 0x51U, 0x6CU, 0x87U, 0xA2U, 0xBDU}, 0x70U},
        {{0xCCU, 0x38U, 0x54U, 0x70U, 0x8CU, 0xA8U, 0xC4U}, 0x69U},
        {{0xDDU, 0x3AU, 0x57U, 0x74U, 0x91U, 0xAEU, 0xCBU}, 0x43U},
        {{0xEEU, 0x3CU, 0x5AU, 0x78U, 0x96U, 0xB4U, 0xD2U}, 0x4AU},
        {{0xFFU, 0x3EU, 0x5DU, 0x7CU, 0x9BU, 0xBAU, 0xD9U}, 0xCCU},
    };
    for (const AutosarVector& vector : vectors) {
        CanFrame value = frame(0x360U);
        std::memcpy(value.data + 1U, vector.protected_payload, sizeof(vector.protected_payload));
        require(engine.applyFrameMutations(value) == 1U, "AUTOSAR Data-ID checksum applied");
        require(value.data[0] == vector.expected_checksum,
                "AUTOSAR Data-ID checksum matches independent vector");
    }
}
void testSignalDecodeHonorsDlc() {
    CanFrame value{};
    value.id = 0x420U;
    value.dlc = 4U;
    value.data[1] = 0x88U;
    value.data[4] = 0x59U;  // storage exists, but is outside a DLC-4 payload

    bored::signalscope::DbcSignalDef inside{};
    inside.start_bit = 8U;
    inside.length = 8U;
    inside.little_endian = true;
    inside.factor = 1.0F;
    float decoded = 0.0F;
    require(bored::signalscope::decodeSignal(value, inside, decoded) && decoded == 136.0F,
            "decode valid byte-1 signal from DLC-4 frame");

    bored::signalscope::DbcSignalDef outside = inside;
    outside.start_bit = 32U;
    require(!bored::signalscope::decodeSignal(value, outside, decoded),
            "reject byte-4 signal outside DLC-4 payload");
    value.dlc = 5U;
    require(bored::signalscope::decodeSignal(value, outside, decoded) && decoded == 89.0F,
            "decode byte-4 signal when payload actually contains it");
}

void testPackageFinalLineNeedsNoSentinel() {
    constexpr char source[] = "STATIC,0x321,A_TO_B,0,8,1,130";
    char exact_bytes[sizeof(source) - 1U] = {};
    std::memcpy(exact_bytes, source, sizeof(exact_bytes));
    size_t loaded = 0U;
    require(RulePackageLoader::loadCsv(exact_bytes, sizeof(exact_bytes), engine, &loaded),
            "parse an exact-length final package row without reading a sentinel byte");
    require(loaded == 1U, "exact-length package loads one rule");
    CanFrame value = frame(0x321U);
    require(engine.applyFrameMutations(value) == 1U && value.data[0] == 130U,
            "exact-length final package row compiles correctly");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("MUTATION_PIPELINE_TESTS_BEGIN");

    values.init();
    tables.init();
    publish(0.0F, 0.0F, 0.0F);
    float empty_transfer_curve[101] = {};
    require(tables.publish("transferCurve", empty_transfer_curve, 101U, false) >= 0,
            "register transfer table");
    engine.init();
    engine.setRuntimeValueRegistry(&values);
    engine.setRuntimeTableRegistry(&tables);

    char rules[] =
        "BIND_TABLE,commandValue,transferCurve\n"
        "BIND_OVERRIDE,commandValue,overrideActive,overrideValue\n"
        "BIND_ACTIVE,profileCode,1|2|3|4|5|6|7|8|9|10\n"
        "SOURCE_INT,0x280,A_TO_B,8,8,1,commandValue,0.5,20,2.54,0,0,0,99.5,254\n"
        "COUNTER,0x2A0,A_TO_B,48,8,1,0x00,0x10,0xF0,0x00\n"
        "CHECKSUM_XOR,0x2A0,A_TO_B,7,0,6,0x00,1\n"
        "CHECKSUM_CRC8_AUTOSAR,0x360,A_TO_B,0,1,1,7,0xA0|0xA1|0xA2|0xA3|0xA4|0xA5|0xA6|0xA7|0xA8|0xA9|0xAA|0xAB|0xAC|0xAD|0xAE|0xAF,1\n";

    size_t loaded = 0U;
    require(RulePackageLoader::loadCsv(rules, sizeof(rules) - 1U, engine, &loaded),
            "load declarative rules");
    require(loaded == 4U, "four declarative mutations loaded");

    testRuntimeOverride();
    testFallbackCurveAndTable();
    testCounterBeforeChecksum();
    testAutosarDataIdProfile();
    testSignalDecodeHonorsDlc();
    testStagedCandidateListing();
    testFailedPackagesAreTransactional();
    testStrictRulePackageGrammar();
    testStagedRuntimePublishesOnlyOnCommit();
    testRuleHandleEpochRejectsSlotReuse();
    testLatestActiveBindingReplacesEarlierStates();
    testCompleteStrictPackageLoads();
    testSequenceRuntimeStateRemainsIndex();
    testDynamicRuntimeRejectsNonFiniteAndClamps32Bit();
    testPackageFinalLineNeedsNoSentinel();
    testClearRulesCommitsEmptyTable();

    if (failures == 0U) Serial.println("MUTATION_PIPELINE_TESTS_PASS");
    else Serial.printf("MUTATION_PIPELINE_TESTS_FAIL count=%lu\n", static_cast<unsigned long>(failures));
}

void loop() {
    delay(1000);
}
