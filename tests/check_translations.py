#!/usr/bin/env python3
"""Check that every label vmc.yaml asks for exists in every language file.

`esphome config` only *warns* about an unresolved ${...} and leaves the literal
text as the entity name, so a label missing from one language would otherwise ship
as "${name_airflow}" on someone's dashboard. This fails instead.

Run from the repository root:

    python3 tests/check_translations.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "vmc.yaml"
TRANSLATIONS = ROOT / "translations"
REFERENCE = "fr.yaml"  # the default language, and the list every other one follows


def keys_of(path: Path) -> set[str]:
    """Top-level keys of a plain `key: value` YAML file."""
    return set(re.findall(r"^([a-z_0-9]+):", path.read_text(encoding="utf-8"), re.M))


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

    # 2. Every ${...} of the config is provided by a language file or by the config.
    provided = expected | own_substitutions(CONFIG)
    for used in sorted(used_substitutions(CONFIG) - provided):
        failures.append(
            f"{CONFIG.name}: '${{{used}}}' is defined nowhere "
            f"(neither in {REFERENCE} nor in its own substitutions)"
        )

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
