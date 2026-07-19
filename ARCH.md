# SignalScope architecture

SignalScope is a reusable CAN runtime with an optional application plugged into it. The separation is intentional: vehicle- or product-specific behavior should not be compiled into the generic forwarding, decoding, or rule layers.

## Data flow

```text
                         low-priority UI/application side
                  ┌────────────────────────────────────────┐
                  │ HTTP API · static UI · app extension   │
                  │ DBC/package loading · session storage  │
                  └───────────┬────────────────────────────┘
                              │ atomic tables / snapshots
                              ▼
Bus A ──physical ingress──► gateway ──rule engine──► physical egress ──► Bus B
  ▲                           │   ▲                                  │
  │                           │   │                                  ▼
  └──physical egress◄─────────┘   └────physical ingress────────── Bus B
                              │
                    frame cache + selected DBC decoding
                              │
                    trace queue + optional recorder
```

The CAN owner forwards physical traffic first. Browser work, filesystem writes, JSON construction, and application UI work are kept off that path.

## Runtime ownership

### CAN task

The high-priority `ss_can` task owns live ingress, forwarding, mutation application, replay dispatch, frame-cache publication, selected signal decoding, and diagnostic-transport scheduling. Its work must remain bounded and allocation-free in frame-facing callbacks.

Bus A uses the ESP32-S3 TWAI peripheral. Bus B uses an MCP2515 over SPI. Both are configured for 500 kbit/s in the current reference build. `GatewayCore` owns retry queues and publishes statistics rather than formatting web responses itself.

### Application task

The `ss_app` task runs an installed extension's `tick()` callback, performs deferred application-resource reloads, and flushes work that does not belong on the CAN task. Extension callbacks run behind the application lock where required.

### UI task

The `ss_ui` task owns `WebServer`, API handlers, and static LittleFS delivery. Large API snapshots use runtime scratch storage (PSRAM when available) instead of the task stack.

## Core components

| Component | Responsibility |
| --- | --- |
| `core/gateway.*` | Bidirectional forwarding, queueing, direction policy, and runtime statistics |
| `core/frame_cache.*` | Latest physical/output frames and mutation provenance |
| `core/dbc_parser.*` | Parse a bounded DBC candidate before it becomes active |
| `core/signal_catalog.*` | Searchable DBC metadata plus recent decoded values |
| `core/signal_cache.*` | Subscribed decoded signal state for native applications |
| `core/observation_manager.*` | Decide which traffic receives decoding work |
| `core/mutation_engine.*` | Stage, compile, atomically commit, and execute rules |
| `core/rule_package.*` | Parse text `.ssrules` packages into a staged rule table |
| `core/replay_engine.*` | Bounded timestamped replay, including a dry-run dispatch mode |
| `core/can_trace.*` | Non-blocking prepared-frame trace queue |
| `core/diagnostic_transport.*` | Bounded ISO-TP and VW TP2.0 transport jobs for an app to use |
| `core/application_extension.*` | Stable registration and service boundary for native apps |
| `fs/` | Persistence and session-log storage owned by the host |
| `data/` | Standalone browser UI and files uploaded to LittleFS |

## DBC lifecycle

A DBC replacement is transactional at the database/cache level:

1. Parse the submitted text into a candidate database.
2. Allocate and prepare a candidate signal cache.
3. Persist the text to `/dbc/active.dbc`.
4. Pause decoding and wait for the decoder to become idle.
5. Swap the candidates into the active runtime.
6. Clear signal subscriptions, observation selections, replay, and rules whose identities belonged to the previous database.
7. Notify the installed application and resume decoding.

The gateway continues to own physical forwarding. A DBC is for interpretation; it is not required for a raw frame to pass between the buses.

On standalone boot, the host tries `/dbc/active.dbc`, `/dbc/default.dbc`, and then the first valid `.dbc` in `/dbc`.

## Rule lifecycle and concurrency

The mutation engine maintains separate staged and active tables. Staging does not alter forwarded frames. `applyCommit()` compiles a complete replacement and publishes it atomically. The old active table remains authoritative if a staged rule cannot compile.

Each published table receives a new `rule_epoch`. Runtime rule IDs are reusable slots, so an API client must present the epoch it received with a rule ID. This prevents a delayed browser request from changing an unrelated rule after a package or DBC swap.

Standalone persistence is explicit:

- `POST /api/rules` with `apply_commit` changes RAM only.
- `POST /api/rules/package` validates, activates, and stores a package under `/rules/`.
- boot prefers `/rules/active.ssrules`, then `/rules/default.ssrules`.

An installed native application may instead own its DBC/rule pair through `requestedDatabasePath()`, `requestedRulePackagePath()`, and `finishConfigure()`. The host applies that pair as one application-resource transaction.

## Observation is a performance control

Frame forwarding is not gated by a DBC match. Decoding is selective because a large DBC may contain thousands of signals:

- `NONE` avoids general decode work;
- `SPECIFIC` decodes selected frame identities;
- `ALL` decodes every matching observed frame;
- native signal subscriptions add mandatory observations for the frames they need.

The frame cache still exposes raw traffic independently. Applications should subscribe only to the values they actually consume.

## Application boundary

Standalone SignalScope uses a weak, no-op `registerSignalScopeApplication()` definition. An app supplies a strong definition that registers one `ApplicationExtension`.

The host remains responsible for CAN hardware, timing, caches, storage, HTTP routing, and diagnostic transport. The application can:

- resolve and subscribe to DBC signals;
- read physical or post-mutation frame snapshots;
- publish named runtime values and lookup tables for dynamic rules;
- submit and inspect diagnostic jobs;
- add structured annotations to an active session log;
- expose status, configuration, and bounded resources;
- request a deferred DBC/rule reload;
- provide a parked-power policy and ignition decoder.

The extension must not perform filesystem, network, allocation-heavy, or blocking work from a CAN-facing callback. See [Application extension](docs/APPLICATION_EXTENSION.md).

## Storage and served assets

LittleFS contains the browser UI, DBC files, rule packages, and the current session-log artifact. Static files are served from `data/` after `uploadfs`; no CDN or package manager is required in the browser.

The current 16 MB partition layout provides two OTA application slots and a large LittleFS partition. Changing flash size or board layout requires reviewing `platformio.ini`, `partitions.csv`, and the board definition together.

## Design invariants

Changes to the framework should preserve these properties:

1. Physical forwarding does not wait for Wi-Fi, JSON, LittleFS, DBC parsing, or app UI work.
2. A partial DBC or rule-package parse never becomes the active table.
3. Staging is inert until an explicit commit.
4. Persistent and in-memory state are named distinctly in APIs and UI.
5. Rule handles are always paired with an epoch.
6. Replay is visibly distinguished from physical ingress, and user-facing tools default to dry-run.
7. Apps can narrow mutation directions, but the standalone framework does not invent vehicle-generation gates.
8. Application semantics stay outside the generic core.
9. Browser assets remain dependency-free and redistributable with the MIT-licensed repository.
