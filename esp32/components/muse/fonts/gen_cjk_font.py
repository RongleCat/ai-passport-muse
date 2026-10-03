#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Regenerate the Passport caption font from Noto Sans SC Regular (SIL OFL).

The committed muse_font_cjk_14.c is what the firmware builds. This script is
only for regenerating it. See README.md for the font version and the measured
size table.

Usage:
  python3 gen_cjk_font.py NotoSansSC-Regular.otf /path/to/lv_font_conv
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
OUT_NAME = "muse_font_cjk_14"


def gb2312_rows(rows: range) -> list[str]:
    """Characters defined in those GB2312-80 rows (1-based, EUC-CN bytes)."""
    found = []
    for row in rows:
        for col in range(1, 95):
            raw = bytes((0xA0 + row, 0xA0 + col))
            try:
                found.append(raw.decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return found


def charset() -> str:
    """GB2312 level-1 hanzi, plus that standard's punctuation and fullwidth rows.

    Rows 1-3 are punctuation, numerals/roman, and fullwidth ASCII. Rows 16-55
    are the 3755 level-1 hanzi. Printable ASCII is passed as a range, not here.
    Rows 4-9 (kana, Greek, Cyrillic, bopomofo, line drawing) are left out.
    """
    chars = gb2312_rows(range(1, 4)) + gb2312_rows(range(16, 56))
    seen: set[str] = set()
    out: list[str] = []
    for ch in chars:
        if ch in seen or ord(ch) < 0x80:
            continue
        seen.add(ch)
        out.append(ch)
    return "".join(out)


def run_conv(conv: str, otf: Path, symbols: str, dest: Path, bpp: int, compress: bool) -> None:
    cmd = [
        conv,
        "--font",
        str(otf),
        "--autohint-off",
        "--no-kerning",
        "--bpp",
        str(bpp),
        "--size",
        "14",
        "--format",
        "lvgl" if dest.suffix == ".c" else "bin",
        "-r",
        "0x20-0x7E",
        "--symbols",
        symbols,
        "-o",
        str(dest),
    ]
    if dest.suffix == ".c":
        cmd.extend(["--lv-font-name", OUT_NAME, "--lv-include", "lvgl.h"])
    if not compress:
        cmd.extend(["--no-compress", "--no-prefilter"])
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"lv_font_conv failed for {dest.name}")
    if proc.stderr.strip():
        print(proc.stderr.strip())


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_cjk_font.py NotoSansSC-Regular.otf lv_font_conv")
    otf = Path(sys.argv[1])
    conv = sys.argv[2]
    symbols = charset()
    level1 = gb2312_rows(range(16, 56))
    punct = gb2312_rows(range(1, 4))
    print(f"level1={len(level1)} punct_fullwidth_rows={len(punct)} symbols={len(symbols)}")
    print(f"symbols_utf8_bytes={len(symbols.encode())}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        sizes = []
        for bpp, compress, label in (
            (4, False, "14px 4bpp --no-compress"),
            (2, False, "14px 2bpp --no-compress"),
            (4, True, "14px 4bpp compressed"),
            (2, True, "14px 2bpp compressed"),
        ):
            dest = root / f"{label.split()[1]}_{'raw' if not compress else 'rle'}.bin"
            run_conv(conv, otf, symbols, dest, bpp, compress)
            size = dest.stat().st_size
            sizes.append((label, size))
            print(f"{label}: bin {size} bytes")

    # Uncompressed bitmaps are drawn from flash into LVGL's existing glyph
    # buffer. The compressed reader mallocs a line buffer per glyph, which
    # this board cannot spare. 4bpp is the one that stays readable at 14px;
    # the bin sizes above are the comparison, and the .c is the 4bpp plain font.
    chosen = HERE / f"{OUT_NAME}.c"
    run_conv(conv, otf, symbols, chosen, 4, False)
    text = chosen.read_text(errors="replace")
    # The converter writes the paths it was given into the file header.
    # Keep that comment portable: a basename, not this machine's directories.
    text = text.replace(str(otf), otf.name).replace(str(chosen), f"{OUT_NAME}.c")
    chosen.write_text(text)
    indexes = [int(n) for n in __import__("re").findall(r"\.bitmap_index = (\d+)", text)]
    print(f"wrote {chosen} ({chosen.stat().st_size} bytes)")
    print(f"glyph_dsc={len(indexes)} max_bitmap_index={max(indexes) if indexes else 0}")
    if indexes and max(indexes) >= 1 << 20:
        raise SystemExit("bitmap index needs LV_FONT_FMT_TXT_LARGE (20-bit field)")
    for label, size in sizes:
        print(f"SIZE\t{size}\t{label}")


if __name__ == "__main__":
    main()
