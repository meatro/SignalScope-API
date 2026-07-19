#!/usr/bin/env python3
"""Inspect or export a generic SignalScope .sslog session file."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator


FILE_HEADER = struct.Struct("<5sBH6I")
RECORD_HEADER = struct.Struct("<BBHII")
FRAME_BASE = struct.Struct("<IBBBB8s")
ANNOTATION_PREFIX = struct.Struct("<BBH")
MAGIC = b"SSLOG"
VERSION = 1
END_CRC_REQUIRED_MARKER = 0x454E4443

RECORD_FRAME = 1
RECORD_ANNOTATION = 2
RECORD_END = 3
FLAG_HAS_INPUT = 1 << 4
FLAG_DIAGNOSTIC_TRANSPORT = 1 << 5

STAGES = {1: "ingress", 2: "prepared", 3: "transmit"}
DIRECTIONS = {0: "A_TO_B", 1: "B_TO_A"}
SCOPES = {0: "physical", 1: "all", 2: "a_to_b", 3: "b_to_a", 4: "mutated"}


@dataclass(frozen=True)
class SessionHeader:
    started_ms: int
    reset_reason: int
    capacity_bytes: int
    scope: int
    durable: bool
    recovery_reset_reason: int
    end_crc_required: bool


def read_header(stream: BinaryIO) -> SessionHeader:
    raw = stream.read(FILE_HEADER.size)
    if len(raw) != FILE_HEADER.size:
        raise ValueError("file is shorter than the SignalScope header")
    (magic, version, header_size, started_ms, reset_reason, capacity, scope,
     durable, capability_or_recovery, *_) = FILE_HEADER.unpack(raw)
    if magic != MAGIC:
        raise ValueError(f"not a SignalScope session log (magic={magic!r})")
    if version != VERSION:
        raise ValueError(f"unsupported SignalScope session version {version}")
    if header_size != FILE_HEADER.size:
        raise ValueError(f"unsupported header size {header_size}")
    end_crc_required = capability_or_recovery == END_CRC_REQUIRED_MARKER
    recovery_reset_reason = 0 if end_crc_required else capability_or_recovery
    return SessionHeader(
        started_ms, reset_reason, capacity, scope, bool(durable),
        recovery_reset_reason, end_crc_required
    )


def _hex(data: bytes, dlc: int) -> str:
    return data[: min(dlc, 8)].hex().upper()


def iter_records(stream: BinaryIO) -> Iterator[dict]:
    while True:
        offset = stream.tell()
        raw_header = stream.read(RECORD_HEADER.size)
        if not raw_header:
            return
        if len(raw_header) != RECORD_HEADER.size:
            yield {"type": "truncated", "offset": offset, "error": "partial_record_header"}
            return
        record_type, flags, payload_length, timestamp_us, sequence = RECORD_HEADER.unpack(raw_header)
        payload = stream.read(payload_length)
        if len(payload) != payload_length:
            yield {"type": "truncated", "offset": offset, "error": "partial_record_payload"}
            return

        common = {"timestampUs": timestamp_us, "sequence": sequence, "offset": offset}
        if record_type == RECORD_FRAME:
            has_input = False
            if len(payload) >= FRAME_BASE.size:
                has_input = bool(payload[7] & FLAG_HAS_INPUT)
            expected_length = FRAME_BASE.size + (8 if has_input else 0)
            if len(payload) != expected_length:
                yield {**common, "type": "invalid", "error": "invalid_frame_payload"}
                continue
            can_id, direction, dlc, stage, frame_flags, data = FRAME_BASE.unpack_from(payload)
            if dlc > 8:
                yield {**common, "type": "invalid", "error": "invalid_frame_dlc", "dlc": dlc}
                continue
            record = {
                **common,
                "type": "frame",
                "stage": STAGES.get(stage, f"unknown_{stage}"),
                "direction": DIRECTIONS.get(direction, f"unknown_{direction}"),
                "canId": can_id,
                "canIdHex": f"0x{can_id:X}",
                "dlc": dlc,
                "data": _hex(data, dlc),
                "diagnosticConsumed": bool(frame_flags & (1 << 0)),
                "diagnosticTransport": bool(frame_flags & FLAG_DIAGNOSTIC_TRANSPORT),
                "gatewayDropped": bool(frame_flags & (1 << 1)),
                "mutated": bool(frame_flags & (1 << 2)),
                "synthetic": bool(frame_flags & (1 << 3)),
            }
            if frame_flags & FLAG_HAS_INPUT:
                record["input"] = _hex(payload[FRAME_BASE.size : FRAME_BASE.size + 8], dlc)
            yield record
        elif record_type == RECORD_ANNOTATION:
            if len(payload) < ANNOTATION_PREFIX.size:
                yield {**common, "type": "invalid", "error": "invalid_annotation_payload"}
                continue
            source_len, kind_len, json_len = ANNOTATION_PREFIX.unpack_from(payload)
            expected = ANNOTATION_PREFIX.size + source_len + kind_len + json_len
            if expected != len(payload):
                yield {**common, "type": "invalid", "error": "annotation_length_mismatch"}
                continue
            cursor = ANNOTATION_PREFIX.size
            source = payload[cursor : cursor + source_len].decode("utf-8", "replace")
            cursor += source_len
            kind = payload[cursor : cursor + kind_len].decode("utf-8", "replace")
            cursor += kind_len
            text = payload[cursor : cursor + json_len].decode("utf-8", "replace")
            try:
                value = json.loads(text)
            except json.JSONDecodeError:
                value = text
                json_valid = False
            else:
                json_valid = True
            yield {**common, "type": "annotation", "source": source, "kind": kind,
                   "jsonValid": json_valid, "data": value}
        elif record_type == RECORD_END:
            text = payload.decode("utf-8", "replace")
            try:
                value = json.loads(text)
            except json.JSONDecodeError:
                value = text
                json_valid = False
            else:
                json_valid = True
            yield {**common, "type": "end", "jsonValid": json_valid,
                   "rawJson": text, "data": value}
        else:
            yield {**common, "type": "unknown", "recordType": record_type, "flags": flags,
                   "payloadHex": payload.hex().upper()}


def load_session(path: Path) -> tuple[SessionHeader, list[dict]]:
    with path.open("rb") as stream:
        header = read_header(stream)
        records = list(iter_records(stream))
    return header, records


def write_jsonl(path: Path, header: SessionHeader, records: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps({"type": "header", **header.__dict__}, separators=(",", ":")) + "\n")
        for record in records:
            output.write(json.dumps(record, separators=(",", ":")) + "\n")


def write_csv(path: Path, records: list[dict]) -> None:
    fields = ["timestamp_us", "sequence", "stage", "direction", "can_id", "can_id_hex", "dlc",
              "data", "input", "mutated", "synthetic", "diagnostic_consumed",
              "diagnostic_transport", "gateway_dropped"]
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for record in records:
            if record.get("type") != "frame":
                continue
            writer.writerow({
                "timestamp_us": record["timestampUs"], "sequence": record["sequence"],
                "stage": record["stage"], "direction": record["direction"],
                "can_id": record["canId"], "can_id_hex": record["canIdHex"],
                "dlc": record["dlc"], "data": record["data"], "input": record.get("input", ""),
                "mutated": record["mutated"], "synthetic": record["synthetic"],
                "diagnostic_consumed": record["diagnosticConsumed"],
                "diagnostic_transport": record["diagnosticTransport"],
                "gateway_dropped": record["gatewayDropped"],
            })


def print_summary(path: Path, header: SessionHeader, records: list[dict]) -> None:
    types = Counter(record.get("type", "unknown") for record in records)
    stages = Counter(record.get("stage") for record in records if record.get("type") == "frame")
    directions = Counter(record.get("direction") for record in records if record.get("type") == "frame")
    identifiers = Counter(record.get("canIdHex") for record in records if record.get("type") == "frame")
    annotation_kinds = Counter(
        f"{record.get('source')}/{record.get('kind')}"
        for record in records if record.get("type") == "annotation"
    )
    invalid_json = sum(
        1 for record in records
        if record.get("type") in {"annotation", "end"} and record.get("jsonValid") is False
    )
    diagnostic_tx = sum(
        1 for record in records
        if record.get("type") == "frame" and record.get("diagnosticTransport")
    )
    diagnostic_rx = sum(
        1 for record in records
        if record.get("type") == "frame" and record.get("diagnosticConsumed")
    )
    diagnostic_events = Counter(
        record.get("data", {}).get("event")
        for record in records
        if record.get("type") == "annotation" and record.get("kind") == "diagnostic" and
        isinstance(record.get("data"), dict) and record.get("data", {}).get("event")
    )
    print(f"SignalScope session: {path}")
    print(f"  bytes={path.stat().st_size} capacity={header.capacity_bytes} "
          f"scope={SCOPES.get(header.scope, f'unknown_{header.scope}')} "
          f"durable={header.durable} "
          f"end_crc_required={header.end_crc_required} "
          f"reset_code={header.reset_reason} recovery_reset_code={header.recovery_reset_reason}")
    print(f"  records={len(records)} frames={types['frame']} annotations={types['annotation']} end={types['end']}")
    print(f"  stages={dict(stages)} directions={dict(directions)}")
    if identifiers:
        print("  busiest_ids=" + ", ".join(f"{key}:{value}" for key, value in identifiers.most_common(12)))
    if annotation_kinds:
        print("  annotations=" + ", ".join(f"{key}:{value}" for key, value in annotation_kinds.items()))
    if diagnostic_tx or diagnostic_rx or diagnostic_events:
        print(f"  diagnostics tx_accepted={diagnostic_tx} consumed_rx={diagnostic_rx} "
              f"events={dict(diagnostic_events)}")
    end_record = next(
        (record for record in reversed(records)
         if record.get("type") == "end" and isinstance(record.get("data"), dict)),
        None,
    )
    end = end_record.get("data", {}) if end_record else {}
    prefix_integrity_valid = None
    end_integrity_valid = None
    integrity_valid = None
    if end_record and "dataBytes" in end and "crc32" in end:
        try:
            data_bytes = int(end["dataBytes"])
            expected_crc = int(str(end["crc32"]), 16)
            prefix = path.read_bytes()[:data_bytes]
            prefix_integrity_valid = (data_bytes == end_record["offset"] and
                                      len(prefix) == data_bytes and
                                      (zlib.crc32(prefix) & 0xFFFFFFFF) == expected_crc)
        except (TypeError, ValueError, OSError):
            prefix_integrity_valid = False
    if end_record and "endCrc32" in end:
        try:
            raw_json = end_record["rawJson"]
            marker = ',"endCrc32":"'
            marker_offset = raw_json.rindex(marker)
            base_json = (raw_json[:marker_offset] + "}").encode()
            end_integrity_valid = (
                (zlib.crc32(base_json) & 0xFFFFFFFF) ==
                int(str(end["endCrc32"]), 16)
            )
        except (KeyError, TypeError, ValueError):
            end_integrity_valid = False
    if prefix_integrity_valid is not None or end_integrity_valid is not None:
        integrity_valid = prefix_integrity_valid is True and end_integrity_valid is True
    if end:
        print(f"  end reason={end.get('reason', '')} duration_ms={end.get('durationMs', 0)} "
              f"trace_drops={end.get('traceDrops', 0)} "
              f"queue_drops={end.get('traceQueueDrops', 0)} "
              f"record_drops={end.get('recordDrops', 0)} "
              f"annotation_rejected={end.get('annotationRejected', 0)} "
              f"truncated={end.get('truncated', False)} integrity={integrity_valid} "
              f"prefix_integrity={prefix_integrity_valid} end_integrity={end_integrity_valid}")
    missing_end = types["end"] == 0
    if (types["truncated"] or types["invalid"] or invalid_json or missing_end or
            integrity_valid is False or
            end.get("truncated") or end.get("traceDrops", 0) or
            end.get("annotationRejected", 0)):
        print(f"  WARNING truncated_records={types['truncated']} invalid={types['invalid']} "
              f"invalid_json={invalid_json} missing_end={missing_end} "
              f"integrity={integrity_valid} "
              f"session_loss={end.get('traceDrops', 0)} "
              f"annotation_rejected={end.get('annotationRejected', 0)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path, help="downloaded .sslog file")
    parser.add_argument("--jsonl", type=Path, help="write all decoded records as JSON Lines")
    parser.add_argument("--csv", type=Path, help="write decoded CAN frame records as CSV")
    args = parser.parse_args()

    header, records = load_session(args.session)
    print_summary(args.session, header, records)
    if args.jsonl:
        write_jsonl(args.jsonl, header, records)
        print(f"  wrote JSONL: {args.jsonl}")
    if args.csv:
        write_csv(args.csv, records)
        print(f"  wrote CSV: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
