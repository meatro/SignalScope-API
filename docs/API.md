# HTTP API

SignalScope serves its UI and JSON/text API over HTTP from the device access point. The usual base URL is:

```text
http://192.168.4.1
```

There is no TLS, user account, or API token in the standalone firmware. Treat access to the local AP as access to the development device.

This document describes the native standalone routes. An installed application can add behavior behind the generic `/api/app/*` routes, but it does not replace the core API.

## Request conventions

- Read routes return JSON unless they explicitly stream a text/binary file.
- Simple commands accept URL query or `application/x-www-form-urlencoded` fields through Arduino `WebServer::arg()`.
- DBC, rule-package, replay CSV, and application-resource uploads use the raw request body (`text/plain` is appropriate).
- CAN IDs may be decimal or `0x` hexadecimal in numeric request fields.
- Directions are `A_TO_B` and `B_TO_A`.
- Boolean parsing accepts common true/false text; examples use `1` and `0`.
- Error JSON normally has `ok:false` and a stable `error` code. Use the HTTP status as well as the body.

Examples use `curl.exe` to avoid PowerShell's historical `curl` alias behavior.

## Runtime and caches

### `GET /api/status`

Returns the broad runtime snapshot used by the dashboard. Important groups include:

- Bus A/B readiness and controller recovery counters;
- ingress, forwarded, dropped, deferred, and failed-transmit counts;
- queue depth and fast/active path latency;
- active/staged rule counts and `rule_epoch`;
- active DBC/package paths and DBC capacity/completeness;
- observation and replay state;
- up to 40 recent frames, including decoded signals where available.

```powershell
curl.exe http://192.168.4.1/api/status
```

Fields may be added as diagnostics improve. Clients should read the keys they need and tolerate additional keys.

### `GET /api/frame_cache?limit=40`

Returns up to 40 frame snapshots:

```json
{
  "ok": true,
  "count": 1,
  "frames": [{
    "can_id": 801,
    "direction": "A_TO_B",
    "dlc": 8,
    "timestamp_us": 123456,
    "rate_hz": 10,
    "mutated": false,
    "data": "82 40 1F 00 00 00 00 00"
  }]
}
```

### `GET /api/signal_catalog`

Searches the active DBC and attaches a recent decoded value from physical frame history.

Query fields:

- `q`: optional name/identity search, maximum 64 characters;
- `offset`: result offset, default 0;
- `limit`: 1–96, default 48;
- `indexes`: optional comma-separated DBC signal indexes instead of a text search.

```powershell
curl.exe "http://192.168.4.1/api/signal_catalog?q=OilTemperature&limit=16"
```

Each signal includes:

```json
{
  "startBit": 0,
  "length": 8,
  "littleEndian": true,
  "signed": false,
  "factor": 1.0,
  "offset": -40.0,
  "index": 0,
  "canId": 801,
  "canIdHex": "0x321",
  "name": "OilTemperature",
  "valid": true,
  "live": true,
  "value": 90.0,
  "ageMs": 12,
  "direction": "A_TO_B"
}
```

`live` currently means the source physical frame is no older than 1.5 seconds. `value:null`, `ageMs:null`, or `direction:null` are valid unavailable states.

### `GET /api/signal_cache`

Returns native signal-cache entries. Use `indexes=0,4,9` to request particular DBC indexes; without it, the endpoint snapshots available entries up to the 384-entry API limit.

Fields include `index`, `can_id`, `name`, `value`, `generation`, `valid`, and `subscribed`.

For a browser signal picker, prefer `/api/signal_catalog`. The cache endpoint is useful when an app already persists selected indexes for the current DBC generation.

## Observation

### `POST /api/observe`

Changes optional DBC decoding work without changing physical forwarding.

```powershell
curl.exe -X POST -d "mode=ALL" http://192.168.4.1/api/observe
curl.exe -X POST -d "mode=NONE" http://192.168.4.1/api/observe
```

`SPECIFIC` also accepts an `ids` list in the firmware's CAN-ID/direction subscription syntax. The web interface should construct that list from selected catalog entries rather than asking a beginner to type it. Native signal subscriptions automatically create mandatory observations for their CAN IDs.

## DBC management

### `POST /api/dbc`

Parses the raw DBC body into a candidate, prepares a matching signal cache, persists it as `/dbc/active.dbc`, and swaps it into the runtime only after validation.

```powershell
curl.exe -X POST -H "Content-Type: text/plain" `
  --data-binary "@my-network.dbc" `
  http://192.168.4.1/api/dbc
```

Success returns message and signal counts. Changing the DBC clears subscriptions, observations, replay, and rules associated with the old database.

### `POST /api/dbc/autoload`

Loads the first valid startup candidate using the firmware's autoload order. Returns 404 when no valid file exists under `/dbc`.

### `POST /api/dbc/select`

Loads an existing `.dbc` under `/dbc/`:

```powershell
curl.exe -X POST -d "path=/dbc/my-network.dbc" `
  http://192.168.4.1/api/dbc/select
```

Traversal and non-DBC paths are rejected.

## Rule candidate and active tables

### `POST /api/rules/stage`

Stages one rule. It does not change active CAN behavior.

Static bit-range example:

```powershell
curl.exe -X POST -d "rule_kind=BIT_RANGE&can_id=0x321&direction=A_TO_B&start_bit=0&length=8&little_endian=1&replace_value=130&enabled=1" `
  http://192.168.4.1/api/rules/stage
```

Fields:

- `rule_kind=BIT_RANGE` (default);
- `can_id`, `direction`, `start_bit`, `length`, `little_endian`;
- `replace_value` (or compatibility alias `op_value1`);
- `dynamic=1` to create a runtime-updatable bit-range rule;
- `enabled`.

Raw-mask example:

```powershell
curl.exe -X POST -d "rule_kind=RAW_MASK&can_id=0x321&direction=A_TO_B&mask=FF00000000000000&value=8200000000000000&enabled=1" `
  http://192.168.4.1/api/rules/stage
```

`mask` and `value` are eight hex bytes. Separators accepted by the shared byte parser may vary; a continuous 16-hex-digit value is the least ambiguous.

The same handler retains a legacy signal-mutation adapter when `operation` is supplied without `rule_kind`. Supported operation names are `REPLACE`, `PASS_THROUGH`, `ADD_OFFSET`, `MULTIPLY`, and `CLAMP`, with DBC-style factor/offset inputs. New applications should use explicit rule staging and packages because those map directly to the active rule model.

### `GET /api/rules` and `GET /api/rules?view=staging`

The query selects which complete table is listed:

- `GET /api/rules` lists the **active table** currently used by live CAN traffic.
- `GET /api/rules?view=staging` lists the **candidate table** being assembled for the next Apply.

Use the candidate view in an editor after calling `/api/rules/stage`; otherwise the newly accepted rule will not appear in the review table. Omitting `view` remains the deliberate, backward-compatible way to inspect live behavior.

```powershell
curl.exe http://192.168.4.1/api/rules
curl.exe "http://192.168.4.1/api/rules?view=staging"
```

Both views return their rules and the current table epoch:

```json
{
  "ok": true,
  "count": 1,
  "rule_epoch": 12,
  "rules": [{
    "rule_id": 0,
    "rule_epoch": 12,
    "priority": 0,
    "active": true,
    "kind": "BIT_RANGE",
    "can_id": 801,
    "direction": "A_TO_B",
    "enabled": true,
    "start_bit": 0,
    "length": 8,
    "little_endian": true,
    "dynamic": false,
    "replace_value": 130,
    "replace_value_text": "130"
  }]
}
```

`replace_value_text` is the exact unsigned decimal representation. Browser
clients should prefer it when displaying or regenerating rules because JSON
numbers cannot represent every 64-bit integer exactly. `replace_value` remains
for compatibility and convenience with ordinary-width rules.

`enabled` tells whether that rule is enabled. `active` is retained as a compatibility alias for the same enabled state; it does **not** tell you whether a row came from the active or candidate table. The request URL selects the table, so a candidate editor should label every row from `?view=staging` as candidate regardless of `active`.

### `POST /api/rules`

The raw body selects a table action:

```powershell
curl.exe -X POST -H "Content-Type: text/plain" --data "apply_commit" http://192.168.4.1/api/rules
curl.exe -X POST -H "Content-Type: text/plain" --data "revert" http://192.168.4.1/api/rules
curl.exe -X POST -H "Content-Type: text/plain" --data "clear_staging" http://192.168.4.1/api/rules
curl.exe -X POST -H "Content-Type: text/plain" --data "clear_rules" http://192.168.4.1/api/rules
```

- `apply_commit`: atomically publishes the complete candidate table in RAM. It does not write a package.
- `revert`: restores staging from the active table.
- `clear_staging`: clears only candidates.
- `clear_rules`: clears the mutation tables and selected-package state in RAM.

### `POST /api/rules/value`

Changes a dynamic `BIT_RANGE` rule's current raw value. The table epoch is mandatory in practice:

```powershell
curl.exe -X POST -d "rule_id=0&rule_epoch=12&value=42&enabled=1" `
  http://192.168.4.1/api/rules/value
```

For a pending dynamic candidate, the value becomes its pending runtime value and is published by Apply. Otherwise it changes active runtime state. Static replacement rules are immutable rows; restage a static rule instead of calling `/value`.

HTTP 409 with `stale_rule_handle` means the table changed. Fetch the same table view again and resolve the intended rule rather than retrying an old slot number.

### `POST /api/rules/enable`

```powershell
curl.exe -X POST -d "rule_id=0&rule_epoch=12&enabled=0" `
  http://192.168.4.1/api/rules/enable
```

The handler can also resolve a rule by CAN ID/direction/field identity, but stable clients should use the returned ID plus epoch and refresh on conflict.

When a slot is pending commit, enabling or disabling it updates the candidate. This makes `/enable` appropriate for candidate-editor rows. The change does not affect live traffic until Apply.

## Persistent rule packages

See [Rules and packages](RULES.md) for the file format.

### `POST /api/rules/package`

Validates, activates, and stores the raw `.ssrules` body. The default path is `/rules/active.ssrules`.

```powershell
curl.exe -X POST -H "Content-Type: text/plain" `
  --data-binary "@my-profile.ssrules" `
  "http://192.168.4.1/api/rules/package?path=/rules/active.ssrules"
```

A short name such as `my-profile.ssrules` is normalized under `/rules/`. Success returns `path`, active `count`, and the new `rule_epoch`.

### `GET /api/rules/package`

Streams the selected package as plain text. With no `path`, it reads the active package path when known, otherwise `/rules/active.ssrules`.

```powershell
curl.exe "http://192.168.4.1/api/rules/package?path=/rules/active.ssrules"
```

Add `download=1` to receive a content-disposition filename. This reads the stored file; it does not synthesize package text from arbitrary active RAM rules.

### `POST /api/rules/select`

Loads and activates an existing package:

```powershell
curl.exe -X POST -d "path=/rules/my-profile.ssrules" `
  http://192.168.4.1/api/rules/select
```

## Session logging

### `GET /api/log`

Returns authoritative recorder state, counts, storage status, and download readiness.

### `POST /api/log`

Start a recording:

```powershell
curl.exe -X POST -d "action=start&scope=physical&durable=1" http://192.168.4.1/api/log
```

Scopes are:

- `physical` (default);
- `all`;
- `a_to_b`;
- `b_to_a`;
- `mutated`.

`durable=1` enables checkpoint-oriented storage behavior. Other actions are `stop`, `delete`/`clear`, and `retry` after a save failure. Stopping is asynchronous; poll `GET /api/log` until the saved artifact is ready.

### `GET /api/log/download`

Streams the completed session-log artifact when `downloadReady` is true. It returns a conflict/error while a durable file is not available.

Use `tools/read_signalscope_session.py` to inspect a saved session off-device.

## Replay

Replay can physically transmit. User-facing tools should set `dry_run=1` by default and require an explicit choice for physical dispatch.

### `POST /api/replay/send`

Builds a short replay from one frame:

```powershell
curl.exe -X POST -d "can_id=0x321&direction=A_TO_B&dlc=8&data=8200000000000000&repeat=1&interval_us=0&auto_start=1&dry_run=1" `
  http://192.168.4.1/api/replay/send
```

Limits and fields:

- `dlc`: clamped to 0–8;
- `repeat`: clamped to 1–256;
- `interval_us`, `start_delay_us`;
- `auto_start` (default true);
- `dry_run` (API default false—set it explicitly).

### `POST /api/replay/load`

Loads up to 1024 timestamped CSV frames from the raw body. Use query/form fields `direction` as a fallback and `dry_run=1` for non-physical dispatch.

Full row shape:

```text
timestamp_us,can_id,dlc,b0,b1,b2,b3,b4,b5,b6,b7,synthetic,direction
```

Example:

```text
0,0x321,8,82,00,00,00,00,00,00,00,0,A_TO_B
100000,0x321,8,83,00,00,00,00,00,00,00,0,A_TO_B
```

### `POST /api/replay`

Controls the loaded replay:

```powershell
curl.exe -X POST -d "action=start&loop_mode=PLAY_ONCE&dry_run=1&start_delay_us=0" http://192.168.4.1/api/replay
curl.exe -X POST -d "action=stop" http://192.168.4.1/api/replay
```

Loop modes are `PLAY_ONCE`, `LOOP_RAW`, and `LOOP_WITH_COUNTER_CONTINUATION`. If `dry_run` is omitted on start, the dispatch mode selected when loading is retained.

## Native application routes

These return 404 when no `ApplicationExtension` is installed.

### `GET /api/app/status`

Returns the JSON produced by the extension's required `writeStatusJson()` callback.

### `POST /api/app/config`

Passes `key` and `value` to the extension, then applies any requested DBC/rule resource pair transactionally:

```powershell
curl.exe -X POST -d "key=displayUnit&value=celsius" http://192.168.4.1/api/app/config
```

### `GET /api/app/resource?name=...`

Reads a bounded application-owned resource. The extension declares capacity; the host rejects zero or more than 96 KiB.

### `POST /api/app/resource?name=...`

Passes the raw body to the extension's bounded resource writer. Diagnostic commands receive delivery-receipt semantics so a browser does not retry a non-idempotent request after an ambiguous response.

## Compatibility aliases

`/api/mutations/stage`, `/api/mutations`, and `/api/mutations/toggle` are retained for older clients. New applications should use `/api/rules/*`; the rule vocabulary matches the current engine, epochs, and persistence model directly.

## Common HTTP statuses

| Status | Meaning |
| ---: | --- |
| 200 | Command or read succeeded |
| 400 | Missing/malformed field or unknown action |
| 404 | File, rule, route capability, or application not found |
| 409 | State conflict such as stale epoch, empty replay, or unavailable download |
| 413 | Uploaded package size invalid |
| 422 | Input was understood but failed validation/compilation |
| 503 | Application, recorder, decoder, replay, or UI scratch resource busy/unavailable |
| 507 | Runtime memory or LittleFS write/allocation failure |
