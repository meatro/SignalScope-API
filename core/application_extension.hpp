#pragma once

#include <cstddef>
#include <cstdint>

#include "diagnostic_transport.hpp"
#include "parked_power.hpp"
#include "types.hpp"

namespace bored::signalscope {

class DbcDatabase;

enum class ApplicationDatabaseLoadResult : uint8_t {
    NOT_HANDLED = 0,
    LOADED,
    FAILED,
};

struct ApplicationFrameSnapshot {
    uint32_t can_id = 0U;
    Direction direction = Direction::A_TO_B;
    uint8_t dlc = 0U;
    uint8_t data[8] = {0};
    uint8_t input_data[8] = {0};
    bool has_input = false;
    bool mutated = false;
    uint32_t timestamp_us = 0U;
    uint32_t total_frames = 0U;
};

struct ApplicationServices {
    int32_t (*findSignalByName)(const char* name) = nullptr;
    // Resolve a DBC signal by its stable catalog identity. Signal indexes are
    // runtime handles and can change whenever the active DBC changes, while
    // the CAN id plus signal name remains suitable for persisted application
    // configuration. The legacy name-only resolver remains available for
    // DBCs where signal names are unambiguous.
    int32_t (*findSignalByIdentity)(uint32_t can_id, const char* name) = nullptr;
    bool (*subscribeSignal)(uint16_t signal_index, bool enabled) = nullptr;
    bool (*readSignal)(uint16_t signal_index, float* value, uint32_t* generation, bool* valid) = nullptr;
    bool (*readSignalState)(uint16_t signal_index, float* value, uint32_t* generation, bool* valid,
                            uint32_t* timestamp_us, Direction* direction) = nullptr;
    bool (*signalCanId)(uint16_t signal_index, uint32_t* can_id) = nullptr;
    bool (*readFrame)(uint32_t can_id, Direction direction, ApplicationFrameSnapshot* frame) = nullptr;
    bool (*readPhysicalFrame)(uint32_t can_id, Direction direction,
                              ApplicationFrameSnapshot* frame) = nullptr;
    size_t (*snapshotFramesByDirection)(Direction direction, ApplicationFrameSnapshot* frames,
                                        size_t capacity) = nullptr;
    size_t (*snapshotMutatedFrames)(ApplicationFrameSnapshot* frames, size_t capacity) = nullptr;
    bool (*publishRuntimeValue)(const char* name, float value) = nullptr;
    bool (*readRuntimeValue)(const char* name, float* value) = nullptr;
    bool (*publishRuntimeTable)(const char* name, const float* values, size_t count, bool valid) = nullptr;
    // Applications can stage a new DBC/rule selection and ask the host to
    // apply it from its normal application/UI task. The callback is deliberately
    // non-blocking: it must never perform filesystem work on a CAN-facing path.
    bool (*requestResourceReload)() = nullptr;
    bool (*submitDiagnostic)(const DiagnosticRequest* request, uint32_t* job_id) = nullptr;
    bool (*cancelDiagnostic)(uint32_t job_id) = nullptr;
    bool (*readDiagnosticResult)(uint32_t job_id, DiagnosticResult* result) = nullptr;
    // Generic session-log annotation channel. The SignalScope host owns the
    // recorder and storage format; applications provide only opaque JSON
    // payloads labeled with an application-defined kind. Call only from the
    // application/UI task (never an ISR or CAN callback). The payload is copied
    // synchronously before return. Kind is limited to 31 UTF-8 bytes and JSON
    // to 1024 bytes; false means that no annotation was stored.
    bool (*sessionLogActive)() = nullptr;
    bool (*appendSessionLogAnnotation)(const char* kind, const char* json,
                                       size_t json_length) = nullptr;
    // The host owns deep-sleep entry and both CAN peripherals. Applications
    // may atomically replace their policy after a committed configuration
    // change and read back the host's authoritative runtime state.
    bool (*setParkedPowerPolicy)(const ParkedPowerPolicy* policy) = nullptr;
    bool (*readParkedPowerStatus)(ParkedPowerStatus* status) = nullptr;
};

struct ApplicationExtension {
    // Stable process-lifetime identifier: 1..31 ASCII letters, digits, '.',
    // '_' or '-'. The host uses it as generic provenance for annotations.
    const char* id = nullptr;
    // Standalone SignalScope and applications that do not narrow this policy
    // retain bidirectional mutation. Product applications may remove a bit to
    // enforce a safety boundary at the gateway, independent of how a generic
    // mutation rule was installed.
    uint8_t mutation_direction_mask = kMutationDirectionBoth;
    void (*begin)() = nullptr;
    void (*tick)() = nullptr;
    bool (*writeStatusJson)(char* output, size_t capacity) = nullptr;
    bool (*configure)(const char* key, const char* value, char* output, size_t capacity) = nullptr;
    // Configuration changes that select external resources may be staged by
    // the application.  The host calls this after the requested DBC and rule
    // package have either both loaded or failed, allowing a transactional commit or
    // rollback while CAN forwarding remains active.
    void (*finishConfigure)(bool commit) = nullptr;
    size_t (*resourceCapacity)(const char* name) = nullptr;
    bool (*readResource)(const char* name, char* output, size_t capacity, size_t* written) = nullptr;
    bool (*writeResource)(const char* name, const char* input, size_t input_length,
                          char* output, size_t capacity, size_t* written) = nullptr;
    const char* (*requestedDatabasePath)() = nullptr;
    // Optional application-owned immutable database source. The host still
    // owns candidate/cache construction and the atomic quiesce-and-swap
    // transaction. NOT_HANDLED preserves ordinary user-supplied LittleFS DBCs.
    ApplicationDatabaseLoadResult (*loadDatabase)(
        const char* logical_path, DbcDatabase* candidate, size_t* source_bytes) = nullptr;
    const char* (*requestedRulePackagePath)() = nullptr;
    void (*attachServices)(const ApplicationServices* services) = nullptr;
    void (*databaseChanged)() = nullptr;
    // Called before CAN startup and filesystem mounting. Implementations must
    // perform only bounded NVS reads and must not depend on attached services.
    bool (*loadParkedPowerPolicy)(ParkedPowerPolicy* policy) = nullptr;
    // Called from the CAN owner for each physical primary-side frame. It must
    // be allocation-free/non-blocking; source is an application-defined code.
    bool (*decodeParkedPowerIgnition)(const CanFrame* frame, bool* ignition_on,
                                      uint8_t* source) = nullptr;
    // Called while holding the application lock from the low-priority task.
    bool (*parkedPowerBusy)() = nullptr;
    void (*prepareForParkedSleep)() = nullptr;
};

class ApplicationExtensionRegistry {
public:
    bool registerExtension(const ApplicationExtension& extension);
    const ApplicationExtension* extension() const;

private:
    ApplicationExtension extension_{};
    bool registered_ = false;
};

// Applications provide a strong definition of this hook. Standalone
// SignalScope uses the weak no-op definition in application_extension.cpp.
void registerSignalScopeApplication(ApplicationExtensionRegistry& registry);

}  // namespace bored::signalscope
