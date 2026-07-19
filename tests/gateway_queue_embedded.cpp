#include <Arduino.h>

#include <cstring>

#include "core/frame_cache.hpp"
#include "core/gateway.hpp"
#include "core/mutation_engine.hpp"

using bored::signalscope::CanFrame;
using bored::signalscope::Direction;
using bored::signalscope::FrameCache;
using bored::signalscope::FrameCacheSnapshot;
using bored::signalscope::GatewayCore;
using bored::signalscope::MutationEngine;
using bored::signalscope::RuleKind;
using bored::signalscope::RuleStageRequest;
using bored::signalscope::kMutationDirectionAtoB;
using bored::signalscope::kMutationDirectionBoth;

namespace {

GatewayCore gateway;
FrameCache frame_cache;
FrameCache ingress_frame_cache;
MutationEngine mutation_engine;
bool block_a_to_b = false;
bool block_b_to_a = false;
uint32_t sent_a_to_b[32] = {};
uint32_t sent_b_to_a[32] = {};
uint8_t sent_a_to_b_byte0[32] = {};
uint8_t sent_b_to_a_byte0[32] = {};
size_t sent_a_to_b_count = 0U;
size_t sent_b_to_a_count = 0U;
uint32_t tx_attempt_count = 0U;
uint32_t trace_count = 0U;
CanFrame trace_input{};
CanFrame trace_output{};
bool trace_mutated = false;
bool trace_synthetic = false;
uint32_t failures = 0U;

void require(bool condition, const char* label) {
    if (condition) return;
    ++failures;
    Serial.printf("FAIL %s\n", label);
}

bool testTx(Direction direction, const CanFrame& frame) {
    ++tx_attempt_count;
    if (direction == Direction::A_TO_B && block_a_to_b) return false;
    if (direction == Direction::B_TO_A && block_b_to_a) return false;

    uint32_t* sent = direction == Direction::A_TO_B ? sent_a_to_b : sent_b_to_a;
    uint8_t* sent_byte0 = direction == Direction::A_TO_B
        ? sent_a_to_b_byte0 : sent_b_to_a_byte0;
    size_t& count = direction == Direction::A_TO_B ? sent_a_to_b_count : sent_b_to_a_count;
    if (count < 32U) {
        sent[count] = frame.id;
        sent_byte0[count] = frame.data[0];
    }
    ++count;
    return true;
}

void testTrace(const CanFrame& input, const CanFrame& output, bool mutated, bool synthetic) {
    ++trace_count;
    trace_input = input;
    trace_output = output;
    trace_mutated = mutated;
    trace_synthetic = synthetic;
}

CanFrame frame(uint32_t id, Direction direction) {
    CanFrame value{};
    value.id = id;
    value.dlc = 8U;
    value.direction = direction;
    return value;
}

void resetGateway() {
    gateway.init();
    frame_cache.init();
    ingress_frame_cache.init();
    gateway.setFrameCache(&frame_cache);
    gateway.setIngressFrameCache(&ingress_frame_cache);
    gateway.setTxDriver(testTx);
    gateway.setTraceCallback(testTrace);
    gateway.setReadyGate(true);
    block_a_to_b = false;
    block_b_to_a = false;
    sent_a_to_b_count = 0U;
    sent_b_to_a_count = 0U;
    tx_attempt_count = 0U;
    trace_count = 0U;
    trace_input = {};
    trace_output = {};
    trace_mutated = false;
    trace_synthetic = false;
    std::memset(sent_a_to_b_byte0, 0, sizeof(sent_a_to_b_byte0));
    std::memset(sent_b_to_a_byte0, 0, sizeof(sent_b_to_a_byte0));
}

void installBidirectionalMutationRules();

void verifyDryRunUsesPipelineWithoutPhysicalDispatch() {
    resetGateway();
    installBidirectionalMutationRules();

    // Leave a real frame waiting for the A-to-B transmitter. Physical replay
    // must yield to this backlog, while dry-run cannot affect its ordering.
    block_a_to_b = true;
    require(gateway.onFrameReceivedFromIsr(frame(0x400U, Direction::A_TO_B)),
            "dry-run backlog frame accepted");
    gateway.pollRx(micros(), millis());
    require(gateway.canOwnerStats().egress_queue_depth_a_to_b == 1U,
            "dry-run test establishes physical TX backlog");
    const uint32_t attempts_before = tx_attempt_count;
    const uint32_t traces_before = trace_count;

    CanFrame replay = frame(0x321U, Direction::A_TO_B);
    replay.data[0] = 0x11U;
    replay.timestamp_us = micros();
    require(gateway.injectReplayFrame(replay, true),
            "dry-run replay is accepted despite physical backlog");
    require(tx_attempt_count == attempts_before,
            "dry-run never calls the physical TX driver");
    require(gateway.canOwnerStats().egress_queue_depth_a_to_b == 1U,
            "dry-run does not consume or extend physical egress backlog");

    FrameCacheSnapshot ingress{};
    require(ingress_frame_cache.read(0x321U, Direction::A_TO_B, &ingress) &&
                ingress.data[0] == 0x11U && !ingress.mutated,
            "dry-run records original ingress provenance");
    FrameCacheSnapshot output{};
    require(frame_cache.read(0x321U, Direction::A_TO_B, &output) &&
                output.data[0] == 0xA5U && output.mutated && output.has_input &&
                output.input_data[0] == 0x11U,
            "dry-run applies mutation and records prepared output provenance");
    require(trace_count == traces_before + 1U && trace_synthetic && trace_mutated &&
                trace_input.data[0] == 0x11U && trace_output.data[0] == 0xA5U,
            "dry-run emits the normal synthetic mutation trace");

    const auto& stats = gateway.canOwnerStats();
    require(stats.replay_dry_run_frames == 1U,
            "dry-run completion has a dedicated counter");
    require(stats.replay_injected_frames == 0U && stats.forwarded_frames == 0U,
            "dry-run is not reported as a physical transmission");
    require(stats.replay_refused_frames == 0U,
            "physical backlog does not count as a dry-run refusal");
    require(stats.active_path_latency_samples == 1U,
            "dry-run reaches the common prepared-frame dispatch boundary");
}

void installBidirectionalMutationRules() {
    mutation_engine.init();
    RuleStageRequest request{};
    request.kind = RuleKind::BIT_RANGE;
    request.can_id = 0x321U;
    request.enabled = true;
    request.start_bit = 0U;
    request.bit_length = 8U;
    request.little_endian = true;

    request.direction = Direction::A_TO_B;
    request.replace_value = 0xA5U;
    require(mutation_engine.stageRule(request), "stage A-to-B mutation rule");
    request.direction = Direction::B_TO_A;
    request.replace_value = 0x5AU;
    require(mutation_engine.stageRule(request), "stage B-to-A mutation rule");
    require(mutation_engine.applyCommit(), "commit bidirectional mutation rules");
    gateway.setMutationEngine(&mutation_engine);
    gateway.setActiveCanWritesAllowed(true);
}

void verifyMutationDirectionBoundary() {
    resetGateway();
    installBidirectionalMutationRules();
    require(gateway.mutationDirectionMask() == kMutationDirectionBoth,
            "standalone gateway defaults to bidirectional mutation");

    CanFrame a_to_b = frame(0x321U, Direction::A_TO_B);
    CanFrame b_to_a = frame(0x321U, Direction::B_TO_A);
    require(gateway.onFrameReceivedFromIsr(a_to_b), "standalone A-to-B mutation frame accepted");
    require(gateway.onFrameReceivedFromIsr(b_to_a), "standalone B-to-A mutation frame accepted");
    gateway.pollRx(micros(), millis());
    require(sent_a_to_b_count == 1U && sent_a_to_b_byte0[0] == 0xA5U,
            "standalone mutates A-to-B traffic");
    require(sent_b_to_a_count == 1U && sent_b_to_a_byte0[0] == 0x5AU,
            "standalone mutates B-to-A traffic");

    resetGateway();
    installBidirectionalMutationRules();
    gateway.setMutationDirectionMask(kMutationDirectionAtoB);
    require(gateway.mutationDirectionAllowed(Direction::A_TO_B),
            "direction policy retains A-to-B mutation");
    require(!gateway.mutationDirectionAllowed(Direction::B_TO_A),
            "direction policy blocks B-to-A mutation");

    require(gateway.onFrameReceivedFromIsr(a_to_b), "restricted A-to-B frame accepted");
    require(gateway.onFrameReceivedFromIsr(b_to_a), "restricted B-to-A frame accepted");
    gateway.pollRx(micros(), millis());
    require(sent_a_to_b_count == 1U && sent_a_to_b_byte0[0] == 0xA5U,
            "allowed A-to-B frame remains mutated");
    require(sent_b_to_a_count == 1U && sent_b_to_a_byte0[0] == 0x00U,
            "blocked B-to-A frame remains byte-identical");
    require(gateway.canOwnerStats().mutation_applied_frames == 1U,
            "blocked direction does not count as an applied mutation");
}

void verifyReplayCannotReplacePhysicalProvenance() {
    resetGateway();
    CanFrame physical = frame(0x118U, Direction::B_TO_A);
    physical.dlc = 4U;
    physical.data[1] = 0x01U;
    require(gateway.onFrameReceivedFromIsr(physical), "physical provenance frame accepted");
    gateway.pollRx(micros(), millis());

    FrameCacheSnapshot snapshot{};
    require(frame_cache.readPhysical(0x118U, Direction::B_TO_A, &snapshot),
            "physical provenance snapshot available");
    require(snapshot.data[1] == 0x01U && snapshot.total_frames == 1U,
            "physical provenance snapshot records wire frame");

    CanFrame replay = physical;
    replay.data[1] = 0x09U;
    replay.timestamp_us = micros();
    gateway.setActiveCanWritesAllowed(true);
    require(gateway.injectReplayFrame(replay), "synthetic replay accepted for bench path");
    require(frame_cache.read(0x118U, Direction::B_TO_A, &snapshot) && snapshot.data[1] == 0x09U,
            "ordinary frame cache still exposes replay for bench UI");
    require(frame_cache.readPhysical(0x118U, Direction::B_TO_A, &snapshot),
            "physical provenance survives replay");
    require(snapshot.data[1] == 0x01U && snapshot.total_frames == 1U,
            "replay cannot advance or replace physical ingress evidence");
}

void verifyDirectionalIsolation(Direction blocked, uint32_t healthy_base) {
    resetGateway();
    const Direction healthy = blocked == Direction::A_TO_B
        ? Direction::B_TO_A : Direction::A_TO_B;
    block_a_to_b = blocked == Direction::A_TO_B;
    block_b_to_a = blocked == Direction::B_TO_A;

    for (size_t index = 0U; index < GatewayCore::kEgressQueueSize; ++index) {
        require(gateway.onFrameReceivedFromIsr(
            frame(static_cast<uint32_t>(0x400U + index), blocked)),
            "blocked ingress accepted while egress fills");
        gateway.pollRx(micros(), millis());
    }

    const auto& full = gateway.canOwnerStats();
    const uint16_t blocked_egress = blocked == Direction::A_TO_B
        ? full.egress_queue_depth_a_to_b : full.egress_queue_depth_b_to_a;
    require(blocked_egress == GatewayCore::kEgressQueueSize,
            "blocked direction reaches bounded egress capacity");
    gateway.publishStats();
    const auto published_full = gateway.snapshotStats();
    require((blocked == Direction::A_TO_B
                ? published_full.egress_queue_depth_a_to_b
                : published_full.egress_queue_depth_b_to_a) == GatewayCore::kEgressQueueSize,
            "published stats snapshot matches the CAN-owner state");

    require(gateway.onFrameReceivedFromIsr(frame(0x5FFU, blocked)),
            "blocked raw head retained");
    constexpr size_t kHealthyFrames = 6U;
    for (size_t index = 0U; index < kHealthyFrames; ++index) {
        require(gateway.onFrameReceivedFromIsr(
            frame(healthy_base + static_cast<uint32_t>(index), healthy)),
            "healthy opposite ingress accepted");
    }
    gateway.pollRx(micros(), millis());

    const size_t healthy_count = healthy == Direction::A_TO_B
        ? sent_a_to_b_count : sent_b_to_a_count;
    const uint32_t* healthy_ids = healthy == Direction::A_TO_B
        ? sent_a_to_b : sent_b_to_a;
    require(healthy_count == kHealthyFrames,
            "full egress in one direction does not block the opposite direction");
    for (size_t index = 0U; index < kHealthyFrames && index < healthy_count; ++index) {
        require(healthy_ids[index] == healthy_base + index,
                "opposite direction preserves ingress order");
    }
    require(gateway.canOwnerStats().rx_queue_depth == 1U,
            "only the blocked direction remains queued");

    gateway.purgeDirection(blocked);
    const auto& purged = gateway.canOwnerStats();
    const uint32_t purged_count = blocked == Direction::A_TO_B
        ? purged.stale_frames_purged_a_to_b : purged.stale_frames_purged_b_to_a;
    require(purged_count == GatewayCore::kEgressQueueSize + 1U,
            "recovery purge removes raw and prepared stale frames");
    require(purged.rx_queue_depth == 0U,
            "recovery purge clears the affected raw queue");
    require(!gateway.physicalBacklogPending(),
            "recovery purge leaves no hidden physical backlog");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    // Give the USB CDC host time to reconnect after flashing/reset so the
    // one-shot result is observable on the bench.
    delay(1500);
    verifyDirectionalIsolation(Direction::A_TO_B, 0x600U);
    verifyDirectionalIsolation(Direction::B_TO_A, 0x700U);
    verifyReplayCannotReplacePhysicalProvenance();
    verifyMutationDirectionBoundary();
    verifyDryRunUsesPipelineWithoutPhysicalDispatch();
    Serial.printf("gateway_queue_embedded failures=%lu\n",
                  static_cast<unsigned long>(failures));
}

void loop() {
    // Repeat the final result so native USB CDC hosts can attach after reset
    // without losing the one-shot setup output.
    Serial.printf("gateway_queue_embedded failures=%lu\n",
                  static_cast<unsigned long>(failures));
    delay(1000);
}
