#include "runtime_memory.hpp"

#include <cstdlib>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace bored::signalscope {

void* allocateRuntimeMemory(size_t bytes) {
    if (bytes == 0U) return nullptr;

#if defined(ESP_PLATFORM)
    void* pointer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pointer != nullptr) return pointer;
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#else
    return std::malloc(bytes);
#endif
}

void freeRuntimeMemory(void* pointer) {
    if (pointer == nullptr) return;
#if defined(ESP_PLATFORM)
    heap_caps_free(pointer);
#else
    std::free(pointer);
#endif
}

}  // namespace bored::signalscope
