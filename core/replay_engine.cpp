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
}

bool ReplayEngine::loadLogCsv(const char* text, size_t length, Direction default_direction) {
    frame_count_ = 0;
    cursor_ = 0;

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

void ReplayEngine::stop() {
    playing_ = false;
}

void ReplayEngine::tick(uint32_t now_us) {
    if (!playing_ || frame_count_ == 0U) {
        return;
    }

    while (playing_ && now_us >= next_due_us_) {
        const ReplayFrame& replay_frame = frames_[cursor_];
        if (tx_callback_ != nullptr) {
            tx_callback_(replay_frame.frame);
        }

        ++cursor_;
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

    const uint32_t timestamp_us = static_cast<uint32_t>(std::strtoul(tokens[0], nullptr, 10));
    const uint32_t can_id = static_cast<uint32_t>(std::strtoul(tokens[1], nullptr, 0));
    const uint8_t dlc = static_cast<uint8_t>(std::strtoul(tokens[2], nullptr, 10));
    if (dlc > 8U) {
        return false;
    }

    CanFrame frame;
    frame.timestamp_us = timestamp_us;
    frame.id = can_id;
    frame.dlc = dlc;
    frame.direction = (token_count >= 12U) ? parseDirection(tokens[11], default_direction) : default_direction;

    for (uint8_t i = 0; i < 8U; ++i) {
        frame.data[i] = static_cast<uint8_t>(std::strtoul(tokens[3U + i], nullptr, 16));
    }

    out_frame.frame = frame;
    out_frame.delta_us = timestamp_us - previous_ts;
    return true;
}

void ReplayEngine::scheduleNext(uint32_t now_us) {
    next_due_us_ = now_us + frames_[cursor_].delta_us;
}

}  // namespace bored::signalscope
