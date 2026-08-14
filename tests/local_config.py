#!/usr/bin/env python3
"""Build a config from this checkout, in the language given.

A per-install config (utility-bridge-d1-mini.yaml by default, or whichever one is
passed as the second argument) pulls utility-bridge-common.yaml from GitHub, with
the language left to whatever file is merged alongside it; utility-bridge-common.yaml
in turn pulls the component from GitHub. All wrong for CI: a pull request has to be
validated against its own tree, not against what is already on main. This writes the
per-install config with the language included, and a localised copy of
utility-bridge-common.yaml (its own component source swapped for the working copy)
alongside it -- writing utility-bridge-common-local.yaml next to it as a side effect.

    python3 tests/local_config.py translations/fr.yaml > vmc-local.yaml
    python3 tests/local_config.py translations/en.yaml utility-bridge-esp32.yaml > esp32-local.yaml
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEVICE = "utility-bridge-d1-mini.yaml"
COMMON = "utility-bridge-common.yaml"
LOCAL_COMMON = "utility-bridge-common-local.yaml"

# Matched by shape, not by its exact text: its options (ref, refresh, ...) change,
# and a literal match would break every time -- which is how this script broke
# once already.
REMOTE_COMPONENT = re.compile(
    r"^external_components:\n  - source: github://\S+\n(?:    \S.*\n)*", re.M
)
LOCAL_COMPONENT = "external_components:\n  - source:\n      type: local\n      path: components\n"

REMOTE_PACKAGE = re.compile(r"^packages:\n  vmc:\n(?:    \S.*\n)+", re.M)


def localise(language: Path, device: str) -> str:
    # utility-bridge-common.yaml is the one that declares external_components now,
    # so it is the one localised; the copy is written next to it (not modified in
    # place) so the original stays what a real install fetches from GitHub.
    common, component_swapped = REMOTE_COMPONENT.subn(
        LOCAL_COMPONENT, (ROOT / COMMON).read_text(encoding="utf-8")
    )
    if component_swapped != 1:
        raise SystemExit(f"error: the github external_components source was not found in {COMMON}")
    (ROOT / LOCAL_COMMON).write_text(common, encoding="utf-8")

    config, package_swapped = REMOTE_PACKAGE.subn(
        f"packages:\n  vmc: !include {LOCAL_COMMON}\n  labels: !include {language}\n",
        (ROOT / device).read_text(encoding="utf-8"),
    )
    if package_swapped != 1:
        raise SystemExit(f"error: the github packages block was not found in {device}")
    return config


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    language = Path(argv[1])
    if not (ROOT / language).is_file():
        raise SystemExit(f"error: no such language file: {language}")
    device = argv[2] if len(argv) == 3 else DEFAULT_DEVICE
    if not (ROOT / device).is_file():
        raise SystemExit(f"error: no such device config: {device}")
    sys.stdout.write(localise(language, device))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
