#include "diagnostic_transport.hpp"

#include <atomic>
#include <cstring>
#include <type_traits>

namespace bored::signalscope {

namespace {

// The diagnostic engine deliberately owns no RTOS object and never waits.  The
// UI core deposits one command in command_mailbox; the CAN core is the only
// owner of ActiveJob and advances it from tick/observe.  This keeps diagnostic
// work out of the gateway's latency-critical forwarding path.
constexpr uint8_t kMailboxEmpty = 0U;
constexpr uint8_t kMailboxReady = 1U;
constexpr uint32_t kMaxCanId = 0x1FFFFFFFU;
constexpr uint32_t kMaxComparableTimeoutMs = 0x7FFFFFFFU;

constexpr uint8_t kIsoTpSingleFrame = 0x00U;
constexpr uint8_t kIsoTpFirstFrame = 0x10U;
constexpr uint8_t kIsoTpConsecutiveFrame = 0x20U;
constexpr uint8_t kIsoTpFlowControlCts = 0x30U;

constexpr uint8_t kNegativeResponseSid = 0x7FU;
constexpr uint8_t kResponsePendingNrc = 0x78U;

constexpr uint8_t kTp20SetupRequest = 0xC0U;
constexpr uint8_t kTp20SetupPositive = 0xD0U;
constexpr uint8_t kTp20ParamsRequest = 0xA0U;
constexpr uint8_t kTp20ParamsResponse = 0xA1U;
constexpr uint8_t kTp20ChannelTestRequest = 0xA3U;
constexpr uint8_t kTp20Disconnect = 0xA8U;
constexpr uint32_t kTp20DisconnectTimeoutMs = 50U;
// Captured VAG tester traffic refreshes an idle negotiated channel roughly
// every 500 ms.  Use a conservative cadence so application-level settle
// intervals can be longer without allowing the ECU to expire the channel.
constexpr uint32_t kTp20ChannelTestIntervalMs = 350U;
constexpr uint32_t kTp20ChannelTestTimeoutMs = 200U;
constexpr size_t kDiagnosticTraceCapacity = 512U;
constexpr uint8_t kDiagnosticTxMaximumAttempts = 8U;
constexpr uint32_t kDiagnosticTxRetryDelayMs = 2U;

enum class Phase : uint8_t {
    NONE = 0,
    ISO_SEND_REQUEST,
    ISO_WAIT_RESPONSE,
    ISO_SEND_FLOW_CONTROL,
    ISO_WAIT_CONSECUTIVE,
    TP20_SEND_SETUP,
    TP20_WAIT_SETUP,
    TP20_SEND_PARAMS,
    TP20_WAIT_PARAMS,
    TP20_SEND_SESSION,
    TP20_WAIT_SESSION_ACK,
    TP20_WAIT_SESSION_RESPONSE,
    TP20_SEND_REQUEST,
    TP20_WAIT_REQUEST_ACK,
    TP20_WAIT_REQUEST_RESPONSE,
    TP20_SEND_ACK,
    TP20_WAIT_REPEAT,
    TP20_SEND_CHANNEL_TEST,
    TP20_WAIT_CHANNEL_TEST,
    TP20_SEND_DISCONNECT,
    TP20_WAIT_DISCONNECT,
};

enum class TxAction : uint8_t {
    NONE = 0,
    ISO_REQUEST,
    ISO_FLOW_CONTROL,
    TP20_SETUP,
    TP20_PARAMS,
    TP20_SESSION,
    TP20_REQUEST,
    TP20_ACK,
    TP20_CHANNEL_TEST,
    TP20_DISCONNECT,
};

enum class AfterAck : uint8_t {
    NONE = 0,
    WAIT_SESSION_RESPONSE,
    WAIT_REQUEST_RESPONSE,
    SESSION_COMPLETE,
    REQUEST_PENDING,
    REQUEST_COMPLETE,
    REQUEST_FAILED,
    SESSION_FAILED,
};

struct CommandMailbox {
    DiagnosticRequest request{};
    uint32_t job_id = 0U;
};

struct TerminalIntent {
    bool valid = false;
    DiagnosticJobState state = DiagnosticJobState::FAILED;
    DiagnosticError error = DiagnosticError::ABORTED;
};

struct ActiveJob {
    bool active = false;
    DiagnosticRequest request{};
    DiagnosticResult result{};
    Phase phase = Phase::NONE;

    CanFrame pending_tx{};
    TxAction pending_tx_action = TxAction::NONE;
    bool pending_tx_valid = false;
    uint8_t pending_tx_attempts = 0U;
    uint32_t pending_tx_retry_not_before_ms = 0U;

    uint32_t deadline_ms = 0U;

    // ISO-TP receive assembly.
    uint16_t iso_total_length = 0U;
    uint8_t iso_next_sequence = 1U;

    // TP2.0 negotiated channel and KWP sequencing.
    bool tp20_channel_open = false;
    uint32_t tp20_tester_tx_id = 0U;
    uint32_t tp20_ecu_tx_id = 0U;
    uint8_t tp20_tx_sequence = 0U;
    uint8_t tp20_expected_ack_sequence = 0U;
    uint8_t tp20_expected_rx_sequence = 0U;
    bool tp20_rx_started = false;
    uint16_t tp20_rx_total_length = 0U;
    uint32_t tp20_disconnect_deadline_ms = 0U;
    uint32_t tp20_repeat_not_before_ms = 0U;
    uint32_t tp20_channel_test_not_before_ms = 0U;
    uint32_t tp20_channel_test_deadline_ms = 0U;
    AfterAck after_ack = AfterAck::NONE;

    TerminalIntent terminal{};
};

std::atomic<uint8_t> initialized{0U};
std::atomic<uint8_t> command_state{kMailboxEmpty};
std::atomic<uint8_t> busy{0U};
std::atomic<uint32_t> next_job_id{1U};
std::atomic<uint32_t> cancel_job_id{0U};
std::atomic<uint32_t> abort_job_id{0U};
std::atomic<uint8_t> abort_error{static_cast<uint8_t>(DiagnosticError::ABORTED)};

CommandMailbox command_mailbox{};
DiagnosticTxDriver tx_driver = nullptr;
ActiveJob job{};
uint32_t last_now_ms = 0U;

// Atomic words make the ~600-byte result formally race-free.  The sequence
// counter prevents readers from accepting a mixture of two publications, and
// neither side ever waits for the other or takes a CAN-core lock.
static_assert(std::is_trivially_copyable<DiagnosticResult>::value,
              "DiagnosticResult snapshots require a trivially-copyable value");
constexpr size_t kResultWordCount =
    (sizeof(DiagnosticResult) + sizeof(uint32_t) - 1U) / sizeof(uint32_t);
std::atomic<uint32_t> result_sequence{0U};
std::atomic<uint32_t> result_words[kResultWordCount]{};

std::atomic<uint32_t> stats_active_job_id{0U};
std::atomic<uint32_t> stats_submitted_jobs{0U};
std::atomic<uint32_t> stats_completed_jobs{0U};
std::atomic<uint32_t> stats_failed_jobs{0U};
std::atomic<uint32_t> stats_cancelled_jobs{0U};
std::atomic<uint32_t> stats_suppressed_frames{0U};
std::atomic<uint32_t> stats_external_collisions{0U};

DiagnosticTraceFrame diagnostic_trace[kDiagnosticTraceCapacity]{};
std::atomic<uint32_t> diagnostic_trace_total{0U};

void recordDiagnosticTraceFrame(
    const CanFrame& frame, bool transmitted, bool successful,
    uint32_t job_id, Phase phase) {
    const uint32_t total = diagnostic_trace_total.load(std::memory_order_relaxed);
    const size_t index = static_cast<size_t>(total % kDiagnosticTraceCapacity);
    DiagnosticTraceFrame entry{};
    entry.timestamp_us = frame.timestamp_us;
    entry.job_id = job_id;
    entry.can_id = frame.id;
    entry.direction = frame.direction;
    entry.dlc = frame.dlc <= 8U ? frame.dlc : 8U;
    std::memcpy(entry.data, frame.data, entry.dlc);
    entry.phase = static_cast<uint8_t>(phase);
    entry.transmitted = transmitted;
    entry.successful = successful;
    diagnostic_trace[index] = entry;
    diagnostic_trace_total.store(total + 1U, std::memory_order_release);
}

Direction oppositeDirection(Direction direction) {
    return (direction == Direction::A_TO_B) ? Direction::B_TO_A : Direction::A_TO_B;
}

bool isExtendedId(uint32_t id) {
    return id > 0x7FFU;
}

bool directionValid(Direction direction) {
    return direction == Direction::A_TO_B || direction == Direction::B_TO_A;
}

bool timeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

uint8_t expectedResponseSid(const ActiveJob& active) {
    if (active.request.expected_response_sid != 0U) {
        return active.request.expected_response_sid;
    }
    return static_cast<uint8_t>(active.result.request_sid + 0x40U);
}

bool frameIdFormatMatches(const CanFrame& frame, uint32_t id, bool extended) {
    return frame.id == id && isExtendedId(frame.id) == extended;
}

bool responseFrameMatches(const CanFrame& frame, uint32_t id) {
    return frame.direction == oppositeDirection(job.request.route.request_direction) &&
        frameIdFormatMatches(frame, id, job.request.route.extended);
}

bool requestFrameMatches(const CanFrame& frame, uint32_t id) {
    return frame.direction == job.request.route.request_direction &&
        frameIdFormatMatches(frame, id, job.request.route.extended);
}

void publishResult(const DiagnosticResult& value) {
    uint32_t words[kResultWordCount] = {0U};
    std::memcpy(words, &value, sizeof(value));

    result_sequence.fetch_add(1U, std::memory_order_acq_rel);  // odd: writing
    for (size_t index = 0U; index < kResultWordCount; ++index) {
        result_words[index].store(words[index], std::memory_order_relaxed);
    }
    result_sequence.fetch_add(1U, std::memory_order_release);  // even: stable
}

bool copyPublishedResult(DiagnosticResult* out) {
    if (out == nullptr) return false;

    // UI reads are not allowed to make the CAN core wait.  A handful of bounded
    // retries is ample because the writer is just one structure assignment.
    for (uint8_t attempt = 0U; attempt < 8U; ++attempt) {
        const uint32_t before = result_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;

        uint32_t words[kResultWordCount] = {0U};
        for (size_t index = 0U; index < kResultWordCount; ++index) {
            words[index] = result_words[index].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t after = result_sequence.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            DiagnosticResult snapshot{};
            std::memcpy(&snapshot, words, sizeof(snapshot));
            *out = snapshot;
            return true;
        }
    }
    return false;
}

void publishActiveProgress() {
    if (!job.active) return;
    job.result.elapsed_ms = last_now_ms - job.result.started_ms;
    publishResult(job.result);
}

void clearPendingTx() {
    job.pending_tx = {};
    job.pending_tx_action = TxAction::NONE;
    job.pending_tx_valid = false;
    job.pending_tx_attempts = 0U;
    job.pending_tx_retry_not_before_ms = 0U;
}

void resetIsoAssembly() {
    job.iso_total_length = 0U;
    job.iso_next_sequence = 1U;
    job.result.response_length = 0U;
}

void resetTp20Assembly() {
    job.tp20_rx_started = false;
    job.tp20_rx_total_length = 0U;
    job.result.response_length = 0U;
}

void finishImmediately(DiagnosticJobState state, DiagnosticError error) {
    if (!job.active) return;

    clearPendingTx();
    job.phase = Phase::NONE;
    job.result.state = state;
    job.result.error = error;
    job.result.ok = state == DiagnosticJobState::COMPLETE;
    job.result.timeout = error == DiagnosticError::TIMEOUT;
    job.result.completed_ms = last_now_ms;
    job.result.elapsed_ms = last_now_ms - job.result.started_ms;
    publishResult(job.result);

    if (state == DiagnosticJobState::COMPLETE) {
        stats_completed_jobs.fetch_add(1U, std::memory_order_relaxed);
    } else if (state == DiagnosticJobState::CANCELLED) {
        stats_cancelled_jobs.fetch_add(1U, std::memory_order_relaxed);
    } else {
        stats_failed_jobs.fetch_add(1U, std::memory_order_relaxed);
    }

    uint32_t cancelled_id = job.result.job_id;
    static_cast<void>(cancel_job_id.compare_exchange_strong(
        cancelled_id, 0U, std::memory_order_acq_rel));
    uint32_t aborted_id = job.result.job_id;
    static_cast<void>(abort_job_id.compare_exchange_strong(
        aborted_id, 0U, std::memory_order_acq_rel));
    stats_active_job_id.store(0U, std::memory_order_release);
    job = {};
    busy.store(0U, std::memory_order_release);
}

void stageTx(uint32_t can_id, Direction direction, const uint8_t* data, uint8_t length,
             TxAction action, Phase sending_phase) {
    job.pending_tx = {};
    job.pending_tx.id = can_id;
    job.pending_tx.direction = direction;
    job.pending_tx.dlc = length <= 8U ? length : 8U;
    for (uint8_t index = 0U; index < job.pending_tx.dlc; ++index) {
        job.pending_tx.data[index] = data[index];
    }
    job.pending_tx_action = action;
    job.pending_tx_valid = true;
    job.pending_tx_attempts = 0U;
    job.pending_tx_retry_not_before_ms = 0U;
    job.phase = sending_phase;
}

void stageTp20Disconnect(DiagnosticJobState state, DiagnosticError error) {
    job.terminal.valid = true;
    job.terminal.state = state;
    job.terminal.error = error;
    clearPendingTx();
    const uint8_t payload[] = {kTp20Disconnect};
    stageTx(job.tp20_tester_tx_id, job.request.route.request_direction,
            payload, sizeof(payload), TxAction::TP20_DISCONNECT,
            Phase::TP20_SEND_DISCONNECT);
}

void finalizeTerminalIntent() {
    if (!job.active || !job.terminal.valid) return;
    const TerminalIntent terminal = job.terminal;
    job.terminal = {};
    finishImmediately(terminal.state, terminal.error);
}

void requestFinish(DiagnosticJobState state, DiagnosticError error, bool disconnect = true) {
    if (!job.active) return;
    if (disconnect && job.request.route.protocol == DiagnosticProtocol::VW_TP20 &&
        job.tp20_channel_open && job.tp20_tester_tx_id != 0U) {
        stageTp20Disconnect(state, error);
        return;
    }
    finishImmediately(state, error);
}

void fail(DiagnosticError error, bool disconnect = true) {
    requestFinish(DiagnosticJobState::FAILED, error, disconnect);
}

bool noteConsumedResponseFrame() {
    ++job.result.rx_frames;
    const bool suppress = job.request.suppress_matching_response;
    if (suppress) {
        job.result.response_suppressed = true;
        stats_suppressed_frames.fetch_add(1U, std::memory_order_relaxed);
    }
    return suppress;
}

bool responseSidBelongsToRequest(const uint8_t* payload, uint16_t length,
                                 bool session_response) {
    if (payload == nullptr || length == 0U) return false;
    const uint8_t request_sid = session_response ? 0x10U : job.result.request_sid;
    const uint8_t positive_sid = session_response ? 0x50U : expectedResponseSid(job);
    if (payload[0] == positive_sid) return true;
    return length >= 2U && payload[0] == kNegativeResponseSid &&
        payload[1] == request_sid;
}

bool isCurrentPendingResponse(bool session_response) {
    const uint8_t request_sid = session_response ? 0x10U : job.result.request_sid;
    return job.result.response_length >= 3U &&
        job.result.response[0] == kNegativeResponseSid &&
        job.result.response[1] == request_sid &&
        job.result.response[2] == kResponsePendingNrc;
}

bool validateCompletedPayload(bool session_response) {
    const uint16_t length = job.result.response_length;
    if (length == 0U) {
        fail(DiagnosticError::UNEXPECTED_RESPONSE);
        return false;
    }

    job.result.response_sid = job.result.response[0];
    if (length >= 3U && job.result.response[0] == kNegativeResponseSid &&
        job.result.response[1] == (session_response ? 0x10U : job.result.request_sid)) {
        if (isCurrentPendingResponse(session_response)) {
            job.result.pending_seen = true;
            return true;
        }
        job.result.negative = true;
        job.result.negative_service_sid = job.result.response[1];
        job.result.nrc = job.result.response[2];
        return false;
    }

    const uint8_t wanted = session_response
        ? 0x50U
        : expectedResponseSid(job);
    return job.result.response[0] == wanted;
}

void encodeTp20CanId(uint32_t id, bool valid, uint8_t* low, uint8_t* validity_prefix) {
    *low = static_cast<uint8_t>(id & 0xFFU);
    *validity_prefix = static_cast<uint8_t>((id >> 8U) & 0x0FU);
    if (!valid) *validity_prefix |= 0x10U;
}

bool decodeTp20CanId(uint8_t low, uint8_t validity_prefix, uint32_t* id) {
    if ((validity_prefix & 0xF0U) != 0U) return false;
    *id = (static_cast<uint32_t>(validity_prefix & 0x0FU) << 8U) | low;
    return *id != 0U && *id <= 0x7FFU;
}

void stageTp20KwpRequest(bool session_request) {
    const uint8_t* payload = job.request.payload;
    uint8_t length = job.request.payload_length;
    uint8_t session_payload[2] = {0x10U, job.request.route.kwp_session};
    if (session_request) {
        payload = session_payload;
        length = sizeof(session_payload);
    }

    uint8_t frame[8] = {0};
    frame[0] = static_cast<uint8_t>(0x10U | (job.tp20_tx_sequence & 0x0FU));
    frame[1] = 0x00U;
    frame[2] = length;
    for (uint8_t index = 0U; index < length; ++index) frame[index + 3U] = payload[index];

    job.tp20_expected_ack_sequence = static_cast<uint8_t>((job.tp20_tx_sequence + 1U) & 0x0FU);
    job.tp20_tx_sequence = job.tp20_expected_ack_sequence;
    resetTp20Assembly();
    stageTx(job.tp20_tester_tx_id, job.request.route.request_direction,
            frame, static_cast<uint8_t>(length + 3U),
            session_request ? TxAction::TP20_SESSION : TxAction::TP20_REQUEST,
            session_request ? Phase::TP20_SEND_SESSION : Phase::TP20_SEND_REQUEST);
}

void handleKwpPayloadOutcome(bool session_response);

void completeKwpRequest() {
    ++job.result.completed_requests;
    if (job.result.completed_requests >= job.request.repeat_count) {
        // Publish the final response only with the terminal result, after the
        // bounded TP2 disconnect releases the single diagnostic slot. This
        // lets a caller safely submit a fallback discovery request as soon as
        // it observes completion. Intermediate responses remain streamable.
        requestFinish(DiagnosticJobState::COMPLETE, DiagnosticError::NONE);
        return;
    }
    publishActiveProgress();
    job.tp20_repeat_not_before_ms = last_now_ms + job.request.repeat_interval_ms;
    job.tp20_channel_test_not_before_ms = last_now_ms + kTp20ChannelTestIntervalMs;
    job.phase = Phase::TP20_WAIT_REPEAT;
}

void stageTp20ChannelTest() {
    const uint8_t payload[] = {kTp20ChannelTestRequest};
    stageTx(job.tp20_tester_tx_id, job.request.route.request_direction,
            payload, sizeof(payload), TxAction::TP20_CHANNEL_TEST,
            Phase::TP20_SEND_CHANNEL_TEST);
}

void applyAfterAck() {
    const AfterAck action = job.after_ack;
    job.after_ack = AfterAck::NONE;
    switch (action) {
        case AfterAck::WAIT_SESSION_RESPONSE:
            job.phase = Phase::TP20_WAIT_SESSION_RESPONSE;
            break;
        case AfterAck::WAIT_REQUEST_RESPONSE:
            job.phase = Phase::TP20_WAIT_REQUEST_RESPONSE;
            break;
        case AfterAck::SESSION_COMPLETE:
            stageTp20KwpRequest(false);
            break;
        case AfterAck::REQUEST_PENDING:
            resetTp20Assembly();
            job.phase = Phase::TP20_WAIT_REQUEST_RESPONSE;
            break;
        case AfterAck::REQUEST_COMPLETE:
            completeKwpRequest();
            break;
        case AfterAck::REQUEST_FAILED:
            requestFinish(DiagnosticJobState::FAILED,
                job.result.negative ? DiagnosticError::NEGATIVE_RESPONSE
                                    : DiagnosticError::UNEXPECTED_RESPONSE);
            break;
        case AfterAck::SESSION_FAILED:
            requestFinish(DiagnosticJobState::FAILED,
                job.result.negative ? DiagnosticError::NEGATIVE_RESPONSE
                                    : DiagnosticError::TP20_NEGOTIATION_FAILED);
            break;
        case AfterAck::NONE:
        default:
            break;
    }
}

void stageTp20Ack(uint8_t received_sequence, AfterAck after_ack) {
    const uint8_t ack[] = {
        static_cast<uint8_t>(0xB0U | ((received_sequence + 1U) & 0x0FU))
    };
    job.after_ack = after_ack;
    stageTx(job.tp20_tester_tx_id, job.request.route.request_direction,
            ack, sizeof(ack), TxAction::TP20_ACK, Phase::TP20_SEND_ACK);
}

AfterAck kwpOutcomeAfterAck(bool session_response) {
    const bool valid = validateCompletedPayload(session_response);
    if (isCurrentPendingResponse(session_response)) {
        return session_response ? AfterAck::WAIT_SESSION_RESPONSE : AfterAck::REQUEST_PENDING;
    }
    if (!valid) {
        return session_response ? AfterAck::SESSION_FAILED : AfterAck::REQUEST_FAILED;
    }
    return session_response ? AfterAck::SESSION_COMPLETE : AfterAck::REQUEST_COMPLETE;
}

void handleKwpPayloadOutcome(bool session_response) {
    const AfterAck outcome = kwpOutcomeAfterAck(session_response);
    if (outcome == AfterAck::WAIT_SESSION_RESPONSE) {
        resetTp20Assembly();
        job.phase = Phase::TP20_WAIT_SESSION_RESPONSE;
    } else if (outcome == AfterAck::REQUEST_PENDING) {
        resetTp20Assembly();
        job.phase = Phase::TP20_WAIT_REQUEST_RESPONSE;
    } else if (outcome == AfterAck::SESSION_COMPLETE) {
        stageTp20KwpRequest(false);
    } else if (outcome == AfterAck::REQUEST_COMPLETE) {
        completeKwpRequest();
    } else if (outcome == AfterAck::SESSION_FAILED) {
        requestFinish(DiagnosticJobState::FAILED,
            job.result.negative ? DiagnosticError::NEGATIVE_RESPONSE
                                : DiagnosticError::TP20_NEGOTIATION_FAILED);
    } else {
        requestFinish(DiagnosticJobState::FAILED,
            job.result.negative ? DiagnosticError::NEGATIVE_RESPONSE
                                : DiagnosticError::UNEXPECTED_RESPONSE);
    }
}

void handleSuccessfulTx(TxAction action) {
    ++job.result.tx_frames;
    switch (action) {
        case TxAction::ISO_REQUEST:
            job.phase = Phase::ISO_WAIT_RESPONSE;
            break;
        case TxAction::ISO_FLOW_CONTROL:
            job.phase = Phase::ISO_WAIT_CONSECUTIVE;
            break;
        case TxAction::TP20_SETUP:
            job.phase = Phase::TP20_WAIT_SETUP;
            break;
        case TxAction::TP20_PARAMS:
            job.phase = Phase::TP20_WAIT_PARAMS;
            break;
        case TxAction::TP20_SESSION:
            job.phase = Phase::TP20_WAIT_SESSION_ACK;
            break;
        case TxAction::TP20_REQUEST:
            job.phase = Phase::TP20_WAIT_REQUEST_ACK;
            break;
        case TxAction::TP20_ACK:
            applyAfterAck();
            break;
        case TxAction::TP20_CHANNEL_TEST:
            job.phase = Phase::TP20_WAIT_CHANNEL_TEST;
            job.tp20_channel_test_deadline_ms =
                last_now_ms + kTp20ChannelTestTimeoutMs;
            break;
        case TxAction::TP20_DISCONNECT:
            // The ECU echoes A8 on its negotiated data ID.  Keep ownership of
            // the channel briefly so that reply is consumed instead of being
            // leaked through the gateway to the other physical bus.
            job.phase = Phase::TP20_WAIT_DISCONNECT;
            job.tp20_disconnect_deadline_ms =
                last_now_ms + kTp20DisconnectTimeoutMs;
            break;
        case TxAction::NONE:
        default:
            break;
    }
}

void transmitOnePendingFrame(uint32_t now_us) {
    if (!job.active || !job.pending_tx_valid || tx_driver == nullptr) return;
    if (!timeReached(last_now_ms, job.pending_tx_retry_not_before_ms)) return;

    CanFrame frame = job.pending_tx;
    const TxAction action = job.pending_tx_action;
    frame.timestamp_us = now_us;

    if (!tx_driver(frame.direction, frame)) {
        recordDiagnosticTraceFrame(
            frame, true, false, job.result.job_id, job.phase);
        ++job.pending_tx_attempts;
        if (job.pending_tx_attempts < kDiagnosticTxMaximumAttempts) {
            job.pending_tx_retry_not_before_ms =
                last_now_ms + kDiagnosticTxRetryDelayMs;
            publishActiveProgress();
            return;
        }
        clearPendingTx();
        if (action == TxAction::TP20_DISCONNECT && job.terminal.valid) {
            finalizeTerminalIntent();
        } else {
            // If a TP2 channel was already negotiated, release it before
            // publishing failure so a bounded application retry is not
            // rejected as "channel already active" by the ECU.
            requestFinish(DiagnosticJobState::FAILED, DiagnosticError::TX_FAILED);
        }
        return;
    }
    clearPendingTx();
    recordDiagnosticTraceFrame(
        frame, true, true, job.result.job_id, job.phase);
    handleSuccessfulTx(action);
    publishActiveProgress();
}

void beginJob(const CommandMailbox& command, uint32_t now_ms) {
    job = {};
    job.active = true;
    job.request = command.request;
    job.result.job_id = command.job_id;
    job.result.state = DiagnosticJobState::ACTIVE;
    job.result.error = DiagnosticError::NONE;
    job.result.route = command.request.route;
    job.result.request_sid = command.request.payload[0];
    job.result.request_length = command.request.payload_length;
    std::memcpy(job.result.request, command.request.payload, command.request.payload_length);
    job.result.active_request_can_id = command.request.route.request_can_id;
    job.result.active_response_can_id = command.request.route.response_can_id;
    job.result.started_ms = now_ms;
    job.deadline_ms = now_ms + command.request.timeout_ms;
    stats_active_job_id.store(command.job_id, std::memory_order_release);

    if (command.request.route.protocol == DiagnosticProtocol::ISO_TP) {
        uint8_t frame[8];
        std::memset(frame, command.request.route.iso_tp_tx_padding, sizeof(frame));
        frame[0] = static_cast<uint8_t>(command.request.payload_length & 0x0FU);
        std::memcpy(&frame[1], command.request.payload, command.request.payload_length);
        const uint8_t frame_length = command.request.route.iso_tp_pad_to_eight
            ? 8U : static_cast<uint8_t>(command.request.payload_length + 1U);
        stageTx(command.request.route.request_can_id,
                command.request.route.request_direction, frame, frame_length,
                TxAction::ISO_REQUEST, Phase::ISO_SEND_REQUEST);
    } else {
        uint8_t rx_low = 0U;
        uint8_t rx_prefix = 0U;
        uint8_t tx_low = 0U;
        uint8_t tx_prefix = 0U;
        encodeTp20CanId(0U, false, &rx_low, &rx_prefix);
        encodeTp20CanId(command.request.route.tp20_requested_ecu_tx_id,
                        true, &tx_low, &tx_prefix);
        const uint8_t setup[] = {
            command.request.route.tp20_logical_address,
            kTp20SetupRequest,
            rx_low,
            rx_prefix,
            tx_low,
            tx_prefix,
            command.request.route.tp20_application_type,
        };
        stageTx(command.request.route.request_can_id,
                command.request.route.request_direction, setup, sizeof(setup),
                TxAction::TP20_SETUP, Phase::TP20_SEND_SETUP);
    }
    publishResult(job.result);
}

bool requestValid(const DiagnosticRequest& request) {
    if (!directionValid(request.route.request_direction) ||
        request.payload_length == 0U ||
        request.payload_length > kDiagnosticRequestMaxPayload ||
        request.timeout_ms == 0U || request.timeout_ms > kMaxComparableTimeoutMs ||
        request.repeat_count == 0U ||
        request.route.request_can_id == 0U || request.route.response_can_id == 0U ||
        request.route.request_can_id > kMaxCanId || request.route.response_can_id > kMaxCanId) {
        return false;
    }

    // CanFrame carries the ID format implicitly, as all existing drivers do.
    // Reject ambiguous "29-bit format with an 11-bit value" routes.
    if (request.route.extended) {
        if (!isExtendedId(request.route.request_can_id) ||
            !isExtendedId(request.route.response_can_id)) return false;
    } else if (isExtendedId(request.route.request_can_id) ||
               isExtendedId(request.route.response_can_id)) {
        return false;
    }

    if (request.route.protocol == DiagnosticProtocol::ISO_TP) {
        return request.payload_length <= 7U && request.repeat_count == 1U;
    }
    if (request.route.protocol != DiagnosticProtocol::VW_TP20 ||
        request.route.extended || request.payload_length > 5U ||
        request.route.tp20_requested_ecu_tx_id == 0U ||
        request.route.tp20_requested_ecu_tx_id > 0x7FFU ||
        (request.repeat_count > 1U && request.repeat_interval_ms == 0U)) {
        return false;
    }
    return true;
}

bool looksLikeTp20ExternalRequest(const CanFrame& frame) {
    if (frame.direction != job.request.route.request_direction || isExtendedId(frame.id)) return false;

    if (frame.id == job.request.route.request_can_id && frame.dlc >= 2U &&
        frame.data[0] == job.request.route.tp20_logical_address &&
        frame.data[1] == kTp20SetupRequest) {
        return true;
    }
    if (job.tp20_tester_tx_id != 0U && frame.id == job.tp20_tester_tx_id && frame.dlc > 0U) {
        return true;
    }
    return false;
}

bool collidesWithQueuedRequest(const CanFrame& frame, const DiagnosticRequest& request) {
    const DiagnosticRoute& route = request.route;
    if (frame.direction != route.request_direction ||
        !frameIdFormatMatches(frame, route.request_can_id, route.extended)) {
        return false;
    }
    if (route.protocol == DiagnosticProtocol::ISO_TP) return true;
    return !route.extended && frame.dlc >= 2U &&
        frame.data[0] == route.tp20_logical_address &&
        frame.data[1] == kTp20SetupRequest;
}

void externalTesterCollision() {
    stats_external_collisions.fetch_add(1U, std::memory_order_relaxed);
    // Once another tester is present, send nothing else (including a TP2.0
    // disconnect) because ownership of the dynamic channel is now ambiguous.
    finishImmediately(DiagnosticJobState::FAILED,
                      DiagnosticError::EXTERNAL_TESTER_COLLISION);
}

bool completeIsoPayloadOrContinuePending() {
    const bool valid = validateCompletedPayload(false);
    if (isCurrentPendingResponse(false)) {
        resetIsoAssembly();
        job.phase = Phase::ISO_WAIT_RESPONSE;
        publishActiveProgress();
        return false;
    }
    if (!valid) {
        fail(job.result.negative ? DiagnosticError::NEGATIVE_RESPONSE
                                 : DiagnosticError::UNEXPECTED_RESPONSE);
        return true;
    }
    requestFinish(DiagnosticJobState::COMPLETE, DiagnosticError::NONE);
    return true;
}

bool observeIsoResponse(const CanFrame& frame) {
    if (!responseFrameMatches(frame, job.request.route.response_can_id) || frame.dlc == 0U) {
        return false;
    }

    if (job.phase == Phase::ISO_WAIT_RESPONSE) {
        const uint8_t pci = frame.data[0] & 0xF0U;
        if (pci == kIsoTpSingleFrame) {
            const uint8_t length = frame.data[0] & 0x0FU;
            if (length == 0U || length > 7U || frame.dlc < static_cast<uint8_t>(length + 1U)) {
                return false;
            }
            // Fixed UDS response IDs may be shared by an external tester.  A
            // transport-shaped frame is ours only when its first service byte
            // is the expected positive SID or a negative response for our SID.
            if (!responseSidBelongsToRequest(&frame.data[1], length, false)) return false;
            std::memcpy(job.result.response, &frame.data[1], length);
            job.result.response_length = length;
            const bool suppress = noteConsumedResponseFrame();
            completeIsoPayloadOrContinuePending();
            return suppress;
        }
        if (pci == kIsoTpFirstFrame && frame.dlc >= 2U) {
            if (frame.dlc < 3U ||
                !responseSidBelongsToRequest(&frame.data[2],
                    static_cast<uint16_t>(frame.dlc - 2U), false)) {
                return false;
            }
            const uint16_t total = static_cast<uint16_t>(
                (static_cast<uint16_t>(frame.data[0] & 0x0FU) << 8U) | frame.data[1]);
            if (total == 0U || total > kDiagnosticResponseMaxPayload) {
                const bool suppress = noteConsumedResponseFrame();
                fail(DiagnosticError::RESPONSE_TOO_LARGE);
                return suppress;
            }
            job.iso_total_length = total;
            job.iso_next_sequence = 1U;
            job.result.response_length = 0U;
            for (uint8_t index = 2U; index < frame.dlc &&
                 job.result.response_length < total; ++index) {
                job.result.response[job.result.response_length++] = frame.data[index];
            }
            const bool suppress = noteConsumedResponseFrame();
            uint8_t flow_control[8];
            std::memset(flow_control, job.request.route.iso_tp_tx_padding,
                        sizeof(flow_control));
            flow_control[0] = kIsoTpFlowControlCts;
            flow_control[1] = job.request.route.iso_tp_flow_control_block_size;
            flow_control[2] = job.request.route.iso_tp_flow_control_st_min;
            const uint8_t flow_control_length = job.request.route.iso_tp_pad_to_eight ? 8U : 3U;
            stageTx(job.request.route.request_can_id,
                    job.request.route.request_direction, flow_control,
                    flow_control_length, TxAction::ISO_FLOW_CONTROL,
                    Phase::ISO_SEND_FLOW_CONTROL);
            publishActiveProgress();
            return suppress;
        }
        return false;
    }

    if (job.phase != Phase::ISO_WAIT_CONSECUTIVE ||
        (frame.data[0] & 0xF0U) != kIsoTpConsecutiveFrame) {
        return false;
    }

    const bool suppress = noteConsumedResponseFrame();
    const uint8_t sequence = frame.data[0] & 0x0FU;
    if (sequence != job.iso_next_sequence) {
        fail(DiagnosticError::SEQUENCE_ERROR);
        return suppress;
    }
    job.iso_next_sequence = static_cast<uint8_t>((job.iso_next_sequence + 1U) & 0x0FU);
    for (uint8_t index = 1U; index < frame.dlc &&
         job.result.response_length < job.iso_total_length; ++index) {
        job.result.response[job.result.response_length++] = frame.data[index];
    }
    if (job.result.response_length >= job.iso_total_length) {
        completeIsoPayloadOrContinuePending();
    } else {
        publishActiveProgress();
    }
    return suppress;
}

bool observeTp20KwpFrame(const CanFrame& frame, bool session_response) {
    if (!responseFrameMatches(frame, job.tp20_ecu_tx_id) || frame.dlc == 0U) return false;

    const uint8_t operation = frame.data[0] >> 4U;
    const uint8_t sequence = frame.data[0] & 0x0FU;
    if (operation == 0x0BU) {
        const bool suppress = noteConsumedResponseFrame();
        if (sequence != job.tp20_expected_ack_sequence) {
            fail(DiagnosticError::SEQUENCE_ERROR);
        } else {
            job.phase = session_response ? Phase::TP20_WAIT_SESSION_RESPONSE
                                         : Phase::TP20_WAIT_REQUEST_RESPONSE;
            publishActiveProgress();
        }
        return suppress;
    }
    if (operation == 0x09U) {
        // TP2.0 NACK/retry control belongs to our channel, but this bounded
        // implementation never retransmits application requests implicitly.
        const bool suppress = noteConsumedResponseFrame();
        fail(DiagnosticError::TP20_NEGOTIATION_FAILED);
        return suppress;
    }
    if (operation > 0x03U) return false;

    // TP2.0 sequence numbers are channel-continuous, not reset for each KWP
    // response.  Captures show session response seq 0, the next response seq 1
    // through 8, then the following response beginning at seq 9.
    if (sequence != job.tp20_expected_rx_sequence) {
        const bool suppress = noteConsumedResponseFrame();
        fail(DiagnosticError::SEQUENCE_ERROR);
        return suppress;
    }

    if (!job.tp20_rx_started) {
        if (frame.dlc < 3U) {
            const bool suppress = noteConsumedResponseFrame();
            fail(DiagnosticError::UNEXPECTED_RESPONSE);
            return suppress;
        }
        const uint16_t total = static_cast<uint16_t>(
            (static_cast<uint16_t>(frame.data[1]) << 8U) | frame.data[2]);
        if (total == 0U || total > kDiagnosticResponseMaxPayload) {
            const bool suppress = noteConsumedResponseFrame();
            fail(DiagnosticError::RESPONSE_TOO_LARGE);
            return suppress;
        }
        job.tp20_rx_started = true;
        job.tp20_rx_total_length = total;
        job.result.response_length = 0U;
        for (uint8_t index = 3U; index < frame.dlc &&
             job.result.response_length < total; ++index) {
            job.result.response[job.result.response_length++] = frame.data[index];
        }
    } else {
        for (uint8_t index = 1U; index < frame.dlc &&
             job.result.response_length < job.tp20_rx_total_length; ++index) {
            job.result.response[job.result.response_length++] = frame.data[index];
        }
    }
    job.tp20_expected_rx_sequence = static_cast<uint8_t>((sequence + 1U) & 0x0FU);

    const bool suppress = noteConsumedResponseFrame();
    const bool complete = job.result.response_length >= job.tp20_rx_total_length;
    const bool sender_waits_for_ack = operation == 0x00U || operation == 0x01U;
    const bool final_fragment = operation == 0x01U || operation == 0x03U;

    if (!complete) {
        // Operations 1/3 describe the last fragment; ending early is a broken
        // length/sequence contract rather than something to wait out.
        if (final_fragment) {
            fail(DiagnosticError::SEQUENCE_ERROR);
        } else if (sender_waits_for_ack) {
            stageTp20Ack(sequence, session_response ? AfterAck::WAIT_SESSION_RESPONSE
                                                    : AfterAck::WAIT_REQUEST_RESPONSE);
        } else {
            job.phase = session_response ? Phase::TP20_WAIT_SESSION_RESPONSE
                                         : Phase::TP20_WAIT_REQUEST_RESPONSE;
            publishActiveProgress();
        }
        return suppress;
    }

    if (!final_fragment) {
        // Reaching the advertised length on op 0/2 is contradictory: those
        // opcodes explicitly promise another fragment.  Do not accept a
        // truncated/replayed block as a complete KWP response.
        fail(DiagnosticError::SEQUENCE_ERROR);
        return suppress;
    }

    const AfterAck outcome = kwpOutcomeAfterAck(session_response);
    if (sender_waits_for_ack) {
        stageTp20Ack(sequence, outcome);
    } else {
        handleKwpPayloadOutcome(session_response);
    }
    return suppress;
}

bool observeTp20Response(const CanFrame& frame) {
    if (job.phase == Phase::TP20_WAIT_DISCONNECT &&
        responseFrameMatches(frame, job.tp20_ecu_tx_id) &&
        frame.dlc > 0U && frame.data[0] == kTp20Disconnect) {
        const bool suppress = noteConsumedResponseFrame();
        finalizeTerminalIntent();
        return suppress;
    }

    if (job.phase == Phase::TP20_WAIT_CHANNEL_TEST &&
        responseFrameMatches(frame, job.tp20_ecu_tx_id) && frame.dlc > 0U &&
        frame.data[0] == kTp20ParamsResponse) {
        const bool suppress = noteConsumedResponseFrame();
        job.tp20_channel_test_not_before_ms =
            last_now_ms + kTp20ChannelTestIntervalMs;
        job.phase = Phase::TP20_WAIT_REPEAT;
        publishActiveProgress();
        return suppress;
    }

    if (job.phase == Phase::TP20_WAIT_SETUP &&
        responseFrameMatches(frame, job.request.route.response_can_id) && frame.dlc >= 2U) {
        if (frame.data[1] != kTp20SetupPositive &&
            !(frame.data[1] >= 0xD6U && frame.data[1] <= 0xD8U)) return false;

        const bool suppress = noteConsumedResponseFrame();
        if (frame.data[1] >= 0xD6U && frame.data[1] <= 0xD8U) {
            fail(DiagnosticError::TP20_SETUP_REJECTED, false);
            return suppress;
        }
        if (frame.dlc < 7U || frame.data[0] != 0x00U ||
            frame.data[6] != job.request.route.tp20_application_type ||
            !decodeTp20CanId(frame.data[2], frame.data[3], &job.tp20_ecu_tx_id) ||
            !decodeTp20CanId(frame.data[4], frame.data[5], &job.tp20_tester_tx_id)) {
            fail(DiagnosticError::TP20_NEGOTIATION_FAILED, false);
            return suppress;
        }

        job.tp20_channel_open = true;
        job.result.active_request_can_id = job.tp20_tester_tx_id;
        job.result.active_response_can_id = job.tp20_ecu_tx_id;
        const uint8_t params[] = {kTp20ParamsRequest, 0x0FU, 0x8AU, 0xFFU, 0x32U, 0xFFU};
        stageTx(job.tp20_tester_tx_id, job.request.route.request_direction,
                params, sizeof(params), TxAction::TP20_PARAMS,
                Phase::TP20_SEND_PARAMS);
        publishActiveProgress();
        return suppress;
    }

    if (job.phase == Phase::TP20_WAIT_PARAMS &&
        responseFrameMatches(frame, job.tp20_ecu_tx_id) && frame.dlc > 0U) {
        if (frame.data[0] != kTp20ParamsResponse) return false;
        const bool suppress = noteConsumedResponseFrame();
        // A TP2.0 parameter reply is six bytes.  The first parameter is the
        // non-zero block size (four bits); the remaining timing parameters
        // must also be populated.  Do not hard-code one ECU's negotiated
        // timing values, but never let a bare/truncated A1 open the channel.
        const bool sensible_params = frame.dlc >= 6U && frame.data[1] != 0U &&
                                     frame.data[1] <= 0x0FU && frame.data[2] != 0U &&
                                     frame.data[3] != 0U && frame.data[4] != 0U &&
                                     frame.data[5] != 0U;
        if (!sensible_params) {
            fail(DiagnosticError::TP20_NEGOTIATION_FAILED);
            return suppress;
        }
        job.tp20_tx_sequence = 0U;
        job.tp20_expected_rx_sequence = 0U;
        if (job.request.route.enter_kwp_session) stageTp20KwpRequest(true);
        else stageTp20KwpRequest(false);
        publishActiveProgress();
        return suppress;
    }

    const bool session_response = job.phase == Phase::TP20_WAIT_SESSION_ACK ||
                                  job.phase == Phase::TP20_WAIT_SESSION_RESPONSE;
    const bool request_response = job.phase == Phase::TP20_WAIT_REQUEST_ACK ||
                                  job.phase == Phase::TP20_WAIT_REQUEST_RESPONSE;
    if (session_response || request_response) {
        return observeTp20KwpFrame(frame, session_response);
    }
    return false;
}

}  // namespace

void diagnosticTransportInit(DiagnosticTxDriver driver) {
    tx_driver = driver;
    command_mailbox = {};
    command_state.store(kMailboxEmpty, std::memory_order_relaxed);
    busy.store(0U, std::memory_order_relaxed);
    cancel_job_id.store(0U, std::memory_order_relaxed);
    abort_job_id.store(0U, std::memory_order_relaxed);
    abort_error.store(static_cast<uint8_t>(DiagnosticError::ABORTED), std::memory_order_relaxed);
    next_job_id.store(1U, std::memory_order_relaxed);
    job = {};
    for (size_t index = 0U; index < kResultWordCount; ++index) {
        result_words[index].store(0U, std::memory_order_relaxed);
    }
    result_sequence.store(0U, std::memory_order_relaxed);
    stats_active_job_id.store(0U, std::memory_order_relaxed);
    stats_submitted_jobs.store(0U, std::memory_order_relaxed);
    stats_completed_jobs.store(0U, std::memory_order_relaxed);
    stats_failed_jobs.store(0U, std::memory_order_relaxed);
    stats_cancelled_jobs.store(0U, std::memory_order_relaxed);
    stats_suppressed_frames.store(0U, std::memory_order_relaxed);
    stats_external_collisions.store(0U, std::memory_order_relaxed);
    initialized.store(driver != nullptr ? 1U : 0U, std::memory_order_release);
}

bool diagnosticTransportSubmit(const DiagnosticRequest& request, uint32_t* job_id) {
    if (initialized.load(std::memory_order_acquire) == 0U || tx_driver == nullptr ||
        !requestValid(request)) return false;

    uint8_t expected = 0U;
    if (!busy.compare_exchange_strong(expected, 1U, std::memory_order_acq_rel)) return false;

    uint32_t id = next_job_id.fetch_add(1U, std::memory_order_relaxed);
    if (id == 0U) id = next_job_id.fetch_add(1U, std::memory_order_relaxed);
    command_mailbox.request = request;
    command_mailbox.job_id = id;

    DiagnosticResult queued{};
    queued.job_id = id;
    queued.state = DiagnosticJobState::QUEUED;
    queued.route = request.route;
    queued.request_sid = request.payload[0];
    queued.request_length = request.payload_length;
    std::memcpy(queued.request, request.payload, request.payload_length);
    queued.active_request_can_id = request.route.request_can_id;
    queued.active_response_can_id = request.route.response_can_id;
    publishResult(queued);

    stats_submitted_jobs.fetch_add(1U, std::memory_order_relaxed);
    command_state.store(kMailboxReady, std::memory_order_release);
    if (job_id != nullptr) *job_id = id;
    return true;
}

bool diagnosticTransportCancel(uint32_t job_id) {
    if (job_id == 0U || busy.load(std::memory_order_acquire) == 0U) return false;

    DiagnosticResult snapshot{};
    if (!copyPublishedResult(&snapshot) || snapshot.job_id != job_id ||
        (snapshot.state != DiagnosticJobState::QUEUED &&
         snapshot.state != DiagnosticJobState::ACTIVE)) return false;
    cancel_job_id.store(job_id, std::memory_order_release);
    return true;
}

bool diagnosticTransportReadResult(uint32_t job_id, DiagnosticResult* result) {
    if (job_id == 0U || result == nullptr) return false;
    DiagnosticResult snapshot{};
    if (!copyPublishedResult(&snapshot) || snapshot.job_id != job_id) return false;
    *result = snapshot;
    return true;
}

bool diagnosticTransportReadLatest(DiagnosticResult* result) {
    if (result == nullptr || !copyPublishedResult(result)) return false;
    return result->job_id != 0U;
}

DiagnosticTransportStats diagnosticTransportStats() {
    DiagnosticTransportStats stats{};
    stats.initialized = initialized.load(std::memory_order_acquire) != 0U;
    stats.active_job_id = stats_active_job_id.load(std::memory_order_acquire);
    stats.active = stats.active_job_id != 0U;
    stats.submitted_jobs = stats_submitted_jobs.load(std::memory_order_relaxed);
    stats.completed_jobs = stats_completed_jobs.load(std::memory_order_relaxed);
    stats.failed_jobs = stats_failed_jobs.load(std::memory_order_relaxed);
    stats.cancelled_jobs = stats_cancelled_jobs.load(std::memory_order_relaxed);
    stats.suppressed_response_frames = stats_suppressed_frames.load(std::memory_order_relaxed);
    stats.external_tester_collisions = stats_external_collisions.load(std::memory_order_relaxed);
    return stats;
}

bool diagnosticTransportTxPending(Direction direction) {
    // ActiveJob is exclusively owned by the CAN task, which is also the only
    // caller of this scheduling hint. The queued mailbox crosses cores and is
    // read only after its release-store state becomes visible.
    if (job.active) {
        return job.pending_tx_valid && job.pending_tx.direction == direction;
    }
    if (command_state.load(std::memory_order_acquire) != kMailboxReady) return false;
    return command_mailbox.request.route.request_direction == direction;
}

bool diagnosticTransportObservePhysicalFrame(const CanFrame& frame) {
    if (!job.active) {
        // Main drains physical ingress before tick claims the UI mailbox.  A
        // released command is therefore collision-active even while its public
        // result still says QUEUED.  Claim and fail it here before any TX.
        if (command_state.load(std::memory_order_acquire) != kMailboxReady) return false;
        const CommandMailbox queued = command_mailbox;
        if (!collidesWithQueuedRequest(frame, queued.request)) return false;
        if (command_state.exchange(kMailboxEmpty, std::memory_order_acq_rel) != kMailboxReady) {
            return false;
        }
        beginJob(queued, last_now_ms);
        externalTesterCollision();
        return false;
    }

    if (job.request.route.protocol == DiagnosticProtocol::ISO_TP) {
        if (requestFrameMatches(frame, job.request.route.request_can_id)) {
            externalTesterCollision();
            return false;  // never consume an external tester's request
        }
        const uint32_t job_id = job.result.job_id;
        const Phase phase = job.phase;
        const bool consumed = observeIsoResponse(frame);
        if (consumed) recordDiagnosticTraceFrame(frame, false, true, job_id, phase);
        return consumed;
    }

    if (looksLikeTp20ExternalRequest(frame)) {
        externalTesterCollision();
        return false;  // preserve the external request for normal gateway forwarding
    }
    const uint32_t job_id = job.result.job_id;
    const Phase phase = job.phase;
    const bool consumed = observeTp20Response(frame);
    if (consumed) recordDiagnosticTraceFrame(frame, false, true, job_id, phase);
    return consumed;
}

void diagnosticTransportTraceReset() {
    diagnostic_trace_total.store(0U, std::memory_order_release);
}

size_t diagnosticTransportTraceCount() {
    const uint32_t total = diagnostic_trace_total.load(std::memory_order_acquire);
    return total < kDiagnosticTraceCapacity
        ? static_cast<size_t>(total) : kDiagnosticTraceCapacity;
}

uint32_t diagnosticTransportTraceDropped() {
    const uint32_t total = diagnostic_trace_total.load(std::memory_order_acquire);
    return total > kDiagnosticTraceCapacity
        ? total - static_cast<uint32_t>(kDiagnosticTraceCapacity) : 0U;
}

bool diagnosticTransportTraceRead(size_t index, DiagnosticTraceFrame* frame) {
    if (frame == nullptr) return false;
    const uint32_t total = diagnostic_trace_total.load(std::memory_order_acquire);
    const size_t count = total < kDiagnosticTraceCapacity
        ? static_cast<size_t>(total) : kDiagnosticTraceCapacity;
    if (index >= count) return false;
    const size_t oldest = total > kDiagnosticTraceCapacity
        ? static_cast<size_t>(total % kDiagnosticTraceCapacity) : 0U;
    *frame = diagnostic_trace[(oldest + index) % kDiagnosticTraceCapacity];
    return true;
}

const char* diagnosticTransportTracePhaseName(uint8_t raw_phase) {
    switch (static_cast<Phase>(raw_phase)) {
        case Phase::ISO_SEND_REQUEST: return "iso_send_request";
        case Phase::ISO_WAIT_RESPONSE: return "iso_wait_response";
        case Phase::ISO_SEND_FLOW_CONTROL: return "iso_send_flow_control";
        case Phase::ISO_WAIT_CONSECUTIVE: return "iso_wait_consecutive";
        case Phase::TP20_SEND_SETUP: return "tp20_send_setup";
        case Phase::TP20_WAIT_SETUP: return "tp20_wait_setup";
        case Phase::TP20_SEND_PARAMS: return "tp20_send_params";
        case Phase::TP20_WAIT_PARAMS: return "tp20_wait_params";
        case Phase::TP20_SEND_SESSION: return "tp20_send_session";
        case Phase::TP20_WAIT_SESSION_ACK: return "tp20_wait_session_ack";
        case Phase::TP20_WAIT_SESSION_RESPONSE: return "tp20_wait_session_response";
        case Phase::TP20_SEND_REQUEST: return "tp20_send_request";
        case Phase::TP20_WAIT_REQUEST_ACK: return "tp20_wait_request_ack";
        case Phase::TP20_WAIT_REQUEST_RESPONSE: return "tp20_wait_request_response";
        case Phase::TP20_SEND_ACK: return "tp20_send_ack";
        case Phase::TP20_WAIT_REPEAT: return "tp20_wait_repeat";
        case Phase::TP20_SEND_CHANNEL_TEST: return "tp20_send_channel_test";
        case Phase::TP20_WAIT_CHANNEL_TEST: return "tp20_wait_channel_test";
        case Phase::TP20_SEND_DISCONNECT: return "tp20_send_disconnect";
        case Phase::TP20_WAIT_DISCONNECT: return "tp20_wait_disconnect";
        case Phase::NONE:
        default: return "none";
    }
}

void diagnosticTransportTick(uint32_t now_us, uint32_t now_ms) {
    last_now_ms = now_ms;

    if (!job.active && command_state.exchange(kMailboxEmpty, std::memory_order_acq_rel) == kMailboxReady) {
        const CommandMailbox command = command_mailbox;
        if (cancel_job_id.load(std::memory_order_acquire) == command.job_id) {
            // Build the normal result first so a queued cancellation retains
            // route and request context, but clear its unsent frame below.
            beginJob(command, now_ms);
            finishImmediately(DiagnosticJobState::CANCELLED, DiagnosticError::CANCELLED);
            return;
        }
        beginJob(command, now_ms);
    }

    if (!job.active) return;

    // Once cleanup starts, preserve the original terminal outcome and allow
    // only the bounded disconnect handshake to finish it.
    if (job.terminal.valid) {
        if (job.phase == Phase::TP20_WAIT_DISCONNECT &&
            timeReached(now_ms, job.tp20_disconnect_deadline_ms)) {
            finalizeTerminalIntent();
            return;
        }
        transmitOnePendingFrame(now_us);
        return;
    }

    if (cancel_job_id.load(std::memory_order_acquire) == job.result.job_id) {
        requestFinish(DiagnosticJobState::CANCELLED, DiagnosticError::CANCELLED);
    } else if (abort_job_id.load(std::memory_order_acquire) == job.result.job_id) {
        uint32_t expected_job_id = job.result.job_id;
        static_cast<void>(abort_job_id.compare_exchange_strong(
            expected_job_id, 0U, std::memory_order_acq_rel));
        requestFinish(DiagnosticJobState::FAILED,
            static_cast<DiagnosticError>(abort_error.load(std::memory_order_acquire)));
    } else if (timeReached(now_ms, job.deadline_ms)) {
        job.result.timeout = true;
        requestFinish(DiagnosticJobState::FAILED, DiagnosticError::TIMEOUT);
    } else if (job.phase == Phase::TP20_WAIT_CHANNEL_TEST &&
               timeReached(now_ms, job.tp20_channel_test_deadline_ms)) {
        job.result.timeout = true;
        requestFinish(DiagnosticJobState::FAILED, DiagnosticError::TIMEOUT);
    }

    if (job.active && !job.terminal.valid && job.phase == Phase::TP20_WAIT_REPEAT) {
        if (timeReached(now_ms, job.tp20_repeat_not_before_ms)) {
            job.result.negative = false;
            job.result.response_sid = 0U;
            job.result.negative_service_sid = 0U;
            job.result.nrc = 0U;
            stageTp20KwpRequest(false);
        } else if (timeReached(now_ms, job.tp20_channel_test_not_before_ms)) {
            stageTp20ChannelTest();
        }
    }

    // This is the only transmit site in the module.  Even a state transition
    // that queues another frame cannot send it until the next tick.
    transmitOnePendingFrame(now_us);
}

void diagnosticTransportAbort(DiagnosticError error) {
    if (busy.load(std::memory_order_acquire) == 0U) return;

    DiagnosticResult snapshot{};
    if (!copyPublishedResult(&snapshot) || snapshot.job_id == 0U ||
        (snapshot.state != DiagnosticJobState::QUEUED &&
         snapshot.state != DiagnosticJobState::ACTIVE)) {
        return;
    }
    abort_error.store(static_cast<uint8_t>(error), std::memory_order_release);
    abort_job_id.store(snapshot.job_id, std::memory_order_release);
}

const char* diagnosticProtocolName(DiagnosticProtocol protocol) {
    switch (protocol) {
        case DiagnosticProtocol::ISO_TP: return "iso_tp";
        case DiagnosticProtocol::VW_TP20: return "vw_tp20";
        default: return "unknown";
    }
}

const char* diagnosticJobStateName(DiagnosticJobState state) {
    switch (state) {
        case DiagnosticJobState::IDLE: return "idle";
        case DiagnosticJobState::QUEUED: return "queued";
        case DiagnosticJobState::ACTIVE: return "active";
        case DiagnosticJobState::COMPLETE: return "complete";
        case DiagnosticJobState::FAILED: return "failed";
        case DiagnosticJobState::CANCELLED: return "cancelled";
        default: return "unknown";
    }
}

const char* diagnosticErrorName(DiagnosticError error) {
    switch (error) {
        case DiagnosticError::NONE: return "none";
        case DiagnosticError::NOT_INITIALIZED: return "not_initialized";
        case DiagnosticError::BUSY: return "busy";
        case DiagnosticError::INVALID_REQUEST: return "invalid_request";
        case DiagnosticError::UNSUPPORTED_REQUEST_LENGTH: return "unsupported_request_length";
        case DiagnosticError::TX_FAILED: return "tx_failed";
        case DiagnosticError::TIMEOUT: return "timeout";
        case DiagnosticError::EXTERNAL_TESTER_COLLISION: return "external_tester_collision";
        case DiagnosticError::UNEXPECTED_RESPONSE: return "unexpected_response";
        case DiagnosticError::NEGATIVE_RESPONSE: return "negative_response";
        case DiagnosticError::RESPONSE_TOO_LARGE: return "response_too_large";
        case DiagnosticError::SEQUENCE_ERROR: return "sequence_error";
        case DiagnosticError::TP20_SETUP_REJECTED: return "tp20_setup_rejected";
        case DiagnosticError::TP20_NEGOTIATION_FAILED: return "tp20_negotiation_failed";
        case DiagnosticError::CANCELLED: return "cancelled";
        case DiagnosticError::ABORTED: return "aborted";
        default: return "unknown";
    }
}

const char* diagnosticNrcName(uint8_t nrc) {
    switch (nrc) {
        case 0x10U: return "general_reject";
        case 0x11U: return "service_not_supported";
        case 0x12U: return "subfunction_not_supported";
        case 0x13U: return "incorrect_message_length_or_invalid_format";
        case 0x14U: return "response_too_long";
        case 0x21U: return "busy_repeat_request";
        case 0x22U: return "conditions_not_correct";
        case 0x24U: return "request_sequence_error";
        case 0x25U: return "no_response_from_subnet_component";
        case 0x26U: return "failure_prevents_execution";
        case 0x31U: return "request_out_of_range";
        case 0x33U: return "security_access_denied";
        case 0x35U: return "invalid_key";
        case 0x36U: return "exceeded_number_of_attempts";
        case 0x37U: return "required_time_delay_not_expired";
        case 0x70U: return "upload_download_not_accepted";
        case 0x71U: return "transfer_data_suspended";
        case 0x72U: return "general_programming_failure";
        case 0x73U: return "wrong_block_sequence_counter";
        case 0x78U: return "response_pending";
        case 0x7EU: return "subfunction_not_supported_in_active_session";
        case 0x7FU: return "service_not_supported_in_active_session";
        case 0x81U: return "rpm_too_high";
        case 0x82U: return "rpm_too_low";
        case 0x83U: return "engine_is_running";
        case 0x84U: return "engine_is_not_running";
        case 0x85U: return "engine_run_time_too_low";
        case 0x86U: return "temperature_too_high";
        case 0x87U: return "temperature_too_low";
        case 0x88U: return "vehicle_speed_too_high";
        case 0x89U: return "vehicle_speed_too_low";
        case 0x8AU: return "throttle_too_high";
        case 0x8BU: return "throttle_too_low";
        case 0x8CU: return "transmission_range_not_neutral";
        case 0x8DU: return "transmission_range_not_in_gear";
        case 0x8FU: return "brake_switch_not_closed";
        case 0x90U: return "shifter_lever_not_in_park";
        case 0x91U: return "torque_converter_clutch_locked";
        case 0x92U: return "voltage_too_high";
        case 0x93U: return "voltage_too_low";
        default: return "unknown_negative_response_code";
    }
}

}  // namespace bored::signalscope
