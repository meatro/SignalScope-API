#include "core/diagnostic_transport.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using bored::signalscope::CanFrame;
using bored::signalscope::DiagnosticError;
using bored::signalscope::DiagnosticJobState;
using bored::signalscope::DiagnosticProtocol;
using bored::signalscope::DiagnosticRequest;
using bored::signalscope::DiagnosticResult;
using bored::signalscope::Direction;

namespace {

std::vector<CanFrame> transmitted;
uint32_t now_ms = 1U;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

bool captureTx(Direction direction, const CanFrame& frame) {
    CanFrame copy = frame;
    copy.direction = direction;
    transmitted.push_back(copy);
    return true;
}

void resetTransport() {
    transmitted.clear();
    now_ms = 1U;
    bored::signalscope::diagnosticTransportInit(captureTx);
}

void tick(uint32_t advance_ms = 1U) {
    now_ms += advance_ms;
    bored::signalscope::diagnosticTransportTick(now_ms * 1000U, now_ms);
}

CanFrame frame(uint32_t id, Direction direction, std::initializer_list<uint8_t> bytes) {
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

void requireBytes(const CanFrame& actual, uint32_t id,
                  std::initializer_list<uint8_t> expected, const std::string& label) {
    require(actual.id == id, label + " CAN ID");
    require(actual.dlc == expected.size(), label + " DLC");
    size_t index = 0U;
    for (uint8_t byte : expected) {
        require(actual.data[index] == byte, label + " byte " + std::to_string(index));
        ++index;
    }
}

DiagnosticResult resultFor(uint32_t job_id) {
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
            "successful request TX releases the reservation while awaiting response");

    resetTransport();
    request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.route.request_direction = Direction::B_TO_A;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id),
            "submit reverse-direction pending-TX scope job");
    require(bored::signalscope::diagnosticTransportTxPending(Direction::B_TO_A) &&
            !bored::signalscope::diagnosticTransportTxPending(Direction::A_TO_B),
            "reservation follows the actual request direction");
}

void testIsoTpSingleFrame() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.expected_response_sid = 0x62U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id), "submit UDS SF");
    tick();
    require(transmitted.size() == 1U, "one UDS request frame");
    requireBytes(transmitted[0], 0x70FU,
        {0x03U, 0x22U, 0xF1U, 0x9EU, 0x55U, 0x55U, 0x55U, 0x55U}, "UDS request");

    const CanFrame response = frame(0x779U, Direction::B_TO_A,
        {0x06U, 0x62U, 0xF1U, 0x9EU, '0', '0', '2', 0x55U});
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(response),
            "owned UDS SF response suppressed");
    const DiagnosticResult result = resultFor(job_id);
    require(result.state == DiagnosticJobState::COMPLETE && result.ok, "UDS SF completes");
    require(result.response_length == 6U && result.response[0] == 0x62U, "UDS SF payload");
}

void testIsoTpMultiFrame() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x19U, 0x02U, 0xAEU});
    request.expected_response_sid = 0x59U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id), "submit UDS MF");
    tick();

    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x779U, Direction::B_TO_A,
        {0x10U, 0x0BU, 0x59U, 0x02U, 0xAEU, 0x50U, 0xA7U, 0x00U})),
        "owned UDS first frame suppressed");
    tick();
    require(transmitted.size() == 2U, "flow control transmitted on following tick");
    requireBytes(transmitted[1], 0x70FU,
        {0x30U, 0x00U, 0x02U, 0x55U, 0x55U, 0x55U, 0x55U, 0x55U}, "UDS flow control");

    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x779U, Direction::B_TO_A, {0x21U, 0x08U, 0x50U, 0xA8U, 0x00U, 0x04U})),
        "owned UDS consecutive frame suppressed");
    const DiagnosticResult result = resultFor(job_id);
    require(result.state == DiagnosticJobState::COMPLETE && result.response_length == 11U,
            "UDS multi-frame completes");
    require(result.response[3] == 0x50U && result.response[10] == 0x04U,
            "UDS multi-frame payload assembled");
}

void testIsoPendingAndOwnership() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    request.expected_response_sid = 0x62U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id), "submit UDS pending");
    tick();

    require(!bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x779U, Direction::B_TO_A, {0x03U, 0x7FU, 0x19U, 0x78U})),
        "wrong-service NRC is not stolen");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x779U, Direction::B_TO_A, {0x03U, 0x7FU, 0x22U, 0x78U})),
        "matching response-pending NRC is owned");
    require(resultFor(job_id).state == DiagnosticJobState::ACTIVE, "NRC 78 keeps job active");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x779U, Direction::B_TO_A, {0x03U, 0x62U, 0xF1U, 0x9EU})),
        "final UDS response is owned");
    const DiagnosticResult result = resultFor(job_id);
    require(result.state == DiagnosticJobState::COMPLETE && result.pending_seen,
            "positive response completes after NRC 78");
}

void testQueuedCollision() {
    resetTransport();
    DiagnosticRequest request = udsRequest({0x22U, 0xF1U, 0x9EU});
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id), "submit collision job");
    require(!bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x70FU, Direction::A_TO_B, {0x03U, 0x22U, 0xF1U, 0x9EU})),
        "external request remains gateway-owned");
    tick();
    require(transmitted.empty(), "queued collision prevents internal transmission");
    const DiagnosticResult result = resultFor(job_id);
    require(result.state == DiagnosticJobState::FAILED &&
            result.error == DiagnosticError::EXTERNAL_TESTER_COLLISION,
            "queued collision reports explicit failure");
}

void testCapturedTp20KwpFlow() {
    resetTransport();
    DiagnosticRequest request = kwpRequest({0x18U, 0x02U, 0xFFU, 0x00U});
    request.expected_response_sid = 0x58U;
    uint32_t job_id = 0U;
    require(bored::signalscope::diagnosticTransportSubmit(request, &job_id), "submit KWP DTC read");

    tick();
    requireBytes(transmitted.at(0), 0x200U,
        {0x0AU, 0xC0U, 0x00U, 0x10U, 0x00U, 0x03U, 0x01U}, "TP2 setup");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x20AU, Direction::B_TO_A, {0x00U, 0xD0U, 0x00U, 0x03U, 0x64U, 0x07U, 0x01U})),
        "TP2 setup response owned");

    tick();
    requireBytes(transmitted.at(1), 0x764U,
        {0xA0U, 0x0FU, 0x8AU, 0xFFU, 0x32U, 0xFFU}, "TP2 parameters");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0xA1U, 0x0FU, 0x8AU, 0xFFU, 0x4AU, 0xFFU})),
        "TP2 parameter response owned");

    tick();
    requireBytes(transmitted.at(2), 0x764U, {0x10U, 0x00U, 0x02U, 0x10U, 0x89U},
                 "KWP session request");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0xB1U})), "KWP session request ACK owned");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0x10U, 0x00U, 0x02U, 0x50U, 0x89U})),
        "KWP session response owned");

    tick();
    requireBytes(transmitted.at(3), 0x764U, {0xB1U}, "KWP session response ACK");
    tick();
    requireBytes(transmitted.at(4), 0x764U,
        {0x11U, 0x00U, 0x04U, 0x18U, 0x02U, 0xFFU, 0x00U}, "KWP DTC request");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0xB2U})), "KWP DTC request ACK owned");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0x11U, 0x00U, 0x02U, 0x58U, 0x00U})),
        "KWP DTC response owned");

    tick();
    requireBytes(transmitted.at(5), 0x764U, {0xB2U}, "KWP DTC response ACK");
    tick();
    requireBytes(transmitted.at(6), 0x764U, {0xA8U}, "TP2 disconnect");
    require(bored::signalscope::diagnosticTransportObservePhysicalFrame(frame(
        0x300U, Direction::B_TO_A, {0xA8U})), "TP2 disconnect response owned");

    const DiagnosticResult result = resultFor(job_id);
    require(result.state == DiagnosticJobState::COMPLETE && result.ok, "KWP DTC read completes");
    require(result.response_length == 2U && result.response[0] == 0x58U && result.response[1] == 0x00U,
            "KWP DTC payload preserved through disconnect");
}

}  // namespace

int main() {
    testPendingTxReservationScope();
    testIsoTpSingleFrame();
    testIsoTpMultiFrame();
    testIsoPendingAndOwnership();
    testQueuedCollision();
    testCapturedTp20KwpFlow();
    std::cout << "diagnostic transport host tests passed\n";
    return 0;
}
