#!/usr/bin/env python3
"""Check that every label utility-bridge-common.yaml asks for exists in every language file.

`esphome config` only *warns* about an unresolved ${...} and leaves the literal
text as the entity name, so a label missing from one language would otherwise ship
as "${name_airflow}" on someone's dashboard. This fails instead.

It also checks that the files each per-install config sources are still there: those
configs point at GitHub, so nothing else in CI would notice a rename on this side.

Run from the repository root:

    python3 tests/check_translations.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "utility-bridge-common.yaml"  # the config the labels are used in
# The per-install configs that source it, from GitHub -- one per board.
DEVICES = [
    ROOT / "utility-bridge-d1-mini.yaml",
    ROOT / "utility-bridge-esp32.yaml",
]
TRANSLATIONS = ROOT / "translations"
REFERENCE = "en.yaml"  # the default language, and the list every other one follows


def keys_of(path: Path) -> set[str]:
    """Keys of the `substitutions:` block of a language file."""
    return set(re.findall(r"^  ([a-z_0-9]+):", path.read_text(encoding="utf-8"), re.M))


def own_substitutions(path: Path) -> set[str]:
    """Keys the config defines itself, under its own `substitutions:` block."""
    text = path.read_text(encoding="utf-8")
    block = re.search(r"^substitutions:\n(.*?)(?=^\S)", text, re.M | re.S)
    if block is None:
        return set()
    return set(re.findall(r"^  ([a-z_0-9]+):", block.group(1), re.M))


def used_substitutions(path: Path) -> set[str]:
    """Every ${...} the config refers to."""
    return set(re.findall(r"\$\{([a-z_0-9]+)\}", path.read_text(encoding="utf-8")))


def sourced_files(path: Path) -> list[str]:
    """Repository paths listed in a package's `files: [...]`."""
    listed = re.findall(r"^    files: \[(.*)\]$", path.read_text(encoding="utf-8"), re.M)
    return [name.strip() for line in listed for name in line.split(",")]


def main() -> int:
    languages = sorted(p for p in TRANSLATIONS.glob("*.yaml"))
    if not languages:
        print(f"error: no language file in {TRANSLATIONS}")
        return 1

    reference = TRANSLATIONS / REFERENCE
    if reference not in languages:
        print(f"error: the reference language {REFERENCE} is missing")
        return 1

    failures = []

    # 1. Every language offers the same labels as the reference one.
    expected = keys_of(reference)
    for lang in languages:
        if lang == reference:
            continue
        keys = keys_of(lang)
        for missing in sorted(expected - keys):
            failures.append(f"{lang.name}: missing '{missing}' (it is in {REFERENCE})")
        for extra in sorted(keys - expected):
            failures.append(f"{lang.name}: '{extra}' is not in {REFERENCE}")

    # 2. Every ${...} of the config is provided by a language file, by the config's
    # own substitutions, or by each per-install config that pulls it in as a package
    # (devicename, upper_devicename -- every per-install config needs those itself,
    # for its own `esphome:` block, so each one is guaranteed to set them).
    for device in DEVICES:
        provided = expected | own_substitutions(CONFIG) | own_substitutions(device)
        for used in sorted(used_substitutions(CONFIG) - provided):
            failures.append(
                f"{CONFIG.name}: '${{{used}}}' is defined nowhere "
                f"(neither in {REFERENCE}, its own substitutions, or {device.name}'s)"
            )

    # 3. Each per-install config sources the common config and a language from
    # GitHub, by path. A rename here breaks every install at its next build, and
    # only this notices.
    for device in DEVICES:
        sourced = sourced_files(device)
        if not sourced:
            failures.append(f"{device.name}: no `files: [...]` package to check")
        for name in sourced:
            if not (ROOT / name).is_file():
                failures.append(f"{device.name}: sources '{name}', which does not exist")
        if CONFIG.name not in sourced:
            failures.append(f"{device.name}: does not source {CONFIG.name}")
        if not any(name.startswith(f"{TRANSLATIONS.name}/") for name in sourced):
            failures.append(f"{device.name}: sources no language file, so no label resolves")

    if failures:
        print("Translation check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(
        f"Translations OK: {len(expected)} labels, "
        f"languages: {', '.join(p.stem for p in languages)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
