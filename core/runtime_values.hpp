#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bored::signalscope {

class RuntimeValueRegistry {
public:
    static constexpr size_t kMaxValues = 32;
    void init();
    int32_t publish(const char* name, float value);
    int32_t find(const char* name) const;
    bool read(uint16_t index, float& value) const;

private:
    struct Entry {
        char name[32] = {0};
        std::atomic<uint32_t> value_bits{0U};
        bool in_use = false;
    };
    Entry entries_[kMaxValues];
};

}  // namespace bored::signalscope
