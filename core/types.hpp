#pragma once

#include <cstddef>
#include <cstdint>

namespace bored::signalscope {

enum class Direction : uint8_t {
    A_TO_B = 0,
    B_TO_A = 1,
};

// Bitmask used by hosts/applications that need to constrain an otherwise
// bidirectional SignalScope capability.  Direction retains its wire-routing
// meaning; the mask only decides which direction may be actively mutated.
constexpr uint8_t kMutationDirectionAtoB = 1U << 0;
constexpr uint8_t kMutationDirectionBtoA = 1U << 1;
constexpr uint8_t kMutationDirectionBoth =
    kMutationDirectionAtoB | kMutationDirectionBtoA;

constexpr uint8_t mutationDirectionBit(Direction direction) {
    return direction == Direction::A_TO_B
        ? kMutationDirectionAtoB
        : kMutationDirectionBtoA;
}

// Return the age of a micros()-domain sample while tolerating a sample that
// was published just after the caller captured `now`. That cross-core race
// otherwise looks almost UINT32_MAX microseconds old. Normal timer rollover
// still works through unsigned subtraction.
constexpr uint32_t microsSampleAge(uint32_t now_us, uint32_t sample_us) {
    const uint32_t elapsed = now_us - sample_us;
    if (elapsed <= 0x7FFFFFFFU) return elapsed;
    const uint32_t published_after_now = sample_us - now_us;
    return published_after_now <= 1000000U ? 0U : UINT32_MAX;
}

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
