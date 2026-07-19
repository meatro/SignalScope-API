#include <Arduino.h>

#include <cmath>
#include <cstring>

#include "core/dbc_parser.hpp"
#include "core/frame_cache.hpp"
#include "core/signal_catalog.hpp"

using bored::signalscope::CanFrame;
using bored::signalscope::DbcSignalDef;
using bored::signalscope::Direction;
using bored::signalscope::FrameCache;
using bored::signalscope::SignalCatalogLiveValue;
using bored::signalscope::readSignalCatalogLiveValue;
using bored::signalscope::signalCatalogQueryMatches;

namespace {

FrameCache frame_cache;
uint32_t failures = 0U;

void require(bool condition, const char* label) {
    if (condition) return;
    ++failures;
    Serial.printf("FAIL %s\n", label);
}

DbcSignalDef signal(uint32_t can_id, const char* name, uint16_t start_bit = 0U) {
    DbcSignalDef result{};
    result.can_id = can_id;
    result.start_bit = start_bit;
    result.length = 8U;
    result.little_endian = true;
    result.factor = 2.0F;
    result.offset = 1.0F;
    std::strncpy(result.name, name, sizeof(result.name) - 1U);
    return result;
}

CanFrame frame(uint32_t can_id, Direction direction, uint32_t timestamp_us,
               uint8_t dlc, uint8_t first_byte) {
    CanFrame result{};
    result.id = can_id;
    result.direction = direction;
    result.timestamp_us = timestamp_us;
    result.dlc = dlc;
    result.data[0] = first_byte;
    return result;
}

void verifyQueryMatching() {
    const DbcSignalDef rpm = signal(0x280U, "MO1_Drehzahl");
    require(signalCatalogQueryMatches(rpm, nullptr), "null query matches all");
    require(signalCatalogQueryMatches(rpm, ""), "empty query matches all");
    require(signalCatalogQueryMatches(rpm, "mo1_dreh"), "name query ignores case");
    require(signalCatalogQueryMatches(rpm, "DREHZAHL"), "uppercase name query matches");
    require(signalCatalogQueryMatches(rpm, "0x280"), "prefixed hexadecimal CAN id matches");
    require(signalCatalogQueryMatches(rpm, "280"), "compact hexadecimal CAN id matches");
    require(signalCatalogQueryMatches(rpm, "640"), "decimal CAN id matches");
    require(!signalCatalogQueryMatches(rpm, "0x281"), "different CAN id does not match");
    require(!signalCatalogQueryMatches(rpm, "vehicle_speed"), "different name does not match");
}

void verifyNewestPhysicalDecodeAndFreshness() {
    frame_cache.init();
    const DbcSignalDef rpm = signal(0x280U, "MO1_Drehzahl");

    const CanFrame older = frame(0x280U, Direction::A_TO_B, 1000000U, 8U, 10U);
    const CanFrame newer = frame(0x280U, Direction::B_TO_A, 1200000U, 8U, 20U);
    frame_cache.update(older, 1000U, false, nullptr, false);
    frame_cache.update(newer, 1200U, false, nullptr, false);

    SignalCatalogLiveValue sample = readSignalCatalogLiveValue(
        frame_cache, rpm, 1300000U, 1500000U);
    require(sample.frame_available, "physical frame is available");
    require(sample.valid && sample.live, "new physical value is decoded and live");
    require(sample.direction == Direction::B_TO_A, "newest physical direction wins");
    require(sample.age_ms == 100U, "live sample age is reported");
    require(std::fabs(sample.value - 41.0F) < 0.001F, "DBC factor and offset are applied");

    // A replay can update the ordinary cache view but must not replace the
    // physical sample used by the catalog.
    const CanFrame replay = frame(0x280U, Direction::B_TO_A, 1290000U, 8U, 99U);
    frame_cache.update(replay, 1290U, false, nullptr, true);
    sample = readSignalCatalogLiveValue(frame_cache, rpm, 1300000U, 1500000U);
    require(std::fabs(sample.value - 41.0F) < 0.001F,
            "synthetic replay cannot replace physical catalog evidence");

    sample = readSignalCatalogLiveValue(frame_cache, rpm, 2700000U, 1500000U);
    require(sample.valid && sample.live, "freshness boundary is inclusive");
    require(sample.age_ms == 1500U, "boundary age is retained");

    sample = readSignalCatalogLiveValue(frame_cache, rpm, 2700001U, 1500000U);
    require(sample.valid && !sample.live, "stale decoded value remains valid but not live");
    require(std::fabs(sample.value - 41.0F) < 0.001F, "stale decoded value is retained");
}

void verifyDlcSafetyAndMissingFrames() {
    frame_cache.init();
    const DbcSignalDef outside_payload = signal(0x321U, "OutsidePayload", 16U);
    const CanFrame short_frame = frame(0x321U, Direction::A_TO_B, 500000U, 2U, 0x12U);
    frame_cache.update(short_frame, 500U, false, nullptr, false);

    SignalCatalogLiveValue sample = readSignalCatalogLiveValue(
        frame_cache, outside_payload, 600000U, 1500000U);
    require(sample.frame_available, "short physical frame remains visible");
    require(!sample.valid && !sample.live, "signal outside DLC is never decoded");
    require(sample.direction == Direction::A_TO_B, "invalid decode retains frame direction");
    require(sample.age_ms == 100U, "invalid decode retains physical age");

    const DbcSignalDef absent = signal(0x555U, "Absent");
    sample = readSignalCatalogLiveValue(frame_cache, absent, 600000U, 1500000U);
    require(!sample.frame_available && !sample.valid && !sample.live,
            "missing physical frame returns an empty sample");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(100U);
    verifyQueryMatching();
    verifyNewestPhysicalDecodeAndFreshness();
    verifyDlcSafetyAndMissingFrames();
    Serial.printf("SIGNAL_CATALOG_TEST_%s failures=%lu\n",
                  failures == 0U ? "PASS" : "FAIL",
                  static_cast<unsigned long>(failures));
}

void loop() {
    delay(1000U);
}
