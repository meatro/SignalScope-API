# Rules and `.ssrules` packages

Rules change selected bits only when a frame's CAN ID and physical direction match. SignalScope supports simple constant replacement, application-published values, selectors, counters, sequences, and checksum post-processing.

Start in the web rule workstation. Its three modes expose different parts of the same engine without pretending that every rule has the same lifetime:

- **Set a signal** stages a `BIT_RANGE` using a DBC signal or manual bit coordinates. A static value can be emitted as a persistent `STATIC` package row. A manually adjustable dynamic value is staged with `dynamic=1`; it is RAM-only and must not be presented as a reboot-persistent package rule.
- **Raw bits** stages an eight-byte `RAW_MASK`. It is useful for explicit pass-through/force-zero/force-one bit editing, but it is RAM-only because `.ssrules` has no `RAW_MASK` row.
- **Advanced package** generates editable recipes for `SOURCE_INT`, `SOURCE_SELECT_INT`, `BIND_TABLE`, `BIND_OVERRIDE`, `BIND_ACTIVE`, `COUNTER`, `SEQUENCE8`, `CHECKSUM_XOR`, and `CHECKSUM_CRC8_AUTOSAR`. These recipes remain browser text until you install the package.

The workstation does not expose the old `ADD_OFFSET`, `MULTIPLY`, or `CLAMP` controls. Those legacy request names are parsed by the compatibility adapter but rejected by the current rule engine.

## The four rule states

Keep these states separate in your UI and mental model:

1. **Authored:** fields or package text exist only in the browser.
2. **Candidate:** the firmware's staging table has accepted the proposed rules, but live traffic is unchanged.
3. **Active:** `apply_commit` published the complete candidate table in RAM.
4. **Persistent:** a valid package was promoted to LittleFS and can load at boot.

Applying is not saving.

```text
POST /api/rules/stage    → candidate only
POST /api/rules          → body: apply_commit → active RAM table
POST /api/rules/package  → validate + activate + store .ssrules in one transaction
```

Read the tables separately:

```text
GET /api/rules                 → active table affecting CAN now
GET /api/rules?view=staging    → candidate table reviewed before Apply
```

The response field `enabled` describes whether a rule is enabled. The compatibility field `active` is an alias for that same enabled flag, not the table name. Determine the table from the URL you requested.

`revert` replaces the candidate/staging table with a copy of the active table. `clear_staging` empties only the candidate. `clear_rules` empties both active behavior and the host's selected-package state, but it does not delete an existing file from LittleFS.

The API exposes `candidate_dirty` so editors can distinguish an unchanged candidate copy from real pending changes. Candidate enable toggles are RAM authoring controls; they do not rewrite package text. If a rule should stay absent or disabled after reboot, make that choice in the package source itself.

## Package locations and boot order

Rule packages must be plain text, no larger than 64 KiB, with a path under `/rules/`, no traversal or hidden-path segments, and a `.ssrules` suffix.

Standalone boot tries:

1. `/rules/active.ssrules`
2. `/rules/default.ssrules`

It does not automatically load every package in the directory. Keep other names for reusable profiles and select them explicitly through `/api/rules/select`.

Installing a package is one validate/activate/store transaction. When a package is uploaded, the firmware:

1. writes `/rules/upload.tmp.ssrules`;
2. parses every non-comment row into the candidate/staging table;
3. compiles and commits the complete table;
4. removes the temporary file on failure;
5. only after success, renames it to the requested path.

An invalid row therefore cannot replace the last known-good startup file or partially alter the active table. Do not describe this action as merely “save”: a successful install both changes the active rule table and stores the startup package.

## Common syntax

Packages are line-oriented CSV. Blank lines and lines beginning with `#` are ignored. Fields are trimmed. Quoted CSV fields and commas inside a field are not supported.

Most frame-field rows begin with:

```text
TYPE,can_id,direction,start_bit,length,little_endian,...
```

- `can_id`: decimal or `0x` hexadecimal, up to 29 bits.
- `direction`: exactly `A_TO_B` or `B_TO_A`; any other package text is rejected.
- `start_bit`: the DBC-style start bit.
- `length`: 1–64 for static fields; dynamic/counter values are limited to 32 bits; sequences are limited to 8 bits.
- `little_endian`: `1` for little-endian, `0` for big-endian.

The package stores raw integers, not physical engineering values. Convert with the DBC first:

```text
raw = (physical - offset) / factor
```

Round or truncate only in the same way your signal definition expects, then confirm the output bytes.

## Static replacement

```text
STATIC,can_id,direction,start_bit,length,little_endian,raw_value
```

Example: replace the synthetic `OilTemperature` byte on `0x321` with raw 130:

```text
STATIC,0x321,A_TO_B,0,8,1,130
```

`STATIC` accepts an unsigned raw value up to 64 bits, and that value must fit the target field length. The web rule builder normally derives it from the selected DBC signal.

## Dynamic integer source

```text
SOURCE_INT,can_id,direction,start_bit,length,little_endian,
value_source,source_gain,source_offset,output_scale,output_offset,
zero_threshold,zero_output,full_threshold,full_output
```

This row reads a named value published by a native application extension. The loader truncates the affine and final output stages for this package type. In the normal path:

```text
affine = floor(source × source_gain + source_offset)
raw = floor(affine × output_scale + output_offset)
```

The output is clamped at zero and at the maximum raw value for the field. Before the normal path:

- `source <= zero_threshold` produces `zero_output`;
- `source >= full_threshold` produces `full_output`.

Example shape (the source name must actually be published by an installed app):

```text
SOURCE_INT,0x321,A_TO_B,0,8,1,desired_oil_raw,1,0,1,0,0,0,255,255
```

Without a registered runtime value of that name, package compilation fails. Standalone browser-only SignalScope does not invent the source.

## Selector-based dynamic source

```text
SOURCE_SELECT_INT,can_id,direction,start_bit,length,little_endian,
value_source,source_gain,source_offset,output_offset,
zero_threshold,zero_output,full_threshold,selector_source,entries
```

`selector_source` must publish a value from 0 through 15. `entries` is a pipe-separated map:

```text
selector:D:direct_output
selector:S:output_scale:full_output
```

- `D` writes one direct raw value for that selector.
- `S` uses the normal affine calculation with a selector-specific scale and full-threshold output.
- a selector omitted from `entries` disables the rule for that state.

Example syntax:

```text
SOURCE_SELECT_INT,0x321,A_TO_B,0,8,1,requested_value,1,0,0,0,0,100,mode,0:D:0|1:S:1.0:100|2:S:2.0:200
```

The exact values above are illustrative, not a vehicle strategy.

## Bind directives

Bind directives are not rules and do not count toward the 96-rule limit. They affect later dynamic rows in the same package.

### Lookup table

```text
BIND_TABLE,value_source,runtime_table_name
```

For later rows whose `value_source` matches, the engine asks the named application-published table for the first entry at or above the source value. That table output replaces the affine intermediate before output scaling.

### Override pair

```text
BIND_OVERRIDE,value_source,override_active_source,override_value_source
```

For matching later rows, an `override_active_source` value of at least 0.5 selects `override_value_source` instead of the normal source path. Output scaling and offset still apply.

### Active-selector gate

```text
BIND_ACTIVE,selector_source,0|1|4
```

This attaches a selector gate to later rows that do not already define a selector. The rule runs only when the published selector rounds to one of the listed 0–15 states. This directive gates a rule; it does not choose selector-specific output scaling.

The loader retains the latest binding of each type as it continues down the file. Place directives before the rows they should affect and use distinct source names when scopes should not overlap.

## Counter

```text
COUNTER,can_id,direction,start_bit,length,little_endian,
initial,step,wrap_after,wrap_to
```

The first matching frame receives `initial`. After each application, the runtime value advances by `step`, or becomes `wrap_to` when the current value is at least `wrap_after`.

```text
COUNTER,0x321,A_TO_B,8,4,1,0,1,15,0
```

Counter state lives in RAM. Reapplying/reloading a table initializes it from the package again.

## Byte sequence

```text
SEQUENCE8,can_id,direction,start_bit,length,little_endian,
value0|value1|...,initial_index
```

The sequence contains 1–16 byte values. The field length must be no more than 8 bits and `initial_index` must refer to an entry in the sequence.

```text
SEQUENCE8,0x321,A_TO_B,16,8,1,0x10|0x20|0x30,0
```

Each matching frame uses the current entry and advances cyclically.

## XOR checksum

```text
CHECKSUM_XOR,can_id,direction,target_byte,start_byte,end_byte,seed,enabled
```

The checksum begins with `seed`, XORs the inclusive byte range, and writes the result to `target_byte`.

```text
CHECKSUM_XOR,0x321,A_TO_B,7,0,6,0x00,1
```

The target and range must stay within bytes 0–7. Confirm whether the target byte should be excluded from the range for your protocol.

## CRC-8/AUTOSAR data-ID checksum

```text
CHECKSUM_CRC8_AUTOSAR,can_id,direction,target_byte,counter_byte,
start_byte,end_byte,id0|id1|...|id15,enabled
```

This is a specialized post-processor, not a universal “AUTOSAR checksum” switch. The current implementation computes CRC-8/AUTOSAR (polynomial `0x2F`, init/xorout `0xFF`) across the inclusive payload range, then appends the data-ID selected by the low nibble of `counter_byte`. The package must provide exactly 16 data-ID bytes.

Use it only when captures or protocol documentation prove that exact profile, byte range, counter position, and data-ID table. A different E2E profile needs application-specific work rather than guessed constants.

## Rule execution order

The compiler groups rules by CAN ID and direction. Within each matching frame it applies kinds in this fixed order:

1. bit-range rules (`STATIC`, `SOURCE_INT`, `SOURCE_SELECT_INT`);
2. raw-mask rules staged through the API;
3. counters;
4. byte sequences;
5. XOR checksums;
6. CRC-8/AUTOSAR data-ID checksums.

Within one kind, staging/package order is preserved. This ensures checksum post-processors see earlier field changes. Overlapping ordinary rules are still easy to misunderstand; avoid them unless the later overwrite is intentional and tested.

## Raw-mask rules

The live HTTP staging API supports `RAW_MASK`, which applies eight mask/value bytes to a matching frame. The current `.ssrules` text loader does **not** define a `RAW_MASK` row. If a project needs persistent byte masks, express non-overlapping changes as `STATIC` bit ranges or extend the package format and its tests explicitly.

Do not assume that an API-only staged rule can be reconstructed from `GET /api/rules/package`; that endpoint reads a stored package file, not a serialization of arbitrary active RAM state.

## Runtime enable and value changes

Fetch the table you intend to control, then use both `rule_id` and `rule_epoch`. Use plain `GET /api/rules` for active runtime controls and `GET /api/rules?view=staging` while reviewing a candidate:

```text
POST /api/rules/value
rule_id=3&rule_epoch=12&view=staging&value=42
```

```text
POST /api/rules/enable
rule_id=3&rule_epoch=12&view=staging&enabled=false
```

`/api/rules/value` controls a manual dynamic `BIT_RANGE` raw value, a `COUNTER`'s next state, or a `SEQUENCE8` next-entry index. The rule listing exposes that state as `runtime_value`/`runtime_value_text` and identifies its meaning with `runtime_value_kind`; sequence indexes are bounded by `sequence_count`. `SOURCE_INT` and `SOURCE_SELECT_INT` reject manual writes because their application source is authoritative. `view=staging` changes the candidate; explicit `view=active` changes live runtime even if the same slot has pending candidate edits. A static replacement is immutable, so restage it with the new value.

`/api/rules/enable` with `view=staging` changes only the RAM candidate. Explicit `view=active` changes only live runtime. Omitting `view` retains legacy pending-slot behavior. None of these forms edits a stored `.ssrules` file.

If the table epoch changed, the firmware returns HTTP 409. Fetch the same view again and resolve the intended rule rather than retrying an old handle blindly.

## Limits

- Maximum active/candidate rules: 96 per table.
- Maximum CAN ID: 29-bit `0x1FFFFFFF`.
- Maximum replay frames: 1024 (separate from rules).
- Package body: 1–65536 bytes.
- Dynamic field/counter width: at most 32 bits.
- Sequence field width: at most 8 bits; 1–16 entries.
- Selector values: 0–15.
- Runtime source, table, and selector names: 1-31 ASCII characters, beginning with a letter or underscore and continuing with letters, numbers, `_`, `.`, or `-`.

Treat rejection as useful feedback. Do not work around a failed package by silently dropping the row; a complete table is safer and easier to reproduce than a partial success.
