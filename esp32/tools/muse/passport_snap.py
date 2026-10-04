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

"""Rebuild one Passport frame from >snap and save a PNG.

    tools/muse/passport_snap.py <port> <out.png> [command ...]
    tools/muse/passport_snap.py --analyze <image.png> [other.png]

A one-character command is a bench key ('a' menu down, 's' menu select).
Any other command is a console line; a leading '>' is added if missing.
`wait=1.5` pauses on the host and is not sent. The script then sends `>snap`.

Pixels are packed big-endian RGB565 (high byte first), the bytes the panel
driver receives after the rounded mask and lv_draw_sw_rgb565_swap. Value 0
is black in this encoding. The panel INVON inversion is not undone, so the
PNG is not a photo of the glass. `--scale` 2 or 3 repeats each pixel.
"""

from __future__ import annotations

import base64
import re
import struct
import sys
import time
import zlib
from pathlib import Path

WIDTH = 240
HEIGHT = 320
RADIUS = 30

_PX = re.compile(
    r"^@px\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s*([A-Za-z0-9+/=]*)\s*$")
_B64_LINE = re.compile(r"^[A-Za-z0-9+/=]+\s*$")
_REDACT = ("password", "passwd", "token", "ssid", "secret")


class SnapError(Exception):
    pass


def b64_len(nbytes: int) -> int:
    return 4 * ((nbytes + 2) // 3)


def redact(text: str) -> str:
    kept = []
    for line in text.splitlines():
        low = line.lower()
        if any(word in low for word in _REDACT):
            kept.append("[redacted line]")
        else:
            kept.append(line)
    return "\n".join(kept)


def iter_rects(text: str) -> list[tuple[int, int, int, int, bytes]]:
    if "@snap off" in text:
        raise SnapError("capture is off (@snap off)")
    abort = re.search(r"^@snap abort.*$", text, re.M)
    if "@snap done" not in text:
        if abort:
            raise SnapError(abort.group(0))
        raise SnapError("no @snap done")
    body = text.split("@snap done", 1)[0]
    lines = body.splitlines()
    rects = []
    index = 0
    while index < len(lines):
        match = _PX.match(lines[index].strip("\r"))
        if not match:
            index += 1
            continue
        x1, y1, x2, y2 = (int(match.group(i)) for i in range(1, 5))
        blob = match.group(5)
        width = x2 - x1 + 1
        height = y2 - y1 + 1
        nbytes = width * height * 2
        if width <= 0 or height <= 0 or nbytes > WIDTH * HEIGHT * 2:
            raise SnapError(f"bad rect {x1} {y1} {x2} {y2}")
        need = b64_len(nbytes)
        index += 1
        while len(blob) < need and index < len(lines):
            extra = lines[index].strip("\r").strip()
            if extra.startswith("@snap") or extra.startswith("@px"):
                break
            if _B64_LINE.match(extra):
                blob += extra
            index += 1
        blob = re.sub(r"[^A-Za-z0-9+/=]", "", blob)
        if len(blob) < need:
            raise SnapError(
                f"short base64 at {x1},{y1}: {len(blob)} < {need}")
        raw = base64.b64decode(blob[:need], validate=True)
        if len(raw) != nbytes:
            raise SnapError(f"decoded {len(raw)} != {nbytes} at {x1},{y1}")
        rects.append((x1, y1, x2, y2, raw))
    if not rects:
        raise SnapError("no @px lines")
    return rects


def paint(rects: list[tuple[int, int, int, int, bytes]],
          width: int = WIDTH, height: int = HEIGHT) -> tuple[bytes, int]:
    raw = bytearray(width * height * 2)
    cover = bytearray(width * height)
    for x1, y1, x2, y2, data in rects:
        if x1 < 0 or y1 < 0 or x2 >= width or y2 >= height or x2 < x1 or y2 < y1:
            raise SnapError(f"rect outside {x1} {y1} {x2} {y2}")
        rect_w = x2 - x1 + 1
        rect_h = y2 - y1 + 1
        if len(data) != rect_w * rect_h * 2:
            raise SnapError("rect length does not match coordinates")
        for row in range(rect_h):
            dst = ((y1 + row) * width + x1) * 2
            src = row * rect_w * 2
            raw[dst:dst + rect_w * 2] = data[src:src + rect_w * 2]
            cover[(y1 + row) * width + x1:(y1 + row) * width + x1 + rect_w] = b"\x01" * rect_w
    return bytes(raw), sum(cover)


def rgb888(raw: bytes, width: int, height: int, scale: int = 1) -> tuple[bytes, int, int]:
    if scale not in (1, 2, 3):
        raise SnapError("scale must be 1, 2, or 3")
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            value = struct.unpack_from(">H", raw, (y * width + x) * 2)[0]
            pixel = bytes((
                (value >> 11) << 3,
                ((value >> 5) & 63) << 2,
                (value & 31) << 3,
            ))
            line += pixel * scale
        rows.append(bytes(line) * scale)
    return b"".join(rows), width * scale, height * scale


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path: str | Path, rgb: bytes, width: int, height: int) -> None:
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(raw))
    png += _png_chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def read_png_rgb(path: str | Path) -> tuple[int, int, bytes]:
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SnapError(f"{path} is not a PNG")
    pos = 8
    width = height = None
    idat = b""
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, bit, color, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if (bit, color, comp, filt, inter) != (8, 2, 0, 0, 0):
                raise SnapError(f"unsupported PNG {bit=} {color=} {inter=}")
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    if width is None or height is None:
        raise SnapError("PNG has no IHDR")
    inflated = zlib.decompress(idat)
    stride = width * 3
    rows = []
    index = 0
    prev = bytearray(stride)
    for _y in range(height):
        filt = inflated[index]
        index += 1
        row = bytearray(inflated[index:index + stride])
        index += stride
        if filt == 1:
            for x in range(stride):
                left = row[x - 3] if x >= 3 else 0
                row[x] = (row[x] + left) & 255
        elif filt == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 255
        elif filt == 3:
            for x in range(stride):
                left = row[x - 3] if x >= 3 else 0
                row[x] = (row[x] + ((left + prev[x]) // 2)) & 255
        elif filt == 4:
            for x in range(stride):
                left = row[x - 3] if x >= 3 else 0
                up = prev[x]
                ul = prev[x - 3] if x >= 3 else 0
                estimate = left + up - ul
                dist_l = abs(estimate - left)
                dist_u = abs(estimate - up)
                dist_ul = abs(estimate - ul)
                if dist_l <= dist_u and dist_l <= dist_ul:
                    pred = left
                elif dist_u <= dist_ul:
                    pred = up
                else:
                    pred = ul
                row[x] = (row[x] + pred) & 255
        elif filt != 0:
            raise SnapError(f"bad PNG filter {filt}")
        prev = row
        rows.append(bytes(row))
    return width, height, b"".join(rows)


def row_span(y: int, radius: int = RADIUS, width: int = WIDTH, height: int = HEIGHT) -> tuple[int, int]:
    """Visible inclusive x range. Same integer circle as board_passport.c."""
    if y < 0 or y >= height:
        raise SnapError(f"y {y} outside")
    if radius <= 0 or (radius <= y < height - radius):
        return 0, width - 1
    edge_y = radius - y if y < radius else y - (height - 1 - radius)
    inset = 0
    while (inset + 1) * (inset + 1) + edge_y * edge_y <= radius * radius:
        inset += 1
    x1 = radius - inset
    x2 = width - radius + inset - 1
    if x1 < 0:
        x1 = 0
    if x2 >= width:
        x2 = width - 1
    return x1, x2


def _pixel(rgb: bytes, width: int, x: int, y: int) -> tuple[int, int, int]:
    i = (y * width + x) * 3
    return rgb[i], rgb[i + 1], rgb[i + 2]


def corner_stats(rgb: bytes, width: int, height: int, radius: int = RADIUS) -> dict[str, dict]:
    boxes = {
        "tl": (0, radius, 0, radius),
        "tr": (width - radius, width, 0, radius),
        "bl": (0, radius, height - radius, height),
        "br": (width - radius, width, height - radius, height),
    }
    stats = {}
    for name, (x0, x1, y0, y1) in boxes.items():
        outside = 0
        nonzero = 0
        samples = []
        for y in range(y0, y1):
            visible = row_span(y, radius, width, height)
            for x in range(x0, x1):
                if visible[0] <= x <= visible[1]:
                    continue
                outside += 1
                pixel = _pixel(rgb, width, x, y)
                if pixel != (0, 0, 0):
                    nonzero += 1
                    if len(samples) < 4:
                        samples.append((x, y, pixel))
        stats[name] = {"outside": outside, "nonzero": nonzero, "samples": samples}
    return stats


def content_bbox(rgb: bytes, width: int, height: int) -> dict | None:
    min_x = min_y = 10 ** 9
    max_x = max_y = -1
    count = 0
    for y in range(height):
        for x in range(width):
            if _pixel(rgb, width, x, y) == (0, 0, 0):
                continue
            count += 1
            min_x = min(min_x, x)
            max_x = max(max_x, x)
            min_y = min(min_y, y)
            max_y = max(max_y, y)
    if count == 0:
        return None
    return {"x1": min_x, "y1": min_y, "x2": max_x, "y2": max_y, "nonzero": count}


def row_bands(rgb: bytes, width: int, height: int, min_pixels: int = 8) -> list[tuple[int, int, int]]:
    counts = []
    for y in range(height):
        counts.append(sum(_pixel(rgb, width, x, y) != (0, 0, 0) for x in range(width)))
    bands = []
    start = None
    for y, count in enumerate(counts):
        if count >= min_pixels:
            if start is None:
                start = y
        elif start is not None:
            bands.append((start, y - 1, sum(counts[start:y])))
            start = None
    if start is not None:
        bands.append((start, height - 1, sum(counts[start:])))
    return bands


def diff_bands(left: bytes, right: bytes, width: int, height: int,
               min_pixels: int = 8) -> list[tuple[int, int, int]]:
    counts = []
    for y in range(height):
        changed = 0
        for x in range(width):
            if _pixel(left, width, x, y) != _pixel(right, width, x, y):
                changed += 1
        counts.append(changed)
    bands = []
    start = None
    for y, count in enumerate(counts):
        if count >= min_pixels:
            if start is None:
                start = y
        elif start is not None:
            bands.append((start, y - 1, sum(counts[start:y])))
            start = None
    if start is not None:
        bands.append((start, height - 1, sum(counts[start:])))
    return bands


def outside_nonzero(rgb: bytes, width: int, height: int, y0: int, y1: int,
                    radius: int = RADIUS) -> int:
    count = 0
    for y in range(max(0, y0), min(height, y1 + 1)):
        visible = row_span(y, radius, width, height)
        for x in range(width):
            if visible[0] <= x <= visible[1]:
                continue
            if _pixel(rgb, width, x, y) != (0, 0, 0):
                count += 1
    return count


def format_facts(rgb: bytes, width: int, height: int) -> str:
    lines = [f"size {width}x{height}"]
    corners = corner_stats(rgb, width, height)
    for name in ("tl", "tr", "bl", "br"):
        item = corners[name]
        lines.append(
            f"corner {name} outside={item['outside']} nonzero={item['nonzero']} samples={item['samples']}")
    box = content_bbox(rgb, width, height)
    lines.append(f"content_bbox {box}")
    lines.append(f"row_bands {row_bands(rgb, width, height)}")
    return "\n".join(lines)


def _open_port(path: str):
    import serial
    # port=None leaves the handle closed so DTR/RTS can be forced low
    # before open. Some pyserial builds reject do_not_open.
    port = serial.Serial()
    port.port = path
    port.baudrate = 115200
    port.timeout = 0.05
    port.dtr = False
    port.rts = False
    port.open()
    port.dtr = False
    port.rts = False
    return port


def _read_until(port, deadline: float) -> bytes:
    buf = bytearray()
    while time.time() < deadline:
        chunk = port.read(65536)
        if chunk:
            buf += chunk
            if b"@snap done" in buf or b"@snap abort" in buf or b"@snap off" in buf:
                time.sleep(0.05)
                extra = port.read(65536)
                if extra:
                    buf += extra
                break
    return bytes(buf)


def _send_command(port, command: str) -> None:
    if len(command) == 1 and not command.startswith(">"):
        port.write(command.encode("utf-8"))
        return
    line = command if command.startswith(">") else ">" + command
    if not line.endswith("\n"):
        line += "\n"
    port.write(line.encode("utf-8"))


class Session:
    def __init__(self, path: str):
        self.ser = _open_port(path)

    def close(self) -> None:
        self.ser.close()

    def send(self, commands: list[str], settle: float) -> None:
        for command in commands:
            if command.startswith("wait="):
                time.sleep(float(command.split("=", 1)[1]))
                continue
            _send_command(self.ser, command)
            time.sleep(settle)

    def snap(self, out: str, commands: list[str], settle: float = 0.45,
             timeout: float = 30.0, scale: int = 1) -> str:
        self.ser.reset_input_buffer()
        self.send(commands, settle)
        self.ser.write(b">snap\n")
        blob = _read_until(self.ser, time.time() + timeout)
        text = blob.decode("latin1")
        try:
            rects = iter_rects(text)
            raw, covered = paint(rects)
        except SnapError:
            tail = redact(text)[-1500:]
            raise SnapError(redact(str(sys.exc_info()[1])) + "\n" + tail)
        rgb, out_w, out_h = rgb888(raw, WIDTH, HEIGHT, scale)
        # Facts are on the unscaled wire image. scale only affects the file.
        facts_rgb = rgb888(raw, WIDTH, HEIGHT, 1)[0]
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        write_png(out, rgb, out_w, out_h)
        done = ""
        for line in text.splitlines():
            if line.startswith("@snap "):
                done = line.strip("\r")
        report = (
            f"{out}: {WIDTH}x{HEIGHT} scale={scale} covered={covered}/{WIDTH * HEIGHT}\n"
            f"{done}\n"
            f"{format_facts(facts_rgb, WIDTH, HEIGHT)}"
        )
        if covered != WIDTH * HEIGHT:
            raise SnapError(report + "\ncoverage incomplete")
        corners = corner_stats(facts_rgb, WIDTH, HEIGHT)
        if any(corners[name]["nonzero"] for name in corners):
            raise SnapError(report + "\ncorner outside pixels are not all 0")
        return report


def _analyze(paths: list[str]) -> str:
    images = []
    lines = []
    for path in paths:
        width, height, rgb = read_png_rgb(path)
        images.append((path, width, height, rgb))
        lines.append(f"# {path}")
        lines.append(format_facts(rgb, width, height))
    if len(images) == 2:
        (p0, w0, h0, a), (p1, w1, h1, b) = images
        if (w0, h0) != (w1, h1):
            lines.append(f"diff skipped: {w0}x{h0} vs {w1}x{h1}")
        else:
            lines.append(f"diff_bands {p0} minus {p1}: {diff_bands(a, b, w0, h0)}")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    args = argv[1:]
    scale = 1
    settle = 0.45
    timeout = 30.0
    while args and args[0].startswith("--"):
        flag = args.pop(0)
        if flag == "--analyze":
            if not args:
                print("usage: passport_snap.py --analyze <image.png> [other.png]", file=sys.stderr)
                return 2
            print(_analyze(args))
            return 0
        if flag == "--scale":
            scale = int(args.pop(0))
        elif flag == "--settle":
            settle = float(args.pop(0))
        elif flag == "--timeout":
            timeout = float(args.pop(0))
        else:
            print(f"unknown flag {flag}", file=sys.stderr)
            return 2
    if len(args) < 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: passport_snap.py <port> <out.png> [command ...]", file=sys.stderr)
        return 2
    port, out, *commands = args
    session = Session(port)
    try:
        print(session.snap(out, commands, settle, timeout, scale))
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except SnapError as exc:
        print(redact(str(exc)), file=sys.stderr)
        raise SystemExit(1)
