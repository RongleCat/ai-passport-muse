<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Passport caption font

Flash-resident glyphs for Passport reply and caption labels. Built only when
`CONFIG_MUSE_CJK_FONT` is on, which only the Passport overlay selects.

| | |
|---|---|
| Source | Noto Sans SC Regular (Simplified Chinese) |
| Version | `2.004` (`name` table version `2.004;hotconv 1.0.118;makeotfexe 2.5.65603`, unique name `2.004;GOOG;NotoSansSC-Regular;ADOBE`) |
| Release | tag `Sans2.004`, published 2022-01-27 |
| Download | https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf |
| License | SIL Open Font License 1.1. See [`OFL.txt`](OFL.txt), copied from https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/LICENSE |
| Copyright | Copyright 2014-2021 Adobe (see `OFL.txt`) |
| Converter | `lv_font_conv` 1.5.3 (`npm install lv_font_conv@1.5.3`) |

The symbol in firmware is `muse_font_cjk_14`. It is not named Noto: that name
is reserved by the license for the original font.

## What's upstream and what's ours

| File | From | License |
|---|---|---|
| `OFL.txt` | upstream license, unmodified | SIL OFL 1.1 |
| `muse_font_cjk_14.c` | generated bitmaps, derived from Noto Sans SC | SIL OFL 1.1 |
| `gen_cjk_font.py` | Meta | Apache-2.0 |
| `README.md` | Meta | Apache-2.0 |

Don't restyle `muse_font_cjk_14.c` or `OFL.txt`, and don't add an Apache
header to either. The outlines are not committed. Download the OTF from the
URL above when regenerating.

## Character set

GB2312-80, decoded as EUC-CN (row byte `0xA0+row`, column byte `0xA0+col`):

| Range | What | Count |
|---|---|---|
| rows 16–55 | level-1 hanzi | 3755 |
| rows 1–3 | punctuation, numerals, fullwidth forms (empty slots skipped) | 260 defined |
| ASCII `0x20–0x7E` | printable ASCII, passed as a range | 95 |
| rows 4–9 | kana, Greek, Cyrillic, bopomofo, line drawing | left out |

ASCII duplicates inside rows 1–3 are dropped. The symbol list is 4015
characters (12036 UTF-8 bytes). `glyph_dsc` has 4111 entries: id 0 reserved,
plus 95 ASCII, plus 4015 symbols. A level-2 hanzi is not in the font, so LVGL
draws its placeholder.

## Size

14 px, 4 bpp, no kerning, autohint off. `lv_font_conv --format bin` on this
set, measured by `gen_cjk_font.py`:

| Variant | Bin bytes |
|---|---|
| 14px 4bpp `--no-compress` | 425852 |
| 14px 2bpp `--no-compress` | 232504 |
| 14px 4bpp compressed | 354832 |
| 14px 2bpp compressed | 210872 |

The committed file is the 4 bpp uncompressed font. The largest
`.bitmap_index` is 385208, under the 20-bit field, so
`LV_FONT_FMT_TXT_LARGE` stays off. Compression would save about 71 KB of the
4 bpp bin and needs `LV_USE_FONT_COMPRESSED`, whose reader allocates a line
buffer per glyph. This board has no spare internal RAM for that. `.line_height`
is 18 and `.base_line` is 4. The `.c` file is about 2.87 MB of source; the
bin above is the flash bitmap size before the descriptor tables.

## Regenerating

```sh
npm install lv_font_conv@1.5.3
python3 gen_cjk_font.py /path/to/NotoSansSC-Regular.otf /path/to/lv_font_conv
```

The script prints the four bin sizes, then writes `muse_font_cjk_14.c` beside
itself. It rewrites the header comment so the font and output paths are file
names, not absolute paths.
