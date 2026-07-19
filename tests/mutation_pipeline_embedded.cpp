#include <Arduino.h>

#include <cstring>

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
    require(engine.stagingCount() == 0U, "empty package failure clears staging");
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
    require(engine.stagingCount() == 0U, "parse failure clears staging");
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
    require(engine.stagingCount() == 0U, "compile failure clears staging");
    require(engine.activeCount() == 4U, "compile failure preserves active table");
    expectActiveSourceRule("compile failure preserves active enabled state");
    expectActiveCounter(0x40U, "compile failure preserves active counter state");
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
    expectDisabledCounter("pending value update does not re-enable active counter");
    require(engine.enableRule(counter_rule_id, true, pending_epoch), "enable pending counter");
    expectDisabledCounter("pending enable remains isolated from active counter");
    require(engine.applyCommit(), "commit enabled counter replacement");
    expectActiveCounter(0xB0U, "commit publishes latest pending counter value and enabled state");
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
    testStagedRuntimePublishesOnlyOnCommit();
    testRuleHandleEpochRejectsSlotReuse();
    testClearRulesCommitsEmptyTable();

    if (failures == 0U) Serial.println("MUTATION_PIPELINE_TESTS_PASS");
    else Serial.printf("MUTATION_PIPELINE_TESTS_FAIL count=%lu\n", static_cast<unsigned long>(failures));
}

void loop() {
    delay(1000);
}
