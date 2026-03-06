#pragma once

#include <cstddef>
#include <cstdint>

namespace bored::signalscope {

enum class Direction : uint8_t {
    A_TO_B = 0,
    B_TO_A = 1,
};

enum class MutationOperation : uint8_t {
    PASS_THROUGH = 0,
    REPLACE = 1,
    ADD_OFFSET = 2,
    MULTIPLY = 3,
    CLAMP = 4,
};

struct CanFrame {
    uint32_t id = 0;
    uint8_t dlc = 0;
    uint8_t data[8] = {0};
    uint32_t timestamp_us = 0;
    Direction direction = Direction::A_TO_B;
};

struct SignalMutation {
    uint32_t can_id = 0;
    Direction direction = Direction::A_TO_B;
    uint16_t start_bit = 0;
    uint8_t length = 0;
    bool little_endian = true;
    bool is_signed = false;
    float factor = 1.0F;
    float offset = 0.0F;

    MutationOperation operation = MutationOperation::PASS_THROUGH;
    float op_value1 = 0.0F;
    float op_value2 = 0.0F;
    bool enabled = false;
};

}  // namespace bored::signalscope
