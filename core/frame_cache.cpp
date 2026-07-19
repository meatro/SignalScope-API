#include "frame_cache.hpp"

#include <algorithm>
#include <cstring>

namespace bored::signalscope {

namespace {

struct TimestampDesc {
    bool operator()(const FrameCacheSnapshot& lhs, const FrameCacheSnapshot& rhs) const {
        return lhs.last_timestamp_us > rhs.last_timestamp_us;
    }
};

}  // namespace

void FrameCache::init() {
    cache_lock_.clear(std::memory_order_release);
    count_.store(0, std::memory_order_relaxed);
    eviction_cursor_ = 0U;
    for (size_t i = 0; i < kMaxEntries; ++i) {
        entries_[i].sequence.store(0, std::memory_order_relaxed);
        entries_[i].in_use.store(0, std::memory_order_relaxed);
        entries_[i].can_id = 0;
        entries_[i].direction = Direction::A_TO_B;
        entries_[i].dlc = 0;
        std::memset(entries_[i].data, 0, sizeof(entries_[i].data));
        std::memset(entries_[i].input_data, 0, sizeof(entries_[i].input_data));
        entries_[i].has_input = false;
        entries_[i].mutated = false;
        entries_[i].last_timestamp_us = 0;
        entries_[i].total_frames = 0;
        entries_[i].physical_available = false;
        entries_[i].physical_dlc = 0;
        std::memset(entries_[i].physical_data, 0, sizeof(entries_[i].physical_data));
        entries_[i].physical_last_timestamp_us = 0;
        entries_[i].physical_total_frames = 0;
        entries_[i].rate_sample_start_ms = 0;
        entries_[i].rate_sample_frames = 0;
        entries_[i].rate_hz = 0;
    }

    for (size_t i = 0; i < kRecentCapacity; ++i) {
        recent_[i] = {};
    }
    recent_head_.store(0, std::memory_order_relaxed);
    recent_count_.store(0, std::memory_order_relaxed);
}

void FrameCache::update(const CanFrame& frame, uint32_t now_ms, bool mutated,
                        const CanFrame* input_frame, bool synthetic) {
    if (!tryLockCache()) return;
    Entry* entry = findOrCreate(frame.id, frame.direction);
    if (entry == nullptr) {
        unlockCache();
        return;
    }

    entry->sequence.fetch_add(1U, std::memory_order_relaxed);

    entry->can_id = frame.id;
    entry->direction = frame.direction;
    entry->dlc = (frame.dlc <= 8U) ? frame.dlc : 8U;
    std::memcpy(entry->data, frame.data, sizeof(entry->data));
    if (input_frame != nullptr) {
        std::memcpy(entry->input_data, input_frame->data, sizeof(entry->input_data));
        entry->has_input = true;
    } else {
        std::memcpy(entry->input_data, frame.data, sizeof(entry->input_data));
        entry->has_input = false;
    }
    entry->mutated = mutated;
    entry->last_timestamp_us = frame.timestamp_us;

    if (!synthetic) {
        const CanFrame& physical = input_frame == nullptr ? frame : *input_frame;
        entry->physical_available = true;
        entry->physical_dlc = physical.dlc <= 8U ? physical.dlc : 8U;
        std::memcpy(entry->physical_data, physical.data, sizeof(entry->physical_data));
        entry->physical_last_timestamp_us = physical.timestamp_us;
        ++entry->physical_total_frames;
    }

    ++entry->total_frames;
    ++entry->rate_sample_frames;

    if (entry->rate_sample_start_ms == 0U) {
        entry->rate_sample_start_ms = now_ms;
    }

    const uint32_t elapsed_ms = now_ms - entry->rate_sample_start_ms;
    if (elapsed_ms >= 1000U) {
        const uint32_t hz = (elapsed_ms == 0U)
            ? 0U
            : ((entry->rate_sample_frames * 1000U) / elapsed_ms);
        entry->rate_hz = static_cast<uint16_t>((hz > 0xFFFFU) ? 0xFFFFU : hz);
        entry->rate_sample_frames = 0;
        entry->rate_sample_start_ms = now_ms;
    }

    FrameCacheSnapshot event{};
    event.can_id = entry->can_id;
    event.direction = entry->direction;
    event.dlc = entry->dlc;
    std::memcpy(event.data, entry->data, sizeof(event.data));
    std::memcpy(event.input_data, entry->input_data, sizeof(event.input_data));
    event.has_input = entry->has_input;
    event.mutated = entry->mutated;
    event.last_timestamp_us = entry->last_timestamp_us;
    event.rate_hz = entry->rate_hz;
    event.total_frames = entry->total_frames;

    const uint16_t slot = recent_head_.load(std::memory_order_relaxed);
    recent_[slot] = event;

    const uint16_t next = static_cast<uint16_t>((slot + 1U) % kRecentCapacity);
    recent_head_.store(next, std::memory_order_release);

    const uint16_t cur_count = recent_count_.load(std::memory_order_relaxed);
    if (cur_count < kRecentCapacity) {
        recent_count_.store(static_cast<uint16_t>(cur_count + 1U), std::memory_order_release);
    }

    entry->sequence.fetch_add(1U, std::memory_order_release);
    unlockCache();
}

size_t FrameCache::snapshot(FrameCacheSnapshot* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0U;
    }

    if (!tryLockCache()) return 0U;
    size_t out_count = 0U;
    for (size_t i = 0; i < kMaxEntries && out_count < capacity; ++i) {
        const Entry* entry = at(i);
        if (entry == nullptr || entry->in_use.load(std::memory_order_acquire) == 0U) {
            continue;
        }

        FrameCacheSnapshot snap{};
        snap.can_id = entry->can_id;
        snap.direction = entry->direction;
        snap.dlc = entry->dlc;
        std::memcpy(snap.data, entry->data, sizeof(snap.data));
        std::memcpy(snap.input_data, entry->input_data, sizeof(snap.input_data));
        snap.has_input = entry->has_input;
        snap.mutated = entry->mutated;
        snap.last_timestamp_us = entry->last_timestamp_us;
        snap.rate_hz = entry->rate_hz;
        snap.total_frames = entry->total_frames;

        out_entries[out_count++] = snap;
    }
    unlockCache();

    std::sort(out_entries, out_entries + out_count, TimestampDesc{});
    return out_count;
}

size_t FrameCache::snapshotDirection(Direction direction, FrameCacheSnapshot* out_entries,
                                     size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0U;
    }

    if (!tryLockCache()) return 0U;
    size_t out_count = 0U;
    for (size_t i = 0; i < kMaxEntries && out_count < capacity; ++i) {
        const Entry* entry = at(i);
        if (entry == nullptr || entry->in_use.load(std::memory_order_acquire) == 0U ||
            entry->direction != direction) {
            continue;
        }

        FrameCacheSnapshot snap{};
        snap.can_id = entry->can_id;
        snap.direction = entry->direction;
        snap.dlc = entry->dlc;
        std::memcpy(snap.data, entry->data, sizeof(snap.data));
        std::memcpy(snap.input_data, entry->input_data, sizeof(snap.input_data));
        snap.has_input = entry->has_input;
        snap.mutated = entry->mutated;
        snap.last_timestamp_us = entry->last_timestamp_us;
        snap.rate_hz = entry->rate_hz;
        snap.total_frames = entry->total_frames;
        out_entries[out_count++] = snap;
    }
    unlockCache();

    std::sort(out_entries, out_entries + out_count, TimestampDesc{});
    return out_count;
}

bool FrameCache::read(uint32_t can_id, Direction direction, FrameCacheSnapshot* out_entry) const {
    if (out_entry == nullptr) {
        return false;
    }

    if (!tryLockCache()) return false;
    const uint32_t base = hashKey(can_id, direction) % kMaxEntries;
    for (size_t probe = 0; probe < kMaxEntries; ++probe) {
        const Entry* entry = at((base + probe) % kMaxEntries);
        if (entry == nullptr || entry->in_use.load(std::memory_order_acquire) == 0U) {
            continue;
        }
        if (entry->can_id != can_id || entry->direction != direction) {
            continue;
        }

        FrameCacheSnapshot snap{};
        snap.can_id = entry->can_id;
        snap.direction = entry->direction;
        snap.dlc = entry->dlc;
        std::memcpy(snap.data, entry->data, sizeof(snap.data));
        std::memcpy(snap.input_data, entry->input_data, sizeof(snap.input_data));
        snap.has_input = entry->has_input;
        snap.mutated = entry->mutated;
        snap.last_timestamp_us = entry->last_timestamp_us;
        snap.rate_hz = entry->rate_hz;
        snap.total_frames = entry->total_frames;
        *out_entry = snap;
        unlockCache();
        return true;
    }

    unlockCache();
    return false;
}

bool FrameCache::readPhysical(uint32_t can_id, Direction direction,
                              FrameCacheSnapshot* out_entry) const {
    if (out_entry == nullptr) return false;

    if (!tryLockCache()) return false;
    const uint32_t base = hashKey(can_id, direction) % kMaxEntries;
    for (size_t probe = 0; probe < kMaxEntries; ++probe) {
        const Entry* entry = at((base + probe) % kMaxEntries);
        if (entry == nullptr || entry->in_use.load(std::memory_order_acquire) == 0U) continue;
        if (entry->can_id != can_id || entry->direction != direction) continue;
        if (!entry->physical_available) {
            unlockCache();
            return false;
        }

        FrameCacheSnapshot snap{};
        snap.can_id = entry->can_id;
        snap.direction = entry->direction;
        snap.dlc = entry->physical_dlc;
        std::memcpy(snap.data, entry->physical_data, sizeof(snap.data));
        std::memcpy(snap.input_data, entry->physical_data, sizeof(snap.input_data));
        snap.has_input = false;
        snap.mutated = false;
        snap.last_timestamp_us = entry->physical_last_timestamp_us;
        snap.total_frames = entry->physical_total_frames;
        *out_entry = snap;
        unlockCache();
        return true;
    }

    unlockCache();
    return false;
}

size_t FrameCache::snapshotRecent(FrameCacheSnapshot* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0U;
    }

    if (!tryLockCache()) return 0U;
    const uint16_t count = recent_count_.load(std::memory_order_acquire);
    const uint16_t head = recent_head_.load(std::memory_order_acquire);
    const size_t take = (count < capacity) ? count : capacity;

    for (size_t i = 0; i < take; ++i) {
        const int32_t idx = static_cast<int32_t>(head) - 1 - static_cast<int32_t>(i);
        const uint16_t slot = static_cast<uint16_t>((idx < 0)
            ? (idx + static_cast<int32_t>(kRecentCapacity))
            : idx);
        out_entries[i] = recent_[slot];
    }

    unlockCache();
    return take;
}

size_t FrameCache::snapshotMutated(FrameCacheSnapshot* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0U;
    }

    if (!tryLockCache()) return 0U;
    size_t out_count = 0U;
    for (size_t i = 0; i < kMaxEntries && out_count < capacity; ++i) {
        const Entry* entry = at(i);
        if (entry == nullptr || entry->in_use.load(std::memory_order_acquire) == 0U ||
            !entry->mutated) {
            continue;
        }

        FrameCacheSnapshot snap{};
        snap.can_id = entry->can_id;
        snap.direction = entry->direction;
        snap.dlc = entry->dlc;
        std::memcpy(snap.data, entry->data, sizeof(snap.data));
        std::memcpy(snap.input_data, entry->input_data, sizeof(snap.input_data));
        snap.has_input = entry->has_input;
        snap.mutated = true;
        snap.last_timestamp_us = entry->last_timestamp_us;
        snap.rate_hz = entry->rate_hz;
        snap.total_frames = entry->total_frames;
        out_entries[out_count++] = snap;
    }
    unlockCache();

    std::sort(out_entries, out_entries + out_count, TimestampDesc{});
    return out_count;
}

uint32_t FrameCache::hashKey(uint32_t can_id, Direction direction) {
    const uint32_t seed = can_id ^ (static_cast<uint32_t>(direction) * 0x9E3779B9U);
    return seed ^ (seed >> 16U);
}

FrameCache::Entry* FrameCache::findOrCreate(uint32_t can_id, Direction direction) {
    const uint32_t base = hashKey(can_id, direction) % kMaxEntries;

    for (size_t probe = 0; probe < kMaxEntries; ++probe) {
        const size_t index = (base + probe) % kMaxEntries;
        Entry& entry = entries_[index];

        if (entry.in_use.load(std::memory_order_acquire) == 0U) {
            entry.can_id = can_id;
            entry.direction = direction;
            entry.dlc = 0;
            std::memset(entry.data, 0, sizeof(entry.data));
            std::memset(entry.input_data, 0, sizeof(entry.input_data));
            entry.has_input = false;
            entry.mutated = false;
            entry.last_timestamp_us = 0;
            entry.total_frames = 0;
            entry.physical_available = false;
            entry.physical_dlc = 0;
            std::memset(entry.physical_data, 0, sizeof(entry.physical_data));
            entry.physical_last_timestamp_us = 0;
            entry.physical_total_frames = 0;
            entry.rate_sample_start_ms = 0;
            entry.rate_sample_frames = 0;
            entry.rate_hz = 0;
            entry.sequence.store(0, std::memory_order_relaxed);
            entry.in_use.store(1U, std::memory_order_release);

            const uint16_t old_count = count_.load(std::memory_order_relaxed);
            if (old_count < kMaxEntries) {
                count_.store(static_cast<uint16_t>(old_count + 1U), std::memory_order_relaxed);
            }

            return &entry;
        }

        if (entry.can_id == can_id && entry.direction == direction) {
            return &entry;
        }
    }

    // The cache is intentionally bounded. Reuse entries round-robin so a
    // noisy bus cannot permanently prevent a later application-critical ID
    // (such as device feedback) from ever becoming visible.
    Entry& entry = entries_[eviction_cursor_];
    eviction_cursor_ = static_cast<uint16_t>((eviction_cursor_ + 1U) % kMaxEntries);
    entry.in_use.store(0U, std::memory_order_relaxed);
    entry.can_id = can_id;
    entry.direction = direction;
    entry.dlc = 0;
    std::memset(entry.data, 0, sizeof(entry.data));
    std::memset(entry.input_data, 0, sizeof(entry.input_data));
    entry.has_input = false;
    entry.mutated = false;
    entry.last_timestamp_us = 0;
    entry.total_frames = 0;
    entry.physical_available = false;
    entry.physical_dlc = 0;
    std::memset(entry.physical_data, 0, sizeof(entry.physical_data));
    entry.physical_last_timestamp_us = 0;
    entry.physical_total_frames = 0;
    entry.rate_sample_start_ms = 0;
    entry.rate_sample_frames = 0;
    entry.rate_hz = 0;
    entry.sequence.store(0U, std::memory_order_relaxed);
    entry.in_use.store(1U, std::memory_order_release);
    return &entry;
}

const FrameCache::Entry* FrameCache::at(size_t index) const {
    if (index >= kMaxEntries) {
        return nullptr;
    }
    return &entries_[index];
}

bool FrameCache::tryLockCache() const {
    // Keep the contention budget short enough that a cache read cannot become
    // a CAN-forwarding latency event. Missing one observational sample is safe;
    // monopolizing a core or deadlocking two same-core tasks is not.
    constexpr size_t kMaximumAttempts = 256U;
    for (size_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
        if (!cache_lock_.test_and_set(std::memory_order_acquire)) return true;
    }
    return false;
}

void FrameCache::unlockCache() const {
    cache_lock_.clear(std::memory_order_release);
}

}  // namespace bored::signalscope
