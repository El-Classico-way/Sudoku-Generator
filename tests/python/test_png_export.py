"""SVG to PNG: scaling, the PNG encoder, rendering through gdk-pixbuf-csource, the folder conversion and the script."""
from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from sudoku_app import Picture, Theme, render_board_svg
from sudoku_app import png_export, raster
from sudoku_app.errors import InvalidArgumentError
from sudoku_app.png_export import convert_directory, scaled_svg, svg_to_png
from sudoku_app.raster import RasterImage, encode_png
from sudoku_app.theme_builder import _png_pixels

ROOT = Path(__file__).resolve().parents[2]
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
needs_tool = pytest.mark.skipif(shutil.which("gdk-pixbuf-csource") is None, reason="gdk-pixbuf-csource is not installed")


# ---- scaling -------------------------------------------------------------------------------------------------------

def test_scaling_changes_only_the_pixel_size():
    svg = render_board_svg(PUZZLE)
    text, w, h = scaled_svg(svg, 2)
    assert (w, h) == (940, 940) and 'width="940" height="940" viewBox="0 0 470 470"' in text
    assert text.replace('width="940" height="940"', 'width="470" height="470"', 1) == svg
    text, w, h = scaled_svg('<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="10.5px" height="4"/>', 3)
    assert (w, h) == (32, 12) and 'viewBox="0 0 10.5 4"' in text


@pytest.mark.parametrize("text, message", [("<html/>", "no <svg>"), ('<svg width="10"/>', "numeric width and height"),
                                           ('<svg width="100%" height="10"/>', "numeric width and height"),
                                           ('<svg width="9000" height="10"/>', "outside")])
def test_scaling_rejects_unusable_documents(text, message):
    with pytest.raises(InvalidArgumentError, match=message):
        scaled_svg(text, 3)


# ---- PNG encoder ---------------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("channels", [3, 4])
def test_encoded_png_decodes_to_the_same_pixels(channels):
    width, height, stride = 5, 3, 5 * channels + 3                        # padded rows must not leak into the PNG
    rows = [bytes((x * 40 + y * 7 + c * 3) % 256 for x in range(width) for c in range(channels)) for y in range(height)]
    image = RasterImage(width, height, channels, stride, b"".join(r + b"\xee" * 3 for r in rows))
    sample = _png_pixels(encode_png(image))
    expect = [tuple(r[x * channels + c] for c in range(3)) for r in rows for x in range(width)]
    assert (sample.width, sample.height) == (width, height)
    assert [tuple(round(v * 255) for v in c) for c in sample.colors] == expect


# ---- rendering ------------------------------------------------------------------------------------------------------------

def pixel(image, x, y):
    i = y * image.rowstride + x * image.channels
    return tuple(image.pixels[i:i + 3])


@needs_tool
def test_board_renders_at_the_requested_size(tmp_path):
    svg = tmp_path / "board.svg"
    svg.write_text(render_board_svg(PUZZLE, original=PUZZLE))
    for scale in (1, 3):
        image = svg_to_png(svg, tmp_path / "board.png", scale)
        assert (image.width, image.height) == (470 * scale, 470 * scale)
        assert pixel(image, 2 * scale, 2 * scale) == (255, 255, 255)        # margin: background
        assert pixel(image, 10 * scale, 100 * scale) == (0, 0, 0)          # outer border line
        assert (tmp_path / "board.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["board.png", "board.svg"]   # no temporary files left


@needs_tool
def test_picture_theme_renders_picture_and_veil(tmp_path):
    white = Picture(b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 4 4"><rect width="4" height="4" fill="#ffffff"/></svg>')
    theme = Theme("t", background="#0b0b20", grid="#c59588", box="#ff7c1e", border="#ffc837", digit="#fff6dc",
                  given="#fff6dc", filled="#ffb54b", picture=white, veil=0.72)
    svg = tmp_path / "p.svg"
    svg.write_text(render_board_svg("." * 81, theme=theme))
    image = svg_to_png(svg, tmp_path / "p.png", 1)
    expected = tuple(round(0.72 * c + 0.28 * 255) for c in (0x0b, 0x0b, 0x20))
    assert all(abs(a - b) <= 2 for a, b in zip(pixel(image, 3, 3), expected))   # veil over a white picture


# ---- folders --------------------------------------------------------------------------------------------------------------

@needs_tool
def test_folder_conversion_is_incremental_and_isolates_failures(tmp_path):
    src = tmp_path / "product"
    src.mkdir()
    for name in ("a", "b"):
        (src / f"{name}.svg").write_text(render_board_svg(PUZZLE))
    (src / "broken.svg").write_text("<svg>")
    (src / ".hidden.svg").write_text(render_board_svg(PUZZLE))
    (src / "notes.txt").write_text("x")
    results = dict(convert_directory(src, src / "png", scale=1))
    assert set(results) == {"a.svg", "b.svg", "broken.svg"}
    assert results["a.svg"] == "converted (470 x 470)" and results["broken.svg"].startswith("failed:")
    assert sorted(p.name for p in (src / "png").iterdir()) == ["a.png", "b.png"]
    assert dict(convert_directory(src, src / "png", scale=1))["a.svg"] == "up to date"
    later = time.time() + 5
    os.utime(src / "b.svg", (later, later))                                 # b changed after its PNG
    again = dict(convert_directory(src, src / "png", scale=1))
    assert again["a.svg"] == "up to date" and again["b.svg"].startswith("converted")
    assert dict(convert_directory(src, src / "png", scale=2, force=True))["a.svg"] == "converted (940 x 940)"
    assert not [p for p in src.rglob(".*") if p.name != ".hidden.svg"]     # temporary files removed


def test_missing_tool_is_reported_per_file(tmp_path, monkeypatch):
    monkeypatch.setattr(raster.shutil, "which", lambda name: None)
    (tmp_path / "a.svg").write_text(render_board_svg(PUZZLE))
    [(name, outcome)] = convert_directory(tmp_path, tmp_path / "png")
    assert "libgdk-pixbuf2.0-bin" in outcome and not list((tmp_path / "png").iterdir())


def test_bad_arguments(tmp_path):
    with pytest.raises(InvalidArgumentError, match="not a directory"):
        convert_directory(tmp_path / "missing", tmp_path / "png")
    with pytest.raises(InvalidArgumentError, match="scale"):
        svg_to_png(tmp_path / "a.svg", tmp_path / "a.png", 0)
    assert png_export.main([str(tmp_path / "missing")]) == 2


@needs_tool
def test_script_converts_product_into_product_png(tmp_path):
    product = tmp_path / "product"
    product.mkdir()
    (product / "x.svg").write_text(render_board_svg(PUZZLE))
    env = {**os.environ, "SUDOKU_OUTPUT_DIR": str(product)}
    done = subprocess.run([str(ROOT / "scripts" / "svg_to_png.sh"), "--scale", "1"], env=env, capture_output=True,
                          text=True, timeout=300)
    assert done.returncode == 0, done.stderr
    assert "x.svg: converted (470 x 470)" in done.stdout and "1 converted, 0 up to date, 0 failed" in done.stdout
    assert (product / "png" / "x.png").is_file()
    (product / "y.svg").write_text("<svg>")
    done = subprocess.run([str(ROOT / "scripts" / "svg_to_png.sh")], env=env, capture_output=True, text=True, timeout=300)
    assert done.returncode == 1 and "y.svg: failed" in done.stderr and "0 converted, 1 up to date, 1 failed" in done.stdout
