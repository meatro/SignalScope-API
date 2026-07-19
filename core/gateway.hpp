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
    uint32_t tx_failures_a_to_b = 0;
    uint32_t tx_failures_b_to_a = 0;
    uint32_t replay_injected_frames = 0;
    uint32_t replay_dry_run_frames = 0;
    uint32_t mutation_applied_frames = 0;
    uint32_t passive_fast_path_frames = 0;
    uint32_t observed_decoded_frames = 0;
    uint32_t rx_drops_boot = 0;
    uint32_t rx_drops_run = 0;
    uint16_t rx_queue_depth = 0;

    // Physical frames are mutated/observed once, then retained here until the
    // destination controller accepts them. A transiently busy TX mailbox is a
    // deferral, not a dropped CAN frame.
    uint32_t egress_deferred_frames_a_to_b = 0;
    uint32_t egress_deferred_frames_b_to_a = 0;
    uint32_t egress_retry_attempts_a_to_b = 0;
    uint32_t egress_retry_attempts_b_to_a = 0;
    uint32_t egress_full_stalls_a_to_b = 0;
    uint32_t egress_full_stalls_b_to_a = 0;
    uint32_t stale_frames_purged_a_to_b = 0;
    uint32_t stale_frames_purged_b_to_a = 0;
    uint32_t replay_refused_frames = 0;
    uint16_t egress_queue_depth_a_to_b = 0;
    uint16_t egress_queue_depth_b_to_a = 0;

    // Runtime latency (micros) from frame processing start to TX dispatch.
    uint32_t fast_path_latency_avg_us = 0;
    uint32_t active_path_latency_avg_us = 0;
    uint32_t fast_path_latency_samples = 0;
    uint32_t active_path_latency_samples = 0;
};

class GatewayCore {
public:
    using TxDriver = bool (*)(Direction tx_direction, const CanFrame& frame);
    using TraceCallback = void (*)(const CanFrame& input, const CanFrame& output,
                                   bool mutated, bool synthetic);

    // Each physical direction owns this much ingress capacity. Keeping the
    // queues independent prevents a stopped destination from blocking frames
    // that are still able to cross in the opposite direction.
    static constexpr size_t kRxQueueSize = 128;
    static constexpr size_t kEgressQueueSize = 64;
    static constexpr size_t kMaxRawFramesPerPoll = 16;
    static constexpr size_t kMaxEgressFramesPerPoll = 16;

    void init();
    void setMutationEngine(MutationEngine* engine);
    void setReplayEngine(ReplayEngine* engine);
    void setTxDriver(TxDriver driver);
    void setTraceCallback(TraceCallback callback);
    void setFrameCache(FrameCache* cache);
    void setIngressFrameCache(FrameCache* cache);
    void setSignalCache(SignalCache* cache);
    void setObservationManager(ObservationManager* manager);
    void setDbcPointer(const std::atomic<const DbcDatabase*>* dbc_ptr);
    void setReadyGate(bool ready);
    void pauseSignalDecoding();
    void resumeSignalDecoding();
    bool signalDecodingIdle() const;
    void setActiveCanWritesAllowed(bool allowed);
    bool activeCanWritesAllowed() const;
    void setMutationDirectionMask(uint8_t direction_mask);
    uint8_t mutationDirectionMask() const;
    bool mutationDirectionAllowed(Direction direction) const;

    // Legacy name retained for API compatibility. Both hardware drivers are
    // drained by the single CAN task; ISRs only notify that task and must never
    // write these non-atomic single-owner rings directly.
    bool onFrameReceivedFromIsr(const CanFrame& frame);
    bool injectReplayFrame(const CanFrame& frame, bool dry_run = false);
    bool physicalBacklogPending() const;
    void purgeDirection(Direction direction);

    void pollRx(uint32_t now_us, uint32_t now_ms);

    // The CAN task owns `canOwnerStats()`. It publishes an immutable triple-buffered
    // view once per loop for UI/core-0 readers, without locking either core.
    const GatewayStats& canOwnerStats() const;
    void publishStats();
    GatewayStats snapshotStats() const;

private:
    struct PreparedFrame {
        CanFrame frame{};
        uint32_t processing_start_us = 0;
        bool active_path = false;
    };

    struct EgressQueue {
        PreparedFrame frames[kEgressQueueSize]{};
        uint8_t head = 0;
        uint8_t tail = 0;
        uint8_t count = 0;
    };

    struct RawQueue {
        CanFrame frames[kRxQueueSize]{};
        uint16_t head = 0;
        uint16_t tail = 0;
    };

    bool forwardFrame(CanFrame& frame, bool from_replay, bool replay_dry_run, uint32_t now_ms);
    bool dispatchPrepared(const PreparedFrame& prepared, bool from_replay, bool replay_dry_run = false);
    bool enqueuePrepared(const PreparedFrame& prepared);
    bool flushOneEgress(Direction direction);
    void flushEgressFair();
    bool egressHasCapacity(Direction direction) const;
    bool egressPending(Direction direction) const;
    bool rawPending(Direction direction) const;
    uint16_t rawDepth(Direction direction) const;
    RawQueue& rawQueue(Direction direction);
    const RawQueue& rawQueue(Direction direction) const;
    EgressQueue& egressQueue(Direction direction);
    const EgressQueue& egressQueue(Direction direction) const;
    void updateRawDepth();
    void updateEgressDepth(Direction direction);
    static uint16_t nextIndex(uint16_t index);

    RawQueue raw_a_to_b_{};
    RawQueue raw_b_to_a_{};
    Direction next_raw_direction_ = Direction::A_TO_B;

    EgressQueue egress_a_to_b_{};
    EgressQueue egress_b_to_a_{};
    Direction next_egress_flush_direction_ = Direction::A_TO_B;

    MutationEngine* mutation_engine_ = nullptr;
    ReplayEngine* replay_engine_ = nullptr;
    TxDriver tx_driver_ = nullptr;
    TraceCallback trace_callback_ = nullptr;
    FrameCache* frame_cache_ = nullptr;
    FrameCache* ingress_frame_cache_ = nullptr;
    SignalCache* signal_cache_ = nullptr;
    ObservationManager* observation_manager_ = nullptr;
    const std::atomic<const DbcDatabase*>* dbc_active_ptr_ = nullptr;
    std::atomic<uint8_t> signal_decode_enabled_{1U};
    std::atomic<uint32_t> signal_decode_readers_{0U};
    std::atomic<uint8_t> active_can_writes_allowed_{0U};
    std::atomic<uint8_t> mutation_direction_mask_{kMutationDirectionBoth};

    bool ready_gate_ = false;
    GatewayStats stats_{};

    static constexpr size_t kStatsSnapshotCount = 3U;
    GatewayStats published_stats_[kStatsSnapshotCount]{};
    std::atomic<uint8_t> active_stats_snapshot_{0U};
    mutable std::atomic<uint16_t> stats_snapshot_readers_[kStatsSnapshotCount]{};
};

}  // namespace bored::signalscope
