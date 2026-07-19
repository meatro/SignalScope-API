#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

struct FrameCacheSnapshot {
    uint32_t can_id = 0;
    Direction direction = Direction::A_TO_B;
    uint8_t dlc = 0;
    uint8_t data[8] = {0};
    uint8_t input_data[8] = {0};
    bool has_input = false;
    bool mutated = false;
    uint32_t last_timestamp_us = 0;
    uint16_t rate_hz = 0;
    uint32_t total_frames = 0;
};

class FrameCache {
public:
    // Keyed by (CAN id, direction). PQ's active matrix alone contains 239
    // message IDs, so the former 128-entry table could remain permanently
    // full and churn selected UI/application frames on an ordinary bus. A
    // 512-entry table keeps large real-world catalogs below a practical load
    // factor and makes lookup cheaper while preserving a hard memory bound.
    static constexpr size_t kMaxEntries = 512;
    static constexpr size_t kRecentCapacity = 256;

    void init();
    void update(const CanFrame& frame, uint32_t now_ms, bool mutated,
                const CanFrame* input_frame = nullptr, bool synthetic = false);

    // Snapshot keyed by (can_id, direction) identity.
    size_t snapshot(FrameCacheSnapshot* out_entries, size_t capacity) const;

    // Snapshot only identities received from one physical bus direction.
    size_t snapshotDirection(Direction direction, FrameCacheSnapshot* out_entries,
                             size_t capacity) const;

    // Read one direction-specific entry without walking/copying the full cache.
    bool read(uint32_t can_id, Direction direction, FrameCacheSnapshot* out_entry) const;

    // Read only the most recent physical-ingress sample for one identity.
    // Replay/synthetic updates remain visible through read() but can never
    // replace this provenance-preserving view.
    bool readPhysical(uint32_t can_id, Direction direction, FrameCacheSnapshot* out_entry) const;

    // Snapshot recent frame events (no identity collapsing), newest first.
    size_t snapshotRecent(FrameCacheSnapshot* out_entries, size_t capacity) const;

    // Snapshot only identities whose most recent forwarded frame was changed
    // by the active SignalScope rule package.
    size_t snapshotMutated(FrameCacheSnapshot* out_entries, size_t capacity) const;

private:
    struct Entry {
        std::atomic<uint32_t> sequence{0};
        std::atomic<uint8_t> in_use{0};

        uint32_t can_id = 0;
        Direction direction = Direction::A_TO_B;
        uint8_t dlc = 0;
        uint8_t data[8] = {0};
        uint8_t input_data[8] = {0};
        bool has_input = false;
        bool mutated = false;
        uint32_t last_timestamp_us = 0;
        uint32_t total_frames = 0;

        bool physical_available = false;
        uint8_t physical_dlc = 0;
        uint8_t physical_data[8] = {0};
        uint32_t physical_last_timestamp_us = 0;
        uint32_t physical_total_frames = 0;

        uint32_t rate_sample_start_ms = 0;
        uint32_t rate_sample_frames = 0;
        uint16_t rate_hz = 0;
    };

    static uint32_t hashKey(uint32_t can_id, Direction direction);
    Entry* findOrCreate(uint32_t can_id, Direction direction);
    const Entry* at(size_t index) const;
    // The cache is observational and must never stall physical CAN. A UI task
    // can be preempted while it owns the lock; an unbounded spin from the
    // higher-priority application task on the same core would then prevent the
    // owner from ever running again. Fail the cache operation after a short,
    // bounded contention window instead.
    bool tryLockCache() const;
    void unlockCache() const;

    Entry entries_[kMaxEntries];
    std::atomic<uint16_t> count_{0};
    uint16_t eviction_cursor_ = 0;

    FrameCacheSnapshot recent_[kRecentCapacity];
    std::atomic<uint16_t> recent_head_{0};
    std::atomic<uint16_t> recent_count_{0};
    mutable std::atomic_flag cache_lock_ = ATOMIC_FLAG_INIT;
};

}  // namespace bored::signalscope
