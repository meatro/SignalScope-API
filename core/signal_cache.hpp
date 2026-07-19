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
    uint32_t timestamp_us = 0;
    Direction direction = Direction::A_TO_B;
    bool valid = false;
    bool subscribed = false;
};

class SignalCache {
public:
    SignalCache() = default;
    ~SignalCache();

    SignalCache(const SignalCache&) = delete;
    SignalCache& operator=(const SignalCache&) = delete;

    void init();
    bool resetForDbc(const DbcDatabase& dbc);
    // Exchanges complete cache storage after the CAN decoder reader set has
    // drained, allowing a DBC and its matching cache to commit together.
    void swap(SignalCache& other);

    void clearSubscriptions();
    void setDecodeAll(bool enabled);
    bool decodeAll() const;
    bool subscribeSignal(uint16_t signal_index, bool enabled);
    bool isSignalSubscribed(uint16_t signal_index) const;

    size_t decodeObservedFrame(const DbcDatabase& dbc, const CanFrame& frame);

    bool readSignal(uint16_t signal_index, float& out_value, uint32_t& out_generation, bool& out_valid) const;
    bool readSignalState(
        uint16_t signal_index,
        float& out_value,
        uint32_t& out_generation,
        bool& out_valid,
        uint32_t& out_timestamp_us,
        Direction& out_direction) const;
    bool signalCanId(uint16_t signal_index, uint32_t& out_can_id) const;
    size_t snapshotByIndexes(
        const uint16_t* signal_indexes,
        size_t signal_count,
        SignalCacheSnapshot* out_entries,
        size_t out_capacity) const;

    int32_t findSignalIndexByName(uint32_t can_id, const char* name) const;
    int32_t findSignalIndexByName(const char* name) const;
    size_t signalCount() const;

private:
    struct NameMapEntry {
        uint32_t can_id = 0;
        char name[40] = {0};
        uint16_t index = 0;
    };

    struct RuntimeEntry {
        // The CAN owner updates the observation fields as one logical sample,
        // while application/UI readers run on the other core. An even value is
        // stable; an odd value means a publication is in progress.
        std::atomic<uint32_t> observation_sequence{0U};
        std::atomic<uint32_t> value_bits{0U};
        std::atomic<uint32_t> generation{0U};
        std::atomic<uint32_t> timestamp_us{0U};
        std::atomic<uint8_t> direction{static_cast<uint8_t>(Direction::A_TO_B)};
        std::atomic<uint8_t> valid{0U};
        std::atomic<uint8_t> subscribed{0U};
        NameMapEntry name_map{};
    };

    static void readRuntimeEntry(
        const RuntimeEntry& entry,
        uint32_t& out_value_bits,
        uint32_t& out_generation,
        uint32_t& out_timestamp_us,
        Direction& out_direction,
        bool& out_valid);
    void releaseEntries();

    RuntimeEntry* entries_ = nullptr;
    size_t capacity_ = 0U;
    std::atomic<uint16_t> signal_count_{0};
    std::atomic<uint8_t> decode_all_{0};
};

}  // namespace bored::signalscope
