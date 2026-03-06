#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dbc_parser.hpp"
#include "types.hpp"

namespace bored::signalscope {

struct SignalCacheSnapshot {
    uint16_t index = 0;
    uint32_t can_id = 0;
    char name[40] = {0};
    float value = 0.0F;
    uint32_t generation = 0;
    bool valid = false;
    bool subscribed = false;
};

class SignalCache {
public:
    static constexpr size_t kMaxSignals = DbcDatabase::kMaxSignals;

    void init();
    void resetForDbc(const DbcDatabase& dbc);

    void clearSubscriptions();
    void setDecodeAll(bool enabled);
    bool decodeAll() const;
    bool subscribeSignal(uint16_t signal_index, bool enabled);
    bool isSignalSubscribed(uint16_t signal_index) const;

    size_t decodeObservedFrame(const DbcDatabase& dbc, const CanFrame& frame);

    bool readSignal(uint16_t signal_index, float& out_value, uint32_t& out_generation, bool& out_valid) const;
    bool signalCanId(uint16_t signal_index, uint32_t& out_can_id) const;
    size_t snapshotByIndexes(
        const uint16_t* signal_indexes,
        size_t signal_count,
        SignalCacheSnapshot* out_entries,
        size_t out_capacity) const;

    int32_t findSignalIndexByName(uint32_t can_id, const char* name) const;
    size_t signalCount() const;

private:
    struct NameMapEntry {
        uint32_t can_id = 0;
        char name[40] = {0};
        uint16_t index = 0;
    };

    std::atomic<uint32_t> value_bits_[kMaxSignals];
    std::atomic<uint32_t> generation_[kMaxSignals];
    std::atomic<uint8_t> valid_[kMaxSignals];
    std::atomic<uint8_t> subscribed_[kMaxSignals];

    NameMapEntry name_map_[kMaxSignals];
    std::atomic<uint16_t> signal_count_{0};
    std::atomic<uint8_t> decode_all_{0};
};

}  // namespace bored::signalscope
