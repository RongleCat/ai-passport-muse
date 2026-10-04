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

"""Caption paging keeps its old columns for ASCII, and counts CJK as two.

The legacy harness is muse_chat_text.c with CONFIG_MUSE_CJK_FONT unset, which
is the previous "every kept character is one column" rule. The wide harness
is the same file with the Passport switch on, and can turn that rule off.
"""

from __future__ import annotations

import os
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

ASCII_PAGES = (
    ("", 16, 2, 0, None),
    ("Hi", 16, 2, 0, "Hi"),
    ("0123456789abcdef", 16, 2, 0, "0123456789abcdef"),
    ("0123456789abcdefX", 16, 2, 0, "0123456789abcdef\nX"),
    (
        "hello world this is a test of wrapping words",
        16,
        2,
        0,
        "hello world this\nis a test of",
    ),
    ("abcdefghijklmnopqrstuvwxyz", 10, 2, 0, "abcdefghij\nklmnopqrst"),
    ("abcdefghijklmnopqrstuvwxyz", 10, 2, 10, "klmnopqrst\nuvwxyz"),
    ("abcdefghijklmnopqrstuvwxyz", 10, 2, 9, "abcdefghij\nklmnopqrst"),
    ("one\ntwo\nthree", 16, 2, 0, "one\ntwo"),
    ("   spaced", 16, 2, 0, "spaced"),
    ("word  double", 16, 2, 0, "word  double"),
    ("a " * 30, 8, 2, 0, "a a a a\na a a a"),
)

# Stand-ins (em dash, quotes, ellipsis, accents) are not ASCII input, but their
# column count is the stand-in length in both builds. They must not move.
STAND_IN_TEXT = (
    "a \u2014 b",
    "\u201cquote\u201d",
    "wait\u2026",
    "caf\u00e9 na\u00efve",
    "cost is 20\u20ac today",
)


def utf8_slices(page: str, source: str) -> None:
    """Each line is a whole-character slice of source. Newlines are the breaks."""
    raw = source.encode()
    for line in page.split("\n"):
        blob = line.encode()
        if not blob:
            continue
        start = raw.find(blob)
        if start < 0:
            raise AssertionError(f"{line!r} is not a slice of {source!r}")
        raw[:start].decode("utf-8")
        raw[: start + len(blob)].decode("utf-8")
        line.encode("utf-8").decode("utf-8")


class CjkCaptionTest(unittest.TestCase):
    legacy: Path
    wide: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.legacy = cls.compile(cc, Path(cls.tmp.name) / "legacy", cjk=False)
        cls.wide = cls.compile(cc, Path(cls.tmp.name) / "wide", cjk=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    @classmethod
    def compile(cls, cc: list[str], dest: Path, cjk: bool) -> Path:
        cmd = [
            *cc,
            "-include",
            str(ROOT / "tests" / "host_compat.h"),
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(MUSE),
            str(ROOT / "tests" / "muse_cjk_caption_harness.c"),
            str(MUSE / "muse_chat_text.c"),
            str(MUSE / "muse_text.c"),
            "-o",
            str(dest),
        ]
        if cjk:
            cmd[1:1] = ["-DCONFIG_MUSE_CJK_FONT=1", "-DMUSE_CJK_COLS_TOGGLE"]
        proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)
        return dest

    def page(self, binary: Path, wide: int, cols: int, lines: int, at: int, text: str) -> str | None:
        raw = text.encode()
        payload = struct.pack("<iiiii", wide, cols, lines, at, len(raw)) + raw
        proc = subprocess.run([str(binary), "page"], input=payload, capture_output=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr.decode())
        self.assertGreaterEqual(len(proc.stdout), 4)
        n = struct.unpack_from("<i", proc.stdout)[0]
        if n < 0:
            self.assertEqual(proc.stdout[4:], b"")
            return None
        body = proc.stdout[4:]
        self.assertEqual(len(body), n)
        return body.decode("utf-8")

    def keep(self, text: str) -> str:
        proc = subprocess.run([str(self.wide), "keep"], input=text.encode(), capture_output=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr.decode())
        return proc.stdout.decode("utf-8")

    def test_ascii_pages_match_the_old_rule(self) -> None:
        for text, cols, lines, at, expected in ASCII_PAGES:
            with self.subTest(text=text, cols=cols, at=at):
                old = self.page(self.legacy, 0, cols, lines, at, text)
                off = self.page(self.wide, 0, cols, lines, at, text)
                on = self.page(self.wide, 1, cols, lines, at, text)
                self.assertEqual(old, expected)
                self.assertEqual(off, old)
                self.assertEqual(on, old)
                if old is not None:
                    utf8_slices(old, text)

    def test_stand_ins_keep_their_columns(self) -> None:
        for text in STAND_IN_TEXT:
            for cols, lines, at in ((16, 2, 0), (8, 2, 0), (8, 2, 4)):
                with self.subTest(text=text, cols=cols, at=at):
                    old = self.page(self.legacy, 0, cols, lines, at, text)
                    on = self.page(self.wide, 1, cols, lines, at, text)
                    self.assertEqual(on, old)
                    if on is not None:
                        utf8_slices(on, text)

    def test_cjk_counts_two_columns_and_stays_whole(self) -> None:
        text = "你好世界"
        old = self.page(self.legacy, 0, 4, 2, 0, text)
        off = self.page(self.wide, 0, 4, 2, 0, text)
        on = self.page(self.wide, 1, 4, 2, 0, text)
        self.assertEqual(old, "你好世界")
        self.assertEqual(off, old)
        self.assertEqual(on, "你好\n世界")
        utf8_slices(on, text)

        self.assertEqual(self.page(self.wide, 1, 2, 1, 0, text), "你")
        self.assertEqual(self.page(self.wide, 1, 2, 1, 1, text), "你")
        self.assertEqual(self.page(self.wide, 1, 2, 1, 3, text), "好")
        for page in ("你", "好"):
            page.encode("utf-8").decode("utf-8")
            self.assertEqual(len(page.encode()), 3)

        mixed = self.page(self.wide, 1, 7, 2, 0, "hello 你")
        self.assertEqual(mixed, "hello\n你")
        self.assertEqual(self.page(self.legacy, 0, 7, 2, 0, "hello 你"), "hello 你")
        utf8_slices(mixed, "hello 你")

        # Fullwidth A is two columns, so cols=3 holds "AＡ" and leaves B for the next line.
        full = self.page(self.wide, 1, 3, 1, 0, "A\uff21B")
        self.assertEqual(full, "A\uff21")
        self.assertEqual(self.page(self.legacy, 0, 3, 1, 0, "A\uff21B"), "A\uff21B")

    def test_chinese_is_kept_and_emoji_is_dropped(self) -> None:
        self.assertEqual(self.keep("你好，世界！"), "你好，世界！")
        self.assertEqual(self.keep("你好\U0001f600"), "你好")
        self.assertEqual(self.keep("\u201chi\u201d"), '"hi"')
        self.assertEqual(self.keep("caf\u00e9"), "cafe")
        self.assertEqual(self.keep("\U0001f600\U0001f642"), "")


if __name__ == "__main__":
    unittest.main()
