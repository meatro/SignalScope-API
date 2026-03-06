#pragma once

#include "types.hpp"

namespace bored::signalscope {

struct DbcSignalDef;

bool decodeSignalRaw(
    const uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    bool is_signed,
    float factor,
    float offset,
    float& out_value);

bool encodeSignalRaw(
    uint8_t data[8],
    uint16_t start_bit,
    uint8_t length,
    bool little_endian,
    bool is_signed,
    float factor,
    float offset,
    float physical_value);

bool decodeSignal(const CanFrame& frame, const DbcSignalDef& signal, float& out_value);
bool encodeSignal(CanFrame& frame, const DbcSignalDef& signal, float physical_value);

bool applyMutationToFrame(CanFrame& frame, const SignalMutation& mutation);

}  // namespace bored::signalscope
