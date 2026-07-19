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
    DbcDatabase() = default;
    ~DbcDatabase();

    DbcDatabase(const DbcDatabase&) = delete;
    DbcDatabase& operator=(const DbcDatabase&) = delete;

    void clear();
    bool parseFromText(const char* text, size_t length);
    // Loads the compact, precompiled SSDB representation into detached
    // storage and commits it only after the complete payload validates.
    // Legacy OHDB v1 input remains readable for applications created before
    // the standalone format was renamed.
    bool loadFromPacked(const uint8_t* data, size_t length);
    // Exchanges the owned, already-parsed storage without allocating. This is
    // used by the runtime DBC transaction after CAN decoding has quiesced.
    void swap(DbcDatabase& other);

    const DbcMessageDef* findMessage(uint32_t can_id) const;
    const DbcSignalDef* findSignal(uint32_t can_id, const char* name) const;
    const DbcSignalDef* signalAt(size_t index) const;

    size_t messageCount() const;
    size_t signalCount() const;
    size_t messageCapacity() const;
    size_t signalCapacity() const;

private:
    bool parseFromTextDestructive(const char* text, size_t length);
    bool reserve(size_t message_capacity, size_t signal_capacity);
    static void countDefinitions(const char* text, size_t length, size_t& message_count, size_t& signal_count);
    bool parseMessageLine(const char* line);
    bool parseSignalLine(const char* line);

    static void trimLine(char* line);
    static void copyToken(char* out, size_t out_size, const char* token);

    DbcMessageDef* messages_ = nullptr;
    DbcSignalDef* signals_ = nullptr;
    size_t message_capacity_ = 0;
    size_t signal_capacity_ = 0;
    size_t message_count_ = 0;
    size_t signal_count_ = 0;
    int32_t current_message_index_ = -1;
};

}  // namespace bored::signalscope
