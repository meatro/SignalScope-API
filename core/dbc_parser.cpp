#include "dbc_parser.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "runtime_memory.hpp"

namespace bored::signalscope {

namespace {

constexpr size_t kPackedHeaderSize = 16U;
constexpr size_t kPackedMessageRecordSize = 50U;
constexpr size_t kPackedSignalRecordSize = 56U;
constexpr uint16_t kPackedVersion = 1U;
constexpr size_t kPackedNameSize = 40U;

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) |
        (static_cast<uint32_t>(data[2]) << 16U) |
        (static_cast<uint32_t>(data[3]) << 24U);
}

float readLeFloat32(const uint8_t* data) {
    static_assert(sizeof(float) == sizeof(uint32_t), "SSDB requires 32-bit IEEE-754 floats");
    const uint32_t bits = readLe32(data);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool packedNameValid(const uint8_t* name) {
    if (name == nullptr || name[0] == 0U) return false;

    size_t terminator = 0U;
    while (terminator < kPackedNameSize && name[terminator] != 0U) ++terminator;
    if (terminator == kPackedNameSize) return false;

    // SSDB names are canonical fixed fields, not arbitrary bytes following a
    // C string. Reject non-zero tail data so every reader sees one record.
    for (size_t i = terminator + 1U; i < kPackedNameSize; ++i) {
        if (name[i] != 0U) return false;
    }
    return true;
}

bool packedSizeValid(size_t message_count, size_t signal_count, size_t length) {
    if (message_count == 0U || signal_count == 0U) return false;
    if (message_count > (SIZE_MAX - kPackedHeaderSize) / kPackedMessageRecordSize) return false;
    const size_t after_messages =
        kPackedHeaderSize + message_count * kPackedMessageRecordSize;
    if (signal_count > (SIZE_MAX - after_messages) / kPackedSignalRecordSize) return false;
    return after_messages + signal_count * kPackedSignalRecordSize == length;
}

}  // namespace

DbcDatabase::~DbcDatabase() {
    clear();
}

void DbcDatabase::clear() {
    freeRuntimeMemory(messages_);
    freeRuntimeMemory(signals_);
    messages_ = nullptr;
    signals_ = nullptr;
    message_capacity_ = 0U;
    signal_capacity_ = 0U;
    message_count_ = 0;
    signal_count_ = 0;
    current_message_index_ = -1;
}

bool DbcDatabase::parseFromText(const char* text, size_t length) {
    // Parse into a detached database so a malformed replacement cannot erase
    // the currently active DBC. Only exchange storage after a complete parse.
    DbcDatabase parsed;
    if (!parsed.parseFromTextDestructive(text, length)) {
        return false;
    }
    swap(parsed);
    return true;
}

bool DbcDatabase::loadFromPacked(const uint8_t* data, size_t length) {
    const bool current_magic = data != nullptr && length >= 4U &&
        std::memcmp(data, "SSDB", 4U) == 0;
    const bool legacy_magic = data != nullptr && length >= 4U &&
        std::memcmp(data, "OHDB", 4U) == 0;
    if (data == nullptr || length < kPackedHeaderSize ||
        (!current_magic && !legacy_magic) ||
        readLe16(data + 4U) != kPackedVersion ||
        readLe16(data + 6U) != kPackedHeaderSize ||
        readLe32(data + 12U) != 0U) {
        return false;
    }

    const size_t message_count = readLe16(data + 8U);
    const size_t signal_count = readLe16(data + 10U);
    if (!packedSizeValid(message_count, signal_count, length)) return false;

    const size_t messages_offset = kPackedHeaderSize;
    const size_t signals_offset =
        messages_offset + message_count * kPackedMessageRecordSize;

    size_t expected_signal_start = 0U;
    for (size_t i = 0U; i < message_count; ++i) {
        const uint8_t* record = data + messages_offset + i * kPackedMessageRecordSize;
        const uint8_t dlc = record[4U];
        const uint8_t reserved = record[5U];
        const size_t signal_start = readLe16(record + 6U);
        const size_t message_signal_count = readLe16(record + 8U);

        if (dlc > 8U || reserved != 0U ||
            signal_start != expected_signal_start ||
            signal_start > signal_count ||
            message_signal_count > signal_count - signal_start ||
            !packedNameValid(record + 10U)) {
            return false;
        }
        expected_signal_start += message_signal_count;
    }
    if (expected_signal_start != signal_count) return false;

    size_t message_index = 0U;
    for (size_t i = 0U; i < signal_count; ++i) {
        while (message_index < message_count) {
            const uint8_t* message =
                data + messages_offset + message_index * kPackedMessageRecordSize;
            const size_t message_start = readLe16(message + 6U);
            const size_t message_end = message_start + readLe16(message + 8U);
            if (i < message_end) break;
            ++message_index;
        }
        if (message_index >= message_count) return false;

        const uint8_t* message =
            data + messages_offset + message_index * kPackedMessageRecordSize;
        const uint8_t* record = data + signals_offset + i * kPackedSignalRecordSize;
        const uint16_t start_bit = readLe16(record + 4U);
        const uint8_t signal_length = record[6U];
        const uint8_t flags = record[7U];
        const float factor = readLeFloat32(record + 8U);
        const float offset = readLeFloat32(record + 12U);

        if (readLe32(record) != readLe32(message) ||
            start_bit >= 64U ||
            signal_length == 0U || signal_length > 64U ||
            (flags & static_cast<uint8_t>(~0x03U)) != 0U ||
            !std::isfinite(factor) || !std::isfinite(offset) ||
            !packedNameValid(record + 16U)) {
            return false;
        }
    }

    DbcDatabase parsed;
    if (!parsed.reserve(message_count, signal_count)) return false;

    for (size_t i = 0U; i < message_count; ++i) {
        const uint8_t* record = data + messages_offset + i * kPackedMessageRecordSize;
        DbcMessageDef& message = parsed.messages_[i];
        message.can_id = readLe32(record);
        message.dlc = record[4U];
        message.signal_start = readLe16(record + 6U);
        message.signal_count = readLe16(record + 8U);
        std::memcpy(message.name, record + 10U, sizeof(message.name));
    }

    for (size_t i = 0U; i < signal_count; ++i) {
        const uint8_t* record = data + signals_offset + i * kPackedSignalRecordSize;
        DbcSignalDef& signal = parsed.signals_[i];
        signal.can_id = readLe32(record);
        signal.start_bit = readLe16(record + 4U);
        signal.length = record[6U];
        signal.little_endian = (record[7U] & 0x01U) != 0U;
        signal.is_signed = (record[7U] & 0x02U) != 0U;
        signal.factor = readLeFloat32(record + 8U);
        signal.offset = readLeFloat32(record + 12U);
        std::memcpy(signal.name, record + 16U, sizeof(signal.name));
    }

    parsed.message_count_ = message_count;
    parsed.signal_count_ = signal_count;
    parsed.current_message_index_ = -1;
    swap(parsed);
    return true;
}

bool DbcDatabase::parseFromTextDestructive(const char* text, size_t length) {
    clear();

    if (text == nullptr || length == 0U) {
        return false;
    }

    size_t required_messages = 0U;
    size_t required_signals = 0U;
    countDefinitions(text, length, required_messages, required_signals);
    if (required_messages == 0U || required_signals == 0U || !reserve(required_messages, required_signals)) {
        clear();
        return false;
    }

    char line[320] = {0};
    size_t line_len = 0;
    bool saw_message = false;
    bool saw_signal = false;

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
        trimLine(line);

        if (line[0] != '\0') {
            if (std::strncmp(line, "BO_ ", 4) == 0) {
                if (parseMessageLine(line)) {
                    saw_message = true;
                } else {
                    // Detach subsequent SG_ lines from prior message on malformed BO_.
                    current_message_index_ = -1;
                }
            } else if (std::strncmp(line, "SG_ ", 4) == 0) {
                if (parseSignalLine(line)) {
                    saw_signal = true;
                }
            }
        }

        line_len = 0;
    }

    const bool complete = message_count_ == required_messages && signal_count_ == required_signals;
    if (!saw_message || !saw_signal || !complete) {
        clear();
        return false;
    }
    return true;
}

void DbcDatabase::swap(DbcDatabase& other) {
    using std::swap;
    swap(messages_, other.messages_);
    swap(signals_, other.signals_);
    swap(message_capacity_, other.message_capacity_);
    swap(signal_capacity_, other.signal_capacity_);
    swap(message_count_, other.message_count_);
    swap(signal_count_, other.signal_count_);
    swap(current_message_index_, other.current_message_index_);
}

const DbcMessageDef* DbcDatabase::findMessage(uint32_t can_id) const {
    for (size_t i = 0; i < message_count_; ++i) {
        if (messages_[i].can_id == can_id) {
            return &messages_[i];
        }
    }
    return nullptr;
}

const DbcSignalDef* DbcDatabase::findSignal(uint32_t can_id, const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }

    for (size_t i = 0; i < signal_count_; ++i) {
        if (signals_[i].can_id == can_id && std::strcmp(signals_[i].name, name) == 0) {
            return &signals_[i];
        }
    }

    return nullptr;
}

const DbcSignalDef* DbcDatabase::signalAt(size_t index) const {
    if (index >= signal_count_) {
        return nullptr;
    }
    return &signals_[index];
}

size_t DbcDatabase::messageCount() const {
    return message_count_;
}

size_t DbcDatabase::signalCount() const {
    return signal_count_;
}

size_t DbcDatabase::messageCapacity() const {
    return message_capacity_;
}

size_t DbcDatabase::signalCapacity() const {
    return signal_capacity_;
}

bool DbcDatabase::reserve(size_t message_capacity, size_t signal_capacity) {
    if (message_capacity == 0U || signal_capacity == 0U || message_capacity > 0xFFFFU || signal_capacity > 0xFFFFU) {
        return false;
    }

    messages_ = static_cast<DbcMessageDef*>(allocateRuntimeMemory(message_capacity * sizeof(DbcMessageDef)));
    signals_ = static_cast<DbcSignalDef*>(allocateRuntimeMemory(signal_capacity * sizeof(DbcSignalDef)));
    if (messages_ == nullptr || signals_ == nullptr) {
        freeRuntimeMemory(messages_);
        freeRuntimeMemory(signals_);
        messages_ = nullptr;
        signals_ = nullptr;
        return false;
    }

    std::memset(messages_, 0, message_capacity * sizeof(DbcMessageDef));
    std::memset(signals_, 0, signal_capacity * sizeof(DbcSignalDef));
    message_capacity_ = message_capacity;
    signal_capacity_ = signal_capacity;
    return true;
}

void DbcDatabase::countDefinitions(const char* text, size_t length, size_t& message_count, size_t& signal_count) {
    message_count = 0U;
    signal_count = 0U;
    char line[320] = {0};
    size_t line_len = 0U;

    for (size_t i = 0; i <= length; ++i) {
        const char c = (i < length) ? text[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            if (line_len + 1U < sizeof(line)) line[line_len++] = c;
            continue;
        }

        line[line_len] = '\0';
        trimLine(line);
        if (std::strncmp(line, "BO_ ", 4) == 0) ++message_count;
        else if (std::strncmp(line, "SG_ ", 4) == 0) ++signal_count;
        line_len = 0U;
    }
}

bool DbcDatabase::parseMessageLine(const char* line) {
    if (message_count_ >= message_capacity_) {
        return false;
    }

    unsigned long can_id = 0;
    unsigned int dlc = 8;
    char name[80] = {0};

    const int matched = std::sscanf(line, "BO_ %lu %79[^:]: %u", &can_id, name, &dlc);
    if (matched != 3) {
        return false;
    }

    DbcMessageDef& message = messages_[message_count_];
    copyToken(message.name, sizeof(message.name), name);
    message.can_id = static_cast<uint32_t>(can_id);
    message.dlc = (dlc > 8U) ? 8U : static_cast<uint8_t>(dlc);
    message.signal_start = static_cast<uint16_t>(signal_count_);
    message.signal_count = 0;

    current_message_index_ = static_cast<int32_t>(message_count_);
    ++message_count_;
    return true;
}

bool DbcDatabase::parseSignalLine(const char* line) {
    if (current_message_index_ < 0 || signal_count_ >= signal_capacity_) {
        return false;
    }

    char name[80] = {0};
    unsigned int start_bit = 0;
    unsigned int length = 0;
    unsigned int byte_order = 1;
    char sign = '+';
    double factor = 1.0;
    double offset = 0.0;

    int matched = std::sscanf(
        line,
        "SG_ %79s : %u|%u@%u%c (%lf,%lf)",
        name,
        &start_bit,
        &length,
        &byte_order,
        &sign,
        &factor,
        &offset);

    if (matched != 7) {
        // Multiplexed lines often include an extra token between signal name and ':'.
        // Example: SG_ SignalName m0 : 8|8@1+ (1,0)
        char mux_token[16] = {0};
        matched = std::sscanf(
            line,
            "SG_ %79s %15s : %u|%u@%u%c (%lf,%lf)",
            name,
            mux_token,
            &start_bit,
            &length,
            &byte_order,
            &sign,
            &factor,
            &offset);
    }

    if (matched != 7 && matched != 8) {
        return false;
    }

    if (length == 0U || length > 64U) {
        return false;
    }

    DbcSignalDef& signal = signals_[signal_count_];
    copyToken(signal.name, sizeof(signal.name), name);
    signal.can_id = messages_[current_message_index_].can_id;
    signal.start_bit = static_cast<uint16_t>(start_bit);
    signal.length = static_cast<uint8_t>(length);
    signal.little_endian = (byte_order == 1U);
    signal.is_signed = (sign == '-');
    signal.factor = static_cast<float>(factor);
    signal.offset = static_cast<float>(offset);

    ++signal_count_;
    ++messages_[current_message_index_].signal_count;
    return true;
}

void DbcDatabase::trimLine(char* line) {
    if (line == nullptr) {
        return;
    }

    size_t len = std::strlen(line);
    while (len > 0U && std::isspace(static_cast<unsigned char>(line[len - 1U])) != 0) {
        line[len - 1U] = '\0';
        --len;
    }

    size_t start = 0;
    while (line[start] != '\0' && std::isspace(static_cast<unsigned char>(line[start])) != 0) {
        ++start;
    }

    if (start > 0U) {
        std::memmove(line, line + start, std::strlen(line + start) + 1U);
    }
}

void DbcDatabase::copyToken(char* out, size_t out_size, const char* token) {
    if (out == nullptr || out_size == 0U || token == nullptr) {
        return;
    }

    std::strncpy(out, token, out_size - 1U);
    out[out_size - 1U] = '\0';

    const size_t len = std::strlen(out);
    if (len > 0U && out[len - 1U] == ':') {
        out[len - 1U] = '\0';
    }
}

}  // namespace bored::signalscope
