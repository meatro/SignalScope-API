#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>

#include "../core/can_trace.hpp"

namespace bored::signalscope {

// Generic, application-neutral SignalScope session recorder. CAN events are
// accumulated in PSRAM while recording. Normal mode touches LittleFS only
// after the producer stops. Optional durable mode checkpoints from the
// application task; the CAN producer itself never performs filesystem work.
class SessionLogRecorder {
public:
    static constexpr const char* kLogPath = "/signalscope-session.sslog";
    static constexpr const char* kTemporaryPath = "/signalscope-session.tmp";
    static constexpr const char* kBackupPath = "/signalscope-session.bak";
    static constexpr const char* kCheckpointPath0 = "/signalscope-session.ck0";
    static constexpr const char* kCheckpointPath1 = "/signalscope-session.ck1";
    static constexpr size_t kMaximumCaptureBytes = 4U * 1024U * 1024U;
    static constexpr size_t kMaximumSourceBytes = 31U;
    static constexpr size_t kMaximumKindBytes = 31U;
    static constexpr size_t kMaximumAnnotationJsonBytes = 1024U;

    bool begin();
    bool start(CanTraceQueue& trace, CanTraceScope scope, bool durable,
               const char* application_id,
               const char*& error);
    bool requestStop(CanTraceQueue& trace, const char* reason = "user");
    bool retrySave(const char*& error);
    bool clear();
    void tick(CanTraceQueue& trace, uint32_t now_ms);

    bool appendAnnotation(const char* source, const char* kind,
                          const char* json, size_t json_length,
                          uint32_t timestamp_us);

    bool writeStatusJson(char* output, size_t capacity) const;
    bool recording() const { return recording_; }
    bool stopping() const { return stopping_; }
    bool saving() const { return saving_; }
    bool available() const { return available_; }
    bool sleepSafe() const {
        return !recording_ && !stopping_ && !saving_ && !save_failed_;
    }
    bool downloadReady() const { return available_ && !recording_ && !stopping_ && !saving_; }
    size_t captureBytes() const { return capture_size_; }
    size_t savedBytes() const { return saved_bytes_; }

private:
    enum class RecordType : uint8_t {
        Frame = 1U,
        Annotation = 2U,
        End = 3U,
    };

    bool allocateCapture(size_t& capacity, const char*& error);
    void releaseCapture();
    bool appendFrame(const CanTraceEvent& event);
    bool appendRecord(RecordType type, uint8_t flags, uint32_t timestamp_us,
                      uint32_t sequence, const void* payload, size_t payload_length,
                      bool use_footer_reserve = true);
    bool appendBytes(const void* data, size_t length, bool use_footer_reserve = true);
    void beginStopping(CanTraceQueue& trace, const char* reason, bool truncated);
    void finalizeMemory(uint32_t now_ms);
    void processSaveChunk();
    void processCheckpoint(uint32_t now_ms);
    void closeCheckpoint();
    bool commitCheckpoint(size_t committed_bytes);
    void removeCheckpointArtifacts(bool remove_temporary);
    void failSave(const char* error);
    void inspectExisting();
    static const char* resetReasonName(uint32_t reason);

    uint8_t* capture_ = nullptr;
    size_t capture_capacity_ = 0U;
    size_t session_capacity_ = 0U;
    size_t capture_size_ = 0U;
    size_t save_offset_ = 0U;
    File save_file_;
    File checkpoint_file_;

    bool recording_ = false;
    bool stopping_ = false;
    bool saving_ = false;
    bool available_ = false;
    bool complete_ = false;
    bool recovered_ = false;
    bool truncated_ = false;
    bool save_failed_ = false;
    bool durable_ = false;
    bool checkpoint_failed_ = false;
    CanTraceScope scope_ = CanTraceScope::Physical;
    uint32_t started_ms_ = 0U;
    uint32_t stopped_ms_ = 0U;
    uint32_t frame_records_ = 0U;
    uint32_t annotation_records_ = 0U;
    uint32_t annotation_rejected_ = 0U;
    size_t annotation_bytes_ = 0U;
    uint32_t trace_queue_drops_ = 0U;
    uint32_t record_drops_ = 0U;
    uint32_t trace_drops_ = 0U;
    size_t saved_bytes_ = 0U;
    size_t checkpoint_offset_ = 0U;
    size_t checkpoint_target_ = 0U;
    uint32_t capture_crc_state_ = 0xFFFFFFFFU;
    uint32_t checkpoint_crc_state_ = 0xFFFFFFFFU;
    uint32_t checkpoint_generation_ = 0U;
    uint8_t checkpoint_manifest_slot_ = 0U;
    uint32_t next_checkpoint_ms_ = 0U;
    uint32_t recovery_reset_reason_ = 0U;
    uint32_t final_crc32_ = 0U;
    size_t final_prefix_bytes_ = 0U;
    char stop_reason_[32] = {};
    char last_error_[64] = {};
};

}  // namespace bored::signalscope
