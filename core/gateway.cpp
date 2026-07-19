#include "gateway.hpp"

#include <Arduino.h>

#include "frame_cache.hpp"
#include "mutation_engine.hpp"
#include "observation_manager.hpp"
#include "signal_cache.hpp"

namespace bored::signalscope {

namespace {

inline void updateRunningAverageLatency(uint32_t latency_us, uint32_t& avg_us, uint32_t& samples) {
    if (samples < 0xFFFFFFFFU) {
        ++samples;
    }

    if (samples <= 1U) {
        avg_us = latency_us;
        return;
    }

    const int32_t delta = static_cast<int32_t>(latency_us) - static_cast<int32_t>(avg_us);
    avg_us = static_cast<uint32_t>(static_cast<int32_t>(avg_us) + (delta / static_cast<int32_t>(samples)));
}

inline void recordTxFailure(Direction direction, GatewayStats& stats) {
    if (direction == Direction::A_TO_B) {
        ++stats.tx_failures_a_to_b;
    } else {
        ++stats.tx_failures_b_to_a;
    }
}

inline void recordDeferred(Direction direction, GatewayStats& stats) {
    if (direction == Direction::A_TO_B) {
        ++stats.egress_deferred_frames_a_to_b;
    } else {
        ++stats.egress_deferred_frames_b_to_a;
    }
}

inline void recordRetryAttempt(Direction direction, GatewayStats& stats) {
    if (direction == Direction::A_TO_B) {
        ++stats.egress_retry_attempts_a_to_b;
    } else {
        ++stats.egress_retry_attempts_b_to_a;
    }
}

inline void recordFullStall(Direction direction, GatewayStats& stats) {
    if (direction == Direction::A_TO_B) {
        ++stats.egress_full_stalls_a_to_b;
    } else {
        ++stats.egress_full_stalls_b_to_a;
    }
}

}  // namespace

void GatewayCore::init() {
    ready_gate_ = false;
    active_can_writes_allowed_.store(0U, std::memory_order_release);
    // SignalScope is bidirectional by default. A registered application may
    // narrow this immediately after init(), before active writes are enabled.
    mutation_direction_mask_.store(kMutationDirectionBoth, std::memory_order_release);
    raw_a_to_b_.head = 0U;
    raw_a_to_b_.tail = 0U;
    raw_b_to_a_.head = 0U;
    raw_b_to_a_.tail = 0U;
    egress_a_to_b_.head = 0U;
    egress_a_to_b_.tail = 0U;
    egress_a_to_b_.count = 0U;
    egress_b_to_a_.head = 0U;
    egress_b_to_a_.tail = 0U;
    egress_b_to_a_.count = 0U;
    next_raw_direction_ = Direction::A_TO_B;
    next_egress_flush_direction_ = Direction::A_TO_B;
    stats_ = {};
    for (size_t index = 0U; index < kStatsSnapshotCount; ++index) {
        published_stats_[index] = {};
        stats_snapshot_readers_[index].store(0U, std::memory_order_relaxed);
    }
    active_stats_snapshot_.store(0U, std::memory_order_release);
}

void GatewayCore::setMutationEngine(MutationEngine* engine) {
    mutation_engine_ = engine;
}

void GatewayCore::setReplayEngine(ReplayEngine* engine) {
    replay_engine_ = engine;
}

void GatewayCore::setTxDriver(TxDriver driver) {
    tx_driver_ = driver;
}

void GatewayCore::setTraceCallback(TraceCallback callback) {
    trace_callback_ = callback;
}

void GatewayCore::setFrameCache(FrameCache* cache) {
    frame_cache_ = cache;
}

void GatewayCore::setIngressFrameCache(FrameCache* cache) {
    ingress_frame_cache_ = cache;
}

void GatewayCore::setSignalCache(SignalCache* cache) {
    signal_cache_ = cache;
}

void GatewayCore::setObservationManager(ObservationManager* manager) {
    observation_manager_ = manager;
}

void GatewayCore::setDbcPointer(const std::atomic<const DbcDatabase*>* dbc_ptr) {
    dbc_active_ptr_ = dbc_ptr;
}

void GatewayCore::setReadyGate(bool ready) {
    ready_gate_ = ready;
}

void GatewayCore::pauseSignalDecoding() {
    signal_decode_enabled_.store(0U, std::memory_order_release);
}

void GatewayCore::resumeSignalDecoding() {
    signal_decode_enabled_.store(1U, std::memory_order_release);
}

bool GatewayCore::signalDecodingIdle() const {
    return signal_decode_readers_.load(std::memory_order_acquire) == 0U;
}

void GatewayCore::setActiveCanWritesAllowed(bool allowed) {
    active_can_writes_allowed_.store(allowed ? 1U : 0U, std::memory_order_release);
}

bool GatewayCore::activeCanWritesAllowed() const {
    return active_can_writes_allowed_.load(std::memory_order_acquire) != 0U;
}

void GatewayCore::setMutationDirectionMask(uint8_t direction_mask) {
    mutation_direction_mask_.store(
        static_cast<uint8_t>(direction_mask & kMutationDirectionBoth),
        std::memory_order_release);
}

uint8_t GatewayCore::mutationDirectionMask() const {
    return mutation_direction_mask_.load(std::memory_order_acquire);
}

bool GatewayCore::mutationDirectionAllowed(Direction direction) const {
    return (mutationDirectionMask() & mutationDirectionBit(direction)) != 0U;
}

bool GatewayCore::onFrameReceivedFromIsr(const CanFrame& frame) {
    RawQueue& queue = rawQueue(frame.direction);
    const uint16_t next_head = nextIndex(queue.head);
    if (next_head == queue.tail) {
        if (ready_gate_) {
            ++stats_.rx_drops_run;
        } else {
            ++stats_.rx_drops_boot;
        }
        return false;
    }

    queue.frames[queue.head] = frame;
    queue.head = next_head;
    updateRawDepth();

    return true;
}

bool GatewayCore::injectReplayFrame(const CanFrame& frame, bool dry_run) {
    if (!activeCanWritesAllowed()) return false;
    // Replay is intentionally best-effort. It must never consume the bounded
    // queues reserved for physical traffic or jump ahead of a physical frame.
    // Dry-run never reaches either transmitter, so it cannot consume or bypass
    // a physical TX slot and remains safe to evaluate while those queues drain.
    if (!dry_run && physicalBacklogPending()) {
        ++stats_.replay_refused_frames;
        return false;
    }
    CanFrame mutable_frame = frame;
    // Frame freshness uses the live micros() stamp supplied by the replay
    // bridge. Cache/rate bookkeeping uses millis() directly so it does not
    // jump every time the shorter micros() counter wraps.
    return forwardFrame(mutable_frame, true, dry_run, millis());
}

bool GatewayCore::physicalBacklogPending() const {
    return rawPending(Direction::A_TO_B) || rawPending(Direction::B_TO_A) ||
        egress_a_to_b_.count != 0U || egress_b_to_a_.count != 0U;
}

void GatewayCore::purgeDirection(Direction direction) {
    RawQueue& raw = rawQueue(direction);
    EgressQueue& egress = egressQueue(direction);
    const uint32_t purged = static_cast<uint32_t>(rawDepth(direction)) + egress.count;

    raw.head = 0U;
    raw.tail = 0U;
    egress.head = 0U;
    egress.tail = 0U;
    egress.count = 0U;
    if (direction == Direction::A_TO_B) {
        stats_.stale_frames_purged_a_to_b += purged;
    } else {
        stats_.stale_frames_purged_b_to_a += purged;
    }
    updateRawDepth();
    updateEgressDepth(direction);
}

void GatewayCore::pollRx(uint32_t now_us, uint32_t now_ms) {
    // Give already-prepared physical frames first use of both transmitters.
    // Each direction stops on its first busy result while the other direction
    // can continue, so a congested bus cannot starve the opposite path.
    flushEgressFair();

    bool blocked_a_to_b = false;
    bool blocked_b_to_a = false;
    size_t processed = 0U;
    while (processed < kMaxRawFramesPerPoll) {
        bool progressed = false;
        const Direction first = next_raw_direction_;
        for (uint8_t offset = 0U; offset < 2U && processed < kMaxRawFramesPerPoll; ++offset) {
            const Direction direction = offset == 0U ? first :
                (first == Direction::A_TO_B ? Direction::B_TO_A : Direction::A_TO_B);
            bool& blocked = direction == Direction::A_TO_B ? blocked_a_to_b : blocked_b_to_a;
            if (blocked || !rawPending(direction)) continue;
            if (!egressHasCapacity(direction)) {
                // Preserve order within this direction, but keep servicing the
                // independent queue whose destination can still transmit.
                recordFullStall(direction, stats_);
                blocked = true;
                continue;
            }

            RawQueue& queue = rawQueue(direction);
            CanFrame frame = queue.frames[queue.tail];
            frame.timestamp_us = now_us;
            queue.tail = nextIndex(queue.tail);
            static_cast<void>(forwardFrame(frame, false, false, now_ms));
            ++processed;
            progressed = true;
        }
        next_raw_direction_ = first == Direction::A_TO_B
            ? Direction::B_TO_A : Direction::A_TO_B;
        if (!progressed || (blocked_a_to_b && blocked_b_to_a)) break;
    }

    updateRawDepth();
}

const GatewayStats& GatewayCore::canOwnerStats() const {
    return stats_;
}

void GatewayCore::publishStats() {
    const uint8_t active = active_stats_snapshot_.load(std::memory_order_relaxed);
    for (size_t offset = 1U; offset < kStatsSnapshotCount; ++offset) {
        const uint8_t candidate = static_cast<uint8_t>((active + offset) % kStatsSnapshotCount);
        if (stats_snapshot_readers_[candidate].load(std::memory_order_acquire) != 0U) continue;
        published_stats_[candidate] = stats_;
        active_stats_snapshot_.store(candidate, std::memory_order_release);
        return;
    }
    // UI snapshots are tiny and normally release within microseconds. If both
    // inactive buffers are pinned, retain the last published sample and let the
    // next CAN loop try again; the CAN owner must never wait for core 0.
}

GatewayStats GatewayCore::snapshotStats() const {
    for (;;) {
        const uint8_t index = active_stats_snapshot_.load(std::memory_order_acquire);
        stats_snapshot_readers_[index].fetch_add(1U, std::memory_order_acq_rel);
        if (active_stats_snapshot_.load(std::memory_order_acquire) != index) {
            stats_snapshot_readers_[index].fetch_sub(1U, std::memory_order_acq_rel);
            continue;
        }
        const GatewayStats snapshot = published_stats_[index];
        stats_snapshot_readers_[index].fetch_sub(1U, std::memory_order_acq_rel);
        return snapshot;
    }
}

bool GatewayCore::forwardFrame(CanFrame& frame, bool from_replay, bool replay_dry_run, uint32_t now_ms) {
    if (from_replay && !replay_dry_run && physicalBacklogPending()) {
        ++stats_.replay_refused_frames;
        return false;
    }

    const uint32_t processing_start_us = micros();
    // SignalScope observations describe traffic as it arrived. Mutation is a
    // forwarding operation and must never feed synthetic values back into the
    // decoder (or into application telemetry such as vehicle speed).
    const CanFrame received_frame = frame;

    if (ingress_frame_cache_ != nullptr) {
        ingress_frame_cache_->update(received_frame, now_ms, false, nullptr, from_replay);
    }

    // Direction policy is enforced here, at the sole production call site
    // that applies mutations to a forwarded frame. Loading a generic rule by
    // any API cannot bypass an application's direction safety boundary.
    const bool has_rules = activeCanWritesAllowed() && mutationDirectionAllowed(frame.direction) &&
        (mutation_engine_ != nullptr) && mutation_engine_->hasRulesForFrame(frame.id, frame.direction);
    const bool observed = (observation_manager_ != nullptr) && observation_manager_->isObserved(frame.id, frame.direction);

    bool active_path = true;
    size_t applied_rules = 0U;
    if (!has_rules && !observed) {
        active_path = false;
        if (frame_cache_ != nullptr) {
            frame_cache_->update(frame, now_ms, false, nullptr, from_replay);
        }
        ++stats_.passive_fast_path_frames;
    } else {
        if (has_rules && mutation_engine_ != nullptr) {
            applied_rules = mutation_engine_->applyFrameMutations(frame);
            stats_.mutation_applied_frames += static_cast<uint32_t>(applied_rules);
        }

        if (observed && signal_cache_ != nullptr && dbc_active_ptr_ != nullptr &&
            signal_decode_enabled_.load(std::memory_order_acquire) != 0U) {
            signal_decode_readers_.fetch_add(1U, std::memory_order_acq_rel);
            if (signal_decode_enabled_.load(std::memory_order_acquire) != 0U) {
                const DbcDatabase* dbc = dbc_active_ptr_->load(std::memory_order_acquire);
                if (dbc != nullptr) {
                    stats_.observed_decoded_frames += static_cast<uint32_t>(
                        signal_cache_->decodeObservedFrame(*dbc, received_frame));
                }
            }
            signal_decode_readers_.fetch_sub(1U, std::memory_order_acq_rel);
        }

        if (frame_cache_ != nullptr) {
            frame_cache_->update(frame, now_ms, applied_rules > 0U, &received_frame, from_replay);
        }
    }

    if (trace_callback_ != nullptr) {
        trace_callback_(received_frame, frame, applied_rules > 0U, from_replay);
    }

    const PreparedFrame prepared{frame, processing_start_us, active_path};
    if (from_replay) {
        const bool sent = dispatchPrepared(prepared, true, replay_dry_run);
        if (!sent) ++stats_.replay_refused_frames;
        return sent;
    }

    // Once a direction has backlog, every newer physical frame joins it. This
    // preserves bus order and prevents a newly-free hardware slot from letting
    // a later frame bypass an earlier prepared frame.
    if (egressPending(frame.direction)) {
        if (!enqueuePrepared(prepared)) {
            recordTxFailure(frame.direction, stats_);
            return false;
        }
        recordDeferred(frame.direction, stats_);
        return true;
    }

    if (dispatchPrepared(prepared, false)) return true;
    if (!enqueuePrepared(prepared)) {
        recordTxFailure(frame.direction, stats_);
        return false;
    }
    recordDeferred(frame.direction, stats_);
    return true;
}

bool GatewayCore::dispatchPrepared(const PreparedFrame& prepared, bool from_replay, bool replay_dry_run) {
    // Dry-run intentionally ends at the same prepared-frame boundary used by
    // physical replay. No hardware driver is inspected or called on this path.
    if (!replay_dry_run &&
        (tx_driver_ == nullptr || !tx_driver_(prepared.frame.direction, prepared.frame))) return false;

    const uint32_t latency_us = micros() - prepared.processing_start_us;
    if (prepared.active_path) {
        updateRunningAverageLatency(latency_us, stats_.active_path_latency_avg_us, stats_.active_path_latency_samples);
    } else {
        updateRunningAverageLatency(latency_us, stats_.fast_path_latency_avg_us, stats_.fast_path_latency_samples);
    }
    if (replay_dry_run) {
        ++stats_.replay_dry_run_frames;
    } else {
        ++stats_.forwarded_frames;
        if (from_replay) ++stats_.replay_injected_frames;
    }
    return true;
}

bool GatewayCore::enqueuePrepared(const PreparedFrame& prepared) {
    EgressQueue& queue = egressQueue(prepared.frame.direction);
    if (queue.count >= kEgressQueueSize) return false;
    queue.frames[queue.head] = prepared;
    queue.head = static_cast<uint8_t>((queue.head + 1U) % kEgressQueueSize);
    ++queue.count;
    updateEgressDepth(prepared.frame.direction);
    return true;
}

bool GatewayCore::flushOneEgress(Direction direction) {
    EgressQueue& queue = egressQueue(direction);
    if (queue.count == 0U) return true;
    recordRetryAttempt(direction, stats_);
    if (!dispatchPrepared(queue.frames[queue.tail], false)) return false;
    queue.tail = static_cast<uint8_t>((queue.tail + 1U) % kEgressQueueSize);
    --queue.count;
    updateEgressDepth(direction);
    return true;
}

void GatewayCore::flushEgressFair() {
    bool blocked_a_to_b = false;
    bool blocked_b_to_a = false;
    size_t budget = kMaxEgressFramesPerPoll;

    while (budget > 0U) {
        bool attempted = false;
        for (uint8_t offset = 0U; offset < 2U && budget > 0U; ++offset) {
            const bool start_a_to_b = next_egress_flush_direction_ == Direction::A_TO_B;
            const Direction direction = ((offset == 0U) == start_a_to_b)
                ? Direction::A_TO_B : Direction::B_TO_A;
            bool& blocked = direction == Direction::A_TO_B ? blocked_a_to_b : blocked_b_to_a;
            if (blocked || !egressPending(direction)) continue;
            attempted = true;
            --budget;
            if (!flushOneEgress(direction)) blocked = true;
        }
        next_egress_flush_direction_ = next_egress_flush_direction_ == Direction::A_TO_B
            ? Direction::B_TO_A : Direction::A_TO_B;
        if (!attempted || (blocked_a_to_b && blocked_b_to_a)) break;
    }
}

bool GatewayCore::egressHasCapacity(Direction direction) const {
    return egressQueue(direction).count < kEgressQueueSize;
}

bool GatewayCore::egressPending(Direction direction) const {
    return egressQueue(direction).count != 0U;
}

bool GatewayCore::rawPending(Direction direction) const {
    const RawQueue& queue = rawQueue(direction);
    return queue.tail != queue.head;
}

uint16_t GatewayCore::rawDepth(Direction direction) const {
    const RawQueue& queue = rawQueue(direction);
    return queue.head >= queue.tail
        ? static_cast<uint16_t>(queue.head - queue.tail)
        : static_cast<uint16_t>((kRxQueueSize - queue.tail) + queue.head);
}

GatewayCore::RawQueue& GatewayCore::rawQueue(Direction direction) {
    return direction == Direction::A_TO_B ? raw_a_to_b_ : raw_b_to_a_;
}

const GatewayCore::RawQueue& GatewayCore::rawQueue(Direction direction) const {
    return direction == Direction::A_TO_B ? raw_a_to_b_ : raw_b_to_a_;
}

GatewayCore::EgressQueue& GatewayCore::egressQueue(Direction direction) {
    return direction == Direction::A_TO_B ? egress_a_to_b_ : egress_b_to_a_;
}

const GatewayCore::EgressQueue& GatewayCore::egressQueue(Direction direction) const {
    return direction == Direction::A_TO_B ? egress_a_to_b_ : egress_b_to_a_;
}

void GatewayCore::updateRawDepth() {
    stats_.rx_queue_depth = static_cast<uint16_t>(
        rawDepth(Direction::A_TO_B) + rawDepth(Direction::B_TO_A));
}

void GatewayCore::updateEgressDepth(Direction direction) {
    const uint16_t depth = egressQueue(direction).count;
    if (direction == Direction::A_TO_B) {
        stats_.egress_queue_depth_a_to_b = depth;
    } else {
        stats_.egress_queue_depth_b_to_a = depth;
    }
}

uint16_t GatewayCore::nextIndex(uint16_t index) {
    return static_cast<uint16_t>((index + 1U) % kRxQueueSize);
}

}  // namespace bored::signalscope
