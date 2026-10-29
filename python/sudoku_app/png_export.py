"""Convert SVG files to PNG (used by ``scripts/svg_to_png.sh``: ``python -m sudoku_app.png_export``).

Rendering is done by librsvg through ``gdk-pixbuf-csource`` (see :mod:`sudoku_app.raster`); the PNG is written here.
``scale`` multiplies the SVG's ``width``/``height`` (the ``viewBox`` keeps the drawing the same, only sharper):
a default 470-unit board becomes 940 x 940 pixels at the default scale 2.

A PNG is (re)written when it is missing, older than its SVG, or ``force`` is set; otherwise it is reported as up to
date. Each PNG is written to a temporary file and renamed, so an interrupted run never leaves a partial image. A
file that cannot be converted is reported and the others are still converted.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Sequence

from .errors import InvalidArgumentError
from .raster import RasterImage, encode_png, load_image

__all__ = ["MIN_SCALE", "MAX_SCALE", "scaled_svg", "svg_to_png", "convert_directory", "main"]

MIN_SCALE, MAX_SCALE = 1, 8
_ROOT_TAG = re.compile(r"<svg\b[^>]*>", re.S)
_LENGTH = r'="([0-9]+(?:\.[0-9]+)?)(?:px)?"'


def scaled_svg(text: str, scale: int) -> tuple[str, int, int]:
    """``text`` with the root ``width``/``height`` multiplied by ``scale`` (a ``viewBox`` is added if missing);
    returns the new text and the pixel size. Needs numeric width and height on the root element."""
    root = _ROOT_TAG.search(text)
    if root is None:
        raise InvalidArgumentError("not an SVG document (no <svg> element)")
    tag = root.group(0)
    width, height = re.search(r"\swidth" + _LENGTH, tag), re.search(r"\sheight" + _LENGTH, tag)
    if width is None or height is None:
        raise InvalidArgumentError("the <svg> element needs numeric width and height attributes")
    w, h = float(width.group(1)), float(height.group(1))
    pixels_w, pixels_h = round(w * scale), round(h * scale)
    if not 0 < pixels_w <= 20_000 or not 0 < pixels_h <= 20_000:
        raise InvalidArgumentError(f"image size {pixels_w} x {pixels_h} is outside 1..20000 pixels")
    new = re.sub(r"(\swidth=)\"[^\"]*\"", rf'\g<1>"{pixels_w}"', tag, count=1)
    new = re.sub(r"(\sheight=)\"[^\"]*\"", rf'\g<1>"{pixels_h}"', new, count=1)
    if "viewBox" not in new:
        new = new.replace("<svg", f'<svg viewBox="0 0 {w:g} {h:g}"', 1)
    return text[:root.start()] + new + text[root.end():], pixels_w, pixels_h


def svg_to_png(svg: str | os.PathLike[str], png: str | os.PathLike[str], scale: int = 2) -> RasterImage:
    """Render ``svg`` at ``scale`` and write ``png`` (replaced if it exists). Raises InvalidArgumentError / OSError."""
    if isinstance(scale, bool) or not isinstance(scale, int) or not MIN_SCALE <= scale <= MAX_SCALE:
        raise InvalidArgumentError(f"scale must be an integer from {MIN_SCALE} to {MAX_SCALE}, got {scale!r}")
    svg, png = Path(svg), Path(png)
    text, width, height = scaled_svg(svg.read_text(encoding="utf-8"), scale)
    # next to the original, so relative references inside the SVG still resolve
    fd, scaled = tempfile.mkstemp(prefix=f".{svg.stem}-", suffix=".svg", dir=svg.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
        image = load_image(scaled, "converting SVG to PNG")
    finally:
        os.unlink(scaled)
    if (image.width, image.height) != (width, height):
        raise InvalidArgumentError(f"the renderer produced {image.width} x {image.height} pixels instead of {width} x {height}")
    fd, partial = tempfile.mkstemp(prefix=f".{png.stem}-", suffix=".png", dir=png.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(encode_png(image))
        os.replace(partial, png)
    except BaseException:
        os.unlink(partial)
        raise
    return image


def convert_directory(source: str | os.PathLike[str], destination: str | os.PathLike[str], *, scale: int = 2,
                      force: bool = False) -> list[tuple[str, str]]:
    """Convert every ``*.svg`` directly in ``source`` to ``destination/<name>.png`` (directory created if needed).

    Returns ``(svg file name, outcome)`` in name order; outcome is ``converted``, ``up to date`` or ``failed: ...``.
    """
    source, destination = Path(source), Path(destination)
    if not source.is_dir():
        raise InvalidArgumentError(f"{str(source)!r} is not a directory")
    destination.mkdir(parents=True, exist_ok=True)
    results = []
    for svg in sorted(p for p in source.glob("*.svg") if p.is_file() and not p.name.startswith(".")):
        png = destination / (svg.stem + ".png")
        if not force and png.exists() and png.stat().st_mtime >= svg.stat().st_mtime:
            results.append((svg.name, "up to date"))
            continue
        try:
            image = svg_to_png(svg, png, scale)
            results.append((svg.name, f"converted ({image.width} x {image.height})"))
        except (InvalidArgumentError, OSError, UnicodeDecodeError) as exc:
            results.append((svg.name, f"failed: {exc}"))
    return results


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="svg_to_png.sh", description="Convert every SVG in a folder to PNG.")
    parser.add_argument("source", help="folder with the SVG files")
    parser.add_argument("--output-dir", help="folder for the PNG files (default: SOURCE/png)")
    parser.add_argument("--scale", type=int, default=2, help=f"pixels per SVG unit, {MIN_SCALE}-{MAX_SCALE} (default 2)")
    parser.add_argument("--force", action="store_true", help="convert again even when the PNG is up to date")
    args = parser.parse_args(argv)
    if not MIN_SCALE <= args.scale <= MAX_SCALE:
        parser.error(f"--scale must be from {MIN_SCALE} to {MAX_SCALE}")
    destination = Path(args.output_dir) if args.output_dir else Path(args.source) / "png"
    try:
        results = convert_directory(args.source, destination, scale=args.scale, force=args.force)
    except (InvalidArgumentError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    for name, outcome in results:
        print(f"{name}: {outcome}", file=sys.stderr if outcome.startswith("failed") else sys.stdout)
    converted = sum(o.startswith("converted") for _, o in results)
    failed = sum(o.startswith("failed") for _, o in results)
    print(f"{converted} converted, {len(results) - converted - failed} up to date, {failed} failed -> {destination}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
