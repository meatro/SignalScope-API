#!/usr/bin/env python3
"""Serve the standalone UI with a tiny in-memory SignalScope API.

This preview is intentionally dependency-free.  It lets contributors inspect
and click through the browser application without a CAN interface or ESP32.
Nothing from this server is sent to a physical bus.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
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
        self.package = ""
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


def rule_from_form(form: dict[str, list[str]]) -> dict[str, object]:
    """Translate the public staging form into the shape returned by GET rules."""

    def first(name: str, default: str) -> str:
        return form.get(name, [default])[0]

    with STATE.lock:
        rule_id = len(STATE.candidate_rules)
        STATE.epoch += 1
        enabled = first("enabled", "1") not in {"0", "false"}
        replace_value = int(first("replace_value", "0"), 0)
        rule = {
            "rule_id": rule_id,
            "rule_epoch": STATE.epoch,
            "priority": rule_id,
            # `active` is a compatibility alias for runtime enabled state.
            "active": enabled,
            "enabled": enabled,
            "kind": first("rule_kind", "BIT_RANGE"),
            "can_id": int(first("can_id", "0"), 0),
            "direction": first("direction", "A_TO_B"),
            "start_bit": int(first("start_bit", "0"), 0),
            "length": int(first("length", "8"), 0),
            "little_endian": first("little_endian", "1") not in {"0", "false"},
            "dynamic": first("dynamic", "0") not in {"0", "false"},
            "replace_value": replace_value,
            "replace_value_text": str(replace_value),
        }
        STATE.candidate_rules.append(rule)
        STATE.candidate_pending = True
        return dict(rule)


def copy_rules(rules: list[dict[str, object]]) -> list[dict[str, object]]:
    """Copy a rule table so candidate edits cannot leak into active behavior."""

    return [dict(rule) for rule in rules]


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
                package_path = "/rules/active.ssrules" if STATE.package else ""
                epoch = STATE.epoch
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
            view = "staging" if query.get("view", ["active"])[0] == "staging" else "active"
            with STATE.lock:
                source = STATE.candidate_rules if view == "staging" else STATE.active_rules
                rules = copy_rules(source)
                epoch = STATE.epoch
            self.send_json({"ok": True, "view": view, "count": len(rules),
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
            self.send_json({"ok": True, "messages": 1, "signals": len(SIGNALS)})
            return
        if request.path == "/api/dbc/autoload":
            self.send_json({"ok": True, "autoloaded": True, "messages": 1, "signals": len(SIGNALS)})
            return
        if request.path == "/api/rules/stage":
            rule = rule_from_form(form)
            with STATE.lock:
                staging_count = len(STATE.candidate_rules)
            self.send_json({"ok": True, "rule_id": rule["rule_id"],
                            "rule_epoch": rule["rule_epoch"], "staging_count": staging_count})
            return
        if request.path == "/api/rules":
            with STATE.lock:
                if "apply_commit" in body:
                    STATE.active_rules = copy_rules(STATE.candidate_rules)
                    STATE.candidate_pending = False
                    action = "apply_commit"
                elif "revert" in body:
                    STATE.candidate_rules = copy_rules(STATE.active_rules)
                    STATE.candidate_pending = False
                    action = "revert"
                elif "clear_rules" in body:
                    STATE.active_rules = []
                    STATE.candidate_rules = []
                    STATE.candidate_pending = False
                    action = "clear_rules"
                else:
                    STATE.candidate_rules = []
                    STATE.candidate_pending = True
                    action = "clear_staging"
                STATE.epoch += 1
                epoch = STATE.epoch
                stamp_rules(STATE.active_rules, epoch)
                stamp_rules(STATE.candidate_rules, epoch)
            self.send_json({"ok": True, "action": action, "rule_epoch": epoch})
            return
        if request.path in {"/api/rules/value", "/api/rules/enable"}:
            rule_id = int(form.get("rule_id", ["-1"])[0])
            with STATE.lock:
                candidate_target = (STATE.candidate_rules[rule_id]
                                    if 0 <= rule_id < len(STATE.candidate_rules) else None)
                active_target = (STATE.active_rules[rule_id]
                                 if 0 <= rule_id < len(STATE.active_rules) else None)
                target = candidate_target if STATE.candidate_pending else active_target
                if target is None:
                    target = candidate_target
                if target is not None and request.path.endswith("value"):
                    value = int(form.get("value", ["0"])[0], 0)
                    target["replace_value"] = value
                    target["replace_value_text"] = str(value)
                    # After Apply, the candidate is a review mirror of active.
                    if not STATE.candidate_pending and candidate_target is not None:
                        candidate_target["replace_value"] = value
                        candidate_target["replace_value_text"] = str(value)
                elif target is not None:
                    enabled = form.get("enabled", ["1"])[0] not in {"0", "false"}
                    target["enabled"] = enabled
                    target["active"] = enabled
                    if not STATE.candidate_pending and candidate_target is not None:
                        candidate_target["enabled"] = enabled
                        candidate_target["active"] = enabled
            self.send_json({"ok": True})
            return
        if request.path == "/api/rules/package":
            with STATE.lock:
                STATE.package = body
                # The real package endpoint validates and publishes a complete
                # table. The preview uses the already reviewed candidate as its
                # defensible in-memory equivalent rather than faking a parser.
                STATE.active_rules = copy_rules(STATE.candidate_rules)
                STATE.candidate_pending = False
                count = len(STATE.active_rules)
                STATE.epoch += 1
                epoch = STATE.epoch
                stamp_rules(STATE.active_rules, epoch)
                stamp_rules(STATE.candidate_rules, epoch)
            self.send_json({"ok": True, "path": "/rules/active.ssrules",
                            "count": count, "rule_epoch": epoch})
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
