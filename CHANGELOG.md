# Changelog

This project follows a human-readable changelog. Until the standalone API stabilizes, compatibility-impacting changes are called out explicitly rather than hidden behind an assumed semantic-version promise.

## Unreleased — standalone framework overhaul (2026-07-18)

### Added

- A dependency-free browser workbench organized around Load DBC → Explore → Build rule → Apply/save.
- Searchable signal-catalog metadata including bit start, length, endian, signedness, factor, offset, live value, freshness, and direction.
- Generic persistent `.ssrules` package read, validated write/promote, and select endpoints.
- Standalone rule-package boot order: `/rules/active.ssrules`, then `/rules/default.ssrules`.
- A synthetic `0x321` DBC and non-autoloaded rule example for learning without implying vehicle coverage.
- End-to-end documentation for beginners, browser app authors, API clients, and native extension developers.
- Oil-temperature browser and native-extension examples.
- A dependency-free local preview server for exploring the complete UI without ESP32 or CAN hardware.
- Generic gateway, mutation-transaction, signal-catalog, diagnostic-transport, packed-DBC, replay, and session-log regression coverage.
- A progressive rule workstation with fixed and live-adjustable fields, a visual 64-bit raw-mask editor, and guided generators for source, selector, binding, counter, sequence, XOR, and CRC-8/AUTOSAR package rows.
- Draft value and enable edits are isolated from live runtime state, rejected package installs restore the active candidate mirror, and the API reports whether a candidate is actually dirty.

### Changed

- Restored the generic SignalScope runtime as an independent project rather than an application-specific firmware tree.
- Removed vehicle-specific DBC defaults, application branding, protected assets, and product UI from the standalone distribution.
- Renamed user-facing lifecycle actions so an in-RAM apply is not represented as persistent save.
- DBC autoload now prefers generic active/default files and otherwise searches `/dbc` for a valid database.
- Packed DBC loading uses the neutral `SSDB` identity while retaining read compatibility with the earlier packed-header marker.
- Parked-power terminology describes physical primary-side traffic rather than one vehicle topology.
- Rule inspection now distinguishes the active runtime table from the candidate table explicitly; browser-safe responses also include exact decimal text for 64-bit replacement values.
- Rule actions now name the authored, draft, live, and startup-package states explicitly, and unsupported staged rule kinds are rejected instead of silently becoming bit-range rules.

### Architecture

- SignalScope remains responsible for transport, decoding, mutation, replay, logging, storage, and generic services.
- Application extensions remain responsible for feature semantics, UI, configuration, and application-published runtime values.
- DBC and rule-table replacements remain candidate-first and atomic.
- Runtime rule handles remain protected by table epochs.

### Known limits

- The included hardware profile is the current ESP32-S3 N16R8/T-2CAN-style reference configuration, not a board-abstraction layer.
- The `.ssrules` package parser does not serialize or accept API-only `RAW_MASK` rows.
- The specialized CRC-8/AUTOSAR data-ID post-processor represents one explicit profile; it is not a universal E2E decoder.
- Browser/API contracts are documented but have not yet been declared a stable 1.0 interface.
