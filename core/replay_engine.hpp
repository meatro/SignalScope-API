#pragma once

#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

enum class ReplayLoopMode : uint8_t {
    PLAY_ONCE = 0,
    LOOP_RAW = 1,
    LOOP_WITH_COUNTER_CONTINUATION = 2,
};

struct ReplayFrame {
    CanFrame frame{};
    uint32_t delta_us = 0;
};

class ReplayEngine {
public:
    using TxCallback = bool (*)(const CanFrame& frame);

    static constexpr size_t kMaxReplayFrames = 1024;

    void init();
    bool loadLogCsv(const char* text, size_t length, Direction default_direction);

    void setTxCallback(TxCallback callback);

    void start(ReplayLoopMode mode, uint32_t now_us);
    void stop();
    void tick(uint32_t now_us);

    bool isPlaying() const;
    size_t frameCount() const;
    size_t cursor() const;

private:
    bool parseLogLine(const char* line, uint32_t previous_ts, Direction default_direction, ReplayFrame& out_frame);
    void scheduleNext(uint32_t now_us);

    ReplayFrame frames_[kMaxReplayFrames];
    size_t frame_count_ = 0;
    size_t cursor_ = 0;

    bool playing_ = false;
    ReplayLoopMode loop_mode_ = ReplayLoopMode::PLAY_ONCE;
    uint32_t next_due_us_ = 0;
    uint32_t loop_counter_ = 0;

    TxCallback tx_callback_ = nullptr;
};

}  // namespace bored::signalscope
