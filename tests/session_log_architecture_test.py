#!/usr/bin/env python3
"""Lock the SignalScope recorder/application boundary at source level."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERIC_FILES = (
    ROOT / "core" / "can_trace.hpp",
    ROOT / "core" / "can_trace.cpp",
    ROOT / "fs" / "session_log.hpp",
    ROOT / "fs" / "session_log.cpp",
    ROOT / "core" / "application_extension.hpp",
)


def require(needle: str, text: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> None:
    generic = "\n".join(path.read_text(encoding="utf-8") for path in GENERIC_FILES)
    generic_lower = generic.lower()

    forbidden_domain_terms = (
        "actual_lock",
        "requested_lock",
        "calibration",
        "drivetrain",
        "vehicle_generation",
        "apps/",
        "apps\\",
    )
    for term in forbidden_domain_terms:
        if term in generic_lower:
            raise AssertionError(
                f"generic session logging contains application/domain term: {term}"
            )

    trace = (ROOT / "core" / "can_trace.cpp").read_text(encoding="utf-8")
    recorder = (ROOT / "fs" / "session_log.cpp").read_text(encoding="utf-8")
    extension = (ROOT / "core" / "application_extension.hpp").read_text(
        encoding="utf-8"
    )
    main_source = (ROOT / "main.cpp").read_text(encoding="utf-8")
    recorder_header = (ROOT / "fs" / "session_log.hpp").read_text(encoding="utf-8")

    if "LittleFS" in trace or "File " in trace:
        raise AssertionError("the CAN trace producer must not perform filesystem work")
    require(
        "dropped_.fetch_add",
        trace,
        "the non-blocking trace queue must explicitly count full-queue drops",
    )
    require(
        "appendSessionLogAnnotation",
        extension,
        "applications need a generic opaque-annotation extension point",
    )
    require(
        "sessionLogActive",
        extension,
        "applications need a generic recorder-active query",
    )
    require(
        "heap_caps_malloc",
        recorder,
        "live session data must be staged in memory",
    )
    require(
        "processSaveChunk",
        recorder,
        "filesystem persistence must remain outside the CAN trace producer",
    )
    for integrity_contract in (
        "endCrc32",
        "readCheckpointManifest",
        "finalizeRecoveredCheckpoint",
        "capture_crc_state_",
        "promoted_size_matches",
        "SessionFileState::Recovered",
        "SessionFileState::CompleteLegacy",
        "::truncate",
        "kEndCrcRequiredMarker",
        "recovery_reset_reason",
        "LegacyCheckpointManifest",
    ):
        require(
            integrity_contract,
            recorder,
            f"session recovery integrity contract missing: {integrity_contract}",
        )

    health_start = main_source.index("void appendSessionHealthSnapshot")
    health_end = main_source.index("void configureHttpServer", health_start)
    route_start = main_source.index('server.on("/api/log"')
    route_end = main_source.index('server.on("/api/frame_cache"', route_start)
    generic_host_blocks = (main_source[health_start:health_end] +
                           main_source[route_start:route_end]).lower()
    for term in forbidden_domain_terms:
        if term in generic_host_blocks:
            raise AssertionError(
                f"generic session host/API block contains application/domain term: {term}"
            )

    forbidden_protocol_semantics = (
        "uds",
        "kwp",
        "tp20",
        "logical_link",
        "logicallink",
        "negative_response",
        "nrcname",
    )
    logging_specific = (generic + generic_host_blocks).lower()
    for term in forbidden_protocol_semantics:
        if term in logging_specific:
            raise AssertionError(
                f"generic session logging contains diagnostic protocol semantics: {term}"
            )

    require(
        "can_trace.pushTransmit(transmitted, true)",
        main_source,
        "successful diagnostic driver acceptance must be captured as raw TX evidence",
    )
    require(
        "can_trace.pushIngress(frame, diagnostic_consumed",
        main_source,
        "diagnostic-consumed ingress must retain raw provenance",
    )
    for reserved_path in (
        "kLogPath",
        "kTemporaryPath",
        "kBackupPath",
        "kCheckpointPath0",
        "kCheckpointPath1",
    ):
        require(reserved_path, recorder_header, f"missing recorder path {reserved_path}")
        require(
            f"SessionLogRecorder::{reserved_path}",
            main_source,
            f"static file serving must reserve {reserved_path}",
        )

    for forbidden_service in (
        "startSessionLog",
        "stopSessionLog",
        "clearSessionLog",
        "setSessionLogScope",
        "sessionLogPath",
        "sessionLogSource",
    ):
        if forbidden_service in extension:
            raise AssertionError(
                f"application service improperly controls the host recorder: {forbidden_service}"
            )

    print("SESSION_LOG_ARCHITECTURE_TEST_PASS")


if __name__ == "__main__":
    main()
