#include "signal_cache.hpp"

#include <cstring>
#include <new>

#include "runtime_memory.hpp"
#include "signal_codec.hpp"

namespace bored::signalscope {

namespace {

uint32_t floatToBits(float value) {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsToFloat(uint32_t bits) {
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

SignalCache::~SignalCache() {
    releaseEntries();
}

void SignalCache::init() {
    releaseEntries();
    signal_count_.store(0, std::memory_order_relaxed);
    decode_all_.store(0, std::memory_order_relaxed);
}

bool SignalCache::resetForDbc(const DbcDatabase& dbc) {
    releaseEntries();
    const size_t dbc_signal_count = dbc.signalCount();
    if (dbc_signal_count == 0U || dbc_signal_count > 0xFFFFU) return false;

    void* memory = allocateRuntimeMemory(dbc_signal_count * sizeof(RuntimeEntry));
    if (memory == nullptr) return false;
    entries_ = static_cast<RuntimeEntry*>(memory);
    capacity_ = dbc_signal_count;
    for (size_t i = 0; i < capacity_; ++i) new (&entries_[i]) RuntimeEntry();

    signal_count_.store(static_cast<uint16_t>(dbc_signal_count), std::memory_order_release);
    decode_all_.store(0U, std::memory_order_release);

    for (size_t i = 0; i < capacity_; ++i) {
        entries_[i].observation_sequence.store(0U, std::memory_order_relaxed);
        entries_[i].value_bits.store(floatToBits(0.0F), std::memory_order_relaxed);
        entries_[i].generation.store(0U, std::memory_order_relaxed);
        entries_[i].timestamp_us.store(0U, std::memory_order_relaxed);
        entries_[i].direction.store(static_cast<uint8_t>(Direction::A_TO_B), std::memory_order_relaxed);
        entries_[i].valid.store(0U, std::memory_order_relaxed);
        entries_[i].subscribed.store(0U, std::memory_order_relaxed);

        entries_[i].name_map.can_id = 0;
        entries_[i].name_map.name[0] = '\0';
        entries_[i].name_map.index = static_cast<uint16_t>(i);

        const DbcSignalDef* signal = dbc.signalAt(i);
        if (signal != nullptr) {
            entries_[i].name_map.can_id = signal->can_id;
            std::strncpy(entries_[i].name_map.name, signal->name, sizeof(entries_[i].name_map.name) - 1U);
            entries_[i].name_map.name[sizeof(entries_[i].name_map.name) - 1U] = '\0';
        }
    }
    return true;
}

void SignalCache::swap(SignalCache& other) {
    RuntimeEntry* const entries = entries_;
    entries_ = other.entries_;
    other.entries_ = entries;

    const size_t capacity = capacity_;
    capacity_ = other.capacity_;
    other.capacity_ = capacity;

    const uint16_t signal_count = signal_count_.load(std::memory_order_relaxed);
    signal_count_.store(other.signal_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.signal_count_.store(signal_count, std::memory_order_relaxed);

    const uint8_t decode_all = decode_all_.load(std::memory_order_relaxed);
    decode_all_.store(other.decode_all_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other.decode_all_.store(decode_all, std::memory_order_relaxed);
}

void SignalCache::releaseEntries() {
    if (entries_ != nullptr) {
        for (size_t i = 0; i < capacity_; ++i) entries_[i].~RuntimeEntry();
        freeRuntimeMemory(entries_);
    }
    entries_ = nullptr;
    capacity_ = 0U;
    signal_count_.store(0U, std::memory_order_relaxed);
}

void SignalCache::clearSubscriptions() {
    decode_all_.store(0U, std::memory_order_release);
    const size_t count = signal_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        entries_[i].subscribed.store(0U, std::memory_order_relaxed);
    }
}

void SignalCache::setDecodeAll(bool enabled) {
    decode_all_.store(enabled ? 1U : 0U, std::memory_order_release);
}

bool SignalCache::decodeAll() const {
    return decode_all_.load(std::memory_order_acquire) != 0U;
}

bool SignalCache::subscribeSignal(uint16_t signal_index, bool enabled) {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }

    entries_[signal_index].subscribed.store(enabled ? 1U : 0U, std::memory_order_release);
    return true;
}

bool SignalCache::isSignalSubscribed(uint16_t signal_index) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }
    return entries_[signal_index].subscribed.load(std::memory_order_acquire) != 0U;
}

size_t SignalCache::decodeObservedFrame(const DbcDatabase& dbc, const CanFrame& frame) {
    const DbcMessageDef* message = dbc.findMessage(frame.id);
    if (message == nullptr || message->signal_count == 0U) {
        return 0U;
    }

    const bool decode_all = decodeAll();
    const size_t signal_limit = signal_count_.load(std::memory_order_acquire);
    size_t updated = 0U;

    for (uint16_t i = 0; i < message->signal_count; ++i) {
        const size_t signal_index = static_cast<size_t>(message->signal_start) + i;
        if (signal_index >= signal_limit) {
            break;
        }

        if (!decode_all && entries_[signal_index].subscribed.load(std::memory_order_relaxed) == 0U) {
            continue;
        }

        const DbcSignalDef* signal = dbc.signalAt(signal_index);
        if (signal == nullptr) {
            continue;
        }

        float value = 0.0F;
        if (!decodeSignal(frame, *signal, value)) {
            continue;
        }

        RuntimeEntry& entry = entries_[signal_index];
        // Single-writer sequence publication keeps value, generation,
        // direction, timestamp, and validity coherent for cross-core readers.
        // The gateway CAN task is the sole decoder owner (physical and replay).
        entry.observation_sequence.fetch_add(1U, std::memory_order_acq_rel);

        const uint32_t bits = floatToBits(value);
        const uint32_t previous = entry.value_bits.load(std::memory_order_relaxed);
        if (previous != bits) {
            entry.value_bits.store(bits, std::memory_order_relaxed);
            entry.generation.fetch_add(1U, std::memory_order_relaxed);
            ++updated;
        }

        // Publish observation metadata for every successfully decoded frame, even
        // when the decoded value (and therefore its generation) did not change.
        entry.direction.store(static_cast<uint8_t>(frame.direction), std::memory_order_relaxed);
        entry.timestamp_us.store(frame.timestamp_us, std::memory_order_relaxed);
        entry.valid.store(1U, std::memory_order_relaxed);
        entry.observation_sequence.fetch_add(1U, std::memory_order_release);
    }

    return updated;
}

void SignalCache::readRuntimeEntry(
    const RuntimeEntry& entry,
    uint32_t& out_value_bits,
    uint32_t& out_generation,
    uint32_t& out_timestamp_us,
    Direction& out_direction,
    bool& out_valid) {

    for (;;) {
        const uint32_t before = entry.observation_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;

        const uint32_t value_bits = entry.value_bits.load(std::memory_order_relaxed);
        const uint32_t generation = entry.generation.load(std::memory_order_relaxed);
        const uint32_t timestamp_us = entry.timestamp_us.load(std::memory_order_relaxed);
        const Direction direction = static_cast<Direction>(
            entry.direction.load(std::memory_order_relaxed));
        const bool valid = entry.valid.load(std::memory_order_relaxed) != 0U;

        const uint32_t after = entry.observation_sequence.load(std::memory_order_acquire);
        if (before != after || (after & 1U) != 0U) continue;

        out_value_bits = value_bits;
        out_generation = generation;
        out_timestamp_us = timestamp_us;
        out_direction = direction;
        out_valid = valid;
        return;
    }
}

bool SignalCache::readSignal(uint16_t signal_index, float& out_value, uint32_t& out_generation, bool& out_valid) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }

    uint32_t bits = 0U;
    uint32_t timestamp_us = 0U;
    Direction direction = Direction::A_TO_B;
    readRuntimeEntry(entries_[signal_index], bits, out_generation, timestamp_us, direction, out_valid);
    out_value = bitsToFloat(bits);
    return true;
}

bool SignalCache::readSignalState(
    uint16_t signal_index,
    float& out_value,
    uint32_t& out_generation,
    bool& out_valid,
    uint32_t& out_timestamp_us,
    Direction& out_direction) const {

    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }

    uint32_t bits = 0U;
    readRuntimeEntry(entries_[signal_index], bits, out_generation, out_timestamp_us,
                     out_direction, out_valid);
    out_value = bitsToFloat(bits);
    return true;
}

bool SignalCache::signalCanId(uint16_t signal_index, uint32_t& out_can_id) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }
    out_can_id = entries_[signal_index].name_map.can_id;
    return true;
}

size_t SignalCache::snapshotByIndexes(
    const uint16_t* signal_indexes,
    size_t signal_count,
    SignalCacheSnapshot* out_entries,
    size_t out_capacity) const {

    if (out_entries == nullptr || out_capacity == 0U) {
        return 0U;
    }

    const size_t total = this->signal_count_.load(std::memory_order_acquire);
    size_t out_count = 0U;

    if (signal_indexes == nullptr || signal_count == 0U) {
        const size_t count = (total < out_capacity) ? total : out_capacity;
        for (size_t i = 0; i < count; ++i) {
            SignalCacheSnapshot& entry = out_entries[out_count++];
            entry.index = static_cast<uint16_t>(i);
            entry.can_id = entries_[i].name_map.can_id;
            std::strncpy(entry.name, entries_[i].name_map.name, sizeof(entry.name) - 1U);
            entry.name[sizeof(entry.name) - 1U] = '\0';

            uint32_t bits = 0U;
            readRuntimeEntry(entries_[i], bits, entry.generation, entry.timestamp_us,
                             entry.direction, entry.valid);
            entry.value = bitsToFloat(bits);
            entry.subscribed = entries_[i].subscribed.load(std::memory_order_acquire) != 0U;
        }
        return out_count;
    }

    for (size_t i = 0; i < signal_count && out_count < out_capacity; ++i) {
        const uint16_t index = signal_indexes[i];
        if (index >= total) {
            continue;
        }

        SignalCacheSnapshot& entry = out_entries[out_count++];
        entry.index = index;
        entry.can_id = entries_[index].name_map.can_id;
        std::strncpy(entry.name, entries_[index].name_map.name, sizeof(entry.name) - 1U);
        entry.name[sizeof(entry.name) - 1U] = '\0';

        uint32_t bits = 0U;
        readRuntimeEntry(entries_[index], bits, entry.generation, entry.timestamp_us,
                         entry.direction, entry.valid);
        entry.value = bitsToFloat(bits);
        entry.subscribed = entries_[index].subscribed.load(std::memory_order_acquire) != 0U;
    }

    return out_count;
}

int32_t SignalCache::findSignalIndexByName(uint32_t can_id, const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }

    const size_t count = signal_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (entries_[i].name_map.can_id == can_id && std::strcmp(entries_[i].name_map.name, name) == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t SignalCache::findSignalIndexByName(const char* name) const {
    if (name == nullptr || name[0] == '\0') return -1;

    const size_t count = signal_count_.load(std::memory_order_acquire);
    int32_t match = -1;
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(entries_[i].name_map.name, name) != 0) continue;
        // A name-only binding must be unambiguous within the selected DBC.
        if (match >= 0) return -1;
        match = static_cast<int32_t>(i);
    }
    return match;
}

size_t SignalCache::signalCount() const {
    return signal_count_.load(std::memory_order_acquire);
}

}  // namespace bored::signalscope
