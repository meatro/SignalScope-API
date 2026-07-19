const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const main = fs.readFileSync(path.join(root, 'main.cpp'), 'utf8');
const gatewayHeader = fs.readFileSync(path.join(root, 'core', 'gateway.hpp'), 'utf8');
const gatewaySource = fs.readFileSync(path.join(root, 'core', 'gateway.cpp'), 'utf8');

function between(source, start, end) {
    const first = source.indexOf(start);
    const last = source.indexOf(end, first + start.length);
    assert.notEqual(first, -1, `missing ${start}`);
    assert.notEqual(last, -1, `missing ${end}`);
    return source.slice(first, last);
}

const setup = between(main, 'void setup()', 'void loop()');
const busInit = setup.indexOf('const bool bus_a_initialized = initBusA();');
const canTask = setup.indexOf('xTaskCreatePinnedToCore(\n        canRuntimeTask');
const fatalCreate = setup.indexOf('failCanRuntimeStartup("task_create_failed")');
const fatalStart = setup.indexOf('failCanRuntimeStartup("task_start_timeout")');
const persistence = setup.indexOf('persistence.begin();');
const wifi = setup.indexOf('startAccessPoint();');
for (const [name, value] of Object.entries({ busInit, canTask, fatalCreate, fatalStart, persistence, wifi })) {
    assert.notEqual(value, -1, `setup contract missing ${name}`);
}
assert.ok(busInit < canTask, 'both physical CAN controllers initialize before the CAN owner task');
assert.ok(canTask < fatalCreate && fatalCreate < persistence,
    'CAN task creation is mandatory before filesystem/application startup');
assert.ok(canTask < fatalStart && fatalStart < persistence,
    'CAN task start acknowledgement is mandatory before filesystem/application startup');
assert.ok(persistence < wifi, 'CAN and persisted app resources are established before Wi-Fi');

const fatalPath = between(main, '[[noreturn]] void failCanRuntimeStartup', 'bool initBusA()');
assert.match(fatalPath, /esp_restart\(\)/, 'a missing CAN owner fails closed by restarting');

const initialRetry = Number(main.match(/kCanInitialRecoveryRetryMs\s*=\s*(\d+)U/)[1]);
const twaiBackoff = Number(main.match(/kTwaiRecoveryRetryMs\s*=\s*(\d+)U/)[1]);
const mcpBackoff = Number(main.match(/kMcpRecoveryRetryMs\s*=\s*(\d+)U/)[1]);
assert.ok(initialRetry > 0 && initialRetry <= 250,
    'the first failed CAN initialization retry remains short');
assert.ok(initialRetry < twaiBackoff && initialRetry < mcpBackoff,
    'the short first retry precedes the steady-state recovery backoff');

const ingress = between(main, 'void pollCanIngress() {', 'void monitorCanHealth(uint32_t now_ms) {');
assert.match(ingress, /saturated_a/);
assert.match(ingress, /saturated_b/);
assert.doesNotMatch(ingress, /\breturn\s*;/,
    'one saturated ingress direction must not return before polling the opposite bus');

const mcpPhysicalTx = between(main, 'bool writeBusB(const CanFrame& frame) {',
    'bool txDriver(Direction tx_direction');
assert.match(mcpPhysicalTx, /diagnosticTransportTxPending\(Direction::A_TO_B\)/,
    'physical MCP TX checks the narrow diagnostic-pending window');
assert.match(mcpPhysicalTx, /sendMessage\(MCP2515::TXB0/);
assert.match(mcpPhysicalTx, /sendMessage\(MCP2515::TXB1/);
assert.doesNotMatch(mcpPhysicalTx, /sendMessage\(MCP2515::TXB2/,
    'physical forwarding never overwrites the reserved diagnostic mailbox');
assert.match(mcpPhysicalTx, /return can_mcp\.sendMessage\(&tx\)/,
    'physical forwarding regains all three buffers outside a pending diagnostic TX');

assert.match(main, /setBusAReady[\s\S]*purgeDirection\(Direction::B_TO_A\)/,
    'Bus A transitions purge frames whose destination is Bus A');
assert.match(main, /setBusBReady[\s\S]*purgeDirection\(Direction::A_TO_B\)/,
    'Bus B transitions purge frames whose destination is Bus B');
assert.match(gatewayHeader, /RawQueue raw_a_to_b_/);
assert.match(gatewayHeader, /RawQueue raw_b_to_a_/);
assert.doesNotMatch(gatewayHeader, /CanFrame rx_queue_\[/,
    'physical directions may not share a head-of-line-blocking raw ring');
assert.match(gatewaySource, /blocked_a_to_b/);
assert.match(gatewaySource, /blocked_b_to_a/);
assert.match(gatewaySource, /void GatewayCore::purgeDirection/);
assert.match(gatewaySource, /GatewayStats GatewayCore::snapshotStats\(\) const/);
assert.match(setup + main, /const GatewayStats stats = gateway\.snapshotStats\(\)/,
    'UI status reads an immutable published stats snapshot');
assert.doesNotMatch(main, /gateway\.stats\(\)/,
    'call sites must choose CAN-owner stats or a cross-core snapshot explicitly');

console.log('can runtime contract test passed');
