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

"""Host-side checks for the >snap decoder. No serial port."""

from __future__ import annotations

import base64
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "muse"))

import passport_snap as snap  # noqa: E402


class PassportSnapDecodeTests(unittest.TestCase):
    def test_row_span_matches_the_board_circle(self) -> None:
        self.assertEqual(snap.row_span(0), (30, 209))
        self.assertEqual(snap.row_span(29), (1, 238))
        self.assertEqual(snap.row_span(30), (0, 239))
        self.assertEqual(snap.row_span(160), (0, 239))
        self.assertEqual(snap.row_span(319), snap.row_span(0))
        self.assertEqual(snap.row_span(290), snap.row_span(29))

    def test_split_base64_ignores_a_log_line_and_decodes_big_endian(self) -> None:
        raw = b"\x00\x00\xf8\x00"
        text_b64 = base64.b64encode(raw).decode("ascii")
        self.assertEqual(len(text_b64), snap.b64_len(len(raw)))
        blob = (
            f"@snap armed free=1 min=1\n"
            f"@px 0 0 1 0 {text_b64[:4]}\n"
            "I (123) board: redraw\n"
            f"{text_b64[4:]}\n"
            "@snap done flushes=1 bytes=4 free=1 min=1 min0=1\n"
        )
        rects = snap.iter_rects(blob)
        self.assertEqual(rects, [(0, 0, 1, 0, raw)])
        pixels, covered = snap.paint(rects)
        self.assertEqual(covered, 2)
        rgb, width, height = snap.rgb888(pixels, snap.WIDTH, snap.HEIGHT, 1)
        self.assertEqual((width, height), (240, 320))
        self.assertEqual(rgb[0:3], b"\x00\x00\x00")
        self.assertEqual(rgb[3:6], bytes((0xF8, 0x00, 0x00)))

    def test_zero_frame_has_no_content_and_black_corners(self) -> None:
        raw = b"\x00\x00" * (snap.WIDTH * snap.HEIGHT)
        rgb = snap.rgb888(raw, snap.WIDTH, snap.HEIGHT, 1)[0]
        self.assertIsNone(snap.content_bbox(rgb, snap.WIDTH, snap.HEIGHT))
        corners = snap.corner_stats(rgb, snap.WIDTH, snap.HEIGHT)
        for name in ("tl", "tr", "bl", "br"):
            self.assertGreater(corners[name]["outside"], 0)
            self.assertEqual(corners[name]["nonzero"], 0)
        self.assertEqual(snap.outside_nonzero(rgb, snap.WIDTH, snap.HEIGHT, 0, 319), 0)

    def test_corner_pixel_outside_the_radius_is_counted(self) -> None:
        raw = bytearray(b"\x00\x00" * (snap.WIDTH * snap.HEIGHT))
        raw[0:2] = b"\xf8\x00"
        rgb = snap.rgb888(bytes(raw), snap.WIDTH, snap.HEIGHT, 1)[0]
        corners = snap.corner_stats(rgb, snap.WIDTH, snap.HEIGHT)
        self.assertEqual(corners["tl"]["nonzero"], 1)
        self.assertEqual(corners["tl"]["samples"][0][0:2], (0, 0))
        box = snap.content_bbox(rgb, snap.WIDTH, snap.HEIGHT)
        self.assertEqual(box["x1"], 0)
        self.assertEqual(box["y1"], 0)
        self.assertEqual(box["nonzero"], 1)

    def test_png_roundtrip_and_redact(self) -> None:
        raw = bytearray(b"\x00\x00" * (snap.WIDTH * 2))
        raw[2:4] = b"\x07\xe0"
        rgb, width, height = snap.rgb888(bytes(raw), snap.WIDTH, 2, 1)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "round.png"
            snap.write_png(path, rgb, width, height)
            got_w, got_h, got = snap.read_png_rgb(path)
        self.assertEqual((got_w, got_h), (width, height))
        self.assertEqual(got, rgb)
        red = snap.redact("keep\nssid=hidden\n>snap\n")
        self.assertIn("[redacted line]", red)
        self.assertIn(">snap", red)
        self.assertNotIn("ssid=hidden", red)

    def test_abort_and_off_are_errors(self) -> None:
        with self.assertRaises(snap.SnapError):
            snap.iter_rects("@snap off\n")
        with self.assertRaises(snap.SnapError):
            snap.iter_rects("@snap abort heap free=1 min=1 floor=12288\n")


if __name__ == "__main__":
    unittest.main()
