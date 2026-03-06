#include "persistence.hpp"

#include <cstring>

namespace bored::signalscope {

bool PersistenceStore::begin() {
    persisted_count_ = 0;
    replay_cursor_ = 0;
    replay_loop_counter_ = 0;
    std::memset(persisted_mutations_, 0, sizeof(persisted_mutations_));
    return true;
}

bool PersistenceStore::saveMutations(const SignalMutation* entries, size_t count) {
    if (entries == nullptr || count > kMaxPersistedMutations) {
        return false;
    }

    if (count > 0U) {
        std::memcpy(persisted_mutations_, entries, count * sizeof(SignalMutation));
    }
    persisted_count_ = count;
    return true;
}

size_t PersistenceStore::loadMutations(SignalMutation* out_entries, size_t capacity) const {
    if (out_entries == nullptr || capacity == 0U) {
        return 0;
    }

    const size_t count = (persisted_count_ < capacity) ? persisted_count_ : capacity;
    if (count > 0U) {
        std::memcpy(out_entries, persisted_mutations_, count * sizeof(SignalMutation));
    }
    return count;
}

bool PersistenceStore::saveReplayState(size_t cursor, uint32_t loop_counter) {
    replay_cursor_ = cursor;
    replay_loop_counter_ = loop_counter;
    return true;
}

bool PersistenceStore::loadReplayState(size_t& out_cursor, uint32_t& out_loop_counter) const {
    out_cursor = replay_cursor_;
    out_loop_counter = replay_loop_counter_;
    return true;
}

}  // namespace bored::signalscope
