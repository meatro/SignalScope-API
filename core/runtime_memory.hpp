#pragma once

#include <cstddef>

namespace bored::signalscope {

// Runtime catalogs can be substantially larger than internal MCU RAM.  On
// ESP32 targets allocation prefers external SPI RAM and falls back to the
// normal heap.  Host builds use the normal heap directly.
void* allocateRuntimeMemory(size_t bytes);
void freeRuntimeMemory(void* pointer);

}  // namespace bored::signalscope
