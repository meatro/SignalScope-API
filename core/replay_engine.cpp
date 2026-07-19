#include "replay_engine.hpp"

#include <cstdlib>
#include <cstring>

namespace bored::signalscope {

namespace {

char* trim(char* value) {
    if (value == nullptr) {
        return value;
    }

    while (*value == ' ' || *value == '\t') {
        ++value;
    }

    size_t len = std::strlen(value);
    while (len > 0U && (value[len - 1U] == ' ' || value[len - 1U] == '\t')) {
        value[len - 1U] = '\0';
        --len;
    }

    return value;
}

bool parseUnsignedToken(const char* text, int base, uint32_t maximum, uint32_t& output) {
    if (text == nullptr || text[0] == '\0' || text[0] == '-') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, base);
    if (end == text || *end != '\0' || value > maximum) return false;
    output = static_cast<uint32_t>(value);
    return true;
}

Direction parseDirection(const char* token, Direction fallback) {
    if (token == nullptr) {
        return fallback;
    }

    if (std::strcmp(token, "A_TO_B") == 0) {
        return Direction::A_TO_B;
    }
    if (std::strcmp(token, "B_TO_A") == 0) {
        return Direction::B_TO_A;
    }

    return fallback;
}

}  // namespace

void ReplayEngine::init() {
    frame_count_ = 0;
    cursor_ = 0;
    playing_ = false;
    loop_mode_ = ReplayLoopMode::PLAY_ONCE;
    next_due_us_ = 0;
    loop_counter_ = 0;
    dispatch_mode_ = ReplayDispatchMode::PHYSICAL;
}

bool ReplayEngine::loadLogCsv(const char* text, size_t length, Direction default_direction,
                              ReplayDispatchMode dispatch_mode) {
    frame_count_ = 0;
    cursor_ = 0;
    dispatch_mode_ = dispatch_mode;

    if (text == nullptr || length == 0U) {
        return false;
    }

    char line[256] = {0};
    size_t line_len = 0;
    uint32_t previous_ts = 0;
    bool first_line = true;
    bool success = true;

    for (size_t i = 0; i <= length; ++i) {
        const char c = (i < length) ? text[i] : '\n';
        if (c == '\r') {
            continue;
        }

        if (c != '\n') {
            if (line_len + 1U < sizeof(line)) {
                line[line_len++] = c;
            }
            continue;
        }

        line[line_len] = '\0';
        line_len = 0;

        if (line[0] == '\0') {
            continue;
        }

        if (frame_count_ >= kMaxReplayFrames) {
            success = false;
            continue;
        }

        ReplayFrame frame;
        if (!parseLogLine(line, previous_ts, default_direction, frame)) {
            success = false;
            continue;
        }

        if (first_line) {
            frame.delta_us = 0;
            first_line = false;
        }

        previous_ts = frame.frame.timestamp_us;
        frames_[frame_count_] = frame;
        ++frame_count_;
    }

    return success && frame_count_ > 0U;
}

void ReplayEngine::setTxCallback(TxCallback callback) {
    tx_callback_ = callback;
}

void ReplayEngine::start(ReplayLoopMode mode, uint32_t now_us) {
    if (frame_count_ == 0U) {
        return;
    }

    loop_mode_ = mode;
    cursor_ = 0;
    playing_ = true;
    next_due_us_ = now_us;
}

void ReplayEngine::start(ReplayLoopMode mode, uint32_t now_us, ReplayDispatchMode dispatch_mode) {
    dispatch_mode_ = dispatch_mode;
    start(mode, now_us);
}

void ReplayEngine::stop() {
    playing_ = false;
}

void ReplayEngine::tick(uint32_t now_us) {
    if (!playing_ || frame_count_ == 0U) {
        return;
    }

    size_t dispatched = 0U;
    while (playing_ && now_us >= next_due_us_ && dispatched < kMaxFramesPerTick) {
        const ReplayFrame& replay_frame = frames_[cursor_];
        bool accepted = true;
        if (tx_callback_ != nullptr) {
            accepted = tx_callback_(replay_frame.frame, dispatch_mode_);
        }

        // A refused replay frame is intentionally consumed rather than retried:
        // the gateway may already have advanced a stateful counter/checksum
        // mutation before learning that the hardware mailbox is busy. Replaying
        // it would apply that mutation twice. Physical frames are never handled
        // here and instead remain in GatewayCore's lossless egress queues.
        ++cursor_;
        ++dispatched;
        if (cursor_ >= frame_count_) {
            if (loop_mode_ == ReplayLoopMode::PLAY_ONCE) {
                playing_ = false;
                return;
            }

            cursor_ = 0;
            if (loop_mode_ == ReplayLoopMode::LOOP_WITH_COUNTER_CONTINUATION) {
                ++loop_counter_;
            }
        }

        scheduleNext(now_us);
        if (!accepted) return;
    }
}

bool ReplayEngine::isPlaying() const {
    return playing_;
}

size_t ReplayEngine::frameCount() const {
    return frame_count_;
}

size_t ReplayEngine::cursor() const {
    return cursor_;
}

ReplayDispatchMode ReplayEngine::dispatchMode() const {
    return dispatch_mode_;
}

bool ReplayEngine::isDryRun() const {
    return dispatch_mode_ == ReplayDispatchMode::DRY_RUN;
}

bool ReplayEngine::parseLogLine(const char* line, uint32_t previous_ts, Direction default_direction, ReplayFrame& out_frame) {
    char copy[256] = {0};
    std::strncpy(copy, line, sizeof(copy) - 1U);

    char* tokens[13] = {nullptr};
    size_t token_count = 0;

    char* context = nullptr;
    char* token = strtok_r(copy, ",", &context);
    while (token != nullptr && token_count < 13U) {
        tokens[token_count++] = trim(token);
        token = strtok_r(nullptr, ",", &context);
    }

    if (token_count < 11U) {
        return false;
    }

    uint32_t timestamp_us = 0U;
    uint32_t can_id = 0U;
    uint32_t dlc_value = 0U;
    if (!parseUnsignedToken(tokens[0], 10, UINT32_MAX, timestamp_us) ||
        !parseUnsignedToken(tokens[1], 0, 0x1FFFFFFFU, can_id) ||
        !parseUnsignedToken(tokens[2], 10, 8U, dlc_value)) return false;
    const uint8_t dlc = static_cast<uint8_t>(dlc_value);

    CanFrame frame;
    frame.timestamp_us = timestamp_us;
    frame.id = can_id;
    frame.dlc = dlc;
    frame.direction = (token_count >= 12U) ? parseDirection(tokens[11], default_direction) : default_direction;

    for (uint8_t i = 0; i < 8U; ++i) {
        uint32_t byte_value = 0U;
        if (!parseUnsignedToken(tokens[3U + i], 16, 0xFFU, byte_value)) return false;
        frame.data[i] = static_cast<uint8_t>(byte_value);
    }

    out_frame.frame = frame;
    out_frame.delta_us = timestamp_us - previous_ts;
    return true;
}

void ReplayEngine::scheduleNext(uint32_t now_us) {
    next_due_us_ = now_us + frames_[cursor_].delta_us;
}

}  // namespace bored::signalscope
