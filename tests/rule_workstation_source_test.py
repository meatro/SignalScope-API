#!/usr/bin/env python3
"""Lock the standalone rule workstation to capabilities the engine really has.

This is intentionally a dependency-free source contract. It catches UI regressions
without needing a browser, Node, or an attached controller.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
INDEX_PATH = ROOT / "data" / "index.html"
SCRIPT_PATH = ROOT / "data" / "assets" / "signalscope.js"
MAIN_PATH = ROOT / "main.cpp"


def require(needle: str, text: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def require_pattern(pattern: str, text: str, message: str) -> None:
    if re.search(pattern, text, flags=re.IGNORECASE | re.DOTALL) is None:
        raise AssertionError(message)


def main() -> None:
    index = INDEX_PATH.read_text(encoding="utf-8")
    script = SCRIPT_PATH.read_text(encoding="utf-8")
    main = MAIN_PATH.read_text(encoding="utf-8")
    ui_source = f"{index}\n{script}"

    # These explicit mode attributes make the progressive workstation readable
    # to both its JavaScript and a source-level reviewer.
    for mode in ("signal", "raw", "package"):
        require(
            f'data-builder-mode="{mode}"',
            index,
            f"rule workstation is missing its {mode!r} builder mode",
        )

    require(
        'rule_kind: "BIT_RANGE"',
        script,
        "Signal mode must stage the engine's explicit BIT_RANGE rule kind",
    )
    require_pattern(
        r"dynamic\s*:\s*[A-Za-z_$][\w$]*\.dynamic\s*\?\s*1\s*:\s*0",
        script,
        "Signal mode must be able to stage dynamic=1 for a manual runtime value",
    )
    require(
        'view: "staging"',
        script,
        "Draft controls must explicitly request candidate-only edits",
    )
    require(
        "setStagedRuleValue",
        main,
        "firmware must isolate Draft value edits from live runtime state",
    )
    require(
        "setStagedRuleEnabled",
        main,
        "firmware must isolate Draft value and enabled edits from live runtime state",
    )
    require(
        "runtime_value_kind",
        main,
        "rule listings must identify manual raw, counter-state, and sequence-index controls",
    )
    require(
        "function exactRuntimeValue",
        script,
        "the workstation must display the authoritative runtime state for advanced rules",
    )
    require_pattern(
        r"manual\s+dynamic|dynamic\s+(?:runtime\s+)?value|(?:live|runtime)[- ]adjustable",
        index,
        "Signal mode must explain the manual dynamic/RAM-only option to users",
    )

    require(
        'rule_kind: "RAW_MASK"',
        script,
        "Raw-bits mode must stage a RAW_MASK payload rather than imitate STATIC",
    )
    require(
        "clear_staging",
        script,
        "the workstation must be able to clear only the candidate table",
    )
    require(
        "unsupported_staged_rule_kind",
        main,
        "package-only or unknown staged kinds must be rejected instead of becoming BIT_RANGE",
    )
    require(
        "function validateBitRange",
        script,
        "guided editors must validate complete Intel and Motorola bit traversal before staging",
    )
    require_pattern(
        r"frameBit\s*=\s*frameBit\s*%\s*8\s*===\s*0\s*\?\s*frameBit\s*\+\s*15\s*:\s*frameBit\s*-\s*1",
        script,
        "Motorola field validation must match the firmware's byte-boundary traversal",
    )
    if len(re.findall(r"validateBitRange\s*\(", script)) < 3:
        raise AssertionError(
            "both the signal builder and advanced recipe builder must use shared bit-range validation"
        )

    recipe_keywords = (
        "SOURCE_INT",
        "SOURCE_SELECT_INT",
        "BIND_TABLE",
        "BIND_OVERRIDE",
        "BIND_ACTIVE",
        "COUNTER",
        "SEQUENCE8",
        "CHECKSUM_XOR",
        "CHECKSUM_CRC8_AUTOSAR",
    )
    for keyword in recipe_keywords:
        require(
            keyword,
            ui_source,
            f"Advanced package mode is missing the {keyword} recipe",
        )
    require(
        "function insertPackageBinding",
        script,
        "guided binding rows must use a non-destructive positional insertion path",
    )
    if "function orderSourceBindings" in script or "single global gate" in script:
        raise AssertionError(
            "the workstation must not collapse positional BIND_* scopes into one global binding"
        )

    # Installing through /api/rules/package is not a file-only save: successful
    # validation activates the package and stores it for boot in one transaction.
    require_pattern(
        r"install\w*.{0,320}validat\w*.{0,160}activat\w*.{0,160}stor\w*.{0,200}one\s+transaction",
        index,
        "package-install help must say that one transaction validates, activates, and stores",
    )

    # These old arithmetic choices are parsed by a compatibility endpoint but
    # rejected by the current engine. Offering them would make the UI lie.
    for rejected in ("ADD_OFFSET", "MULTIPLY", "CLAMP"):
        if re.search(rf"\b{re.escape(rejected)}\b", ui_source, flags=re.IGNORECASE):
            raise AssertionError(
                f"rule workstation must not offer rejected legacy operation {rejected}"
            )

    print("RULE_WORKSTATION_SOURCE_TEST_PASS")


if __name__ == "__main__":
    main()
