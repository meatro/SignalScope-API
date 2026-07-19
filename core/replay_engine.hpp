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

enum class ReplayDispatchMode : uint8_t {
    PHYSICAL = 0,
    DRY_RUN = 1,
};

struct ReplayFrame {
    CanFrame frame{};
    uint32_t delta_us = 0;
};

// Convert a stored/scheduled replay record into a live gateway ingress event.
// Keeping this pure preserves deterministic GatewayCore tests while ensuring
// production SignalCache freshness uses the current micros() time domain.
inline CanFrame liveReplayIngressFrame(const CanFrame& scheduled_frame, uint32_t now_us) {
    CanFrame live_frame = scheduled_frame;
    live_frame.timestamp_us = now_us;
    return live_frame;
}

class ReplayEngine {
public:
    using TxCallback = bool (*)(const CanFrame& frame, ReplayDispatchMode dispatch_mode);

    static constexpr size_t kMaxReplayFrames = 1024;
    static constexpr size_t kMaxFramesPerTick = 16;

    void init();
    bool loadLogCsv(const char* text, size_t length, Direction default_direction,
                    ReplayDispatchMode dispatch_mode = ReplayDispatchMode::PHYSICAL);

    void setTxCallback(TxCallback callback);

    void start(ReplayLoopMode mode, uint32_t now_us);
    void start(ReplayLoopMode mode, uint32_t now_us, ReplayDispatchMode dispatch_mode);
    void stop();
    void tick(uint32_t now_us);

    bool isPlaying() const;
    size_t frameCount() const;
    size_t cursor() const;
    ReplayDispatchMode dispatchMode() const;
    bool isDryRun() const;

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
    ReplayDispatchMode dispatch_mode_ = ReplayDispatchMode::PHYSICAL;

    TxCallback tx_callback_ = nullptr;
};

}  // namespace bored::signalscope
