#pragma once

#include <cstddef>
#include <cstdint>

namespace bored::signalscope {

struct DbcSignalDef {
    char name[40] = {0};
    uint32_t can_id = 0;
    uint16_t start_bit = 0;
    uint8_t length = 0;
    bool little_endian = true;
    bool is_signed = false;
    float factor = 1.0F;
    float offset = 0.0F;
};

struct DbcMessageDef {
    char name[40] = {0};
    uint32_t can_id = 0;
    uint8_t dlc = 8;
    uint16_t signal_start = 0;
    uint16_t signal_count = 0;
};

class DbcDatabase {
public:
    static constexpr size_t kMaxMessages = 128;
    static constexpr size_t kMaxSignals = 512;

    void clear();
    bool parseFromText(const char* text, size_t length);

    const DbcMessageDef* findMessage(uint32_t can_id) const;
    const DbcSignalDef* findSignal(uint32_t can_id, const char* name) const;
    const DbcSignalDef* signalAt(size_t index) const;

    size_t messageCount() const;
    size_t signalCount() const;

private:
    bool parseMessageLine(const char* line);
    bool parseSignalLine(const char* line);

    static void trimLine(char* line);
    static void copyToken(char* out, size_t out_size, const char* token);

    DbcMessageDef messages_[kMaxMessages];
    DbcSignalDef signals_[kMaxSignals];
    size_t message_count_ = 0;
    size_t signal_count_ = 0;
    int32_t current_message_index_ = -1;
};

}  // namespace bored::signalscope
