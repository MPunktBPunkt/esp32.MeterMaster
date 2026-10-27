#!/usr/bin/env python3
"""Packt web/index.html als gzip-PROGMEM nach include/UiPagesGz.h."""
from __future__ import annotations

import gzip
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "web" / "index.html"
OUT = ROOT / "src" / "web" / "UiPagesGz.h"


def main() -> int:
    if not SRC.is_file():
        print(f"pack_ui: fehlt {SRC}", file=sys.stderr)
        return 1
    raw = SRC.read_bytes()
    gz = gzip.compress(raw, compresslevel=9)
    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "/** Autogeneriert von tools/pack_ui.py — nicht von Hand editieren.",
        f" *  Quelle: web/index.html ({len(raw)} B) → gzip ({len(gz)} B).",
        " */",
        f"static const size_t PAGE_MAIN_GZ_LEN = {len(gz)};",
        "static const uint8_t PAGE_MAIN_GZ[] PROGMEM = {",
    ]
    row: list[str] = []
    for b in gz:
        row.append(f"0x{b:02x}")
        if len(row) == 12:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    if OUT.is_file() and OUT.read_text() == text:
        print(f"pack_ui: unveraendert ({len(raw)} → {len(gz)} B)")
        return 0
    OUT.write_text(text)
    print(f"pack_ui: {SRC.name} {len(raw)} B → {OUT.name} {len(gz)} B gzip")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
