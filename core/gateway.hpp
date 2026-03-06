#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dbc_parser.hpp"
#include "types.hpp"

namespace bored::signalscope {

class FrameCache;
class MutationEngine;
class ObservationManager;
class ReplayEngine;
class SignalCache;

struct GatewayStats {
    uint32_t forwarded_frames = 0;
    uint32_t replay_injected_frames = 0;
    uint32_t mutation_applied_frames = 0;
    uint32_t passive_fast_path_frames = 0;
    uint32_t observed_decoded_frames = 0;
    uint32_t rx_drops_boot = 0;
    uint32_t rx_drops_run = 0;
    uint16_t rx_queue_depth = 0;

    // Runtime latency (micros) from frame processing start to TX dispatch.
    uint32_t fast_path_latency_avg_us = 0;
    uint32_t active_path_latency_avg_us = 0;
    uint32_t fast_path_latency_samples = 0;
    uint32_t active_path_latency_samples = 0;
};

class GatewayCore {
public:
    using TxDriver = bool (*)(Direction tx_direction, const CanFrame& frame);

    static constexpr size_t kRxQueueSize = 128;

    void init();
    void setMutationEngine(MutationEngine* engine);
    void setReplayEngine(ReplayEngine* engine);
    void setTxDriver(TxDriver driver);
    void setFrameCache(FrameCache* cache);
    void setSignalCache(SignalCache* cache);
    void setObservationManager(ObservationManager* manager);
    void setDbcPointer(const std::atomic<const DbcDatabase*>* dbc_ptr);
    void setReadyGate(bool ready);

    bool onFrameReceivedFromIsr(const CanFrame& frame);
    bool injectReplayFrame(const CanFrame& frame);

    void pollRx(uint32_t now_us, uint32_t now_ms);

    const GatewayStats& stats() const;

private:
    void forwardFrame(CanFrame& frame, bool from_replay, uint32_t now_ms);
    static uint16_t nextIndex(uint16_t index);

    CanFrame rx_queue_[kRxQueueSize];
    volatile uint16_t queue_head_ = 0;
    volatile uint16_t queue_tail_ = 0;

    MutationEngine* mutation_engine_ = nullptr;
    ReplayEngine* replay_engine_ = nullptr;
    TxDriver tx_driver_ = nullptr;
    FrameCache* frame_cache_ = nullptr;
    SignalCache* signal_cache_ = nullptr;
    ObservationManager* observation_manager_ = nullptr;
    const std::atomic<const DbcDatabase*>* dbc_active_ptr_ = nullptr;

    bool ready_gate_ = false;
    GatewayStats stats_{};
};

}  // namespace bored::signalscope

