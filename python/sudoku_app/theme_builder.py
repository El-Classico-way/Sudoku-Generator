"""Add a picture theme to a theme pack: read the picture's colours, derive a readable palette, update themes.json.

Used by ``scripts/add_theme.sh`` (``python -m sudoku_app.theme_builder PICTURE``). Nothing here touches the engine.

Colours are read without third-party Python packages:

* SVG - the ``#rrggbb``/``#rgb`` colours written in the file, weighted by how often they occur;
* PNG - decoded here (non-interlaced, 8 or 16 bits per sample, every colour type);
* JPEG (and interlaced PNG) - through the ``gdk-pixbuf-csource`` tool (Debian/Ubuntu package
  ``libgdk-pixbuf2.0-bin``) when it is installed; otherwise use ``--like THEME`` to start from an existing theme.

Palette: the strongest hue of the colourful pixels becomes the main accent, a second clearly different hue (if
any) the second accent, and the hue of the dark pixels tints the veil. Each colour is then lightened only as much
as :func:`~sudoku_app.svg.worst_case_contrast` requires (digits 4.5:1, lines 3:1 over any picture), so the result
always passes the picture-theme check. The suggestion is a starting point: edit the colours in themes.json freely.
"""
from __future__ import annotations

import argparse
import colorsys
import json
import math
import os
import re
import shutil
import struct
import sys
import tempfile
import unicodedata
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .errors import ConfigurationError, InvalidArgumentError
from . import raster
from .protocol import loads_strict
from .raster import load_image
from .svg import DEFAULT_VEIL, MAX_PICTURE_BYTES, PICTURE_FOCUS, THEMES, Picture, Theme, worst_case_contrast
from .themes import THEME_PACK_SCHEMA, load_theme_pack

__all__ = ["PixelSample", "read_pixels", "suggest_colors", "theme_name_for", "add_theme", "main"]

RGB = tuple[float, float, float]
_MAX_SAMPLES = 40_000
_NEEDS = {"given": 4.5, "filled": 4.5, "grid": 3.0, "box": 3.0, "border": 3.0}
_THEME_NAME = re.compile(r"[a-z][a-z0-9-]{0,31}")


@dataclass(frozen=True)
class PixelSample:
    """Colours of a picture: ``colors[i]`` (0-1 RGB) stands for ``weights[i]`` pixels; size in pixels if known."""

    colors: tuple[RGB, ...]
    weights: tuple[float, ...]
    width: int | None = None
    height: int | None = None


# ------------------------------------------------------------------------------------------------ reading pixels

_SVG_COLOR = re.compile(r"(stop-color|fill|stroke|color)\s*[=:]\s*[\"']?\s*#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b")


def _svg_pixels(data: bytes) -> PixelSample:
    """Colours by number of uses; colours of gradient stops and filled rectangles (backgrounds: few uses, large
    area) together weigh as much as everything else."""
    text = data.decode("utf-8", errors="replace")
    plain: dict[str, float] = {}
    area: dict[str, float] = {}
    for match in _SVG_COLOR.finditer(text):
        kind, hex_color = match.group(1), match.group(2).lower()
        if len(hex_color) == 3:
            hex_color = "".join(c * 2 for c in hex_color)
        tag_start = text.rfind("<", 0, match.start())
        is_area = kind == "stop-color" or (kind == "fill" and text.startswith("<rect", tag_start))
        bucket = area if is_area else plain
        bucket[hex_color] = bucket.get(hex_color, 0) + 1
    total_plain, total_area = sum(plain.values()), sum(area.values())
    counts = dict(plain)
    for hex_color, n in area.items():
        counts[hex_color] = counts.get(hex_color, 0) + (n / total_area) * max(total_plain, 1)
    if not counts:
        raise InvalidArgumentError("the SVG picture contains no #rrggbb colours to derive a palette from")
    colors = tuple(tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)) for h in counts)
    width = height = None
    box = re.search(r"viewBox\s*=\s*[\"']\s*[-\d.]+[\s,]+[-\d.]+[\s,]+([\d.]+)[\s,]+([\d.]+)", text)
    if box:
        width, height = round(float(box.group(1))), round(float(box.group(2)))
    return PixelSample(colors, tuple(counts.values()), width, height)  # type: ignore[arg-type]


def _sample(rows: Iterable[bytes], width: int, height: int, channels: int, step_bytes: int,
            palette: Sequence[RGB] | None = None) -> PixelSample:
    """Every k-th pixel of every k-th row (k chosen for about _MAX_SAMPLES samples); 8-bit samples."""
    k = max(1, math.ceil(math.sqrt(width * height / _MAX_SAMPLES)))
    colors: list[RGB] = []
    for y, row in enumerate(rows):
        if y % k:
            continue
        for x in range(0, width, k):
            px = row[x * step_bytes: x * step_bytes + step_bytes]
            if palette is not None:
                colors.append(palette[px[0]])
            elif channels < 3:
                colors.append((px[0] / 255,) * 3)                       # grey (+ alpha)
            else:
                colors.append((px[0] / 255, px[1] / 255, px[2] / 255))
    return PixelSample(tuple(colors), (1.0,) * len(colors), width, height)


def _png_pixels(data: bytes) -> PixelSample | None:
    """Decode a PNG; None for the variants not handled here (interlaced, < 8 bits per sample)."""
    pos, idat, palette, header = 8, [], None, None
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8: pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(v / 255 for v in body[i:i + 3]) for i in range(0, len(body) - 2, 3)]
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + length
    if header is None:
        raise InvalidArgumentError("the PNG picture has no IHDR chunk")
    width, height, depth, color_type, _, _, interlace = header
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None or interlace or depth not in (8, 16) or (color_type == 3 and depth != 8):
        return None
    bpp = channels * depth // 8
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat))
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1: start + 1 + stride])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[::2]) if depth == 16 else bytes(line))   # 16 bit: keep the high byte
        prev = line
    return _sample(rows, width, height, channels, channels, palette if color_type == 3 else None)


def _csource_pixels(path: Path) -> PixelSample:
    if shutil.which(raster.CSOURCE_TOOL) is None:
        raise InvalidArgumentError(
            f"cannot read the colours of {path.name}: JPEG (and interlaced PNG) needs the {raster.CSOURCE_TOOL} tool "
            f"({raster.INSTALL_HINT}), or pass --like THEME to start from an existing theme")
    image = load_image(path, f"reading {path.name}")
    return _sample(image.rows(), image.width, image.height, image.channels, image.channels)


def parse_csource(source: str) -> PixelSample:
    """Colour sample of ``gdk-pixbuf-csource --raw`` output (see :func:`sudoku_app.raster.parse_csource`)."""
    image = raster.parse_csource(source)
    return _sample(image.rows(), image.width, image.height, image.channels, image.channels)


def read_pixels(path: str | os.PathLike[str]) -> PixelSample:
    """Colour sample of a PNG, JPEG or SVG picture (see the module documentation for what is supported)."""
    path = Path(path)
    data = path.read_bytes()
    media_type = Picture(data).media_type                                  # validates type and size
    if media_type == "image/svg+xml":
        return _svg_pixels(data)
    if media_type == "image/png":
        sample = _png_pixels(data)
        if sample is not None:
            return sample
    return _csource_pixels(path)


# ------------------------------------------------------------------------------------------------- the palette

def _hex(rgb: Sequence[float]) -> str:
    return "#" + "".join(f"{round(max(0.0, min(1.0, v)) * 255):02x}" for v in rgb)


def _rgb(hex_color: str) -> RGB:
    return tuple(int(hex_color[i:i + 2], 16) / 255 for i in (1, 3, 5))  # type: ignore[return-value]


def _circular_mean(hues: Iterable[float], weights: Iterable[float]) -> float | None:
    x = y = 0.0
    for h, w in zip(hues, weights):
        x += w * math.cos(2 * math.pi * h)
        y += w * math.sin(2 * math.pi * h)
    if abs(x) < 1e-12 and abs(y) < 1e-12:
        return None
    return (math.atan2(y, x) / (2 * math.pi)) % 1.0


def _hue_distance(a: float, b: float) -> float:
    d = abs(a - b) % 1.0
    return min(d, 1 - d)


class _HueHistogram:
    """Weighted hues in 15-degree bins; peaks are refined to the weighted mean hue of their neighbourhood."""

    BINS = 24

    def __init__(self) -> None:
        self.weight = [0.0] * self.BINS
        self.members: list[list[tuple[float, float]]] = [[] for _ in range(self.BINS)]

    def add(self, hue: float, weight: float) -> None:
        i = int(hue * self.BINS) % self.BINS
        self.weight[i] += weight
        self.members[i].append((hue, weight))

    def peaks(self, count: int, *, apart: float = 45 / 360, share: float = 0.15) -> list[float]:
        """Up to ``count`` hues: the strongest, then the strongest at least ``apart`` from those already taken and
        with at least ``share`` of the first's weight. Empty if nothing was added."""
        b = self.BINS
        smooth = [self.weight[i - 1] * 0.5 + self.weight[i] + self.weight[(i + 1) % b] * 0.5 for i in range(b)]
        if max(smooth) <= 0:
            return []
        taken: list[int] = []
        for _ in range(count):
            options = [i for i in range(b) if smooth[i] > 0 and smooth[i] >= share * (smooth[taken[0]] if taken else 0)
                       and all(_hue_distance(i / b, t / b) >= apart for t in taken)]
            if not options:
                break
            taken.append(max(options, key=lambda i: smooth[i]))
        hues = []
        for i in taken:
            near = self.members[i - 1] + self.members[i] + self.members[(i + 1) % b]
            mean = _circular_mean([h for h, _ in near], [w for _, w in near])
            hues.append(i / b if mean is None else mean)
        return hues


def _accent_hues(sample: PixelSample) -> tuple[float, float] | None:
    """The strongest hue and a second one at least 45 degrees away (or the first again); None for a grey picture."""
    histogram = _HueHistogram()
    for (r, g, b), n in zip(sample.colors, sample.weights):
        h, s, v = colorsys.rgb_to_hsv(r, g, b)
        if s >= 0.3 and v >= 0.3:
            histogram.add(h, n * s * v)
    hues = histogram.peaks(2)
    if not hues:
        return None
    return hues[0], hues[-1]


def _veil_color(sample: PixelSample, fallback_hue: float) -> str:
    """A very dark colour with the main hue of the picture's dark areas (navy for a night sky, brown for candlelight)."""
    histogram, sats = _HueHistogram(), []
    for (r, g, b), n in zip(sample.colors, sample.weights):
        h, s, v = colorsys.rgb_to_hsv(r, g, b)
        if v < 0.4:
            histogram.add(h, n * s)
            sats.append((s, n))
    peaks = histogram.peaks(1)
    hue = peaks[0] if peaks else None
    total = sum(n for _, n in sats)
    saturation = sum(s * n for s, n in sats) / total if total else 0.3
    return _hex(colorsys.hsv_to_rgb(fallback_hue if hue is None else hue, min(0.65, max(0.15, saturation)), 0.125))


def _readable(color: str, background: str, veil: float, needed: float) -> str:
    """``color`` mixed with white (or black) just enough to pass; the hue stays."""
    for toward in ("#ffffff", "#000000"):
        target = _rgb(toward)
        for step in range(101):
            f = step / 100
            mixed = _hex([(1 - f) * c + f * t for c, t in zip(_rgb(color), target)])
            if worst_case_contrast(mixed, background, veil) >= needed + 0.02:
                return mixed
    raise InvalidArgumentError(f"at veil {veil:g} no colour is readable enough over every picture "
                               f"(veil colour {background}); use a stronger veil, for example --veil 0.72")


def suggest_colors(sample: PixelSample, veil: float = DEFAULT_VEIL) -> dict[str, str]:
    """A palette (``background``, ``grid``, ``box``, ``border``, ``given``, ``filled``) that passes the contrast check."""
    hues = _accent_hues(sample)
    if hues is None:                       # a grey picture: neutral lines and clues, one warm accent for written digits
        mean = _circular_mean([colorsys.rgb_to_hsv(*c)[0] for c in sample.colors], sample.weights) or 0.6
        h1, h2, strength = mean, 40 / 360, 0.12
    else:
        (h1, h2), strength = hues, 1.0
    background = _veil_color(sample, h1)

    def hls(h: float, light: float, sat: float) -> str:
        return _hex(colorsys.hls_to_rgb(h, light, sat))

    raw = {"given": hls(h1, 0.94, 0.7 * strength), "filled": hls(h2, 0.68, 0.9),
           "box": hls(h1, 0.62, 0.75 * strength), "border": hls(h2, 0.76, 0.85 * max(strength, 0.4)),
           "grid": hls(h1, 0.58, 0.18 * strength)}
    colors = {"background": background}
    for key in ("grid", "box", "border", "given", "filled"):
        colors[key] = _readable(raw[key], background, veil, _NEEDS[key])
    return colors


# ------------------------------------------------------------------------------------------------- the manifest

def theme_name_for(stem: str) -> str:
    """A valid theme name from a file name: lower case, other characters become '-', starts with a letter."""
    ascii_stem = unicodedata.normalize("NFKD", stem).encode("ascii", "ignore").decode("ascii")   # Úžasný -> Uzasny
    name = re.sub(r"[^a-z0-9]+", "-", ascii_stem.lower()).strip("-")
    if not name or not name[0].isalpha():
        name = "picture-" + name if name else "picture"
    return name[:32].rstrip("-")


def _read_manifest(path: Path) -> dict:
    if not path.exists():
        return {"schema": THEME_PACK_SCHEMA, "themes": []}
    load_theme_pack(path)                                    # refuse to extend a pack that is already broken
    return loads_strict(path.read_text(encoding="utf-8"))


def _dump_manifest(document: dict) -> str:
    """One theme per line, the layout of the hand-written packs."""
    entries = ",\n".join("  " + json.dumps(entry) for entry in document["themes"])
    return f'{{"schema": {json.dumps(document["schema"])},\n "themes": [\n{entries}\n ]}}\n'


def add_theme(picture: str | os.PathLike[str], manifest: str | os.PathLike[str], *, name: str | None = None,
              focus: str | None = None, veil: float = DEFAULT_VEIL, like: str | None = None,
              replace: bool = False) -> dict:
    """Add (or with ``replace`` update) the entry for ``picture``; returns the entry written.

    ``picture`` is a file name inside ``<pack directory>/originals/`` or a path to any file, which is then copied
    there (an existing different file of that name is never overwritten). The manifest is written atomically and
    only after the complete pack, new entry included, loads without error.
    """
    manifest = Path(manifest)
    originals = manifest.parent / "originals"
    source = Path(picture)
    if not source.is_file() and (originals / source.name).is_file() and source.name == str(picture):
        source = originals / source.name
    if not source.is_file():
        raise InvalidArgumentError(f"picture {str(picture)!r} not found (looked in {str(originals)!r} as well)")
    target = originals / source.name
    if source.resolve() != target.resolve():
        if target.exists() and target.read_bytes() != source.read_bytes():
            raise InvalidArgumentError(f"{str(target)!r} already exists and is a different file; rename the picture")
        if source.stat().st_size > MAX_PICTURE_BYTES:
            raise InvalidArgumentError(f"picture is larger than {MAX_PICTURE_BYTES} bytes")
        Picture(source.read_bytes())                          # check the type before copying anything
        originals.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            shutil.copyfile(source, target)

    document = _read_manifest(manifest)
    name = name or theme_name_for(target.stem)
    if not _THEME_NAME.fullmatch(name):
        raise InvalidArgumentError(f"theme name must match {_THEME_NAME.pattern!r}, got {name!r}")
    if name in THEMES:
        raise InvalidArgumentError(f"{name!r} is a built-in theme name; choose another with --name")
    existing = [i for i, entry in enumerate(document["themes"]) if isinstance(entry, dict) and entry.get("name") == name]
    if existing and not replace:
        raise InvalidArgumentError(f"theme {name!r} already exists; use --replace to update it or --name for another name")

    if like is not None:
        pack = load_theme_pack(manifest) if manifest.exists() else {}
        base = THEMES.get(like) or pack.get(like)
        if base is None:
            raise InvalidArgumentError(f"--like: unknown theme {like!r}")
        colors = {k: getattr(base, k) for k in ("background", "grid", "box", "border", "given", "filled")}
        colors = {"background": colors["background"],
                  **{k: _readable(colors[k], colors["background"], veil, _NEEDS[k]) for k in _NEEDS}}
        sample = None
    else:
        sample = read_pixels(target)
        colors = suggest_colors(sample, veil)
    if focus is None:
        portrait = sample is not None and sample.width and sample.height and sample.height > 1.1 * sample.width
        focus = "top" if portrait else "center"
    if focus not in PICTURE_FOCUS:
        raise InvalidArgumentError(f"focus must be one of {', '.join(PICTURE_FOCUS)}, got {focus!r}")

    entry = {"name": name, "picture": f"originals/{target.name}"}
    if focus != "center":
        entry["focus"] = focus
    entry["veil"] = veil
    entry["colors"] = {k: colors[k] for k in ("background", "grid", "box", "border", "given", "filled")}
    Theme(name, digit=colors["given"], picture=Picture.from_file(target, focus), veil=veil, **entry["colors"])
    if existing:
        document["themes"][existing[0]] = entry
    else:
        document["themes"].append(entry)

    manifest.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".themes-", suffix=".json", dir=manifest.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(_dump_manifest(document))
        load_theme_pack(temporary)                            # the whole pack, as the application will read it
        os.replace(temporary, manifest)
    except BaseException:
        os.unlink(temporary)
        raise
    return entry


# ------------------------------------------------------------------------------------------------- command line

def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="add_theme.sh", description="Add a picture theme to themes.json with a palette derived from the picture.")
    parser.add_argument("picture", help="file name in themePictures/originals/, or a path (the file is copied there)")
    parser.add_argument("--manifest", required=True, help="themes.json to update (created if missing)")
    parser.add_argument("--name", help="theme name (default: from the file name)")
    parser.add_argument("--focus", choices=list(PICTURE_FOCUS), help="part kept when cropping (default: top for portraits, else center)")
    parser.add_argument("--veil", type=float, default=DEFAULT_VEIL, help=f"veil opacity 0-1 (default {DEFAULT_VEIL})")
    parser.add_argument("--like", metavar="THEME", help="start from the colours of this theme instead of the picture's")
    parser.add_argument("--replace", action="store_true", help="update the theme if it already exists")
    args = parser.parse_args(argv)
    if not 0 <= args.veil <= 1:
        parser.error("--veil must be between 0 and 1")
    try:
        entry = add_theme(args.picture, args.manifest, name=args.name, focus=args.focus, veil=args.veil,
                          like=args.like, replace=args.replace)
    except (InvalidArgumentError, ConfigurationError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"saved theme {entry['name']} ({entry['picture']}, "
          f"focus {entry.get('focus', 'center')}, veil {entry['veil']:g})")
    for key, value in entry["colors"].items():
        print(f"  {key:10} {value}")
    print(entry["name"])                                       # last line: the name, for scripts
    return 0


if __name__ == "__main__":
    sys.exit(main())
