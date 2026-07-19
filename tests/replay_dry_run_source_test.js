const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');
const replayHeader = read('core/replay_engine.hpp');
const replaySource = read('core/replay_engine.cpp');
const gatewayHeader = read('core/gateway.hpp');
const gatewaySource = read('core/gateway.cpp');
const main = read('main.cpp');

function section(source, begin, end) {
    const start = source.indexOf(begin);
    const finish = source.indexOf(end, start + begin.length);
    assert.ok(start >= 0, `missing section start: ${begin}`);
    assert.ok(finish > start, `missing section end: ${end}`);
    return source.slice(start, finish);
}

assert.match(replayHeader, /enum class ReplayDispatchMode[\s\S]*PHYSICAL[\s\S]*DRY_RUN/,
    'replay sessions need an explicit dispatch mode');
assert.match(replayHeader,
    /loadLogCsv\([\s\S]{0,220}ReplayDispatchMode::PHYSICAL/,
    'loading a replay must remain physical by default');
assert.match(replayHeader,
    /TxCallback\s*=\s*bool \(\*\)\(const CanFrame& frame, ReplayDispatchMode dispatch_mode\)/,
    'dispatch mode must travel with each callback rather than through a global toggle');
assert.match(replaySource, /tx_callback_\(replay_frame\.frame, dispatch_mode_\)/,
    'the replay owner must pass its locked session mode into frame dispatch');

const bridge = section(main,
    'bool replayTxBridge(const CanFrame& frame, ReplayDispatchMode dispatch_mode) {',
    'void pollCanIngress()');
assert.match(replayHeader,
    /CanFrame liveReplayIngressFrame[\s\S]{0,180}CanFrame live_frame\s*=\s*scheduled_frame[\s\S]{0,100}live_frame\.timestamp_us\s*=\s*now_us/,
    'live ingress conversion must copy and stamp rather than mutate stored scheduling data');
assert.match(bridge, /liveReplayIngressFrame\(frame, micros\(\)\)/,
    'CSV schedule time must be replaced with live ingress time before decoding');
assert.match(bridge,
    /injectReplayFrame\([\s\S]{0,100}dispatch_mode\s*==\s*ReplayDispatchMode::DRY_RUN/,
    'the per-session dry-run mode must reach the gateway');

assert.match(gatewayHeader, /injectReplayFrame\(const CanFrame& frame, bool dry_run = false\)/,
    'gateway replay must retain backward-compatible physical injection');
const inject = section(gatewaySource, 'bool GatewayCore::injectReplayFrame(',
    'bool GatewayCore::physicalBacklogPending() const');
assert.match(inject, /!dry_run\s*&&\s*physicalBacklogPending\(\)/,
    'dry-run must bypass the early physical-backlog refusal');
assert.match(inject, /forwardFrame\(mutable_frame, true, dry_run, millis\(\)\)/,
    'replay cache bookkeeping must use millis() independently of micros() rollover');
const forward = section(gatewaySource, 'bool GatewayCore::forwardFrame(',
    'bool GatewayCore::dispatchPrepared(');
assert.match(forward, /from_replay\s*&&\s*!replay_dry_run\s*&&\s*physicalBacklogPending\(\)/,
    'dry-run must bypass the common-path physical-backlog refusal');
assert.match(forward, /ingress_frame_cache_->update\(received_frame/,
    'dry-run must retain ingress provenance');
assert.match(forward, /decodeObservedFrame\(\*dbc, received_frame\)/,
    'dry-run must decode the unmodified replay frame');
assert.match(forward, /applyFrameMutations\(frame\)/,
    'dry-run must execute the real mutation path');
assert.match(forward, /frame_cache_->update\(frame[\s\S]{0,100}&received_frame/,
    'dry-run must retain prepared output/input provenance');
assert.match(forward, /trace_callback_\(received_frame, frame,[\s\S]{0,80}from_replay\)/,
    'dry-run must emit the normal synthetic trace');

const dispatch = section(gatewaySource, 'bool GatewayCore::dispatchPrepared(',
    'bool GatewayCore::enqueuePrepared(');
assert.match(dispatch, /if \(!replay_dry_run\s*&&[\s\S]{0,120}tx_driver_/,
    'the hardware driver must be unreachable when dry-run is true');
assert.match(dispatch, /replay_dry_run_frames/,
    'completed dry-runs need a dedicated counter');

const load = section(main, 'void handleReplayLoad() {', 'void handleReplayControl() {');
const control = section(main, 'void handleReplayControl() {', 'void handleReplaySend() {');
const send = section(main, 'void handleReplaySend() {', 'void handleDbcUpload() {');
for (const [name, handler] of Object.entries({ load, control, send })) {
    assert.match(handler, /dry_run/, `${name} replay handler must expose dry-run selection`);
}
assert.match(load, /parseBoolText\(server\.arg\("dry_run"\), false\)/,
    'loaded replay defaults to physical dispatch');
assert.match(send, /parseBoolText\(server\.arg\("dry_run"\), false\)/,
    'one-shot replay defaults to physical dispatch');
assert.match(control, /Retain the dispatch mode selected when the replay was loaded/,
    'start without an override must preserve loaded replay ownership');
assert.match(main, /stats\.replay_dry_run_frames/,
    'status must expose completed dry-run frames');
assert.match(main, /replay_dry_run\s*=\s*replay_engine\.isDryRun\(\)/,
    'status must expose the loaded replay mode');

console.log('replay dry-run source contract passed');
