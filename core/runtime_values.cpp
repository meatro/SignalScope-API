#include "runtime_values.hpp"

#include <cstring>

namespace bored::signalscope {
namespace {
uint32_t floatBits(float value) { uint32_t bits = 0U; std::memcpy(&bits, &value, sizeof(bits)); return bits; }
float bitsFloat(uint32_t bits) { float value = 0.0F; std::memcpy(&value, &bits, sizeof(value)); return value; }
}

void RuntimeValueRegistry::init() {
    for (Entry& entry : entries_) {
        entry.name[0] = '\0';
        entry.value_bits.store(0U, std::memory_order_relaxed);
        entry.in_use = false;
    }
}

int32_t RuntimeValueRegistry::publish(const char* name, float value) {
    if (name == nullptr || name[0] == '\0' || std::strlen(name) >= sizeof(entries_[0].name)) return -1;
    int32_t index = find(name);
    if (index < 0) {
        for (size_t i = 0; i < kMaxValues; ++i) {
            if (entries_[i].in_use) continue;
            std::strncpy(entries_[i].name, name, sizeof(entries_[i].name) - 1U);
            entries_[i].name[sizeof(entries_[i].name) - 1U] = '\0';
            entries_[i].in_use = true;
            index = static_cast<int32_t>(i);
            break;
        }
    }
    if (index < 0) return -1;
    entries_[index].value_bits.store(floatBits(value), std::memory_order_release);
    return index;
}

int32_t RuntimeValueRegistry::find(const char* name) const {
    if (name == nullptr || name[0] == '\0') return -1;
    for (size_t i = 0; i < kMaxValues; ++i) {
        if (entries_[i].in_use && std::strcmp(entries_[i].name, name) == 0) return static_cast<int32_t>(i);
    }
    return -1;
}

bool RuntimeValueRegistry::read(uint16_t index, float& value) const {
    if (index >= kMaxValues || !entries_[index].in_use) return false;
    value = bitsFloat(entries_[index].value_bits.load(std::memory_order_acquire));
    return true;
}

}  // namespace bored::signalscope
