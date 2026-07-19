#!/usr/bin/env python3
"""Serve the standalone UI with a tiny in-memory SignalScope API.

This preview is intentionally dependency-free.  It lets contributors inspect
and click through the browser application without a CAN interface or ESP32.
Nothing from this server is sent to a physical bus.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import json
import math
import mimetypes
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


DATA_ROOT = (Path(__file__).resolve().parents[1] / "data").resolve()


class PreviewState:
    """Mutable device-like state shared by requests during one preview run."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.epoch = 4
        # Keep the proposed table separate from the committed table, just like
        # firmware. An enabled flag describes a rule; it never identifies the
        # table that contains that rule.
        self.candidate_rules: list[dict[str, object]] = []
        self.active_rules: list[dict[str, object]] = []
        self.candidate_pending = False
        self.pending_rule_ids: set[int] = set()
        self.package = ""
        self.package_row_types: list[str] = []
        # Stored package text and the package currently backing live rules are
        # separate states. RAM apply/DBC swaps clear the selection without
        # deleting the simulated LittleFS file.
        self.active_package_path = ""
        self.recording = False
        self.recorded_frames = 0


STATE = PreviewState()

SIGNALS = [
    {
        "index": 0,
        "canId": 0x321,
        "canIdHex": "0x321",
        "name": "OilTemperature",
        "startBit": 0,
        "length": 8,
        "littleEndian": True,
        "signed": False,
        "factor": 1.0,
        "offset": -40.0,
        "valid": True,
        "live": True,
        "value": 90.0,
        "ageMs": 18,
        "direction": "A_TO_B",
    },
    {
        "index": 1,
        "canId": 0x321,
        "canIdHex": "0x321",
        "name": "EngineSpeed",
        "startBit": 8,
        "length": 16,
        "littleEndian": True,
        "signed": False,
        "factor": 0.25,
        "offset": 0.0,
        "valid": True,
        "live": True,
        "value": 842.0,
        "ageMs": 18,
        "direction": "A_TO_B",
    },
    {
        "index": 2,
        "canId": 0x321,
        "canIdHex": "0x321",
        "name": "VehicleSpeed",
        "startBit": 24,
        "length": 16,
        "littleEndian": True,
        "signed": False,
        "factor": 0.01,
        "offset": 0.0,
        "valid": True,
        "live": False,
        "value": 0.0,
        "ageMs": 2420,
        "direction": "A_TO_B",
    },
]


class PackageParseError(ValueError):
    """A package row could not be represented by the real rule engine."""


def parse_unsigned(text: str, maximum: int, label: str) -> int:
    """Parse the package's decimal/0x integer spelling without truncation."""

    value_text = text.strip()
    if not value_text or value_text.startswith(("-", "+")):
        raise PackageParseError(f"{label} must be an unsigned integer")
    base = 16 if value_text.lower().startswith("0x") else 10
    digits = value_text[2:] if base == 16 else value_text
    if not digits or any(character not in ("0123456789abcdefABCDEF" if base == 16 else "0123456789")
                         for character in digits):
        raise PackageParseError(f"{label} must be decimal or 0x hexadecimal")
    value = int(digits, base)
    if value > maximum:
        raise PackageParseError(f"{label} is outside 0..{maximum}")
    return value


def parse_float(text: str, label: str) -> float:
    value_text = text.strip()
    if not value_text:
        raise PackageParseError(f"{label} is required")
    try:
        value = float(value_text)
    except ValueError as error:
        raise PackageParseError(f"{label} must be a number") from error
    if not math.isfinite(value):
        raise PackageParseError(f"{label} must be finite")
    return value


def parse_flag(text: str, label: str) -> bool:
    if text == "0":
        return False
    if text == "1":
        return True
    raise PackageParseError(f"{label} must be 0 or 1")


def parse_name(text: str, label: str) -> str:
    value = text.strip()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]{0,30}", value):
        raise PackageParseError(
            f"{label} must start with a letter or underscore and use only letters, numbers, _, ., or -"
        )
    return value


def validate_bit_range(start_bit: int, length: int, little_endian: bool, maximum_length: int) -> None:
    if length < 1 or length > maximum_length:
        raise PackageParseError(f"field length must be 1..{maximum_length}")
    if little_endian:
        if start_bit + length > 64:
            raise PackageParseError("little-endian field extends beyond the eight-byte frame")
        return

    frame_bit = start_bit
    for _ in range(length):
        if frame_bit > 63:
            raise PackageParseError("big-endian field extends beyond the eight-byte frame")
        frame_bit = frame_bit + 15 if frame_bit % 8 == 0 else frame_bit - 1


def common_package_rule(fields: list[str], maximum_length: int) -> dict[str, object]:
    can_id = parse_unsigned(fields[1], 0x1FFFFFFF, "CAN ID")
    direction = fields[2]
    if direction not in {"A_TO_B", "B_TO_A"}:
        raise PackageParseError("direction must be A_TO_B or B_TO_A")
    start_bit = parse_unsigned(fields[3], 63, "start bit")
    length = parse_unsigned(fields[4], maximum_length, "field length")
    little_endian = parse_flag(fields[5], "little_endian")
    validate_bit_range(start_bit, length, little_endian, maximum_length)
    return {
        "can_id": can_id,
        "direction": direction,
        "start_bit": start_bit,
        "length": length,
        "little_endian": little_endian,
    }


def base_rule(package_type: str, kind: str, common: dict[str, object] | None = None) -> dict[str, object]:
    rule: dict[str, object] = {
        # `kind` mirrors RuleKind in GET /api/rules. `package_type` preserves
        # the declarative spelling that produced that engine rule.
        "kind": kind,
        "package_type": package_type,
        "active": True,
        "enabled": True,
        "dynamic": False,
        "manual_dynamic": False,
        "value_source": "",
        "replace_value": 0,
        "replace_value_text": "0",
        "runtime_value": 0,
        "runtime_value_text": "0",
        "runtime_value_kind": "none",
        "sequence_count": 0,
        "start_bit": 0,
        "length": 1,
        "little_endian": True,
    }
    if common:
        rule.update(common)
    return rule


def rule_identity(rule: dict[str, object]) -> tuple[object, ...]:
    kind = rule["kind"]
    prefix = (kind, rule["can_id"], rule["direction"])
    if kind in {"BIT_RANGE", "COUNTER", "SEQUENCE8"}:
        return prefix + (
            rule["start_bit"], rule["length"], rule["little_endian"], rule["dynamic"]
        )
    if kind in {"CHECKSUM_XOR", "CHECKSUM_CRC8_AUTOSAR"}:
        return prefix + (rule["checksum_target_byte"],)
    return prefix


def parse_selector_states(text: str, label: str) -> list[int]:
    parts = text.split("|")
    if not parts or any(not part.strip() for part in parts):
        raise PackageParseError(f"{label} must contain selector values separated by |")
    values = [parse_unsigned(part, 15, label) for part in parts]
    if len(set(values)) != len(values):
        raise PackageParseError(f"{label} contains a duplicate selector")
    return values


def apply_active_binding(rule: dict[str, object], active_binding: tuple[str, list[int]] | None) -> None:
    if active_binding is None or rule.get("selector_source"):
        return
    selector_source, states = active_binding
    rule["selector_source"] = selector_source
    rule["selector_states"] = list(states)
    rule["selector_active_mask"] = sum(1 << state for state in states)


def parse_rule_package(text: str) -> tuple[list[dict[str, object]], list[str]]:
    """Parse a .ssrules package into the mutation rows the preview can show.

    Runtime source/table existence cannot be proven without a native
    application extension, but all package syntax, widths, ranges, selector
    maps, binding order, and engine-level frame constraints are validated.
    """

    if not text or not text.strip():
        raise PackageParseError("package is empty")
    if "\0" in text:
        raise PackageParseError("package contains a NUL byte")

    rules: list[dict[str, object]] = []
    identities: dict[tuple[object, ...], int] = {}
    row_types: list[str] = []
    table_binding: tuple[str, str] | None = None
    override_binding: tuple[str, str, str] | None = None
    active_binding: tuple[str, list[int]] | None = None

    def add_rule(rule: dict[str, object]) -> None:
        identity = rule_identity(rule)
        if identity in identities:
            rules[identities[identity]] = rule
            return
        if len(rules) >= 96:
            raise PackageParseError("package exceeds the 96-rule limit")
        identities[identity] = len(rules)
        rules.append(rule)

    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if '"' in line:
            raise PackageParseError(f"line {line_number}: quoted CSV fields are not supported")
        fields = [field.strip() for field in line.split(",")]
        row_type = fields[0] if fields else ""
        try:
            if row_type == "BIND_TABLE":
                if len(fields) != 3:
                    raise PackageParseError("BIND_TABLE requires exactly 3 fields")
                table_binding = (
                    parse_name(fields[1], "bound value source"),
                    parse_name(fields[2], "runtime table name"),
                )
            elif row_type == "BIND_OVERRIDE":
                if len(fields) != 4:
                    raise PackageParseError("BIND_OVERRIDE requires exactly 4 fields")
                override_binding = (
                    parse_name(fields[1], "bound value source"),
                    parse_name(fields[2], "override active source"),
                    parse_name(fields[3], "override value source"),
                )
            elif row_type == "BIND_ACTIVE":
                if len(fields) != 3:
                    raise PackageParseError("BIND_ACTIVE requires exactly 3 fields")
                active_binding = (
                    parse_name(fields[1], "active selector source"),
                    parse_selector_states(fields[2], "active selector states"),
                )
            elif row_type == "STATIC":
                if len(fields) != 7:
                    raise PackageParseError("STATIC requires exactly 7 fields")
                common = common_package_rule(fields, 64)
                raw_value = parse_unsigned(fields[6], 0xFFFFFFFFFFFFFFFF, "raw value")
                field_maximum = (1 << int(common["length"])) - 1
                if raw_value > field_maximum:
                    raise PackageParseError("raw value does not fit inside the target field")
                rule = base_rule(row_type, "BIT_RANGE", common)
                rule["replace_value"] = raw_value
                rule["replace_value_text"] = str(raw_value)
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            elif row_type == "SOURCE_INT":
                if len(fields) != 15:
                    raise PackageParseError("SOURCE_INT requires exactly 15 fields")
                common = common_package_rule(fields, 32)
                value_source = parse_name(fields[6], "value source")
                rule = base_rule(row_type, "BIT_RANGE", common)
                rule.update({
                    "dynamic": True,
                    "value_source": value_source,
                    "source_gain": parse_float(fields[7], "source gain"),
                    "source_offset": parse_float(fields[8], "source offset"),
                    "output_scale": parse_float(fields[9], "output scale"),
                    "output_offset": parse_float(fields[10], "output offset"),
                    "zero_threshold": parse_float(fields[11], "zero threshold"),
                    "zero_output": parse_unsigned(fields[12], 0xFFFFFFFF, "zero output"),
                    "full_threshold": parse_float(fields[13], "full threshold"),
                    "full_output": parse_unsigned(fields[14], 0xFFFFFFFF, "full output"),
                })
                if table_binding and table_binding[0] == value_source:
                    rule["lookup_table"] = table_binding[1]
                if override_binding and override_binding[0] == value_source:
                    rule["override_active_source"] = override_binding[1]
                    rule["override_value_source"] = override_binding[2]
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            elif row_type == "SOURCE_SELECT_INT":
                if len(fields) != 15:
                    raise PackageParseError("SOURCE_SELECT_INT requires exactly 15 fields")
                common = common_package_rule(fields, 32)
                value_source = parse_name(fields[6], "value source")
                selector_source = parse_name(fields[13], "selector source")
                entries: list[dict[str, object]] = []
                seen_selectors: set[int] = set()
                for entry_text in fields[14].split("|"):
                    parts = [part.strip() for part in entry_text.split(":")]
                    if len(parts) not in {3, 4}:
                        raise PackageParseError("selector entry must be selector:D:value or selector:S:scale:full")
                    selector = parse_unsigned(parts[0], 15, "selector")
                    if selector in seen_selectors:
                        raise PackageParseError("selector map contains a duplicate selector")
                    seen_selectors.add(selector)
                    mode = parts[1]
                    if mode == "D" and len(parts) == 3:
                        entries.append({
                            "selector": selector,
                            "mode": "direct",
                            "output": parse_unsigned(parts[2], 0xFFFFFFFF, "direct output"),
                        })
                    elif mode == "S" and len(parts) == 4:
                        entries.append({
                            "selector": selector,
                            "mode": "scale",
                            "output_scale": parse_float(parts[2], "selector output scale"),
                            "full_output": parse_unsigned(parts[3], 0xFFFFFFFF, "selector full output"),
                        })
                    else:
                        raise PackageParseError("selector entry type/field count is invalid")
                if not entries:
                    raise PackageParseError("selector map is empty")
                rule = base_rule(row_type, "BIT_RANGE", common)
                rule.update({
                    "dynamic": True,
                    "value_source": value_source,
                    "source_gain": parse_float(fields[7], "source gain"),
                    "source_offset": parse_float(fields[8], "source offset"),
                    "output_offset": parse_float(fields[9], "output offset"),
                    "zero_threshold": parse_float(fields[10], "zero threshold"),
                    "zero_output": parse_unsigned(fields[11], 0xFFFFFFFF, "zero output"),
                    "full_threshold": parse_float(fields[12], "full threshold"),
                    "selector_source": selector_source,
                    "selector_entries": entries,
                    "selector_active_mask": sum(1 << int(entry["selector"]) for entry in entries),
                })
                if table_binding and table_binding[0] == value_source:
                    rule["lookup_table"] = table_binding[1]
                if override_binding and override_binding[0] == value_source:
                    rule["override_active_source"] = override_binding[1]
                    rule["override_value_source"] = override_binding[2]
                add_rule(rule)
            elif row_type == "COUNTER":
                if len(fields) != 10:
                    raise PackageParseError("COUNTER requires exactly 10 fields")
                common = common_package_rule(fields, 32)
                field_maximum = (1 << int(common["length"])) - 1
                rule = base_rule(row_type, "COUNTER", common)
                rule.update({
                    "counter_initial": parse_unsigned(fields[6], field_maximum, "counter initial"),
                    "counter_step": parse_unsigned(fields[7], 0xFFFFFFFF, "counter step"),
                    "counter_wrap_after": parse_unsigned(fields[8], field_maximum, "counter wrap-after"),
                    "counter_wrap_to": parse_unsigned(fields[9], field_maximum, "counter wrap-to"),
                })
                rule["runtime_value"] = rule["counter_initial"]
                rule["runtime_value_text"] = str(rule["counter_initial"])
                rule["runtime_value_kind"] = "counter_state"
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            elif row_type == "SEQUENCE8":
                if len(fields) != 8:
                    raise PackageParseError("SEQUENCE8 requires exactly 8 fields")
                common = common_package_rule(fields, 8)
                field_maximum = (1 << int(common["length"])) - 1
                sequence_parts = fields[6].split("|")
                if not 1 <= len(sequence_parts) <= 16 or any(not part.strip() for part in sequence_parts):
                    raise PackageParseError("sequence must contain 1..16 values")
                values = [parse_unsigned(part, field_maximum, "sequence value") for part in sequence_parts]
                initial_index = parse_unsigned(fields[7], 15, "sequence initial index")
                if initial_index >= len(values):
                    raise PackageParseError("sequence initial index is outside the sequence")
                rule = base_rule(row_type, "SEQUENCE8", common)
                rule["sequence_values"] = values
                rule["sequence_initial_index"] = initial_index
                rule["runtime_value"] = initial_index
                rule["runtime_value_text"] = str(initial_index)
                rule["runtime_value_kind"] = "sequence_index"
                rule["sequence_count"] = len(values)
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            elif row_type == "CHECKSUM_XOR":
                if len(fields) != 8:
                    raise PackageParseError("CHECKSUM_XOR requires exactly 8 fields")
                can_id = parse_unsigned(fields[1], 0x1FFFFFFF, "CAN ID")
                direction = fields[2]
                if direction not in {"A_TO_B", "B_TO_A"}:
                    raise PackageParseError("direction must be A_TO_B or B_TO_A")
                target = parse_unsigned(fields[3], 7, "checksum target byte")
                start = parse_unsigned(fields[4], 7, "checksum start byte")
                end = parse_unsigned(fields[5], 7, "checksum end byte")
                if start > end:
                    raise PackageParseError("checksum start byte must not exceed end byte")
                rule = base_rule(row_type, "CHECKSUM_XOR")
                rule.update({
                    "can_id": can_id,
                    "direction": direction,
                    "checksum_target_byte": target,
                    "checksum_start_byte": start,
                    "checksum_end_byte": end,
                    "checksum_seed": parse_unsigned(fields[6], 255, "checksum seed"),
                    "enabled": parse_flag(fields[7], "checksum enabled"),
                })
                rule["active"] = rule["enabled"]
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            elif row_type == "CHECKSUM_CRC8_AUTOSAR":
                if len(fields) != 9:
                    raise PackageParseError("CHECKSUM_CRC8_AUTOSAR requires exactly 9 fields")
                can_id = parse_unsigned(fields[1], 0x1FFFFFFF, "CAN ID")
                direction = fields[2]
                if direction not in {"A_TO_B", "B_TO_A"}:
                    raise PackageParseError("direction must be A_TO_B or B_TO_A")
                target = parse_unsigned(fields[3], 7, "checksum target byte")
                counter = parse_unsigned(fields[4], 7, "checksum counter byte")
                start = parse_unsigned(fields[5], 7, "checksum start byte")
                end = parse_unsigned(fields[6], 7, "checksum end byte")
                if start > end:
                    raise PackageParseError("checksum start byte must not exceed end byte")
                id_parts = fields[7].split("|")
                if len(id_parts) != 16 or any(not part.strip() for part in id_parts):
                    raise PackageParseError("AUTOSAR data-ID list must contain exactly 16 bytes")
                rule = base_rule(row_type, "CHECKSUM_CRC8_AUTOSAR")
                rule.update({
                    "can_id": can_id,
                    "direction": direction,
                    "checksum_target_byte": target,
                    "checksum_counter_byte": counter,
                    "checksum_start_byte": start,
                    "checksum_end_byte": end,
                    "data_ids": [parse_unsigned(part, 255, "AUTOSAR data ID") for part in id_parts],
                    "enabled": parse_flag(fields[8], "checksum enabled"),
                })
                rule["active"] = rule["enabled"]
                apply_active_binding(rule, active_binding)
                add_rule(rule)
            else:
                raise PackageParseError(f"unsupported row type {row_type or '<empty>'}")
        except PackageParseError as error:
            if str(error).startswith("line "):
                raise
            raise PackageParseError(f"line {line_number}: {error}") from error
        row_types.append(row_type)

    if not row_types:
        raise PackageParseError("package contains only comments or blank lines")
    if not rules:
        raise PackageParseError("package contains bind directives but no mutation rules")

    for rule_id, rule in enumerate(rules):
        rule["rule_id"] = rule_id
        rule["priority"] = rule_id
    return rules, row_types


def rule_from_form(form: dict[str, list[str]]) -> dict[str, object]:
    """Translate the public staging form into the shape returned by GET rules."""

    def first(name: str, default: str) -> str:
        return form.get(name, [default])[0]

    def form_flag(name: str, default: bool) -> bool:
        text = first(name, "1" if default else "0")
        if text in {"1", "true", "TRUE", "on"}:
            return True
        if text in {"0", "false", "FALSE", "off"}:
            return False
        raise PackageParseError(f"{name} must be true or false")

    kind = first("rule_kind", first("kind", "BIT_RANGE"))
    if kind not in {"BIT_RANGE", "RAW_MASK"}:
        raise PackageParseError("only BIT_RANGE and RAW_MASK can be staged directly")
    can_id = parse_unsigned(first("can_id", ""), 0x1FFFFFFF, "CAN ID")
    direction = first("direction", "A_TO_B")
    if direction not in {"A_TO_B", "B_TO_A"}:
        raise PackageParseError("direction must be A_TO_B or B_TO_A")
    enabled = form_flag("enabled", True)

    start_bit = 0
    length = 1
    little_endian = True
    dynamic = False
    replace_value = 0
    mask = ""
    mask_value = ""
    if kind == "RAW_MASK":
        mask = first("mask", "").strip()
        mask_value = first("value", "").strip()
        if not re.fullmatch(r"[0-9A-Fa-f]{16}", mask) or not re.fullmatch(r"[0-9A-Fa-f]{16}", mask_value):
            raise PackageParseError("RAW_MASK mask and value must each contain exactly eight hexadecimal bytes")
        mask = mask.upper()
        mask_value = mask_value.upper()
    else:
        start_bit = parse_unsigned(first("start_bit", "0"), 63, "start bit")
        length = parse_unsigned(first("length", "8"), 64, "field length")
        little_endian = form_flag("little_endian", True)
        dynamic = form_flag("dynamic", False)
        validate_bit_range(start_bit, length, little_endian, 32 if dynamic else 64)
        replace_value = parse_unsigned(first("replace_value", first("op_value1", "0")),
                                       0xFFFFFFFFFFFFFFFF, "replacement value")
        if replace_value > (1 << length) - 1:
            raise PackageParseError("replacement value does not fit inside the target field")

    with STATE.lock:
        STATE.epoch += 1
        rule = {
            "rule_epoch": STATE.epoch,
            # `active` is a compatibility alias for runtime enabled state.
            "active": enabled,
            "enabled": enabled,
            "kind": kind,
            "can_id": can_id,
            "direction": direction,
            "start_bit": start_bit,
            "length": length,
            "little_endian": little_endian,
            "dynamic": dynamic,
            "manual_dynamic": dynamic,
            "value_source": "",
            "replace_value": replace_value,
            "replace_value_text": str(replace_value),
            "runtime_value": replace_value if dynamic else 0,
            "runtime_value_text": str(replace_value if dynamic else 0),
            "runtime_value_kind": "raw" if dynamic else "none",
            "sequence_count": 0,
        }
        if kind == "RAW_MASK":
            rule["mask"] = mask
            rule["value"] = mask_value
        identity = rule_identity(rule)
        existing_index = next(
            (index for index, candidate in enumerate(STATE.candidate_rules)
             if rule_identity(candidate) == identity),
            None,
        )
        if existing_index is None:
            if len(STATE.candidate_rules) >= 96:
                raise PackageParseError("candidate exceeds the 96-rule limit")
            rule_id = len(STATE.candidate_rules)
            rule["rule_id"] = rule_id
            rule["priority"] = rule_id
            STATE.candidate_rules.append(rule)
        else:
            previous = STATE.candidate_rules[existing_index]
            rule["rule_id"] = previous["rule_id"]
            rule["priority"] = previous.get("priority", existing_index)
            STATE.candidate_rules[existing_index] = rule
        STATE.pending_rule_ids.add(int(rule["rule_id"]))
        STATE.candidate_pending = True
        return dict(rule)


def copy_rules(rules: list[dict[str, object]]) -> list[dict[str, object]]:
    """Copy a rule table so candidate edits cannot leak into active behavior."""

    return deepcopy(rules)


def compile_active_rules(rules: list[dict[str, object]]) -> list[dict[str, object]]:
    """Mirror MutationEngine's grouped active-table layout and priorities."""

    order = {
        "BIT_RANGE": 0,
        "RAW_MASK": 1,
        "COUNTER": 2,
        "SEQUENCE8": 3,
        "CHECKSUM_XOR": 4,
        "CHECKSUM_CRC8_AUTOSAR": 5,
    }
    authored = copy_rules(rules)
    group_order: list[tuple[int, str]] = []
    grouped: dict[tuple[int, str], list[dict[str, object]]] = {}
    for rule in authored:
        key = (int(rule.get("can_id", 0)), str(rule.get("direction", "A_TO_B")))
        if key not in grouped:
            grouped[key] = []
            group_order.append(key)

    priority = 0
    for wanted_kind, _ in sorted(order.items(), key=lambda item: item[1]):
        for rule in authored:
            if str(rule.get("kind")) != wanted_kind:
                continue
            key = (int(rule.get("can_id", 0)), str(rule.get("direction", "A_TO_B")))
            rule["priority"] = priority
            grouped[key].append(rule)
            priority += 1

    # Firmware stores groups by the first authored occurrence, while the
    # compile priority above is global across rule kinds. Consequently the
    # returned list can intentionally have non-monotonic priority numbers.
    return [rule for key in group_order for rule in grouped[key]]


def find_rule_by_id(rules: list[dict[str, object]], rule_id: int) -> dict[str, object] | None:
    return next((rule for rule in rules if int(rule.get("rule_id", -1)) == rule_id), None)


def stamp_rules(rules: list[dict[str, object]], epoch: int) -> None:
    """Give every mock handle the generation returned beside its table."""

    for rule in rules:
        rule["rule_epoch"] = epoch


class PreviewHandler(BaseHTTPRequestHandler):
    server_version = "SignalScopePreview/1.0"

    def log_message(self, message: str, *args: object) -> None:
        print(f"[preview] {self.address_string()} {message % args}")

    def send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            # Headless browsers and page refreshes may close an old polling
            # request after the response headers arrive. That is normal for a
            # preview server and should not bury useful logs in a traceback.
            pass

    def send_json(self, payload: object, status: int = 200) -> None:
        self.send_bytes(status, json.dumps(payload).encode("utf-8"), "application/json; charset=utf-8")

    def read_body(self) -> str:
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length).decode("utf-8") if length else ""

    def form_body(self, body: str) -> dict[str, list[str]]:
        return parse_qs(body, keep_blank_values=True)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        request = urlparse(self.path)
        query = parse_qs(request.query, keep_blank_values=True)
        if request.path == "/api/status":
            with STATE.lock:
                active = len(STATE.active_rules)
                candidate = len(STATE.candidate_rules)
                package_path = STATE.active_package_path
                epoch = STATE.epoch
                candidate_dirty = STATE.candidate_pending
            self.send_json({
                "bus_a_ready": True,
                "bus_b_ready": True,
                "bus_a_util_pct": 13,
                "bus_b_util_pct": 11,
                "frame_rate_fps": 112,
                "dropped_frames": 0,
                "dbc_loaded": True,
                "dbc_complete": True,
                "dbc_path": "/dbc/default.dbc",
                "dbc_message_count": 1,
                "dbc_signal_count": len(SIGNALS),
                "active_mutations": active,
                # Count every candidate row, including disabled candidates.
                "staging_mutations": candidate,
                "candidate_dirty": candidate_dirty,
                "rule_package_path": package_path,
                "rule_epoch": epoch,
            })
            return
        if request.path == "/api/frame_cache":
            self.send_json({"ok": True, "count": 2, "frames": [
                {"can_id": 0x321, "direction": "A_TO_B", "dlc": 8,
                 "timestamp_us": 123456789, "rate_hz": 50.0, "mutated": False,
                 "data": "82 28 0D 00 00 00 00 00"},
                {"can_id": 0x456, "direction": "B_TO_A", "dlc": 8,
                 "timestamp_us": 123450000, "rate_hz": 10.0, "mutated": False,
                 "data": "00 10 20 30 40 50 60 70"},
            ]})
            return
        if request.path == "/api/signal_catalog":
            rows = SIGNALS
            if "indexes" in query:
                wanted = {int(item) for item in query["indexes"][0].split(",") if item}
                rows = [signal for signal in rows if signal["index"] in wanted]
            else:
                needle = query.get("q", [""])[0].casefold()
                rows = [signal for signal in rows if needle in str(signal["name"]).casefold()]
            total = len(rows)
            offset = int(query.get("offset", ["0"])[0] or 0)
            limit = int(query.get("limit", ["48"])[0] or 48)
            self.send_json({"ok": True, "total": total, "signals": rows[offset:offset + limit]})
            return
        if request.path == "/api/rules":
            view = query.get("view", ["active"])[0]
            if view not in {"active", "staging"}:
                self.send_json({"ok": False, "error": "invalid_rules_view"}, 400)
                return
            with STATE.lock:
                source = STATE.candidate_rules if view == "staging" else STATE.active_rules
                rules = copy_rules(source)
                epoch = STATE.epoch
                candidate_dirty = STATE.candidate_pending
                stamp_rules(rules, epoch)
            self.send_json({"ok": True, "view": view, "count": len(rules),
                            "candidate_dirty": candidate_dirty,
                            "rule_epoch": epoch, "rules": rules})
            return
        if request.path == "/api/rules/package":
            with STATE.lock:
                package = STATE.package
            if not package:
                self.send_json({"ok": False, "error": "rule_package_not_found"}, 404)
            else:
                self.send_bytes(200, package.encode("utf-8"), "text/plain; charset=utf-8")
            return
        if request.path == "/api/log":
            with STATE.lock:
                active = STATE.recording
                frames = STATE.recorded_frames
            self.send_json({"ok": True, "phase": "recording" if active else "idle",
                            "active": active, "frames": frames, "bytes": frames * 24})
            return
        if request.path.startswith("/api/"):
            self.send_json({"ok": False, "error": "preview_route_not_implemented"}, 404)
            return
        self.serve_static(request.path)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        request = urlparse(self.path)
        body = self.read_body()
        form = self.form_body(body)
        if request.path == "/api/dbc":
            with STATE.lock:
                STATE.active_rules = []
                STATE.candidate_rules = []
                STATE.candidate_pending = False
                STATE.pending_rule_ids.clear()
                STATE.active_package_path = ""
                STATE.epoch += 1
            self.send_json({"ok": True, "messages": 1, "signals": len(SIGNALS)})
            return
        if request.path == "/api/dbc/autoload":
            with STATE.lock:
                STATE.active_rules = []
                STATE.candidate_rules = []
                STATE.candidate_pending = False
                STATE.pending_rule_ids.clear()
                STATE.active_package_path = ""
                STATE.epoch += 1
            self.send_json({"ok": True, "autoloaded": True, "messages": 1, "signals": len(SIGNALS)})
            return
        if request.path == "/api/rules/stage":
            try:
                rule = rule_from_form(form)
            except PackageParseError as error:
                self.send_json({
                    "ok": False,
                    "error": "invalid_rule_stage",
                    "detail": str(error),
                }, 400)
                return
            with STATE.lock:
                staging_count = len(STATE.candidate_rules)
            self.send_json({"ok": True, "rule_id": rule["rule_id"],
                            "rule_epoch": rule["rule_epoch"], "staging_count": staging_count})
            return
        if request.path == "/api/rules":
            with STATE.lock:
                if "apply_commit" in body:
                    STATE.active_rules = compile_active_rules(STATE.candidate_rules)
                    STATE.candidate_pending = False
                    STATE.pending_rule_ids.clear()
                    STATE.active_package_path = ""
                    action = "apply_commit"
                elif "revert" in body:
                    STATE.candidate_rules = copy_rules(STATE.active_rules)
                    STATE.candidate_pending = False
                    STATE.pending_rule_ids.clear()
                    action = "revert"
                elif "clear_rules" in body:
                    STATE.active_rules = []
                    STATE.candidate_rules = []
                    STATE.candidate_pending = False
                    STATE.pending_rule_ids.clear()
                    STATE.active_package_path = ""
                    action = "clear_rules"
                elif "clear_staging" in body:
                    STATE.candidate_rules = []
                    STATE.candidate_pending = bool(STATE.active_rules)
                    STATE.pending_rule_ids.clear()
                    action = "clear_staging"
                else:
                    self.send_json({"ok": False, "error": "unknown_action"}, 400)
                    return
                STATE.epoch += 1
                epoch = STATE.epoch
                stamp_rules(STATE.active_rules, epoch)
                stamp_rules(STATE.candidate_rules, epoch)
            self.send_json({"ok": True, "action": action, "rule_epoch": epoch})
            return
        if request.path in {"/api/rules/value", "/api/rules/enable"}:
            query = parse_qs(request.query, keep_blank_values=True)
            view_supplied = "view" in form or "view" in query
            view = (form.get("view") or query.get("view") or ["active"])[0]
            if view not in {"active", "staging"}:
                self.send_json({"ok": False, "error": "invalid_rules_view"}, 400)
                return
            try:
                rule_id = int((form.get("rule_id") or query.get("rule_id") or ["-1"])[0], 10)
            except ValueError:
                self.send_json({"ok": False, "error": "invalid_rule_id"}, 400)
                return
            if rule_id < 0 or rule_id >= 96:
                self.send_json({"ok": False, "error": "invalid_rule_id"}, 400)
                return
            try:
                expected_epoch = int((form.get("rule_epoch") or form.get("epoch") or
                                      query.get("rule_epoch") or query.get("epoch") or ["0"])[0], 10)
            except ValueError:
                expected_epoch = -1
            with STATE.lock:
                if expected_epoch != STATE.epoch:
                    self.send_json({"ok": False, "error": "stale_rule_handle"}, 409)
                    return
                candidate_target = find_rule_by_id(STATE.candidate_rules, rule_id)
                active_target = find_rule_by_id(STATE.active_rules, rule_id)
                if view == "staging":
                    target = candidate_target
                    target_view = "staging"
                elif (not view_supplied and candidate_target is not None and
                      rule_id in STATE.pending_rule_ids):
                    # The compatibility API follows an existing-ID replacement
                    # into its pending candidate, matching MutationEngine's
                    # publish_runtime_on_commit behavior.
                    target = candidate_target
                    target_view = "staging"
                elif active_target is not None:
                    target = active_target
                    target_view = "active"
                elif not view_supplied:
                    # Match the firmware's backwards-compatible exception for
                    # a brand-new slot that has not been applied yet.
                    target = candidate_target
                    target_view = "staging"
                else:
                    target = None
                    target_view = "active"
                if target is None:
                    self.send_json({"ok": False, "error": "rule_not_found"}, 404)
                    return
                if request.path.endswith("value"):
                    try:
                        value = parse_unsigned(form.get("value", ["0"])[0], 0xFFFFFFFF, "value")
                    except PackageParseError:
                        self.send_json({"ok": False, "error": "invalid_rule_value"}, 400)
                        return
                    manual_dynamic = (target.get("kind") == "BIT_RANGE" and
                                      target.get("dynamic") is True and
                                      target.get("manual_dynamic") is not False and
                                      not target.get("value_source"))
                    counter = target.get("kind") == "COUNTER"
                    sequence = target.get("kind") == "SEQUENCE8"
                    width = int(target.get("length", 0))
                    if not (manual_dynamic or counter or sequence) or width < 1 or width > 32:
                        self.send_json({"ok": False, "error": "rule_value_rejected"}, 422)
                        return
                    maximum = ((1 << width) - 1) if width < 32 else 0xFFFFFFFF
                    if value > maximum or (sequence and value >= len(target.get("sequence_values", []))):
                        self.send_json({"ok": False, "error": "rule_value_rejected"}, 422)
                        return
                    if manual_dynamic:
                        target["replace_value"] = value
                        target["replace_value_text"] = str(value)
                    target["runtime_value"] = value
                    target["runtime_value_text"] = str(value)
                    if counter:
                        target["counter_initial"] = value
                    elif sequence:
                        target["sequence_initial_index"] = value
                    if (target_view == "active" and candidate_target is not None and
                            rule_id not in STATE.pending_rule_ids):
                        # Live controls keep the candidate shadow synchronized,
                        # just as MutationEngine::setRuleValue does.
                        if manual_dynamic:
                            candidate_target["replace_value"] = value
                            candidate_target["replace_value_text"] = str(value)
                        candidate_target["runtime_value"] = value
                        candidate_target["runtime_value_text"] = str(value)
                        if counter:
                            candidate_target["counter_initial"] = value
                        elif sequence:
                            candidate_target["sequence_initial_index"] = value
                else:
                    enabled = form.get("enabled", ["1"])[0] not in {"0", "false"}
                    target["enabled"] = enabled
                    target["active"] = enabled
                    if (target_view == "active" and candidate_target is not None and
                            rule_id not in STATE.pending_rule_ids):
                        candidate_target["enabled"] = enabled
                        candidate_target["active"] = enabled
                if target_view == "staging":
                    STATE.pending_rule_ids.add(rule_id)
                    STATE.candidate_pending = True
            self.send_json({"ok": True, "view": target_view})
            return
        if request.path == "/api/rules/package":
            try:
                parsed_rules, row_types = parse_rule_package(body)
            except PackageParseError as error:
                # The parser uses candidate storage as scratch. Its failure
                # guard restores a clean active mirror so a later Apply cannot
                # accidentally publish a partially parsed or empty table.
                with STATE.lock:
                    STATE.candidate_rules = copy_rules(STATE.active_rules)
                    STATE.candidate_pending = False
                    STATE.pending_rule_ids.clear()
                    STATE.epoch += 1
                    epoch = STATE.epoch
                    stamp_rules(STATE.active_rules, epoch)
                    stamp_rules(STATE.candidate_rules, epoch)
                self.send_json({
                    "ok": False,
                    "error": "rule_package_invalid",
                    "detail": str(error),
                    "rule_epoch": epoch,
                }, 422)
                return
            with STATE.lock:
                STATE.package = body
                STATE.package_row_types = list(row_types)
                STATE.active_package_path = "/rules/active.ssrules"
                # A successful package install is validate + activate + store,
                # not a file-only save. Candidate remains a separate mirror.
                STATE.candidate_rules = copy_rules(parsed_rules)
                STATE.active_rules = compile_active_rules(parsed_rules)
                STATE.candidate_pending = False
                STATE.pending_rule_ids.clear()
                count = len(STATE.active_rules)
                STATE.epoch += 1
                epoch = STATE.epoch
                stamp_rules(STATE.active_rules, epoch)
                stamp_rules(STATE.candidate_rules, epoch)
            self.send_json({
                "ok": True,
                "path": "/rules/active.ssrules",
                "count": count,
                "parsed_rows": len(row_types),
                "directives": sum(1 for row_type in row_types if row_type.startswith("BIND_")),
                "row_types": row_types,
                "rule_epoch": epoch,
                # Source/table availability is application-owned and cannot be
                # proven by this hardware-free preview process.
                "runtime_bindings_verified": not any(
                    row_type.startswith("BIND_") or row_type.startswith("SOURCE_")
                    for row_type in row_types
                ),
            })
            return
        if request.path == "/api/log":
            action = form.get("action", [""])[0]
            with STATE.lock:
                STATE.recording = action == "start"
                if STATE.recording:
                    STATE.recorded_frames = 128
                active = STATE.recording
                frames = STATE.recorded_frames
            self.send_json({"ok": True, "phase": "recording" if active else "idle",
                            "active": active, "frames": frames, "bytes": frames * 24})
            return
        if request.path == "/api/replay/send":
            dry_run = form.get("dry_run", ["0"])[0] in {"1", "true"}
            self.send_json({"ok": True, "frames": 1, "repeat": 1,
                            "dry_run": dry_run, "started": True})
            return
        self.send_json({"ok": False, "error": "preview_route_not_implemented"}, 404)

    def serve_static(self, raw_path: str) -> None:
        relative = unquote(raw_path).lstrip("/") or "index.html"
        candidate = (DATA_ROOT / relative).resolve()
        if candidate != DATA_ROOT and DATA_ROOT not in candidate.parents:
            self.send_json({"ok": False, "error": "invalid_path"}, 403)
            return
        if not candidate.is_file():
            self.send_json({"ok": False, "error": "not_found"}, 404)
            return
        mime = mimetypes.guess_type(candidate.name)[0] or "application/octet-stream"
        if mime.startswith("text/") or mime in {"application/javascript", "application/json"}:
            mime += "; charset=utf-8"
        self.send_bytes(200, candidate.read_bytes(), mime)


def main() -> None:
    parser = argparse.ArgumentParser(description="Preview SignalScope without hardware")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), PreviewHandler)
    print(f"SignalScope preview: http://{args.host}:{args.port}/")
    print("Press Ctrl+C to stop. No physical CAN writes are possible in preview mode.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
