#include "runtime_tables.hpp"

#include <cstring>
#include <new>

#include "runtime_memory.hpp"

namespace bored::signalscope {

void RuntimeTableRegistry::init() {
    if (entries_ == nullptr) entries_ = static_cast<Entry*>(allocateRuntimeMemory(sizeof(Entry) * kMaxTables));
    if (entries_ == nullptr) return;
    for (size_t i = 0; i < kMaxTables; ++i) {
        new (&entries_[i]) Entry();
    }
}

int32_t RuntimeTableRegistry::publish(const char* name, const float* values, size_t count, bool valid) {
    if (entries_ == nullptr || name == nullptr || name[0] == '\0' ||
        std::strlen(name) >= sizeof(entries_[0].name) || count > kMaxEntries || (count > 0U && values == nullptr)) {
        return -1;
    }
    int32_t index = find(name);
    if (index < 0) {
        for (size_t i = 0; i < kMaxTables; ++i) {
            if (entries_[i].in_use) continue;
            std::strncpy(entries_[i].name, name, sizeof(entries_[i].name) - 1U);
            entries_[i].name[sizeof(entries_[i].name) - 1U] = '\0';
            entries_[i].in_use = true;
            index = static_cast<int32_t>(i);
            break;
        }
    }
    if (index < 0) return -1;

    Entry& entry = entries_[index];
    uint32_t generation = entry.generation.load(std::memory_order_relaxed);
    entry.generation.store(generation + 1U, std::memory_order_release);  // odd: writer active
    for (size_t i = 0; i < count; ++i) entry.values[i] = values[i];
    entry.count.store(static_cast<uint16_t>(count), std::memory_order_relaxed);
    entry.valid.store(valid && count > 0U ? 1U : 0U, std::memory_order_relaxed);
    entry.generation.store(generation + 2U, std::memory_order_release);  // even: stable
    return index;
}

int32_t RuntimeTableRegistry::find(const char* name) const {
    if (entries_ == nullptr || name == nullptr || name[0] == '\0') return -1;
    for (size_t i = 0; i < kMaxTables; ++i) {
        if (entries_[i].in_use && std::strcmp(entries_[i].name, name) == 0) return static_cast<int32_t>(i);
    }
    return -1;
}

bool RuntimeTableRegistry::lookupFirstAtLeast(uint16_t table_index, float target, uint32_t& output_index) const {
    if (entries_ == nullptr || table_index >= kMaxTables) return false;
    const Entry& entry = entries_[table_index];
    for (uint8_t attempt = 0; attempt < 3U; ++attempt) {
        const uint32_t before = entry.generation.load(std::memory_order_acquire);
        if ((before & 1U) != 0U || entry.valid.load(std::memory_order_relaxed) == 0U) continue;
        const uint16_t count = entry.count.load(std::memory_order_relaxed);
        uint32_t selected = count > 0U ? static_cast<uint32_t>(count - 1U) : 0U;
        for (uint16_t i = 0; i < count; ++i) {
            if (entry.values[i] >= target) { selected = i; break; }
        }
        const uint32_t after = entry.generation.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) { output_index = selected; return true; }
    }
    return false;
}

}  // namespace bored::signalscope
