# Core concepts

You do not need to memorize the entire CAN protocol to build with SignalScope. You do need a clear picture of five things: frames, directions, DBC definitions, observations, and rules.

## A CAN frame is a small labeled envelope

For this project, a frame has:

- an identifier such as `0x321`;
- a data length (`DLC`) from 0 to 8;
- up to eight data bytes;
- a direction through the inline gateway;
- a timestamp and observed rate.

The identifier is not a universal command. `0x321` can mean different things on different networks. Its meaning comes from the system design, supporting evidence, and often a DBC.

## Bus A and Bus B are physical sides

SignalScope sits between two CAN segments:

```text
Bus A  →  A_TO_B  →  Bus B
Bus A  ←  B_TO_A  ←  Bus B
```

A rule identity includes direction because the same CAN ID may appear on both sides with different consequences. Direction is physical topology, not a vehicle-generation label.

## A DBC turns bits into a signal

A DBC signal definition tells the decoder:

- which CAN ID carries the value;
- the start bit and bit length;
- byte/bit ordering (often called Intel/little-endian or Motorola/big-endian);
- whether the integer is signed;
- its factor and offset;
- its name, unit, and optional range.

The common conversion is:

```text
physical value = raw integer × factor + offset
raw integer = (physical value - offset) ÷ factor
```

For the synthetic oil-temperature signal:

```text
factor = 1
offset = -40
raw 130 → 130 × 1 - 40 = 90 °C
```

SignalScope's catalog returns both the DBC metadata and a recent decoded value. That lets the browser create a rule without hard-coding a bit offset that already exists in the DBC.

### A DBC is a hypothesis you can test

DBC files are commonly community-built, project-specific, incomplete, or version-specific. A signal name alone does not establish that it matches your exact network. Verify the output:

- Does throttle return near zero when your foot is off and rise smoothly when pressed?
- Does engine speed follow the tachometer?
- Does vehicle speed remain zero at a standstill?
- Does a switch signal change only with that physical switch?

A value can be decoded correctly while a different, undocumented control frame remains absent from the DBC. SignalScope therefore never disables raw forwarding or raw rules just because a frame lacks a DBC entry.

## Observation controls decoding, not forwarding

Forwarding every physical frame is the gateway's primary job. Decoding every signal in a huge DBC is optional work.

Observation modes let you choose the cost:

- `NONE`: no broad DBC decoding;
- `SPECIFIC`: decode selected CAN ID/direction pairs;
- `ALL`: decode all matching traffic;
- native subscriptions: an application marks the specific frames it requires.

Raw frames remain visible through the frame cache. Selecting less decoding does not create a generation gate and does not change whether physical traffic passes.

## Live, stale, valid, and unavailable are different

The signal catalog reports:

- `valid`: the bytes could be decoded using the DBC;
- `live`: the source frame is recent (within 1.5 seconds in this build);
- `ageMs`: how old the latest matching physical frame is;
- `direction`: the side from which that sample arrived.

Your UI should not display an old number as if it were live. The oil-temperature example visibly marks stale and unavailable states.

## A rule describes a deterministic frame change

A basic rule says:

> When CAN ID `0x321` travels `A_TO_B`, replace bits 0–7 with raw value 130.

Rules can also use raw byte masks, counters, sequences, checksums, or values published by a native application. Multiple rules form an ordered active table.

The engine does not ask which vehicle generation you selected. It matches the concrete identity in the rule: CAN ID, direction, and affected field.

## Staged is not active; active is not persistent

SignalScope has three distinct states:

| State | Changes live frames? | Survives reboot? |
| --- | ---: | ---: |
| Authored in browser | No | Only if your browser/app stores it |
| Staged in firmware | No | No |
| Applied active table | Yes | No |
| Valid saved `.ssrules` package | Yes | Yes |

An apply operation publishes the complete staged table atomically in RAM. A package upload is separately validated and promoted into LittleFS.

## Rule IDs need an epoch

An active rule receives a small runtime `rule_id`. That slot can be reused after a DBC or package change. SignalScope also returns `rule_epoch`, the generation of the entire table.

When changing a runtime value or enable state, send both. If the epoch changed, the API returns `409 stale_rule_handle` instead of letting an old UI request operate on a new rule.

## Physical, mutated, and synthetic traffic

These terms should remain visible in logs and UI:

- **Physical input:** bytes actually received from a CAN controller.
- **Forwarded output:** the bytes sent toward the other physical side, possibly changed by rules.
- **Mutated:** input and output differ because an active rule applied.
- **Synthetic/replay:** a frame originated in the replay engine rather than physical ingress.
- **Dry-run replay:** replay timing and accounting execute, but the frame is not transmitted to physical CAN.

Do not use a replay result as proof that an ECU accepted a message. It proves what SignalScope scheduled; physical response evidence comes from the bus.

## The app/core boundary

SignalScope answers reusable questions:

- How do I receive, forward, decode, cache, mutate, record, and replay CAN safely within this runtime?
- How do I atomically swap a DBC or a rule table?
- How can a browser or native application read the result?

Your application answers domain questions:

- Which signal is oil temperature on this network?
- What should a mode do?
- When should a rule be enabled?
- What should the user see and configure?

Keeping that boundary intact is what lets the same framework power unrelated CAN apps.
