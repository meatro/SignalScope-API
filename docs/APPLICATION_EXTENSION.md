# Native application extension

Use a native extension when your app must calculate or retain behavior on the ESP32 even when no browser is connected. Dashboards and simple configuration tools can stay entirely in `data/`; they do not need C++ merely to read decoded signals.

The extension boundary preserves the main design rule:

> SignalScope owns CAN transport, decoding, rules, logging, storage, and generic HTTP routes. The application owns feature meaning, state, configuration, and UI.

The complete public contract is in `core/application_extension.hpp`. A commented starter is in `examples/application-extension/starter_app.cpp`.

## Registration model

Standalone SignalScope includes this weak no-op hook:

```cpp
void registerSignalScopeApplication(ApplicationExtensionRegistry& registry);
```

Your app provides one strong definition with the same namespace/signature and registers one `ApplicationExtension`. Registration happens once during boot and is immutable for the process lifetime.

At minimum, the descriptor needs:

- a valid `id`: 1–31 ASCII letters, digits, `.`, `_`, or `-`;
- `writeStatusJson`: the required implementation behind `GET /api/app/status`.

The registry rejects a second extension, an invalid ID, a missing status writer, or an invalid mutation-direction mask.

To compile an extension, either place its `.cpp` under a directory included by `build_src_filter` (the current project compiles `core/*.cpp`) or add its path explicitly in `platformio.ini`. Keep app source in its own clearly named folder once you define that build rule; do not paste app behavior into `main.cpp` or `mutation_engine.cpp`.

## Minimal shape

```cpp
#include "core/application_extension.hpp"

#include <cstdio>

namespace bored::signalscope {
namespace my_app {

bool writeStatus(char* output, size_t capacity) {
    return std::snprintf(output, capacity, "{\"ok\":true}") > 0;
}

}  // namespace my_app

void registerSignalScopeApplication(ApplicationExtensionRegistry& registry) {
    ApplicationExtension app{};
    app.id = "my-can-app";
    app.writeStatusJson = my_app::writeStatus;
    registry.registerExtension(app);
}

}  // namespace bored::signalscope
```

Production code should also check that `snprintf` fit within `capacity`. The full example does.

## Lifecycle order

The relevant boot sequence is:

1. register the extension;
2. initialize the generic runtime and attach `ApplicationServices`;
3. mount LittleFS and autoload a DBC;
4. call the extension's `begin()`;
5. apply the app-requested DBC/rule resource pair, if provided;
6. allow active CAN writes;
7. run `tick()` from the application task.

`attachServices()` therefore runs before the DBC is necessarily available. Save the service pointer there, then resolve signals in `begin()` and again in `databaseChanged()`.

## Resolve stable identities, not old indexes

DBC signal indexes are runtime handles. They can change whenever the active database changes. Persist this:

```text
CAN ID + signal name
```

Then resolve it:

```cpp
const int32_t index = services->findSignalByIdentity(0x321U, "OilTemperature");
if (index >= 0) {
    services->subscribeSignal(static_cast<uint16_t>(index), true);
}
```

The name-only resolver remains available for DBCs where names are guaranteed unique, but CAN-ID/name is the safer stored identity.

Subscribing marks the signal in `SignalCache` and adds mandatory observation of that CAN ID in both directions. It does not create a mutation rule.

## Reading values and frames

Use `readSignalState()` when freshness and direction matter:

```cpp
float value = 0.0F;
uint32_t generation = 0U;
bool valid = false;
uint32_t timestamp_us = 0U;
Direction direction = Direction::A_TO_B;

services->readSignalState(
    signal_index, &value, &generation, &valid, &timestamp_us, &direction);
```

`generation` changes as new decoded samples publish. The extension decides what age is acceptable for its behavior; a last value without freshness logic should not silently become a permanent command.

Frame services provide different evidence:

- `readFrame`: latest cached output plus input bytes/mutation marker when available;
- `readPhysicalFrame`: physical ingress only;
- `snapshotFramesByDirection`: up to 16 current frames for one direction;
- `snapshotMutatedFrames`: up to 16 frames whose output differed from input.

Prefer a signal subscription for a known DBC value. Read raw frames when your app genuinely needs byte-level provenance.

## Publishing values into rules

An extension can publish up to 32 named float values:

```cpp
services->publishRuntimeValue("requested_output", requested_output);
```

A `SOURCE_INT` or `SOURCE_SELECT_INT` package row can consume that name. This is the clean boundary for a controller:

```text
app state/map → published runtime value → generic rule engine → frame bits
```

The app computes *what value it wants*. The generic rule describes *where and how that raw value is written*.

Names are at most 31 characters. Publish a source before loading a package that references it, or rule compilation will reject the unresolved source.

### Runtime tables

An app can publish up to eight tables of 128 float thresholds:

```cpp
const float thresholds[] = {0.0F, 10.0F, 25.0F, 50.0F};
services->publishRuntimeTable("request_steps", thresholds, 4, true);
```

`BIND_TABLE` makes a matching dynamic rule choose the index of the first threshold greater than or equal to the source (or the final index). The selected index becomes the affine intermediate before output scaling. Tables should be ordered and their index semantics documented by the app.

Republishing a named value/table updates its stable registry slot atomically. Exhausting the fixed registry or using an invalid name returns `false`; handle that rather than assuming publication succeeded.

## App status, configuration, and resources

The generic host owns the routes:

| Extension callback | Host route | Purpose |
| --- | --- | --- |
| `writeStatusJson` | `GET /api/app/status` | Current app state as valid JSON |
| `configure` | `POST /api/app/config` | One key/value configuration command |
| `resourceCapacity` + `readResource` | `GET /api/app/resource?name=...` | Read a bounded app document |
| `resourceCapacity` + `writeResource` | `POST /api/app/resource?name=...` | Validate/write a bounded app document or command |

Return complete JSON, not fragments. Escape any text that can originate outside compiled constants. Keep callbacks bounded; the host caps declared resource capacity at 96 KiB, but a smaller honest cap is better.

If `configure()` changes which DBC/rules the app needs, stage that choice in app state. The host then asks:

- `requestedDatabasePath()`;
- optional `loadDatabase()` for an immutable app-owned database source;
- `requestedRulePackagePath()`.

It loads the pair while holding the application transaction and calls `finishConfigure(true)` only when both succeed. On failure it calls `finishConfigure(false)` so the app can roll back its pending selection. `requestResourceReload()` provides the same work as a deferred request from the normal application task; it must not cause filesystem work directly from a CAN callback.

## Diagnostic jobs

SignalScope provides bounded ISO-TP and VW TP2.0 transport, not a vehicle diagnostic catalog. The app supplies a proven `DiagnosticRequest` containing route, payload, expected response SID, timeout, and protocol settings.

The service flow is asynchronous:

```text
submitDiagnostic(request) → job_id
          ↓
readDiagnosticResult(job_id) until COMPLETE / FAILED / CANCELLED
          ↓
optional cancelDiagnostic(job_id)
```

There is one bounded command slot and one active job. A request payload is at most 32 bytes; the retained response is at most 512 bytes. The CAN task owns transport scheduling, while the app owns interpretation and user-facing operation names.

Do not replay a rejected diagnostic command automatically from a browser. A timeout can be ambiguous, and diagnostic operations may not be idempotent.

## Session annotations

The host owns the session recorder and storage format. While a recording is active, an app can append copied JSON context:

```cpp
const char payload[] = "{\"mode\":\"test\",\"target\":25}";
services->appendSessionLogAnnotation("mode_change", payload, sizeof(payload) - 1U);
```

The kind is limited to 31 UTF-8 bytes and JSON to 1024 bytes. Call from the application/UI task, never an ISR or CAN callback. An annotation supplements the physical trace; it does not replace raw evidence.

## Mutation-direction policy

Standalone SignalScope allows both directions. An app may narrow the gateway boundary in its immutable descriptor:

```cpp
app.mutation_direction_mask = kMutationDirectionAtoB;
```

This is an application architecture decision, not a guessed vehicle-generation gate. It cannot grant a direction outside the host's two valid mask bits.

## Parked-power callbacks

The host owns deep-sleep entry, Wi-Fi shutdown, LittleFS shutdown, and both CAN controllers. An app may provide:

- a bounded NVS-only `loadParkedPowerPolicy()` before filesystem/network startup;
- an allocation-free `decodeParkedPowerIgnition()` for each physical primary-side frame;
- `parkedPowerBusy()` to defer sleep during app work;
- `prepareForParkedSleep()` for bounded app cleanup.

Do not enable parked-power behavior until the ignition definition and wake wiring are proven for the actual installation. Returning “unknown” is better than inventing state from an unrelated frame.

## Callback constraints

| Callback class | Allowed style |
| --- | --- |
| `decodeParkedPowerIgnition` | Allocation-free, non-blocking, constant bounded work |
| `loadParkedPowerPolicy` | Bounded NVS reads only; no services/filesystem/network |
| `tick`, status/config/resource callbacks | Low-priority task work, still bounded and responsive |
| `databaseChanged`, `attachServices` | Rebind handles/store pointers; do not block |
| session annotation | Application/UI task only |

Never call HTTP, LittleFS, long serial formatting, delay loops, or dynamic configuration from a CAN-facing callback.

## Extension review checklist

- [ ] App ID is valid and stable.
- [ ] Status callback always returns bounded valid JSON.
- [ ] Persisted signals use CAN-ID/name identities, not indexes.
- [ ] `databaseChanged()` invalidates and resolves all cached handles.
- [ ] Every consumed signal has freshness/plausibility handling.
- [ ] Runtime values exist before dependent packages compile.
- [ ] App behavior is not duplicated as hard-coded mutation logic.
- [ ] Diagnostics use proven routes and explicit timeout/error handling.
- [ ] Filesystem/network work stays off CAN-facing callbacks.
- [ ] UI assets are licensed for redistribution and work without a CDN.
