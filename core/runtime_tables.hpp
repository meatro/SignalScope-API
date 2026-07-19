#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bored::signalscope {

class RuntimeTableRegistry {
public:
    static constexpr size_t kMaxTables = 8;
    static constexpr size_t kMaxEntries = 128;

    void init();
    int32_t publish(const char* name, const float* values, size_t count, bool valid);
    int32_t find(const char* name) const;
    bool lookupFirstAtLeast(uint16_t table_index, float target, uint32_t& output_index) const;

private:
    struct Entry {
        char name[32] = {0};
        float values[kMaxEntries] = {0.0F};
        std::atomic<uint32_t> generation{0U};
        std::atomic<uint16_t> count{0U};
        std::atomic<uint8_t> valid{0U};
        bool in_use = false;
    };

    Entry* entries_ = nullptr;
};

}  // namespace bored::signalscope
