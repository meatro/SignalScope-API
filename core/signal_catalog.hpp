#pragma once

#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

class FrameCache;
struct DbcSignalDef;

struct SignalCatalogLiveValue {
    bool frame_available = false;
    bool valid = false;
    bool live = false;
    float value = 0.0F;
    uint32_t age_ms = 0U;
    Direction direction = Direction::A_TO_B;
};

// Empty queries match every signal. Non-empty queries match a
// case-insensitive signal-name substring, decimal CAN id text, hexadecimal CAN
// id text with a 0x prefix, or compact hexadecimal CAN id text.
bool signalCatalogQueryMatches(const DbcSignalDef& signal, const char* query);

// Decodes directly from the newest physical frame in either direction. This
// is an HTTP/UI read path: it does not enable broad decoding, subscribe the
// signal, or mutate the cache. A decoded-but-old value remains valid while
// live is false.
SignalCatalogLiveValue readSignalCatalogLiveValue(
    const FrameCache& frame_cache,
    const DbcSignalDef& signal,
    uint32_t now_us,
    uint32_t freshness_us);

}  // namespace bored::signalscope
