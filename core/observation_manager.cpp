#include "observation_manager.hpp"

#include <cstring>

namespace bored::signalscope {

void ObservationManager::init() {
    mode_.store(ObservationMode::NONE, std::memory_order_relaxed);
    clearTable(specific_tables_[0]);
    clearTable(specific_tables_[1]);
    active_specific_index_ = 0;
    active_specific_.store(&specific_tables_[active_specific_index_], std::memory_order_release);
}

ObservationMode ObservationManager::mode() const {
    return mode_.load(std::memory_order_acquire);
}

void ObservationManager::setMode(ObservationMode mode) {
    mode_.store(mode, std::memory_order_release);
}

bool ObservationManager::isObserved(uint32_t can_id, Direction direction) const {
    const ObservationMode mode = mode_.load(std::memory_order_acquire);
    if (mode == ObservationMode::ALL) {
        return true;
    }
    if (mode == ObservationMode::NONE) {
        return false;
    }

    const SpecificTable* table = activeSpecificTable();
    return (table != nullptr) && contains(*table, can_id, direction);
}

bool ObservationManager::setSpecific(const ObservationKey* keys, size_t count) {
    if (keys == nullptr && count > 0U) {
        return false;
    }
    if (count > kMaxSpecificKeys) {
        return false;
    }

    SpecificTable* next = inactiveSpecificTable();
    if (next == nullptr) {
        return false;
    }

    if (!buildTable(*next, keys, count)) {
        return false;
    }

    swapActiveSpecificTable();
    mode_.store((count > 0U) ? ObservationMode::SPECIFIC : ObservationMode::NONE, std::memory_order_release);
    return true;
}

bool ObservationManager::addSpecific(uint32_t can_id, Direction direction) {
    ObservationKey keys[kMaxSpecificKeys];
    const size_t count = snapshotSpecific(keys, kMaxSpecificKeys);
    if (count >= kMaxSpecificKeys) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (keys[i].can_id == can_id && keys[i].direction == direction) {
            mode_.store(ObservationMode::SPECIFIC, std::memory_order_release);
            return true;
        }
    }

    keys[count].can_id = can_id;
    keys[count].direction = direction;
    return setSpecific(keys, count + 1U);
}

bool ObservationManager::removeSpecific(uint32_t can_id, Direction direction) {
    ObservationKey keys[kMaxSpecificKeys];
    const size_t count = snapshotSpecific(keys, kMaxSpecificKeys);

    size_t write = 0U;
    bool removed = false;
    for (size_t i = 0; i < count; ++i) {
        if (keys[i].can_id == can_id && keys[i].direction == direction) {
            removed = true;
            continue;
        }
        keys[write++] = keys[i];
    }

    if (!removed) {
        return false;
    }

    return setSpecific(keys, write);
}

void ObservationManager::clearSpecific() {
    SpecificTable* next = inactiveSpecificTable();
    if (next == nullptr) {
        return;
    }

    clearTable(*next);
    swapActiveSpecificTable();
}

size_t ObservationManager::snapshotSpecific(ObservationKey* out_keys, size_t capacity) const {
    if (out_keys == nullptr || capacity == 0U) {
        return 0U;
    }

    const SpecificTable* table = activeSpecificTable();
    if (table == nullptr) {
        return 0U;
    }

    const size_t count = (table->count < capacity) ? table->count : capacity;
    for (size_t i = 0; i < count; ++i) {
        out_keys[i].can_id = table->entries[i].can_id;
        out_keys[i].direction = table->entries[i].direction;
    }
    return count;
}

uint32_t ObservationManager::keyHash(uint32_t can_id, Direction direction) {
    const uint32_t seed = can_id ^ (static_cast<uint32_t>(direction) * 0x9E3779B9U);
    return seed ^ (seed >> 16U);
}

bool ObservationManager::keyEquals(const ObservationKey& lhs, const ObservationKey& rhs) {
    return lhs.can_id == rhs.can_id && lhs.direction == rhs.direction;
}

void ObservationManager::clearTable(SpecificTable& table) {
    table.count = 0;
    for (size_t i = 0; i < kMaxSpecificKeys; ++i) {
        table.entries[i].can_id = 0;
        table.entries[i].direction = Direction::A_TO_B;
        table.entries[i].next = -1;
    }
    std::memset(table.bucket_head, 0xFF, sizeof(table.bucket_head));
}

bool ObservationManager::contains(const SpecificTable& table, uint32_t can_id, Direction direction) {
    const uint32_t bucket = keyHash(can_id, direction) % kBucketCount;
    int16_t idx = table.bucket_head[bucket];
    while (idx >= 0) {
        const SpecificEntry& entry = table.entries[idx];
        if (entry.can_id == can_id && entry.direction == direction) {
            return true;
        }
        idx = entry.next;
    }
    return false;
}

bool ObservationManager::buildTable(SpecificTable& table, const ObservationKey* keys, size_t count) {
    clearTable(table);
    if (count > kMaxSpecificKeys) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const ObservationKey key = keys[i];
        if (contains(table, key.can_id, key.direction)) {
            continue;
        }

        if (table.count >= kMaxSpecificKeys) {
            return false;
        }

        SpecificEntry& entry = table.entries[table.count];
        entry.can_id = key.can_id;
        entry.direction = key.direction;
        entry.next = -1;

        const uint32_t bucket = keyHash(key.can_id, key.direction) % kBucketCount;
        entry.next = table.bucket_head[bucket];
        table.bucket_head[bucket] = static_cast<int16_t>(table.count);
        ++table.count;
    }

    return true;
}

const ObservationManager::SpecificTable* ObservationManager::activeSpecificTable() const {
    return active_specific_.load(std::memory_order_acquire);
}

ObservationManager::SpecificTable* ObservationManager::inactiveSpecificTable() {
    const uint8_t next_index = (active_specific_index_ == 0U) ? 1U : 0U;
    return &specific_tables_[next_index];
}

void ObservationManager::swapActiveSpecificTable() {
    active_specific_index_ = (active_specific_index_ == 0U) ? 1U : 0U;
    active_specific_.store(&specific_tables_[active_specific_index_], std::memory_order_release);
}

}  // namespace bored::signalscope

