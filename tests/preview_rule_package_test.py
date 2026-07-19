#!/usr/bin/env python3
"""Exercise the hardware-free preview's real rule-package transaction."""

from __future__ import annotations

import importlib.util
import json
import sys
import threading
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("signalscope_preview", ROOT / "tools" / "preview_ui.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


FULL_PACKAGE = """# every supported persistent row type
BIND_TABLE,commandValue,transferCurve
BIND_OVERRIDE,commandValue,overrideActive,overrideValue
BIND_ACTIVE,profileCode,1|2|4
STATIC,0x100,A_TO_B,0,8,1,130
SOURCE_INT,0x101,A_TO_B,8,8,1,commandValue,0.5,20,2.54,0,0,0,99.5,254
SOURCE_SELECT_INT,0x102,B_TO_A,16,8,1,requestedValue,1,0,0,0,0,100,mode,0:D:0|1:S:1.0:100
COUNTER,0x103,A_TO_B,24,4,1,0,1,15,0
SEQUENCE8,0x104,A_TO_B,32,8,1,0x10|0x20|0x30,1
CHECKSUM_XOR,0x105,A_TO_B,7,0,6,0x00,0
CHECKSUM_CRC8_AUTOSAR,0x106,B_TO_A,0,1,1,7,0xA0|0xA1|0xA2|0xA3|0xA4|0xA5|0xA6|0xA7|0xA8|0xA9|0xAA|0xAB|0xAC|0xAD|0xAE|0xAF,1
"""


class QuietPreviewHandler(MODULE.PreviewHandler):
    def log_message(self, message: str, *args: object) -> None:
        pass


def request(base_url: str, path: str, body: str | None = None) -> tuple[int, object]:
    headers = {}
    data = None
    if body is not None:
        data = body.encode("utf-8")
        headers["Content-Type"] = "text/plain" if path.startswith("/api/rules/package") else "application/x-www-form-urlencoded"
    req = Request(base_url + path, data=data, headers=headers, method="POST" if body is not None else "GET")
    try:
        with urlopen(req, timeout=5) as response:
            payload = response.read()
            content_type = response.headers.get_content_type()
            return response.status, json.loads(payload) if content_type == "application/json" else payload.decode("utf-8")
    except HTTPError as error:
        return error.code, json.loads(error.read())


def require_invalid(package: str) -> None:
    try:
        MODULE.parse_rule_package(package)
    except MODULE.PackageParseError:
        return
    raise AssertionError(f"package should have been rejected: {package!r}")


def main() -> None:
    rules, row_types = MODULE.parse_rule_package(FULL_PACKAGE)
    assert len(row_types) == 10
    assert len(rules) == 7  # bind directives do not become mutation rows
    assert [rule["package_type"] for rule in rules] == [
        "STATIC",
        "SOURCE_INT",
        "SOURCE_SELECT_INT",
        "COUNTER",
        "SEQUENCE8",
        "CHECKSUM_XOR",
        "CHECKSUM_CRC8_AUTOSAR",
    ]
    assert rules[0]["selector_source"] == "profileCode"
    assert rules[1]["lookup_table"] == "transferCurve"
    assert rules[1]["override_active_source"] == "overrideActive"
    assert rules[1]["override_value_source"] == "overrideValue"
    assert rules[1]["selector_states"] == [1, 2, 4]
    # SOURCE_SELECT_INT owns its selector and is not overwritten by BIND_ACTIVE.
    assert rules[2]["selector_source"] == "mode"
    assert rules[2]["selector_entries"][1]["mode"] == "scale"
    assert rules[5]["enabled"] is False
    assert len(rules[6]["data_ids"]) == 16

    shuffled, _ = MODULE.parse_rule_package(
        "CHECKSUM_XOR,0x200,A_TO_B,7,0,6,0,1\n"
        "COUNTER,0x200,A_TO_B,48,8,1,0,1,255,0\n"
        "STATIC,0x200,A_TO_B,0,8,1,12\n"
    )
    compiled = MODULE.compile_active_rules(shuffled)
    assert [rule["kind"] for rule in compiled] == ["BIT_RANGE", "COUNTER", "CHECKSUM_XOR"]
    assert [rule["rule_id"] for rule in compiled] == [2, 1, 0]

    multi_can, _ = MODULE.parse_rule_package(
        "CHECKSUM_XOR,0x100,A_TO_B,7,0,6,0,1\n"
        "STATIC,0x200,A_TO_B,0,8,1,12\n"
        "COUNTER,0x100,A_TO_B,48,8,1,0,1,255,0\n"
    )
    compiled_multi_can = MODULE.compile_active_rules(multi_can)
    assert [rule["rule_id"] for rule in compiled_multi_can] == [2, 0, 1]
    assert [rule["priority"] for rule in compiled_multi_can] == [1, 2, 0]

    for invalid in (
        "",
        "# comments only\n",
        "BIND_TABLE,value,table\n",
        "STATIC,0x100,SIDEWAYS,0,8,1,1\n",
        "STATIC,0x100,A_TO_B,0,8,1,1,extra\n",
        "STATIC,0x100,A_TO_B,0,4,1,16\n",
        "SEQUENCE8,0x100,A_TO_B,0,8,1,1|2,2\n",
        "CHECKSUM_CRC8_AUTOSAR,0x100,A_TO_B,0,1,1,7,0x01|0x02,1\n",
    ):
        require_invalid(invalid)

    MODULE.STATE = MODULE.PreviewState()
    server = MODULE.ThreadingHTTPServer(("127.0.0.1", 0), QuietPreviewHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        initial_package = "STATIC,0x321,A_TO_B,0,8,1,130\n"
        status, payload = request(base_url, "/api/rules/package", initial_package)
        assert status == 200 and payload["count"] == 1
        epoch = payload["rule_epoch"]

        # Build a second candidate without changing the active table.
        stage_body = "rule_kind=BIT_RANGE&can_id=0x322&direction=A_TO_B&start_bit=0&length=8&little_endian=1&dynamic=1&replace_value=10&enabled=1"
        status, staged = request(base_url, "/api/rules/stage", stage_body)
        assert status == 200
        epoch = staged["rule_epoch"]

        # Bad direct-stage input returns JSON and leaves the candidate intact;
        # the preview must not drop the HTTP connection on a Python ValueError.
        status, bad_stage = request(
            base_url,
            "/api/rules/stage",
            "rule_kind=BIT_RANGE&can_id=not-a-number&direction=A_TO_B&start_bit=0&length=8&little_endian=1&replace_value=1",
        )
        assert status == 400 and bad_stage["error"] == "invalid_rule_stage"
        status, bad_motorola = request(
            base_url,
            "/api/rules/stage",
            "rule_kind=BIT_RANGE&can_id=0x322&direction=A_TO_B&start_bit=56&length=2&little_endian=0&replace_value=1",
        )
        assert status == 400 and bad_motorola["error"] == "invalid_rule_stage"
        status, candidate_after_bad_stage = request(base_url, "/api/rules?view=staging")
        assert candidate_after_bad_stage["count"] == 2

        # Legacy callers that omit `view` must follow a pending replacement of
        # an existing active slot into Draft instead of changing live traffic.
        pending_existing = (
            "rule_kind=BIT_RANGE&can_id=0x321&direction=A_TO_B&start_bit=0&length=8"
            "&little_endian=1&dynamic=0&replace_value=131&enabled=1"
        )
        status, pending = request(base_url, "/api/rules/stage", pending_existing)
        assert status == 200 and pending["rule_id"] == 0
        epoch = pending["rule_epoch"]
        status, omitted_view = request(
            base_url,
            "/api/rules/enable",
            f"rule_id=0&rule_epoch={epoch}&enabled=0",
        )
        assert status == 200 and omitted_view["view"] == "staging"
        status, live_after_legacy = request(base_url, "/api/rules")
        status, candidate_after_legacy = request(base_url, "/api/rules?view=staging")
        assert live_after_legacy["rules"][0]["enabled"] is True
        assert candidate_after_legacy["rules"][0]["enabled"] is False
        # Re-staging the same engine identity replaces it in place.
        replacement_body = stage_body.replace("replace_value=10", "replace_value=11")
        status, replacement = request(base_url, "/api/rules/stage", replacement_body)
        assert status == 200 and replacement["rule_id"] == staged["rule_id"]
        epoch = replacement["rule_epoch"]
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert candidate["count"] == 2 and candidate["rules"][1]["replace_value"] == 11
        status, live = request(base_url, "/api/rules")
        assert status == 200 and live["count"] == 1
        assert live["candidate_dirty"] is True
        status, preview_status = request(base_url, "/api/status")
        assert preview_status["candidate_dirty"] is True

        # Explicit staging controls never leak into the active row/table.
        status, _ = request(
            base_url,
            "/api/rules/value",
            f"rule_id=1&rule_epoch={epoch}&view=staging&value=42",
        )
        assert status == 200
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert candidate["rules"][1]["replace_value"] == 42
        status, live = request(base_url, "/api/rules")
        assert live["count"] == 1

        before_candidate = candidate["rules"][0]["enabled"]
        status, invalid_view = request(
            base_url,
            "/api/rules/enable",
            f"rule_id=0&rule_epoch={epoch}&view=banana&enabled=0",
        )
        assert status == 400 and invalid_view["error"] == "invalid_rules_view"
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert candidate["rules"][0]["enabled"] == before_candidate

        # Failed package installs restore a clean active mirror. Live rules and
        # the previously installed file remain intact.
        status, rejected = request(base_url, "/api/rules/package", "BIND_ACTIVE,mode,1|2\n")
        assert status == 422 and rejected["error"] == "rule_package_invalid"
        status, live = request(base_url, "/api/rules")
        assert live["count"] == 1
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert candidate["count"] == 1
        assert candidate["candidate_dirty"] is False
        assert candidate["rules"] == live["rules"]
        status, preview_status = request(base_url, "/api/status")
        assert preview_status["candidate_dirty"] is False
        status, stored = request(base_url, "/api/rules/package")
        assert status == 200 and stored == initial_package

        # A valid replacement activates what it parsed, not an unrelated old
        # candidate table, and mirrors the result into staging for review.
        status, installed = request(base_url, "/api/rules/package", FULL_PACKAGE)
        assert status == 200
        assert installed["count"] == 7
        assert installed["parsed_rows"] == 10
        assert installed["directives"] == 3
        assert installed["row_types"] == row_types
        status, live = request(base_url, "/api/rules")
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert live["count"] == candidate["count"] == 7
        assert live["rules"][1]["package_type"] == "SOURCE_INT"

        counter_rule = next(rule for rule in candidate["rules"] if rule["kind"] == "COUNTER")
        sequence_rule = next(rule for rule in candidate["rules"] if rule["kind"] == "SEQUENCE8")
        assert counter_rule["runtime_value_kind"] == "counter_state"
        assert counter_rule["runtime_value"] == 0
        assert sequence_rule["runtime_value_kind"] == "sequence_index"
        assert sequence_rule["runtime_value"] == 1
        assert sequence_rule["sequence_count"] == 3

        # The advanced workstation's runtime controls update only the draft
        # until Apply, and sequence indexes are bounded by their value list.
        epoch = installed["rule_epoch"]
        status, _ = request(
            base_url,
            "/api/rules/value",
            f"rule_id={counter_rule['rule_id']}&rule_epoch={epoch}&view=staging&value=7",
        )
        assert status == 200
        status, candidate = request(base_url, "/api/rules?view=staging")
        status, live = request(base_url, "/api/rules")
        assert next(rule for rule in candidate["rules"] if rule["kind"] == "COUNTER")["runtime_value"] == 7
        assert next(rule for rule in live["rules"] if rule["kind"] == "COUNTER")["runtime_value"] == 0
        status, invalid_sequence = request(
            base_url,
            "/api/rules/value",
            f"rule_id={sequence_rule['rule_id']}&rule_epoch={epoch}&view=staging&value=3",
        )
        assert status == 422 and invalid_sequence["error"] == "rule_value_rejected"

        # A typo must never silently clear the preview draft. Firmware rejects
        # unknown actions, so the hardware-free preview does the same.
        status, before_unknown = request(base_url, "/api/rules?view=staging")
        status, unknown = request(base_url, "/api/rules", "definitely_not_an_action")
        assert status == 400 and unknown["error"] == "unknown_action"
        status, after_unknown = request(base_url, "/api/rules?view=staging")
        assert after_unknown["rules"] == before_unknown["rules"]

        status, rejected_value = request(
            base_url,
            "/api/rules/value",
            f"rule_id=1&rule_epoch={epoch}&view=staging&value=77",
        )
        assert status == 422 and rejected_value["error"] == "rule_value_rejected"
        status, rejected_static = request(
            base_url,
            "/api/rules/value",
            f"rule_id=0&rule_epoch={epoch}&view=staging&value=999",
        )
        assert status == 422 and rejected_static["error"] == "rule_value_rejected"
        status, _ = request(
            base_url,
            "/api/rules/enable",
            f"rule_id=0&rule_epoch={epoch}&view=staging&enabled=0",
        )
        assert status == 200
        status, live = request(base_url, "/api/rules")
        status, candidate = request(base_url, "/api/rules?view=staging")
        assert live["rules"][1]["replace_value"] == 0
        assert candidate["rules"][1]["replace_value"] == 0
        assert live["rules"][0]["enabled"] is True
        assert candidate["rules"][0]["enabled"] is False

        for emptyish in ("", "# no mutations\n"):
            status, rejected = request(base_url, "/api/rules/package", emptyish)
            assert status == 422 and rejected["error"] == "rule_package_invalid"

        # A RAM apply detaches live rules from their startup-package selection
        # without deleting the stored package text.
        status, applied = request(base_url, "/api/rules", "apply_commit")
        assert status == 200 and applied["action"] == "apply_commit"
        status, preview_status = request(base_url, "/api/status")
        assert preview_status["rule_package_path"] == ""
        status, stored = request(base_url, "/api/rules/package")
        assert status == 200 and stored == FULL_PACKAGE

        # Reinstall, then mirror firmware's DBC replacement lifecycle: rules
        # and selection clear, while the LittleFS package remains readable.
        status, _ = request(base_url, "/api/rules/package", FULL_PACKAGE)
        assert status == 200
        status, _ = request(base_url, "/api/dbc", "dummy dbc")
        assert status == 200
        status, live = request(base_url, "/api/rules")
        status, candidate = request(base_url, "/api/rules?view=staging")
        status, preview_status = request(base_url, "/api/status")
        assert live["count"] == candidate["count"] == 0
        assert preview_status["candidate_dirty"] is False
        assert preview_status["rule_package_path"] == ""
        status, stored = request(base_url, "/api/rules/package")
        assert status == 200 and stored == FULL_PACKAGE
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    print("PREVIEW_RULE_PACKAGE_TEST_PASS")


if __name__ == "__main__":
    main()
