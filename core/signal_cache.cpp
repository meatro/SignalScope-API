#include "signal_cache.hpp"

#include <cstring>

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

void SignalCache::init() {
    signal_count_.store(0, std::memory_order_relaxed);
    decode_all_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kMaxSignals; ++i) {
        value_bits_[i].store(floatToBits(0.0F), std::memory_order_relaxed);
        generation_[i].store(0U, std::memory_order_relaxed);
        valid_[i].store(0U, std::memory_order_relaxed);
        subscribed_[i].store(0U, std::memory_order_relaxed);
        name_map_[i].can_id = 0;
        name_map_[i].name[0] = '\0';
        name_map_[i].index = static_cast<uint16_t>(i);
    }
}

void SignalCache::resetForDbc(const DbcDatabase& dbc) {
    const size_t dbc_signal_count = dbc.signalCount();
    const size_t bounded_count = (dbc_signal_count > kMaxSignals) ? kMaxSignals : dbc_signal_count;

    signal_count_.store(static_cast<uint16_t>(bounded_count), std::memory_order_release);
    decode_all_.store(0U, std::memory_order_release);

    for (size_t i = 0; i < kMaxSignals; ++i) {
        value_bits_[i].store(floatToBits(0.0F), std::memory_order_relaxed);
        generation_[i].store(0U, std::memory_order_relaxed);
        valid_[i].store(0U, std::memory_order_relaxed);
        subscribed_[i].store(0U, std::memory_order_relaxed);

        name_map_[i].can_id = 0;
        name_map_[i].name[0] = '\0';
        name_map_[i].index = static_cast<uint16_t>(i);

        if (i < bounded_count) {
            const DbcSignalDef* signal = dbc.signalAt(i);
            if (signal != nullptr) {
                name_map_[i].can_id = signal->can_id;
                std::strncpy(name_map_[i].name, signal->name, sizeof(name_map_[i].name) - 1U);
                name_map_[i].name[sizeof(name_map_[i].name) - 1U] = '\0';
            }
        }
    }
}

void SignalCache::clearSubscriptions() {
    decode_all_.store(0U, std::memory_order_release);
    const size_t count = signal_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        subscribed_[i].store(0U, std::memory_order_relaxed);
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

    subscribed_[signal_index].store(enabled ? 1U : 0U, std::memory_order_release);
    return true;
}

bool SignalCache::isSignalSubscribed(uint16_t signal_index) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }
    return subscribed_[signal_index].load(std::memory_order_acquire) != 0U;
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

        if (!decode_all && subscribed_[signal_index].load(std::memory_order_relaxed) == 0U) {
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

        const uint32_t bits = floatToBits(value);
        const uint32_t previous = value_bits_[signal_index].load(std::memory_order_relaxed);
        if (previous != bits) {
            value_bits_[signal_index].store(bits, std::memory_order_relaxed);
            generation_[signal_index].fetch_add(1U, std::memory_order_release);
            ++updated;
        }

        valid_[signal_index].store(1U, std::memory_order_relaxed);
    }

    return updated;
}

bool SignalCache::readSignal(uint16_t signal_index, float& out_value, uint32_t& out_generation, bool& out_valid) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }

    const uint32_t bits = value_bits_[signal_index].load(std::memory_order_acquire);
    out_value = bitsToFloat(bits);
    out_generation = generation_[signal_index].load(std::memory_order_acquire);
    out_valid = valid_[signal_index].load(std::memory_order_acquire) != 0U;
    return true;
}

bool SignalCache::signalCanId(uint16_t signal_index, uint32_t& out_can_id) const {
    const size_t count = signal_count_.load(std::memory_order_acquire);
    if (signal_index >= count) {
        return false;
    }
    out_can_id = name_map_[signal_index].can_id;
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
            entry.can_id = name_map_[i].can_id;
            std::strncpy(entry.name, name_map_[i].name, sizeof(entry.name) - 1U);
            entry.name[sizeof(entry.name) - 1U] = '\0';

            entry.value = bitsToFloat(value_bits_[i].load(std::memory_order_acquire));
            entry.generation = generation_[i].load(std::memory_order_acquire);
            entry.valid = valid_[i].load(std::memory_order_acquire) != 0U;
            entry.subscribed = subscribed_[i].load(std::memory_order_acquire) != 0U;
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
        entry.can_id = name_map_[index].can_id;
        std::strncpy(entry.name, name_map_[index].name, sizeof(entry.name) - 1U);
        entry.name[sizeof(entry.name) - 1U] = '\0';

        entry.value = bitsToFloat(value_bits_[index].load(std::memory_order_acquire));
        entry.generation = generation_[index].load(std::memory_order_acquire);
        entry.valid = valid_[index].load(std::memory_order_acquire) != 0U;
        entry.subscribed = subscribed_[index].load(std::memory_order_acquire) != 0U;
    }

    return out_count;
}

int32_t SignalCache::findSignalIndexByName(uint32_t can_id, const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }

    const size_t count = signal_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (name_map_[i].can_id == can_id && std::strcmp(name_map_[i].name, name) == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

size_t SignalCache::signalCount() const {
    return signal_count_.load(std::memory_order_acquire);
}

}  // namespace bored::signalscope
