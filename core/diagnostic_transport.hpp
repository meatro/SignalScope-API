#pragma once

#include <cstddef>
#include <cstdint>

#include "types.hpp"

namespace bored::signalscope {

constexpr size_t kDiagnosticRequestMaxPayload = 32U;
constexpr size_t kDiagnosticResponseMaxPayload = 512U;

enum class DiagnosticProtocol : uint8_t {
    ISO_TP = 0,
    VW_TP20 = 1,
};

enum class DiagnosticJobState : uint8_t {
    IDLE = 0,
    QUEUED,
    ACTIVE,
    COMPLETE,
    FAILED,
    CANCELLED,
};

enum class DiagnosticError : uint8_t {
    NONE = 0,
    NOT_INITIALIZED,
    BUSY,
    INVALID_REQUEST,
    UNSUPPORTED_REQUEST_LENGTH,
    TX_FAILED,
    TIMEOUT,
    EXTERNAL_TESTER_COLLISION,
    UNEXPECTED_RESPONSE,
    NEGATIVE_RESPONSE,
    RESPONSE_TOO_LARGE,
    SEQUENCE_ERROR,
    TP20_SETUP_REJECTED,
    TP20_NEGOTIATION_FAILED,
    CANCELLED,
    ABORTED,
};

struct DiagnosticRoute {
    DiagnosticProtocol protocol = DiagnosticProtocol::ISO_TP;
    Direction request_direction = Direction::A_TO_B;
    uint32_t request_can_id = 0U;
    uint32_t response_can_id = 0U;
    bool extended = false;
    bool iso_tp_pad_to_eight = true;
    uint8_t iso_tp_tx_padding = 0x55U;
    uint8_t iso_tp_flow_control_block_size = 0x00U;
    uint8_t iso_tp_flow_control_st_min = 0x02U;

    // VW TP2.0 channel setup. Dynamic data-channel IDs are negotiated for
    // each job; request_can_id/response_can_id describe the setup channel.
    uint8_t tp20_logical_address = 0x0AU;
    uint32_t tp20_requested_ecu_tx_id = 0x300U;
    uint8_t tp20_application_type = 0x01U;
    uint8_t kwp_session = 0x89U;
    bool enter_kwp_session = true;
};

struct DiagnosticRequest {
    DiagnosticRoute route{};
    uint8_t payload[kDiagnosticRequestMaxPayload] = {0};
    uint8_t payload_length = 0U;
    uint8_t expected_response_sid = 0U;
    uint32_t timeout_ms = 3000U;
    bool suppress_matching_response = true;
    // VW TP2.0 only: repeat the same KWP payload on the negotiated channel.
    // The first response is published with completed_requests=1 while the
    // job remains ACTIVE; the final response is retained in COMPLETE.
    uint8_t repeat_count = 1U;
    uint16_t repeat_interval_ms = 0U;
};

struct DiagnosticResult {
    uint32_t job_id = 0U;
    DiagnosticJobState state = DiagnosticJobState::IDLE;
    DiagnosticError error = DiagnosticError::NONE;
    DiagnosticRoute route{};

    bool ok = false;
    bool timeout = false;
    bool negative = false;
    bool pending_seen = false;
    bool response_suppressed = false;

    uint8_t request_sid = 0U;
    uint8_t response_sid = 0U;
    uint8_t negative_service_sid = 0U;
    uint8_t nrc = 0U;

    uint8_t request[kDiagnosticRequestMaxPayload] = {0};
    uint8_t request_length = 0U;
    uint8_t response[kDiagnosticResponseMaxPayload] = {0};
    uint16_t response_length = 0U;

    uint32_t active_request_can_id = 0U;
    uint32_t active_response_can_id = 0U;
    uint32_t started_ms = 0U;
    uint32_t completed_ms = 0U;
    uint32_t elapsed_ms = 0U;
    uint16_t tx_frames = 0U;
    uint16_t rx_frames = 0U;
    uint16_t completed_requests = 0U;
};

struct DiagnosticTransportStats {
    bool initialized = false;
    bool active = false;
    uint32_t active_job_id = 0U;
    uint32_t submitted_jobs = 0U;
    uint32_t completed_jobs = 0U;
    uint32_t failed_jobs = 0U;
    uint32_t cancelled_jobs = 0U;
    uint32_t suppressed_response_frames = 0U;
    uint32_t external_tester_collisions = 0U;
};

// Bounded in-memory trace of frames owned by the diagnostic transport.  It is
// intentionally separate from general CAN logging: applications and focused
// troubleshooting can retain the exact tester/ECU exchange without writing
// flash from the real-time CAN task.
struct DiagnosticTraceFrame {
    uint32_t timestamp_us = 0U;
    uint32_t job_id = 0U;
    uint32_t can_id = 0U;
    Direction direction = Direction::A_TO_B;
    uint8_t dlc = 0U;
    uint8_t data[8] = {0};
    uint8_t phase = 0U;
    bool transmitted = false;
    bool successful = true;
};

using DiagnosticTxDriver = bool (*)(Direction tx_direction, const CanFrame& frame);

// The transport has one bounded command slot and one active job. submit/read
// are safe from the UI core; tick/observe are owned exclusively by the CAN
// core. tick performs at most one CAN transmit per call.
void diagnosticTransportInit(DiagnosticTxDriver tx_driver);
bool diagnosticTransportSubmit(const DiagnosticRequest& request, uint32_t* job_id);
bool diagnosticTransportCancel(uint32_t job_id);
bool diagnosticTransportReadResult(uint32_t job_id, DiagnosticResult* result);
bool diagnosticTransportReadLatest(DiagnosticResult* result);
DiagnosticTransportStats diagnosticTransportStats();
// CAN-task-only scheduling hint. It is true only while a command for this
// physical direction is queued or an actual transport frame is awaiting TX;
// response/session wait time does not reserve a hardware mailbox.
bool diagnosticTransportTxPending(Direction direction);
void diagnosticTransportTraceReset();
size_t diagnosticTransportTraceCount();
uint32_t diagnosticTransportTraceDropped();
bool diagnosticTransportTraceRead(size_t index, DiagnosticTraceFrame* frame);
const char* diagnosticTransportTracePhaseName(uint8_t phase);

// Called only for physical ingress, before the general gateway queue. Returns
// true only for a frame belonging to an internally-originated transaction that
// should not be rebroadcast to the opposite vehicle bus.
bool diagnosticTransportObservePhysicalFrame(const CanFrame& frame);

// Called after live ingress has been forwarded on the CAN task.
void diagnosticTransportTick(uint32_t now_us, uint32_t now_ms);
void diagnosticTransportAbort(DiagnosticError error = DiagnosticError::ABORTED);

const char* diagnosticProtocolName(DiagnosticProtocol protocol);
const char* diagnosticJobStateName(DiagnosticJobState state);
const char* diagnosticErrorName(DiagnosticError error);
const char* diagnosticNrcName(uint8_t nrc);

}  // namespace bored::signalscope
