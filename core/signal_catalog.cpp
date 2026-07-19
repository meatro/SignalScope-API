#include "signal_catalog.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "dbc_parser.hpp"
#include "frame_cache.hpp"
#include "signal_codec.hpp"

namespace bored::signalscope {
namespace {

bool containsCaseInsensitive(const char* text, const char* query) {
    if (query == nullptr || query[0] == '\0') return true;
    if (text == nullptr || text[0] == '\0') return false;

    for (const char* start = text; *start != '\0'; ++start) {
        const char* left = start;
        const char* right = query;
        while (*left != '\0' && *right != '\0' &&
               std::tolower(static_cast<unsigned char>(*left)) ==
                   std::tolower(static_cast<unsigned char>(*right))) {
            ++left;
            ++right;
        }
        if (*right == '\0') return true;
    }
    return false;
}

bool readNewestPhysicalFrame(
    const FrameCache& frame_cache,
    uint32_t can_id,
    uint32_t now_us,
    FrameCacheSnapshot& newest,
    uint32_t& age_us) {

    FrameCacheSnapshot a_to_b{};
    FrameCacheSnapshot b_to_a{};
    const bool has_a_to_b = frame_cache.readPhysical(can_id, Direction::A_TO_B, &a_to_b);
    const bool has_b_to_a = frame_cache.readPhysical(can_id, Direction::B_TO_A, &b_to_a);
    if (!has_a_to_b && !has_b_to_a) return false;

    if (has_a_to_b && has_b_to_a) {
        const uint32_t age_a_to_b = now_us - a_to_b.last_timestamp_us;
        const uint32_t age_b_to_a = now_us - b_to_a.last_timestamp_us;
        newest = age_a_to_b <= age_b_to_a ? a_to_b : b_to_a;
    } else {
        newest = has_a_to_b ? a_to_b : b_to_a;
    }
    age_us = now_us - newest.last_timestamp_us;
    return true;
}

}  // namespace

bool signalCatalogQueryMatches(const DbcSignalDef& signal, const char* query) {
    if (query == nullptr || query[0] == '\0') return true;
    if (containsCaseInsensitive(signal.name, query)) return true;

    char decimal[16] = {};
    char hexadecimal[16] = {};
    char compact_hexadecimal[16] = {};
    std::snprintf(decimal, sizeof(decimal), "%lu", static_cast<unsigned long>(signal.can_id));
    std::snprintf(hexadecimal, sizeof(hexadecimal), "0x%lX", static_cast<unsigned long>(signal.can_id));
    std::snprintf(compact_hexadecimal, sizeof(compact_hexadecimal), "%lX",
                  static_cast<unsigned long>(signal.can_id));
    return containsCaseInsensitive(decimal, query) ||
        containsCaseInsensitive(hexadecimal, query) ||
        containsCaseInsensitive(compact_hexadecimal, query);
}

SignalCatalogLiveValue readSignalCatalogLiveValue(
    const FrameCache& frame_cache,
    const DbcSignalDef& signal,
    uint32_t now_us,
    uint32_t freshness_us) {

    SignalCatalogLiveValue result{};
    FrameCacheSnapshot physical{};
    uint32_t age_us = 0U;
    if (!readNewestPhysicalFrame(frame_cache, signal.can_id, now_us, physical, age_us)) {
        return result;
    }

    result.frame_available = true;
    result.direction = physical.direction;
    result.age_ms = age_us / 1000U;

    CanFrame frame{};
    frame.id = physical.can_id;
    frame.direction = physical.direction;
    frame.dlc = physical.dlc <= 8U ? physical.dlc : 8U;
    frame.timestamp_us = physical.last_timestamp_us;
    for (uint8_t i = 0U; i < frame.dlc; ++i) frame.data[i] = physical.data[i];

    float value = 0.0F;
    result.valid = decodeSignal(frame, signal, value) && std::isfinite(value);
    if (result.valid) result.value = value;
    result.live = result.valid && age_us <= freshness_us;
    return result;
}

}  // namespace bored::signalscope
