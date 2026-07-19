#include <Arduino.h>

#include <atomic>
#include <initializer_list>

#include "core/diagnostic_transport.hpp"

using bored::signalscope::CanFrame;
using bored::signalscope::DiagnosticError;
using bored::signalscope::DiagnosticJobState;
using bored::signalscope::DiagnosticProtocol;
using bored::signalscope::DiagnosticRequest;
using bored::signalscope::DiagnosticResult;
using bored::signalscope::Direction;

namespace {

constexpr size_t kSentCapacity = 32U;
CanFrame sent[kSentCapacity]{};
size_t sent_count = 0U;
uint32_t now_ms = 1U;
uint32_t failures = 0U;

std::atomic<bool> snapshot_reader_stop{false};
std::atomic<bool> snapshot_reader_done{false};
std::atomic<uint32_t> snapshot_reader_reads{0U};
std::atomic<uint32_t> snapshot_reader_failures{0U};
TaskHandle_t snapshot_reader_task = nullptr;

void require(bool condition, const char* label) {
    if (condition) return;
    ++failures;
    Serial.printf("FAIL %s\n", label);
}

bool captureTx(Direction direction, const CanFrame& frame) {
    if (sent_count >= kSentCapacity) return false;
    sent[sent_count] = frame;
    sent[sent_count].direction = direction;
    ++sent_count;
    return true;
}

void resetTransport() {
    sent_count = 0U;
    now_ms = 1U;
    bored::signalscope::diagnosticTransportInit(captureTx);
}

void tick(uint32_t advance_ms = 1U) {
    now_ms += advance_ms;
    bored::signalscope::diagnosticTransportTick(now_ms * 1000U, now_ms);
}

CanFrame makeFrame(uint32_t id, Direction direction,
                   std::initializer_list<uint8_t> bytes) {
    CanFrame value{};
    value.id = id;
    value.direction = direction;
    value.dlc = static_cast<uint8_t>(bytes.size());
    size_t index = 0U;
    for (uint8_t byte : bytes) value.data[index++] = byte;
    return value;
}

DiagnosticRequest udsRequest(std::initializer_list<uint8_t> payload) {
    DiagnosticRequest request{};
    request.route.protocol = DiagnosticProtocol::ISO_TP;
    request.route.request_direction = Direction::A_TO_B;
    request.route.request_can_id = 0x70FU;
    request.route.response_can_id = 0x779U;
    request.timeout_ms = 5000U;
    request.payload_length = static_cast<uint8_t>(payload.size());
    size_t index = 0U;
    for (uint8_t byte : payload) request.payload[index++] = byte;
    return request;
}

DiagnosticRequest kwpRequest(std::initializer_list<uint8_t> payload) {
    DiagnosticRequest request{};
    request.route.protocol = DiagnosticProtocol::VW_TP20;
    request.route.request_direction = Direction::A_TO_B;
    request.route.request_can_id = 0x200U;
    request.route.response_can_id = 0x20AU;
    request.route.tp20_logical_address = 0x0AU;
    request.route.tp20_requested_ecu_tx_id = 0x300U;
    request.route.tp20_application_type = 0x01U;
    request.route.kwp_session = 0x89U;
    request.route.enter_kwp_session = true;
    request.timeout_ms = 6000U;
    request.payload_length = static_cast<uint8_t>(payload.size());
    size_t index = 0U;
    for (uint8_t byte : payload) request.payload[index++] = byte;
    return request;
}

bool bytesEqual(const CanFrame& actual, uint32_t id,
                std::initializer_list<uint8_t> expected) {
    if (actual.id != id || actual.dlc != expected.size()) return false;
    size_t index = 0U;
    for (uint8_t byte : expected) {
        if (actual.data[index++] != byte) return false;
    }
    return true;
}

DiagnosticResult readResult(uint32_t job_id) {
    DiagnosticResult result{};
    require(bored::signalscope::diagnosticTransportReadResult(job_id, &result),
            "published result available");
    return result;
}

void testPendingTxReservationScope() {
    resetTransport();
    require(!bored::signalscope::diagnosticTransportTxPending(Direction::A_TO_B),
            "idle transport has no pending A-to-B TX");
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit pending-TX scope job");
    require(bored::signalscope::diagnosticTransportTxPending(Direction::A_TO_B),
            "queued A-to-B command reserves its destination mailbox");
    require(!bored::signalscope::diagnosticTransportTxPending(Direction::B_TO_A),
            "queued command does not reserve the opposite destination");
    tick();
    require(!bored::signalscope::diagnosticTransportTxPending(Direction::A_TO_B),
            "successful request TX releases reservation while awaiting response");

    resetTransport();
    request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.route.request_direction = Direction::B_TO_A;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit reverse-direction pending-TX scope job");
    require(bored::signalscope::diagnosticTransportTxPending(Direction::B_TO_A) &&
            !bored::signalscope::diagnosticTransportTxPending(Direction::A_TO_B),
            "reservation follows the actual request direction");
}

void snapshotReader(void*) {
    while (!snapshot_reader_stop.load(std::memory_order_acquire)) {
        DiagnosticResult result{};
        if (bored::signalscope::diagnosticTransportReadLatest(&result)) {
            snapshot_reader_reads.fetch_add(1U, std::memory_order_relaxed);
            const bool valid = result.job_id != 0U &&
                static_cast<uint8_t>(result.state) <=
                    static_cast<uint8_t>(DiagnosticJobState::CANCELLED) &&
                result.request_length <= bored::signalscope::kDiagnosticRequestMaxPayload &&
                result.response_length <= bored::signalscope::kDiagnosticResponseMaxPayload;
            if (!valid) snapshot_reader_failures.fetch_add(1U, std::memory_order_relaxed);
        }
        taskYIELD();
    }
    snapshot_reader_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void testIsoSingleFrame() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.expected_response_sid = 0x62U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit UDS single frame");
    tick();
    require(sent_count == 1U, "one UDS request TX");
    require(bytesEqual(sent[0], 0x70FU,
        {0x03U, 0x22U, 0xF1U, 0x9EU, 0x55U, 0x55U, 0x55U, 0x55U}),
        "captured UDS padding");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A,
        {0x06U, 0x62U, 0xF1U, 0x9EU, '0', '0', '2', 0x55U})),
        "suppress owned UDS response");
    const DiagnosticResult result = readResult(job_id);
    require(result.ok && result.state == DiagnosticJobState::COMPLETE,
            "UDS single frame completes");
}

void testIsoMultiFrame() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x19U, 0x02U, 0xAEU});
    request.expected_response_sid = 0x59U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit UDS multi frame");
    tick();
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A,
        {0x10U, 0x0BU, 0x59U, 0x02U, 0xAEU, 0x00U, 0x41U, 0x06U})),
        "suppress UDS first frame");
    tick();
    require(sent_count == 2U && bytesEqual(sent[1], 0x70FU,
        {0x30U, 0x00U, 0x02U, 0x55U, 0x55U, 0x55U, 0x55U, 0x55U}),
        "captured UDS flow control");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A,
        {0x21U, 0x08U, 0x00U, 0x41U, 0x07U, 0x04U})),
        "suppress UDS consecutive frame");
    const DiagnosticResult result = readResult(job_id);
    require(result.ok && result.response_length == 11U &&
            result.response[3] == 0x00U && result.response[10] == 0x04U,
            "assemble UDS multi frame");
}

void testIsoPendingAndOwnership() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.expected_response_sid = 0x62U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit UDS pending");
    tick();
    require(!bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A, {0x03U, 0x7FU, 0x19U, 0x78U})),
        "preserve wrong-service NRC");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A, {0x03U, 0x7FU, 0x22U, 0x78U})),
        "own matching NRC 78");
    require(readResult(job_id).state == DiagnosticJobState::ACTIVE,
            "NRC 78 remains active");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x779U, Direction::B_TO_A, {0x03U, 0x62U, 0xF1U, 0x9EU})),
        "own final UDS response");
    const DiagnosticResult result = readResult(job_id);
    require(result.ok && result.pending_seen, "complete after NRC 78");
}

void testQueuedCollision() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit collision job");
    require(!bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x70FU, Direction::A_TO_B, {0x03U, 0x22U, 0xF1U, 0x9EU})),
        "preserve external tester request");
    tick();
    const DiagnosticResult result = readResult(job_id);
    require(sent_count == 0U && result.state == DiagnosticJobState::FAILED &&
            result.error == DiagnosticError::EXTERNAL_TESTER_COLLISION,
            "queued external-tester collision");
}

void testCapturedTp20KwpFlow() {
    resetTransport();
    DiagnosticRequest request = kwpRequest({0x18U, 0x02U, 0xFFU, 0x00U});
    request.expected_response_sid = 0x58U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit KWP DTC read");
    tick();
    require(bytesEqual(sent[0], 0x200U,
        {0x0AU, 0xC0U, 0x00U, 0x10U, 0x00U, 0x03U, 0x01U}),
        "captured TP2 setup");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x20AU, Direction::B_TO_A,
        {0x00U, 0xD0U, 0x00U, 0x03U, 0x64U, 0x07U, 0x01U})),
        "own TP2 setup response");
    tick();
    require(bytesEqual(sent[1], 0x764U,
        {0xA0U, 0x0FU, 0x8AU, 0xFFU, 0x32U, 0xFFU}),
        "captured TP2 parameters");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0xA1U, 0x0FU, 0x8AU, 0xFFU, 0x4AU, 0xFFU})),
        "own TP2 parameters response");
    tick();
    require(bytesEqual(sent[2], 0x764U,
        {0x10U, 0x00U, 0x02U, 0x10U, 0x89U}),
        "captured KWP session");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xB1U})), "own session ACK");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0x10U, 0x00U, 0x02U, 0x50U, 0x89U})),
        "own session response");
    tick();
    require(bytesEqual(sent[3], 0x764U, {0xB1U}), "ACK session response");
    tick();
    require(bytesEqual(sent[4], 0x764U,
        {0x11U, 0x00U, 0x04U, 0x18U, 0x02U, 0xFFU, 0x00U}),
        "captured KWP DTC request");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xB2U})), "own DTC ACK");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0x11U, 0x00U, 0x02U, 0x58U, 0x00U})),
        "own DTC response");
    tick();
    require(bytesEqual(sent[5], 0x764U, {0xB2U}), "ACK DTC response");
    tick();
    require(bytesEqual(sent[6], 0x764U, {0xA8U}), "TP2 disconnect");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xA8U})), "own TP2 disconnect response");
    const DiagnosticResult result = readResult(job_id);
    require(result.ok && result.response_length == 2U &&
            result.response[0] == 0x58U && result.response[1] == 0x00U,
            "complete KWP captured flow");
}

void testRepeatedTp20KwpFlow() {
    resetTransport();
    DiagnosticRequest request = kwpRequest({0x21U, 0x02U});
    request.expected_response_sid = 0x61U;
    request.repeat_count = 2U;
    // Longer than the controller's idle-channel tolerance.  The transport
    // must preserve this application settle interval with an A3/A1 channel
    // test instead of reconnecting or allowing the TP2 channel to expire.
    request.repeat_interval_ms = 650U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit repeated KWP measuring block");

    tick();
    require(bytesEqual(sent[0], 0x200U,
        {0x0AU, 0xC0U, 0x00U, 0x10U, 0x00U, 0x03U, 0x01U}),
        "repeated KWP TP2 setup");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x20AU, Direction::B_TO_A,
        {0x00U, 0xD0U, 0x00U, 0x03U, 0x64U, 0x07U, 0x01U})),
        "own repeated KWP setup response");
    tick();
    require(bytesEqual(sent[1], 0x764U,
        {0xA0U, 0x0FU, 0x8AU, 0xFFU, 0x32U, 0xFFU}),
        "repeated KWP parameters");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0xA1U, 0x0FU, 0x8AU, 0xFFU, 0x4AU, 0xFFU})),
        "own repeated KWP parameters response");
    tick();
    require(bytesEqual(sent[2], 0x764U,
        {0x10U, 0x00U, 0x02U, 0x10U, 0x89U}),
        "repeated KWP session request");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xB1U})),
        "own repeated KWP session ACK");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0x10U, 0x00U, 0x02U, 0x50U, 0x89U})),
        "own repeated KWP session response");
    tick();
    require(bytesEqual(sent[3], 0x764U, {0xB1U}),
        "ACK repeated KWP session response");
    tick();
    require(bytesEqual(sent[4], 0x764U,
        {0x11U, 0x00U, 0x02U, 0x21U, 0x02U}),
        "first repeated KWP request");

    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xB2U})),
        "own first repeated KWP ACK");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0x11U, 0x00U, 0x05U, 0x61U, 0x02U, 0x0EU, 0xC8U, 0x06U})),
        "own first repeated KWP response");
    tick();
    require(bytesEqual(sent[5], 0x764U, {0xB2U}),
        "ACK first repeated KWP response");
    DiagnosticResult progress = readResult(job_id);
    require(progress.state == DiagnosticJobState::ACTIVE &&
            progress.completed_requests == 1U &&
            progress.response_length == 5U && progress.response[4] == 0x06U,
        "publish first repeated KWP result while channel remains active");

    tick(350U);
    require(bytesEqual(sent[6], 0x764U, {0xA3U}),
        "channel test preserves repeated KWP settle interval");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0xA1U, 0x0FU, 0x8AU, 0xFFU, 0x4AU, 0xFFU})),
        "own repeated KWP channel-test response");

    tick(300U);
    require(bytesEqual(sent[7], 0x764U,
        {0x12U, 0x00U, 0x02U, 0x21U, 0x02U}),
        "second KWP request reuses negotiated channel");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xB3U})),
        "own second repeated KWP ACK");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A,
        {0x12U, 0x00U, 0x05U, 0x61U, 0x02U, 0x0EU, 0xC8U, 0x07U})),
        "own second repeated KWP response");
    tick();
    require(bytesEqual(sent[8], 0x764U, {0xB3U}),
        "ACK second repeated KWP response");
    tick();
    require(bytesEqual(sent[9], 0x764U, {0xA8U}),
        "disconnect only after final repeated KWP response");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
        0x300U, Direction::B_TO_A, {0xA8U})),
        "own repeated KWP disconnect response");

    const DiagnosticResult result = readResult(job_id);
    require(result.ok && result.state == DiagnosticJobState::COMPLETE &&
            result.completed_requests == 2U &&
            result.response_length == 5U && result.response[4] == 0x07U,
        "complete repeated KWP flow on one TP2 channel");
}

void testCrossCoreSnapshots() {
    resetTransport();
    snapshot_reader_stop.store(false, std::memory_order_release);
    snapshot_reader_done.store(false, std::memory_order_release);
    snapshot_reader_reads.store(0U, std::memory_order_release);
    snapshot_reader_failures.store(0U, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(
        snapshotReader, "diag_snapshot_reader", 4096, nullptr, 1,
        &snapshot_reader_task, 0);
    require(created == pdPASS, "start cross-core snapshot reader");
    if (created != pdPASS) return;

    for (uint16_t iteration = 0U; iteration < 250U; ++iteration) {
        // The fake driver keeps only the current write; this stress loop is
        // testing snapshot publication rather than accumulating a CAN log.
        sent_count = 0U;
        DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
        request.expected_response_sid = 0x62U;
        uint32_t job_id = 0U;
        require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
                "snapshot stress submit");
        tick();
        const uint8_t marker = static_cast<uint8_t>(iteration & 0xFFU);
        require(bored::signalscope::diagnosticTransportObservePhysicalFrame(makeFrame(
            0x779U, Direction::B_TO_A,
            {0x04U, 0x62U, 0xF1U, 0x9EU, marker})),
            "snapshot stress response");
        require(readResult(job_id).state == DiagnosticJobState::COMPLETE,
                "snapshot stress completion");
    }

    delay(50);
    snapshot_reader_stop.store(true, std::memory_order_release);
    for (uint16_t wait = 0U; wait < 100U &&
         !snapshot_reader_done.load(std::memory_order_acquire); ++wait) delay(1);
    require(snapshot_reader_done.load(std::memory_order_acquire),
            "cross-core snapshot reader stopped");
    require(snapshot_reader_reads.load(std::memory_order_acquire) > 0U,
            "cross-core snapshot reader exercised");
    require(snapshot_reader_failures.load(std::memory_order_acquire) == 0U,
            "cross-core snapshots remain coherent");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("DIAGNOSTIC_TRANSPORT_TESTS_BEGIN");

    testPendingTxReservationScope();
    testIsoSingleFrame();
    testIsoMultiFrame();
    testIsoPendingAndOwnership();
    testQueuedCollision();
    testCapturedTp20KwpFlow();
    testRepeatedTp20KwpFlow();
    testCrossCoreSnapshots();

    if (failures == 0U) {
        Serial.printf("DIAGNOSTIC_TRANSPORT_TESTS_PASS snapshots=%lu\n",
            static_cast<unsigned long>(snapshot_reader_reads.load(std::memory_order_acquire)));
    } else {
        Serial.printf("DIAGNOSTIC_TRANSPORT_TESTS_FAIL count=%lu snapshots=%lu snapshot_failures=%lu\n",
            static_cast<unsigned long>(failures),
            static_cast<unsigned long>(snapshot_reader_reads.load(std::memory_order_acquire)),
            static_cast<unsigned long>(snapshot_reader_failures.load(std::memory_order_acquire)));
    }
}

void loop() {
    delay(1000);
}
