import importlib.util
import json
import struct
import sys
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "read_signalscope_session", ROOT / "tools" / "read_signalscope_session.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def record(kind, timestamp, sequence, payload, flags=0):
    return MODULE.RECORD_HEADER.pack(kind, flags, len(payload), timestamp, sequence) + payload


def main():
    header = MODULE.FILE_HEADER.pack(MODULE.MAGIC, MODULE.VERSION, MODULE.FILE_HEADER.size,
                                     1234, 1, 65536, 3, 0,
                                     MODULE.END_CRC_REQUIRED_MARKER)
    frame_payload = MODULE.FRAME_BASE.pack(0x123, 1, 8, 1, 0, bytes.fromhex("1020304050607080"))
    prepared_base = MODULE.FRAME_BASE.pack(
        0x456, 0, 8, 2, (1 << 2) | MODULE.FLAG_HAS_INPUT,
        bytes.fromhex("0102030405060708"),
    )
    prepared_payload = prepared_base + bytes.fromhex("0002030405060708")
    transmit_payload = MODULE.FRAME_BASE.pack(
        0x700, 0, 3, 3, MODULE.FLAG_DIAGNOSTIC_TRANSPORT,
        bytes.fromhex("0210010000000000"),
    )
    annotation_json = json.dumps({"sample": 128}).encode()
    prefix = MODULE.ANNOTATION_PREFIX.pack(
        len(b"example-app"), len(b"telemetry"), len(annotation_json)
    )
    annotation_payload = prefix + b"example-app" + b"telemetry" + annotation_json
    prefix_content = (header + record(MODULE.RECORD_FRAME, 2000, 1, frame_payload) +
                      record(MODULE.RECORD_FRAME, 2050, 2, prepared_payload) +
                      record(MODULE.RECORD_FRAME, 2075, 3, transmit_payload) +
                      record(MODULE.RECORD_ANNOTATION, 2100, 0, annotation_payload))
    end_base = json.dumps({
        "reason": "test",
        "truncated": False,
        "dataBytes": len(prefix_content),
        "crc32": f"{zlib.crc32(prefix_content) & 0xFFFFFFFF:08X}",
    }, separators=(",", ":"))
    end_payload = (
        end_base[:-1] +
        f',"endCrc32":"{zlib.crc32(end_base.encode()) & 0xFFFFFFFF:08X}"}}'
    ).encode()
    content = prefix_content + record(MODULE.RECORD_END, 2200, 0, end_payload)

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "test.sslog"
        path.write_bytes(content)
        parsed_header, records = MODULE.load_session(path)
        assert parsed_header.started_ms == 1234
        assert parsed_header.scope == 3
        assert parsed_header.durable is False
        assert parsed_header.recovery_reset_reason == 0
        assert parsed_header.end_crc_required is True
        assert [item["type"] for item in records] == [
            "frame", "frame", "frame", "annotation", "end"
        ]
        assert records[0]["canId"] == 0x123
        assert records[0]["data"] == "1020304050607080"
        assert records[1]["stage"] == "prepared"
        assert records[1]["mutated"] is True
        assert records[1]["data"] == "0102030405060708"
        assert records[1]["input"] == "0002030405060708"
        assert records[2]["stage"] == "transmit"
        assert records[2]["diagnosticTransport"] is True
        assert records[2]["data"] == "021001"
        assert records[3]["source"] == "example-app"
        assert records[3]["data"]["sample"] == 128
        assert records[4]["data"]["reason"] == "test"

        truncated = Path(directory) / "truncated.sslog"
        truncated.write_bytes(content[:-3])
        _, truncated_records = MODULE.load_session(truncated)
        assert truncated_records[-1]["type"] == "truncated"

        mismatched = Path(directory) / "mismatched.sslog"
        mismatched_payload = MODULE.FRAME_BASE.pack(
            0x777, 0, 8, 2, MODULE.FLAG_HAS_INPUT, bytes(8)
        )
        mismatched.write_bytes(
            header + record(MODULE.RECORD_FRAME, 100, 1, mismatched_payload)
        )
        _, mismatched_records = MODULE.load_session(mismatched)
        assert mismatched_records[0]["type"] == "invalid"
        assert mismatched_records[0]["error"] == "invalid_frame_payload"

        bad_dlc = Path(directory) / "bad-dlc.sslog"
        bad_dlc_payload = MODULE.FRAME_BASE.pack(0x778, 0, 9, 1, 0, bytes(8))
        bad_dlc.write_bytes(header + record(MODULE.RECORD_FRAME, 100, 1, bad_dlc_payload))
        _, bad_dlc_records = MODULE.load_session(bad_dlc)
        assert bad_dlc_records[0]["type"] == "invalid"
        assert bad_dlc_records[0]["error"] == "invalid_frame_dlc"

    print("SESSION_LOG_PARSER_TEST_PASS")


if __name__ == "__main__":
    main()
