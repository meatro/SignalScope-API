#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/types.hpp"

namespace bored::signalscope {

class PersistenceStore {
public:
    static constexpr size_t kMaxPersistedMutations = 128;

    bool begin();

    bool saveMutations(const SignalMutation* entries, size_t count);
    size_t loadMutations(SignalMutation* out_entries, size_t capacity) const;

    bool saveReplayState(size_t cursor, uint32_t loop_counter);
    bool loadReplayState(size_t& out_cursor, uint32_t& out_loop_counter) const;

private:
    SignalMutation persisted_mutations_[kMaxPersistedMutations];
    size_t persisted_count_ = 0;
    size_t replay_cursor_ = 0;
    uint32_t replay_loop_counter_ = 0;
};

}  // namespace bored::signalscope
