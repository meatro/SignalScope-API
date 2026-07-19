// Host build examples (run from the repository root):
//   c++ -std=c++17 -I. tests/dbc_packed_host.cpp core/dbc_parser.cpp core/runtime_memory.cpp -o dbc_packed_host
//   cl /nologo /std:c++17 /EHsc /I. tests\dbc_packed_host.cpp core\dbc_parser.cpp core\runtime_memory.cpp /Fe:dbc_packed_host.exe
#include "core/dbc_parser.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using bored::signalscope::DbcDatabase;
using bored::signalscope::DbcMessageDef;
using bored::signalscope::DbcSignalDef;

namespace {

constexpr size_t kHeaderSize = 16U;
constexpr size_t kMessageSize = 50U;
constexpr size_t kSignalSize = 56U;

void putU16(std::vector<uint8_t>& output, size_t offset, uint16_t value) {
    output[offset] = static_cast<uint8_t>(value & 0xFFU);
    output[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}

void putU32(std::vector<uint8_t>& output, size_t offset, uint32_t value) {
    output[offset] = static_cast<uint8_t>(value & 0xFFU);
    output[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    output[offset + 2U] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    output[offset + 3U] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

void putFloat(std::vector<uint8_t>& output, size_t offset, float value) {
    uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(output, offset, bits);
}

void putName(std::vector<uint8_t>& output, size_t offset, const char* name) {
    assert(name != nullptr && std::strlen(name) < 40U);
    std::memcpy(output.data() + offset, name, std::strlen(name));
}

std::vector<uint8_t> validDatabase(const char* magic) {
    assert(magic != nullptr && std::strlen(magic) == 4U);
    constexpr uint16_t message_count = 2U;
    constexpr uint16_t signal_count = 3U;
    std::vector<uint8_t> output(
        kHeaderSize + message_count * kMessageSize + signal_count * kSignalSize, 0U);

    std::memcpy(output.data(), magic, 4U);
    putU16(output, 4U, 1U);
    putU16(output, 6U, kHeaderSize);
    putU16(output, 8U, message_count);
    putU16(output, 10U, signal_count);

    size_t message = kHeaderSize;
    putU32(output, message, 0x123U);
    output[message + 4U] = 8U;
    putU16(output, message + 6U, 0U);
    putU16(output, message + 8U, 2U);
    putName(output, message + 10U, "FirstMessage");

    message += kMessageSize;
    putU32(output, message, 0x456U);
    output[message + 4U] = 4U;
    putU16(output, message + 6U, 2U);
    putU16(output, message + 8U, 1U);
    putName(output, message + 10U, "SecondMessage");

    size_t signal = kHeaderSize + message_count * kMessageSize;
    putU32(output, signal, 0x123U);
    putU16(output, signal + 4U, 0U);
    output[signal + 6U] = 8U;
    output[signal + 7U] = 0x01U;
    putFloat(output, signal + 8U, 0.5F);
    putFloat(output, signal + 12U, -1.0F);
    putName(output, signal + 16U, "UnsignedIntel");

    signal += kSignalSize;
    putU32(output, signal, 0x123U);
    putU16(output, signal + 4U, 8U);
    output[signal + 6U] = 16U;
    output[signal + 7U] = 0x02U;
    putFloat(output, signal + 8U, 2.0F);
    putFloat(output, signal + 12U, 3.0F);
    putName(output, signal + 16U, "SignedMotorola");

    signal += kSignalSize;
    putU32(output, signal, 0x456U);
    putU16(output, signal + 4U, 7U);
    output[signal + 6U] = 1U;
    output[signal + 7U] = 0x01U;
    putFloat(output, signal + 8U, 1.0F);
    putFloat(output, signal + 12U, 0.0F);
    putName(output, signal + 16U, "Flag");
    return output;
}

void requireOriginalDatabase(const DbcDatabase& database) {
    assert(database.messageCount() == 2U);
    assert(database.signalCount() == 3U);
    const DbcMessageDef* message = database.findMessage(0x123U);
    assert(message != nullptr && std::strcmp(message->name, "FirstMessage") == 0);
    assert(message->dlc == 8U && message->signal_start == 0U && message->signal_count == 2U);

    const DbcSignalDef* intel = database.findSignal(0x123U, "UnsignedIntel");
    assert(intel != nullptr && intel->start_bit == 0U && intel->length == 8U);
    assert(intel->little_endian && !intel->is_signed);
    assert(std::fabs(intel->factor - 0.5F) < 0.0001F);
    assert(std::fabs(intel->offset + 1.0F) < 0.0001F);

    const DbcSignalDef* motorola = database.findSignal(0x123U, "SignedMotorola");
    assert(motorola != nullptr && !motorola->little_endian && motorola->is_signed);
}

void rejectWithoutReplacing(DbcDatabase& database, std::vector<uint8_t> invalid) {
    assert(!database.loadFromPacked(invalid.data(), invalid.size()));
    requireOriginalDatabase(database);
}

}  // namespace

int main() {
    const std::vector<uint8_t> packed = validDatabase("SSDB");
    DbcDatabase database;
    assert(database.loadFromPacked(packed.data(), packed.size()));
    requireOriginalDatabase(database);

    // OHDB was the original application-specific magic. Keep this explicit
    // compatibility assertion separate so new fixtures use SignalScope's SSDB.
    const std::vector<uint8_t> legacy_packed = validDatabase("OHDB");
    DbcDatabase legacy_database;
    assert(legacy_database.loadFromPacked(legacy_packed.data(), legacy_packed.size()));
    requireOriginalDatabase(legacy_database);

    std::vector<uint8_t> invalid = packed;
    std::memcpy(invalid.data(), "NOPE", 4U);
    rejectWithoutReplacing(database, invalid);

    invalid = packed;
    invalid.pop_back();
    rejectWithoutReplacing(database, invalid);

    invalid = packed;
    putU16(invalid, kHeaderSize + 6U, 1U);
    rejectWithoutReplacing(database, invalid);

    invalid = packed;
    invalid[kHeaderSize + 10U] = 0U;
    invalid[kHeaderSize + 11U] = 1U;
    rejectWithoutReplacing(database, invalid);

    const size_t first_signal = kHeaderSize + 2U * kMessageSize;
    invalid = packed;
    invalid[first_signal + 7U] = 0x04U;
    rejectWithoutReplacing(database, invalid);

    invalid = packed;
    putFloat(invalid, first_signal + 8U, std::numeric_limits<float>::infinity());
    rejectWithoutReplacing(database, invalid);

    invalid = packed;
    putU32(invalid, first_signal, 0x999U);
    rejectWithoutReplacing(database, invalid);

    std::cout << "DBC_PACKED_HOST_TEST_PASS\n";
    return 0;
}
