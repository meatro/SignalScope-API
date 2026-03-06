#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

enum class ObservationMode : uint8_t {
    NONE = 0,
    SPECIFIC = 1,
    ALL = 2,
};

struct ObservationKey {
    uint32_t can_id = 0;
    Direction direction = Direction::A_TO_B;
};

class ObservationManager {
public:
    static constexpr size_t kMaxSpecificKeys = 128;

    void init();

    ObservationMode mode() const;
    void setMode(ObservationMode mode);

    bool isObserved(uint32_t can_id, Direction direction) const;

    bool setSpecific(const ObservationKey* keys, size_t count);
    bool addSpecific(uint32_t can_id, Direction direction);
    bool removeSpecific(uint32_t can_id, Direction direction);
    void clearSpecific();

    size_t snapshotSpecific(ObservationKey* out_keys, size_t capacity) const;

private:
    static constexpr size_t kBucketCount = 64;

    struct SpecificEntry {
        uint32_t can_id = 0;
        Direction direction = Direction::A_TO_B;
        int16_t next = -1;
    };

    struct SpecificTable {
        size_t count = 0;
        SpecificEntry entries[kMaxSpecificKeys];
        int16_t bucket_head[kBucketCount];
    };

    static uint32_t keyHash(uint32_t can_id, Direction direction);
    static bool keyEquals(const ObservationKey& lhs, const ObservationKey& rhs);
    static void clearTable(SpecificTable& table);
    static bool contains(const SpecificTable& table, uint32_t can_id, Direction direction);
    static bool buildTable(SpecificTable& table, const ObservationKey* keys, size_t count);

    const SpecificTable* activeSpecificTable() const;
    SpecificTable* inactiveSpecificTable();
    void swapActiveSpecificTable();

    std::atomic<ObservationMode> mode_{ObservationMode::NONE};
    SpecificTable specific_tables_[2];
    std::atomic<const SpecificTable*> active_specific_{nullptr};
    uint8_t active_specific_index_ = 0;
};

}  // namespace bored::signalscope
