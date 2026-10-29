"""Raster images without third-party Python packages: decoding through ``gdk-pixbuf-csource`` and PNG encoding.

``gdk-pixbuf-csource`` (Debian/Ubuntu package ``libgdk-pixbuf2.0-bin``) loads any format GdkPixbuf has a loader
for - JPEG, PNG and, through librsvg, SVG - and prints the pixels as C string literals. :func:`load_image` runs it
and :func:`parse_csource` turns that output back into bytes; :func:`encode_png` writes a PNG with :mod:`zlib`.
Used by :mod:`sudoku_app.theme_builder` (picture colours) and :mod:`sudoku_app.png_export` (SVG to PNG).
"""
from __future__ import annotations

import codecs
import os
import re
import shutil
import struct
import subprocess
import zlib
from dataclasses import dataclass

from .errors import InvalidArgumentError

__all__ = ["RasterImage", "CSOURCE_TOOL", "INSTALL_HINT", "load_image", "parse_csource", "encode_png"]

CSOURCE_TOOL = "gdk-pixbuf-csource"
INSTALL_HINT = "Ubuntu: sudo apt install libgdk-pixbuf2.0-bin"


@dataclass(frozen=True)
class RasterImage:
    """8-bit RGB (``channels`` 3) or RGBA (4) pixels, row ``y`` at ``pixels[y * rowstride:][:width * channels]``."""

    width: int
    height: int
    channels: int
    rowstride: int
    pixels: bytes

    def rows(self):
        span = self.width * self.channels
        for y in range(self.height):
            start = y * self.rowstride
            yield self.pixels[start:start + span]


def parse_csource(source: str) -> RasterImage:
    """The image in ``gdk-pixbuf-csource --raw`` output: C string literals holding a GdkPixdata block."""
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', source)
    blob = b"".join(codecs.escape_decode(lit.encode("latin-1"))[0] for lit in literals)  # type: ignore[attr-defined]
    if len(blob) < 24 or blob[:4] != b"GdkP":
        raise InvalidArgumentError(f"unexpected {CSOURCE_TOOL} output (no GdkPixdata header)")
    _, _, kind, rowstride, width, height = struct.unpack(">4sIIIII", blob[:24])
    channels = {1: 3, 2: 4}.get(kind & 0xFF)
    if channels is None or kind & 0x0F000000 != 0x01000000 or kind & 0x00FF0000 != 0x00010000:  # RGB(A), 8 bit, raw
        raise InvalidArgumentError(f"unexpected {CSOURCE_TOOL} pixel format 0x{kind:08x}")
    pixels = blob[24:]
    if width == 0 or height == 0 or len(pixels) < rowstride * (height - 1) + width * channels:
        raise InvalidArgumentError(f"truncated {CSOURCE_TOOL} output")
    return RasterImage(width, height, channels, rowstride, pixels)


def load_image(path: str | os.PathLike[str], purpose: str) -> RasterImage:
    """Decode a picture with the external tool; ``purpose`` completes the error message if the tool is missing."""
    tool = shutil.which(CSOURCE_TOOL)
    if tool is None:
        raise InvalidArgumentError(f"{purpose} needs the {CSOURCE_TOOL} tool ({INSTALL_HINT})")
    done = subprocess.run([tool, "--raw", "--name=picture", os.fspath(path)], capture_output=True, timeout=300)
    if done.returncode != 0:
        message = done.stderr.decode(errors="replace").strip() or f"exit code {done.returncode}"
        raise InvalidArgumentError(f"{CSOURCE_TOOL} could not read {os.fspath(path)!r}: {message}")
    return parse_csource(done.stdout.decode("latin-1"))


def encode_png(image: RasterImage, level: int = 9) -> bytes:
    """A PNG (8 bit RGB or RGBA, non-interlaced, filter type None) of ``image``."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    color_type = {3: 2, 4: 6}[image.channels]
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, color_type, 0, 0, 0)
    raw = b"".join(b"\0" + row for row in image.rows())
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, level))
            + chunk(b"IEND", b""))
