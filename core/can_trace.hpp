#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

// Generic SignalScope recording scopes. Host/control-plane clients choose a
// scope; the trace layer has no knowledge of application signals or behavior.
enum class CanTraceScope : uint8_t {
    Physical = 0,
    All = 1,
    AToB = 2,
    BToA = 3,
    Mutated = 4,
};

enum class CanTraceStage : uint8_t {
    Ingress = 1,
    Prepared = 2,
    Transmit = 3,
};

enum CanTraceFlag : uint8_t {
    CanTraceDiagnosticConsumed = 1U << 0U,
    CanTraceGatewayDropped = 1U << 1U,
    CanTraceMutated = 1U << 2U,
    CanTraceSynthetic = 1U << 3U,
    CanTraceHasInput = 1U << 4U,
    CanTraceDiagnosticTransport = 1U << 5U,
};

struct CanTraceEvent {
    uint32_t sequence = 0U;
    uint32_t timestamp_us = 0U;
    uint32_t can_id = 0U;
    Direction direction = Direction::A_TO_B;
    uint8_t dlc = 0U;
    CanTraceStage stage = CanTraceStage::Ingress;
    uint8_t flags = 0U;
    uint8_t data[8] = {0};
    uint8_t input_data[8] = {0};
};

// One CAN-task producer and one application-task consumer. The producer never
// locks, allocates, waits, or performs filesystem work. A full queue drops the
// trace event and increments a counter; physical forwarding always wins.
class CanTraceQueue {
public:
    static constexpr size_t kDefaultCapacity = 4096U;

    bool init(size_t capacity = kDefaultCapacity);
    bool available() const { return events_ != nullptr && capacity_ > 1U; }
    size_t capacity() const { return capacity_; }

    bool start(CanTraceScope scope);
    void stop();
    bool active() const { return active_.load(std::memory_order_acquire) != 0U; }
    CanTraceScope scope() const {
        return static_cast<CanTraceScope>(scope_.load(std::memory_order_acquire));
    }

    void pushIngress(const CanFrame& frame, bool diagnostic_consumed, bool gateway_dropped);
    void pushPrepared(const CanFrame& input, const CanFrame& output, bool mutated, bool synthetic);
    void pushTransmit(const CanFrame& frame, bool diagnostic_transport);

    bool pop(CanTraceEvent& event);
    size_t pending() const;
    bool producerIdle() const {
        return producer_inflight_.load(std::memory_order_acquire) == 0U;
    }
    uint32_t dropped() const { return dropped_.load(std::memory_order_acquire); }
    uint32_t produced() const { return produced_.load(std::memory_order_acquire); }

    static const char* scopeName(CanTraceScope scope);
    static bool parseScope(const char* text, CanTraceScope& scope);

private:
    bool acceptsDirection(Direction direction) const;
    void push(const CanTraceEvent& event);

    CanTraceEvent* events_ = nullptr;
    size_t capacity_ = 0U;
    std::atomic<uint32_t> write_index_{0U};
    std::atomic<uint32_t> read_index_{0U};
    std::atomic<uint32_t> next_sequence_{0U};
    std::atomic<uint32_t> dropped_{0U};
    std::atomic<uint32_t> produced_{0U};
    std::atomic<uint32_t> producer_inflight_{0U};
    std::atomic<uint8_t> scope_{static_cast<uint8_t>(CanTraceScope::Physical)};
    std::atomic<uint8_t> active_{0U};
};

}  // namespace bored::signalscope
