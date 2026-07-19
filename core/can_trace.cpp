#include "can_trace.hpp"

#include <algorithm>
#include <cstring>

#include "runtime_memory.hpp"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace bored::signalscope {

bool CanTraceQueue::init(size_t capacity) {
    active_.store(0U, std::memory_order_release);
    if (events_ != nullptr) {
        freeRuntimeMemory(events_);
        events_ = nullptr;
    }
    capacity_ = 0U;
    if (capacity < 2U) return false;
    size_t allocated_capacity = capacity;
#if defined(ESP_PLATFORM)
    events_ = static_cast<CanTraceEvent*>(heap_caps_malloc(
        sizeof(CanTraceEvent) * allocated_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (events_ == nullptr) {
        constexpr size_t kInternalFallbackCapacity = 512U;
        constexpr size_t kInternalHeapReserve = 64U * 1024U;
        const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t usable = internal_free > kInternalHeapReserve
            ? internal_free - kInternalHeapReserve : 0U;
        allocated_capacity = std::min(
            std::min(capacity, kInternalFallbackCapacity),
            std::min(usable, largest) / sizeof(CanTraceEvent));
        if (allocated_capacity >= 64U) {
            events_ = static_cast<CanTraceEvent*>(heap_caps_malloc(
                sizeof(CanTraceEvent) * allocated_capacity,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
#else
    events_ = static_cast<CanTraceEvent*>(
        allocateRuntimeMemory(sizeof(CanTraceEvent) * allocated_capacity));
#endif
    if (events_ == nullptr) return false;
    std::memset(events_, 0, sizeof(CanTraceEvent) * allocated_capacity);
    capacity_ = allocated_capacity;
    write_index_.store(0U, std::memory_order_relaxed);
    read_index_.store(0U, std::memory_order_relaxed);
    next_sequence_.store(0U, std::memory_order_relaxed);
    dropped_.store(0U, std::memory_order_relaxed);
    produced_.store(0U, std::memory_order_relaxed);
    producer_inflight_.store(0U, std::memory_order_relaxed);
    return true;
}

bool CanTraceQueue::start(CanTraceScope scope) {
    if (!available()) return false;
    active_.store(0U, std::memory_order_release);
    if (!producerIdle()) return false;
    const uint32_t head = write_index_.load(std::memory_order_acquire);
    read_index_.store(head, std::memory_order_release);
    next_sequence_.store(0U, std::memory_order_release);
    dropped_.store(0U, std::memory_order_release);
    produced_.store(0U, std::memory_order_release);
    scope_.store(static_cast<uint8_t>(scope), std::memory_order_release);
    active_.store(1U, std::memory_order_release);
    return true;
}

void CanTraceQueue::stop() {
    active_.store(0U, std::memory_order_release);
}

bool CanTraceQueue::acceptsDirection(Direction direction) const {
    const CanTraceScope selected = scope();
    if (selected == CanTraceScope::AToB) return direction == Direction::A_TO_B;
    if (selected == CanTraceScope::BToA) return direction == Direction::B_TO_A;
    return true;
}

void CanTraceQueue::pushIngress(const CanFrame& frame, bool diagnostic_consumed,
                                bool gateway_dropped) {
    if (!active() || !acceptsDirection(frame.direction) || scope() == CanTraceScope::Mutated) return;
    CanTraceEvent event{};
    event.timestamp_us = frame.timestamp_us;
    event.can_id = frame.id;
    event.direction = frame.direction;
    event.dlc = frame.dlc <= 8U ? frame.dlc : 8U;
    event.stage = CanTraceStage::Ingress;
    if (diagnostic_consumed) event.flags |= CanTraceDiagnosticConsumed;
    if (gateway_dropped) event.flags |= CanTraceGatewayDropped;
    std::memcpy(event.data, frame.data, sizeof(event.data));
    push(event);
}

void CanTraceQueue::pushPrepared(const CanFrame& input, const CanFrame& output,
                                 bool mutated, bool synthetic) {
    if (!active() || !acceptsDirection(output.direction)) return;
    const CanTraceScope selected = scope();
    if (!mutated && selected != CanTraceScope::All) return;
    if (synthetic && selected == CanTraceScope::Physical) return;
    CanTraceEvent event{};
    event.timestamp_us = output.timestamp_us;
    event.can_id = output.id;
    event.direction = output.direction;
    event.dlc = output.dlc <= 8U ? output.dlc : 8U;
    event.stage = CanTraceStage::Prepared;
    event.flags = CanTraceHasInput;
    if (mutated) event.flags |= CanTraceMutated;
    if (synthetic) event.flags |= CanTraceSynthetic;
    std::memcpy(event.data, output.data, sizeof(event.data));
    std::memcpy(event.input_data, input.data, sizeof(event.input_data));
    push(event);
}

void CanTraceQueue::pushTransmit(const CanFrame& frame, bool diagnostic_transport) {
    if (!active() || !acceptsDirection(frame.direction) || scope() == CanTraceScope::Mutated) return;
    CanTraceEvent event{};
    event.timestamp_us = frame.timestamp_us;
    event.can_id = frame.id;
    event.direction = frame.direction;
    event.dlc = frame.dlc <= 8U ? frame.dlc : 8U;
    event.stage = CanTraceStage::Transmit;
    if (diagnostic_transport) event.flags |= CanTraceDiagnosticTransport;
    std::memcpy(event.data, frame.data, sizeof(event.data));
    push(event);
}

void CanTraceQueue::push(const CanTraceEvent& source) {
    producer_inflight_.fetch_add(1U, std::memory_order_acq_rel);
    if (!active() || events_ == nullptr || capacity_ == 0U) {
        producer_inflight_.fetch_sub(1U, std::memory_order_release);
        return;
    }
    CanTraceEvent event = source;
    // Allocate sequence before the capacity check so an overflow is visible as
    // a gap in the next persisted event, in addition to the explicit counter.
    event.sequence = next_sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const uint32_t write = write_index_.load(std::memory_order_relaxed);
    const uint32_t read = read_index_.load(std::memory_order_acquire);
    if (write - read >= capacity_) {
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        producer_inflight_.fetch_sub(1U, std::memory_order_release);
        return;
    }
    events_[write % capacity_] = event;
    write_index_.store(write + 1U, std::memory_order_release);
    produced_.fetch_add(1U, std::memory_order_relaxed);
    producer_inflight_.fetch_sub(1U, std::memory_order_release);
}

bool CanTraceQueue::pop(CanTraceEvent& event) {
    if (events_ == nullptr || capacity_ == 0U) return false;
    const uint32_t read = read_index_.load(std::memory_order_relaxed);
    const uint32_t write = write_index_.load(std::memory_order_acquire);
    if (read == write) return false;
    event = events_[read % capacity_];
    read_index_.store(read + 1U, std::memory_order_release);
    return true;
}

size_t CanTraceQueue::pending() const {
    const uint32_t write = write_index_.load(std::memory_order_acquire);
    const uint32_t read = read_index_.load(std::memory_order_acquire);
    return static_cast<size_t>(write - read);
}

const char* CanTraceQueue::scopeName(CanTraceScope scope) {
    switch (scope) {
        case CanTraceScope::All: return "all";
        case CanTraceScope::AToB: return "a_to_b";
        case CanTraceScope::BToA: return "b_to_a";
        case CanTraceScope::Mutated: return "mutated";
        case CanTraceScope::Physical:
        default: return "physical";
    }
}

bool CanTraceQueue::parseScope(const char* text, CanTraceScope& scope) {
    if (text == nullptr || text[0] == '\0' || std::strcmp(text, "physical") == 0) {
        scope = CanTraceScope::Physical;
        return true;
    }
    if (std::strcmp(text, "all") == 0) scope = CanTraceScope::All;
    else if (std::strcmp(text, "a_to_b") == 0) scope = CanTraceScope::AToB;
    else if (std::strcmp(text, "b_to_a") == 0) scope = CanTraceScope::BToA;
    else if (std::strcmp(text, "mutated") == 0) scope = CanTraceScope::Mutated;
    else return false;
    return true;
}

}  // namespace bored::signalscope
