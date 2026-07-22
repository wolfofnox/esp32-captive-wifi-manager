#!/usr/bin/env python3
"""Convert ESP-IDF sdkconfig.h macros into preprocess directives."""

from __future__ import annotations

import re
import sys
from pathlib import Path


DEFINE = re.compile(r"^\s*#define\s+(CONFIG_[A-Za-z0-9_]+)\s+(.+?)\s*$")


def main(source: Path, destination: Path) -> None:
    definitions: list[str] = []
    for line in source.read_text(encoding="utf-8").splitlines():
        match = DEFINE.match(line)
        if match:
            name, value = match.groups()
            definitions.append(f"// #define {name} {value}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(definitions) + "\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: sdkconfig_to_preprocess.py <sdkconfig.h> <output.h>")
    main(Path(sys.argv[1]), Path(sys.argv[2]))
