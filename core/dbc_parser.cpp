#include "dbc_parser.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bored::signalscope {

void DbcDatabase::clear() {
    std::memset(messages_, 0, sizeof(messages_));
    std::memset(signals_, 0, sizeof(signals_));
    message_count_ = 0;
    signal_count_ = 0;
    current_message_index_ = -1;
}

bool DbcDatabase::parseFromText(const char* text, size_t length) {
    clear();

    if (text == nullptr || length == 0U) {
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
            if (std::strncmp(line, "BO_", 3) == 0) {
                if (parseMessageLine(line)) {
                    saw_message = true;
                } else {
                    // Detach subsequent SG_ lines from prior message on malformed BO_.
                    current_message_index_ = -1;
                }
            } else if (std::strncmp(line, "SG_", 3) == 0) {
                if (parseSignalLine(line)) {
                    saw_signal = true;
                }
            }
        }

        line_len = 0;
    }

    return saw_message && saw_signal;
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

bool DbcDatabase::parseMessageLine(const char* line) {
    if (message_count_ >= kMaxMessages) {
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
    if (current_message_index_ < 0 || signal_count_ >= kMaxSignals) {
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
