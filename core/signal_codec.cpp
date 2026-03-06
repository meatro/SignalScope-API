#include "signal_codec.hpp"

#include <cmath>
#include <cstdint>

#include "dbc_parser.hpp"

namespace bored::signalscope {

namespace {

constexpr uint16_t kMaxFrameBits = 64;

bool readFrameBit(const uint8_t data[8], uint16_t bit_index) {
    if (bit_index >= kMaxFrameBits) {
        return false;
    }

    const uint16_t byte_index = static_cast<uint16_t>(bit_index / 8U);
    const uint16_t bit_in_byte = static_cast<uint16_t>(bit_index % 8U);
    return (data[byte_index] & static_cast<uint8_t>(1U << bit_in_byte)) != 0U;
}

void writeFrameBit(uint8_t data[8], uint16_t bit_index, bool value) {
    if (bit_index >= kMaxFrameBits) {
        return;
    }

    const uint16_t byte_index = static_cast<uint16_t>(bit_index / 8U);
    const uint16_t bit_in_byte = static_cast<uint16_t>(bit_index % 8U);
    const uint8_t mask = static_cast<uint8_t>(1U << bit_in_byte);

    if (value) {
        data[byte_index] |= mask;
    } else {
        data[byte_index] &= static_cast<uint8_t>(~mask);
    }
}

uint16_t nextMotorolaBit(uint16_t current) {
    if ((current % 8U) == 0U) {
        return static_cast<uint16_t>(current + 15U);
    }
    return static_cast<uint16_t>(current - 1U);
}

bool extractRaw(
    const uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    uint64_t& out_raw) {

    if (length == 0U || length > 64U) {
        return false;
    }

    uint64_t raw = 0;

    if (little_endian) {
        for (uint8_t i = 0; i < length; ++i) {
            const uint16_t bit_index = static_cast<uint16_t>(start_bit + i);
            if (bit_index >= kMaxFrameBits) {
                return false;
            }
            if (readFrameBit(data, bit_index)) {
                raw |= (1ULL << i);
            }
        }
    } else {
        uint16_t bit_index = start_bit;
        for (uint8_t i = 0; i < length; ++i) {
            if (bit_index >= kMaxFrameBits) {
                return false;
            }

            if (readFrameBit(data, bit_index)) {
                const uint8_t shift = static_cast<uint8_t>((length - 1U) - i);
                raw |= (1ULL << shift);
            }
            bit_index = nextMotorolaBit(bit_index);
        }
    }

    out_raw = raw;
    return true;
}

bool insertRaw(
    uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    uint64_t raw) {

    if (length == 0U || length > 64U) {
        return false;
    }

    if (little_endian) {
        for (uint8_t i = 0; i < length; ++i) {
            const uint16_t bit_index = static_cast<uint16_t>(start_bit + i);
            if (bit_index >= kMaxFrameBits) {
                return false;
            }
            const bool bit_value = (raw & (1ULL << i)) != 0ULL;
            writeFrameBit(data, bit_index, bit_value);
        }
    } else {
        uint16_t bit_index = start_bit;
        for (uint8_t i = 0; i < length; ++i) {
            if (bit_index >= kMaxFrameBits) {
                return false;
            }
            const uint8_t shift = static_cast<uint8_t>((length - 1U) - i);
            const bool bit_value = (raw & (1ULL << shift)) != 0ULL;
            writeFrameBit(data, bit_index, bit_value);
            bit_index = nextMotorolaBit(bit_index);
        }
    }

    return true;
}

}  // namespace

bool decodeSignalRaw(
    const uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    bool is_signed,
    float factor,
    float offset,
    float& out_value) {

    uint64_t raw = 0;
    if (!extractRaw(data, start_bit, length, little_endian, raw)) {
        return false;
    }

    int64_t signed_value = static_cast<int64_t>(raw);
    if (is_signed && length < 64U) {
        const uint64_t sign_mask = 1ULL << (length - 1U);
        if ((raw & sign_mask) != 0ULL) {
            const uint64_t extend_mask = ~((1ULL << length) - 1ULL);
            signed_value = static_cast<int64_t>(raw | extend_mask);
        }
    }

    const double physical = (static_cast<double>(signed_value) * static_cast<double>(factor)) + static_cast<double>(offset);
    out_value = static_cast<float>(physical);
    return true;
}

bool encodeSignalRaw(
    uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    bool is_signed,
    float factor,
    float offset,
    float physical_value) {

    if (length == 0U || length > 64U || factor == 0.0F) {
        return false;
    }

    const double scaled = (static_cast<double>(physical_value) - static_cast<double>(offset)) / static_cast<double>(factor);
    int64_t integer_value = static_cast<int64_t>(std::llround(scaled));

    int64_t min_value = 0;
    int64_t max_value = 0;
    if (is_signed) {
        max_value = (length == 64U) ? INT64_MAX : ((1LL << (length - 1U)) - 1LL);
        min_value = (length == 64U) ? INT64_MIN : (-(1LL << (length - 1U)));
    } else {
        min_value = 0;
        max_value = (length == 64U) ? INT64_MAX : static_cast<int64_t>((1ULL << length) - 1ULL);
    }

    if (integer_value < min_value) {
        integer_value = min_value;
    }
    if (integer_value > max_value) {
        integer_value = max_value;
    }

    uint64_t raw = 0;
    if (is_signed) {
        raw = static_cast<uint64_t>(integer_value);
        if (length < 64U) {
            raw &= ((1ULL << length) - 1ULL);
        }
    } else {
        raw = static_cast<uint64_t>(integer_value);
    }

    return insertRaw(data, start_bit, length, little_endian, raw);
}

bool decodeSignal(const CanFrame& frame, const DbcSignalDef& signal, float& out_value) {
    return decodeSignalRaw(
        frame.data,
        signal.start_bit,
        signal.length,
        signal.little_endian,
        signal.is_signed,
        signal.factor,
        signal.offset,
        out_value);
}

bool encodeSignal(CanFrame& frame, const DbcSignalDef& signal, float physical_value) {
    return encodeSignalRaw(
        frame.data,
        signal.start_bit,
        signal.length,
        signal.little_endian,
        signal.is_signed,
        signal.factor,
        signal.offset,
        physical_value);
}

bool applyMutationToFrame(CanFrame& frame, const SignalMutation& mutation) {
    if (!mutation.enabled) {
        return false;
    }

    float current_value = 0.0F;
    if (!decodeSignalRaw(
            frame.data,
            mutation.start_bit,
            mutation.length,
            mutation.little_endian,
            mutation.is_signed,
            mutation.factor,
            mutation.offset,
            current_value)) {
        return false;
    }

    float next_value = current_value;
    switch (mutation.operation) {
    case MutationOperation::PASS_THROUGH:
        return true;
    case MutationOperation::REPLACE:
        next_value = mutation.op_value1;
        break;
    case MutationOperation::ADD_OFFSET:
        next_value = current_value + mutation.op_value1;
        break;
    case MutationOperation::MULTIPLY:
        next_value = current_value * mutation.op_value1;
        break;
    case MutationOperation::CLAMP:
        if (next_value < mutation.op_value1) {
            next_value = mutation.op_value1;
        }
        if (next_value > mutation.op_value2) {
            next_value = mutation.op_value2;
        }
        break;
    default:
        return false;
    }

    return encodeSignalRaw(
        frame.data,
        mutation.start_bit,
        mutation.length,
        mutation.little_endian,
        mutation.is_signed,
        mutation.factor,
        mutation.offset,
        next_value);
}

}  // namespace bored::signalscope
