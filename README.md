# SignalScope

SignalScope is an API-first, deterministic message-level CAN gateway runtime for ESP32-class boards.

It is designed so CAN forwarding/mutation/replay stay deterministic, while UI/client logic remains replaceable.

## What This Project Is

- Dual-CAN inline gateway runtime (forward A->B and B->A)
- Deterministic mutation engine (`BIT_RANGE`, `RAW_MASK`) with stage/commit model
- Replay engine that runs through the same gateway pipeline
- Optional DBC load + demand-driven decode (observation controlled)
- HTTP API + web UI served from LittleFS

## What This Project Is Not

- Not tied to one product behavior (no product-specific CAN logic in core engine)
- Not a cloud service (local AP + local HTTP API)

## Hardware and Runtime Defaults

Current default target in this repo:
- ESP32-S3 board profile: `esp32-s3-devkitc1-n16r8`
- Bus A: native ESP32 TWAI
- Bus B: MCP2515 over SPI

Core pinning selector (current code):
- `kUseDualCore = true` (default): CAN/runtime core `1`, UI/server core `0`
- `kUseDualCore = false`: both runtime tasks pinned to core `0`
- On single-core targets, runtime automatically falls back to core `0` even if `kUseDualCore=true`

Dual-core is recommended for headroom, but single-core scheduling is supported.

## Quick Start

### 1) Build Prerequisites

- PlatformIO CLI (or VSCode + PlatformIO extension)
- USB serial access to your ESP32-S3 board

### 2) Flash Firmware + Filesystem

From repo root:

```powershell
platformio run -t upload
platformio run -t uploadfs
```

If filesystem/partition state is questionable:

```powershell
platformio run -t erase
platformio run -t upload
platformio run -t uploadfs
```

### 3) Connect

- SSID: `SignalScope-AP`
- Password: `signalscope`
- URL: `http://192.168.4.1/`

## Project Layout

- `main.cpp`: board wiring, CAN drivers, task runtime split, HTTP handlers
- `core/`: gateway, mutation, replay, DBC parser, frame/signal caches
- `fs/`: persistence abstraction
- `data/`: LittleFS web assets
- `boards/`: custom board JSON definitions
- `ARCH.md`: architecture notes and runtime behavior

## API Overview

Base URL on AP network:

```text
http://192.168.4.1
```

Request content types used by current handlers:
- `GET` query params for reads
- `application/x-www-form-urlencoded` for most POST field-based writes
- `text/plain` for body-token actions and DBC/CSV uploads

### Core Endpoints

#### `GET /api/status`

Returns gateway stats, queue/drop counters, DBC/replay state, active/staging rule counts, and recent frame events.

Example:

```powershell
curl "http://192.168.4.1/api/status"
```

#### `GET /api/frame_cache?limit=<n>`

Returns frame cache snapshot keyed by `(can_id, direction)`.

- `limit`: optional, capped by firmware status frame limit

Example:

```powershell
curl "http://192.168.4.1/api/frame_cache?limit=40"
```

#### `GET /api/signal_cache?indexes=0,1,2`

Returns decoded signal cache snapshots.

- `indexes`: optional CSV of signal indexes; omit for full snapshot

Example:

```powershell
curl "http://192.168.4.1/api/signal_cache?indexes=0,1,2"
```

#### `POST /api/observe`

Controls frame observation/decode mode.

Fields:
- `mode`: `none` | `specific` | `all`
- `ids`: CSV of `can_id:direction` entries for `specific` mode
  - Example token: `0x280:A_TO_B`

Example:

```powershell
curl -X POST "http://192.168.4.1/api/observe" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "mode=specific&ids=0x280:A_TO_B,0x1A0:B_TO_A"
```

## Rules and Mutation API

### `POST /api/rules/stage`

Stages a mutation rule (does not activate until commit).

`BIT_RANGE` fields:
- `rule_kind=BIT_RANGE`
- `can_id`, `direction`
- `start_bit`, `length`
- `replace_value`
- `little_endian` (optional, default true)
- `dynamic` (optional, default false)
- `enabled` (optional, default true)

Example (`BIT_RANGE`):

```powershell
curl -X POST "http://192.168.4.1/api/rules/stage" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "rule_kind=BIT_RANGE&can_id=0x280&direction=A_TO_B&start_bit=0&length=8&replace_value=5&enabled=1"
```

`RAW_MASK` fields:
- `rule_kind=RAW_MASK`
- `can_id`, `direction`
- `mask` (16 hex chars = 8 bytes)
- `value` (16 hex chars = 8 bytes)
- `enabled` (optional)

Example (`RAW_MASK`):

```powershell
curl -X POST "http://192.168.4.1/api/rules/stage" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "rule_kind=RAW_MASK&can_id=0x280&direction=A_TO_B&mask=00000000000000FF&value=0000000000000001&enabled=1"
```

### `POST /api/rules`

Rule table actions via plain body token:
- `apply_commit`
- `revert`
- `clear_staging`
- `clear_rules`

Example:

```powershell
curl -X POST "http://192.168.4.1/api/rules" \
  -H "Content-Type: text/plain" \
  --data-binary "apply_commit"
```

### `GET /api/rules`

Returns current active rule list.

```powershell
curl "http://192.168.4.1/api/rules"
```

### `POST /api/rules/value`

Updates dynamic value for a rule.

Fields:
- `rule_id`
- `value`
- `enabled` (optional)

```powershell
curl -X POST "http://192.168.4.1/api/rules/value" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "rule_id=3&value=42"
```

### `POST /api/rules/enable`

Enable/disable a rule by `rule_id`, or identity fields.

```powershell
curl -X POST "http://192.168.4.1/api/rules/enable" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "rule_id=3&enabled=0"
```

## Replay API

### `POST /api/replay/load`

Loads replay CSV into replay buffer.

- Body: CSV text (`text/plain`)
- Optional field/query: `direction` default if CSV line omits direction

```powershell
curl -X POST "http://192.168.4.1/api/replay/load?direction=A_TO_B" \
  -H "Content-Type: text/plain" \
  --data-binary "0,0x280,8,00,01,02,03,04,05,06,07,A_TO_B"
```

### `POST /api/replay`

Replay control endpoint.

Supported styles:
- legacy body token (`text/plain`): `start`, `stop`, optional loop token
- form fields (`application/x-www-form-urlencoded`):
  - `action=start|stop`
  - `loop_mode=PLAY_ONCE|LOOP_RAW|LOOP_WITH_COUNTER_CONTINUATION`
  - `start_delay_us=<microseconds>`

Examples:

```powershell
curl -X POST "http://192.168.4.1/api/replay" -H "Content-Type: text/plain" --data-binary "start"
curl -X POST "http://192.168.4.1/api/replay" -H "Content-Type: text/plain" --data-binary "start LOOP_RAW"
curl -X POST "http://192.168.4.1/api/replay" -H "Content-Type: text/plain" --data-binary "stop"
curl -X POST "http://192.168.4.1/api/replay" -H "Content-Type: application/x-www-form-urlencoded" --data "action=start&loop_mode=LOOP_RAW&start_delay_us=50000"
```

### `POST /api/replay/send`

Convenience send endpoint that builds replay frames internally and optionally starts playback immediately.

Fields:
- `can_id`, `direction`, `dlc`
- `data` (16 hex chars = 8 bytes)
- `repeat` (1..256)
- `interval_us`
- `start_delay_us`
- `auto_start` (`1` or `0`)

Example:

```powershell
curl -X POST "http://192.168.4.1/api/replay/send" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "can_id=0x280&direction=A_TO_B&dlc=8&data=0001020304050607&repeat=10&interval_us=10000&start_delay_us=50000&auto_start=1"
```

### Replay CSV Format

Per line:

```text
timestamp_us,can_id,dlc,b0,b1,b2,b3,b4,b5,b6,b7[,direction]
```

- `timestamp_us`: integer microseconds
- `can_id`: decimal or hex (`0x...`)
- `dlc`: 0..8
- `b0..b7`: hex byte values
- `direction` optional: `A_TO_B` or `B_TO_A`

## DBC API

### `POST /api/dbc`

Loads DBC text from request body (`text/plain`).

If body is empty and `/dbc/active.dbc` exists, firmware attempts loading that file.

On successful DBC load, runtime resets DBC-related state:
- signal cache reset
- subscriptions cleared
- observation mode reset to `none`
- replay stopped
- rules cleared

```powershell
curl -X POST "http://192.168.4.1/api/dbc" \
  -H "Content-Type: text/plain" \
  --data-binary "$(Get-Content .\data\dbc\vw_pq.dbc -Raw)"
```

### `POST /api/dbc/autoload`

Forces a rescan/autoload from the `/dbc` folder using the same boot order.

Example:

```powershell
curl -X POST "http://192.168.4.1/api/dbc/autoload"
```

### DBC Auto-Load at Boot

First valid file wins:
1. `/dbc/active.dbc`
2. `/dbc/default.dbc`
3. `/dbc/vw_pq.dbc`
4. first `*.dbc` found in `/dbc`

## Legacy Compatibility Endpoints

Still available:
- `POST /api/mutations/stage`
- `POST /api/mutations`
- `POST /api/mutations/toggle`

Note: legacy staged arithmetic operations may be accepted by payload/UI, but deterministic engine execution currently centers on `REPLACE` and `PASS_THROUGH` behavior in this path.

## Using SignalScope on Other Boards (Short Version)

You do not need to rewrite the core runtime.

Do this:
1. Add a new PlatformIO env in `platformio.ini` for your board.
2. Update board-specific pins + CAN backend wiring in `main.cpp`.
3. Keep `core/` untouched (gateway/mutation/replay/cache logic).
4. Verify with `/api/status`:
   - `bus_a_ready=true`
   - `bus_b_ready=true`
   - ingress counters increasing
   - no sustained `rx_drops_run`

Minimum hardware requirement for full inline dual-bus behavior:
- two CAN channels total (native + external, or two external, etc.)
- enough MCU headroom for your traffic rate (dual-core recommended)

## Troubleshooting

- `LittleFS not mounted`: run erase -> upload -> uploadfs
- `partition "spiffs" could not be found`: ensure partition label remains `littlefs`
- UI missing/stale: verify `data/index.html` exists and run `uploadfs` again
- Sparse live data: check `ingress_a_frames` and `ingress_b_frames` in `/api/status`

## License

See [LICENSE](LICENSE).

![Screenshot](screenshot.png)
