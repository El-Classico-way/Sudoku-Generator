"""theme_builder / scripts/add_theme.sh: reading picture colours, the suggested palette, updating themes.json."""
from __future__ import annotations

import json
import os
import random
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

from sudoku_app import Picture, Theme, load_theme_pack, theme_builder
from sudoku_app.errors import ConfigurationError, InvalidArgumentError
from sudoku_app.svg import worst_case_contrast
from sudoku_app.theme_builder import PixelSample, add_theme, parse_csource, read_pixels, suggest_colors, theme_name_for

ROOT = Path(__file__).resolve().parents[2]
NAVY_SVG = (b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 50"><defs><radialGradient id="bg">'
            b'<stop offset="0" stop-color="#16163a"/><stop offset="1" stop-color="#03030a"/></radialGradient></defs>'
            b'<rect width="100%" height="100%" fill="url(#bg)"/>'
            + b"".join(b'<polyline stroke="#%s" points="0,0 1,1"/>' % c for c in [b"4a0000"] * 50 + [b"ff7c1e"] * 30)
            + b"</svg>")


# ---- a small PNG encoder, so the decoder is checked against known pixels -------------------------------------------------

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def make_png(rows, color_type=2, depth=8, palette=None, interlace=0):
    """rows: lists of sample tuples. Row y uses filter y % 5, so every filter type is exercised."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    bpp = channels * depth // 8
    raw, prev = b"", bytes(len(rows[0]) * bpp)
    for y, row in enumerate(rows):
        line = b"".join(struct.pack(">" + ("B" if depth == 8 else "H") * channels, *px) for px in row)
        kind = y % 5
        out = bytearray()
        for i, x in enumerate(line):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) // 2, _paeth(a, b, c)][kind]
            out.append((x - pred) & 255)
        raw += bytes([kind]) + bytes(out)
        prev = line

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    header = struct.pack(">IIBBBBB", len(rows[0]), len(rows), depth, color_type, 0, 0, interlace)
    plte = chunk(b"PLTE", b"".join(bytes(c) for c in palette)) if palette else b""
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + plte + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def pixels(w, h, seed=1):
    rng = random.Random(seed)
    return [[tuple(rng.randrange(256) for _ in range(3)) for _ in range(w)] for _ in range(h)]


def as_rgb(sample):
    return [tuple(round(v * 255) for v in c) for c in sample.colors]


@pytest.mark.parametrize("color_type", [2, 6, 0, 4, 3])
def test_png_decoder_reproduces_every_pixel(tmp_path, color_type):
    rgb = pixels(7, 10)
    if color_type == 2:
        rows, expect = rgb, rgb
    elif color_type == 6:
        rows, expect = [[(*p, 200) for p in row] for row in rgb], rgb
    elif color_type == 0:
        rows = [[(p[0],) for p in row] for row in rgb]
        expect = [[(p[0],) * 3 for p in row] for row in rgb]
    elif color_type == 4:
        rows = [[(p[0], 9) for p in row] for row in rgb]
        expect = [[(p[0],) * 3 for p in row] for row in rgb]
    else:
        palette = [(i, 255 - i, i // 2) for i in range(256)]
        rows = [[(p[0],) for p in row] for row in rgb]
        expect = [[palette[p[0]] for p in row] for row in rgb]
    path = tmp_path / "p.png"
    path.write_bytes(make_png(rows, color_type, palette=palette if color_type == 3 else None))
    sample = read_pixels(path)
    assert (sample.width, sample.height) == (7, 10)
    assert as_rgb(sample) == [p for row in expect for p in row]


def test_png_16_bit_keeps_the_high_byte(tmp_path):
    rgb = pixels(4, 5)
    path = tmp_path / "p.png"
    path.write_bytes(make_png([[tuple(v * 256 + 3 for v in p) for p in row] for row in rgb], 2, depth=16))
    assert as_rgb(read_pixels(path)) == [p for row in rgb for p in row]


def test_large_pictures_are_sampled(tmp_path):
    path = tmp_path / "p.png"
    path.write_bytes(make_png([[(10, 20, 30)] * 400 for _ in range(300)]))
    sample = read_pixels(path)
    assert 10_000 < len(sample.colors) <= 40_000 and set(as_rgb(sample)) == {(10, 20, 30)}


def test_without_the_csource_tool_jpeg_explains_what_to_do(tmp_path, monkeypatch):
    monkeypatch.setattr(theme_builder.shutil, "which", lambda name: None)
    jpeg = tmp_path / "p.jpg"
    jpeg.write_bytes(b"\xff\xd8\xff\xe0" + bytes(32))
    with pytest.raises(InvalidArgumentError, match="libgdk-pixbuf2.0-bin.*--like"):
        read_pixels(jpeg)
    interlaced = tmp_path / "i.png"
    interlaced.write_bytes(make_png(pixels(3, 3), interlace=1))
    with pytest.raises(InvalidArgumentError, match="gdk-pixbuf-csource"):
        read_pixels(interlaced)


def csource(width, height, rows, channels=3, kind=0x01010001):
    rowstride = width * channels + 2                                     # padded rows, as GdkPixbuf may do
    blob = b"GdkP" + struct.pack(">IIIII", 24 + rowstride * height, kind, rowstride, width, height)
    blob += b"".join(bytes(v for p in row for v in p) + b"\0\0" for row in rows)
    escaped = "".join(f"\\{b:03o}" for b in blob)
    lines = [f'  "{escaped[i:i + 60]}"' for i in range(0, len(escaped), 60)]
    return "/* GdkPixbuf RGB C-Source image dump */\nstatic const guint8 picture[] = \n{ \"\"\n" + "\n".join(lines) + "};\n"


def test_csource_output_is_parsed():
    rgb = pixels(5, 4)
    sample = parse_csource(csource(5, 4, rgb))
    assert (sample.width, sample.height) == (5, 4) and as_rgb(sample) == [p for row in rgb for p in row]
    rgba = [[(*p, 1) for p in row] for row in rgb]
    assert as_rgb(parse_csource(csource(5, 4, rgba, 4, 0x01010002))) == [p for row in rgb for p in row]
    for bad in ("nothing here", csource(5, 4, rgb, kind=0x02010001), csource(5, 4, rgb)[:200] + "};"):
        with pytest.raises(InvalidArgumentError):
            parse_csource(bad)


@pytest.mark.skipif(shutil.which("gdk-pixbuf-csource") is None, reason="gdk-pixbuf-csource is not installed")
def test_the_real_csource_tool_reads_a_png(tmp_path):
    rgb = pixels(6, 6)
    path = tmp_path / "plain.png"
    path.write_bytes(make_png(rgb))
    assert as_rgb(theme_builder._csource_pixels(path)) == [p for row in rgb for p in row]


def test_svg_colours_weigh_backgrounds_by_area(tmp_path):
    path = tmp_path / "p.svg"
    path.write_bytes(NAVY_SVG)
    sample = read_pixels(path)
    weights = dict(zip(as_rgb(sample), sample.weights))
    assert (sample.width, sample.height) == (100, 50)
    assert weights[(0x4a, 0, 0)] == 50 and weights[(0xff, 0x7c, 0x1e)] == 30
    assert weights[(0x16, 0x16, 0x3a)] == weights[(3, 3, 10)] == 40                 # half of the 80 strokes each
    colors = suggest_colors(sample)
    assert colors["background"] in ("#0b0b20", "#0c0b20", "#0b0c20")                  # navy, not the red strokes
    with pytest.raises(InvalidArgumentError, match="no #rrggbb"):
        path.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg"/>')
        read_pixels(path)


# ---- palette ------------------------------------------------------------------------------------------------------

def check_readable(colors, veil=0.72):
    Theme("t", digit=colors["given"], picture=Picture(NAVY_SVG), veil=veil, **colors)   # raises if not readable


def test_suggested_palettes_are_always_readable():
    rng = random.Random(7)
    for _ in range(60):
        n = rng.randrange(1, 30)
        sample = PixelSample(tuple(tuple(rng.random() for _ in range(3)) for _ in range(n)),
                             tuple(rng.random() * 10 for _ in range(n)))
        veil = rng.choice([0.72, 0.85, 1.0])                              # from 0.72 up a palette always exists
        check_readable(suggest_colors(sample, veil), veil)
    weak = PixelSample(((0.2, 0.9, 0.6),) * 5 + ((0.1, 0.6, 0.5),) * 5, (1.0,) * 10)
    for veil in (0.0, 0.3, 0.5):
        try:
            check_readable(suggest_colors(weak, veil), veil)               # either a readable palette ...
        except InvalidArgumentError as exc:
            assert "use a stronger veil" in str(exc)                     # ... or a clear refusal, never a bad one


def test_palette_follows_the_picture():
    orange = PixelSample(((1.0, 0.5, 0.1),) * 10 + ((0.05, 0.02, 0.0),) * 30, (1.0,) * 40)
    colors = suggest_colors(orange)
    r, g, b = (int(colors["box"][i:i + 2], 16) for i in (1, 3, 5))
    assert r > g > b                                                      # an orange accent
    grey = PixelSample(((0.5, 0.5, 0.5), (0.1, 0.1, 0.1)), (1.0, 1.0))
    check_readable(suggest_colors(grey))
    assert suggest_colors(orange) == colors                                # deterministic


# ---- names and the manifest -----------------------------------------------------------------------------------------

@pytest.mark.parametrize("stem, name", [("cavill1", "cavill1"), ("Avatar Night_2", "avatar-night-2"), ("2024 sea", "picture-2024-sea"),
                                        ("___", "picture"), ("ÚŽASNÝ obrázek", "uzasny-obrazek"), ("x" * 40, "x" * 32)])
def test_theme_names_from_file_names(stem, name):
    assert theme_name_for(stem) == name


@pytest.fixture()
def pack(tmp_path):
    root = tmp_path / "themePictures"
    (root / "originals").mkdir(parents=True)
    (root / "originals" / "night.svg").write_bytes(NAVY_SVG)
    (root / "originals" / "tall.png").write_bytes(make_png([[(200, 120, 40)] * 10 for _ in range(30)]))
    return root / "themes.json"


def test_add_creates_the_manifest_and_appends(pack):
    entry = add_theme("night.svg", pack)
    assert entry["name"] == "night" and entry["picture"] == "originals/night.svg" and "focus" not in entry
    tall = add_theme("tall.png", pack)
    assert tall["focus"] == "top"                                          # portrait
    loaded = load_theme_pack(pack)
    assert list(loaded) == ["night", "tall"] and loaded["tall"].picture.focus == "top"
    text = pack.read_text()
    assert text.count("\n  {") == 2 and json.loads(text)["schema"] == "sudoku.themes/1"


def test_add_keeps_existing_entries_and_refuses_duplicates(pack):
    add_theme("night.svg", pack, name="first")
    before = pack.read_text()
    with pytest.raises(InvalidArgumentError, match="already exists; use --replace"):
        add_theme("night.svg", pack, name="first")
    with pytest.raises(InvalidArgumentError, match="built-in"):
        add_theme("night.svg", pack, name="dark")
    with pytest.raises(InvalidArgumentError, match="theme name"):
        add_theme("night.svg", pack, name="Bad")
    assert pack.read_text() == before
    add_theme("tall.png", pack, name="second")
    add_theme("tall.png", pack, name="first", replace=True, veil=0.9, focus="bottom")
    loaded = load_theme_pack(pack)
    assert list(loaded) == ["first", "second"]                             # replaced in place
    assert loaded["first"].veil == 0.9 and loaded["first"].picture == Picture.from_file(pack.parent / "originals" / "tall.png", "bottom")


def test_add_copies_a_picture_from_elsewhere(pack, tmp_path):
    outside = tmp_path / "Sea View.svg"
    outside.write_bytes(NAVY_SVG)
    entry = add_theme(outside, pack)
    assert entry["name"] == "sea-view" and (pack.parent / "originals" / "Sea View.svg").read_bytes() == NAVY_SVG
    add_theme(outside, pack, name="again")                                  # the same file again: no conflict
    different = tmp_path / "other" / "night.svg"
    different.parent.mkdir()
    different.write_bytes(NAVY_SVG.replace(b"ff7c1e", b"00ff00"))
    with pytest.raises(InvalidArgumentError, match="different file"):
        add_theme(different, pack, name="x")
    assert (pack.parent / "originals" / "night.svg").read_bytes() == NAVY_SVG
    with pytest.raises(InvalidArgumentError, match="not found"):
        add_theme("missing.png", pack)
    text_file = tmp_path / "notes.txt"
    text_file.write_text("hello")
    with pytest.raises(InvalidArgumentError, match="PNG, JPEG or SVG"):
        add_theme(text_file, pack)
    assert not (pack.parent / "originals" / "notes.txt").exists()


def test_add_refuses_to_extend_a_broken_manifest(pack):
    pack.write_text('{"schema": "sudoku.themes/1", "themes": [{"name": "x"}]}')
    with pytest.raises(ConfigurationError):
        add_theme("night.svg", pack)
    assert pack.read_text() == '{"schema": "sudoku.themes/1", "themes": [{"name": "x"}]}'
    assert [p.name for p in pack.parent.iterdir() if p.name.startswith(".themes-")] == []


def test_like_starts_from_another_theme(pack):
    add_theme("night.svg", pack, name="base")
    base = load_theme_pack(pack)["base"]
    entry = add_theme("tall.png", pack, name="copy", like="base")
    assert entry["colors"]["box"] == base.box and entry["colors"]["background"] == base.background
    dark = add_theme("tall.png", pack, name="from-dark", like="dark")         # built-in, made readable over a picture
    assert worst_case_contrast(dark["colors"]["grid"], dark["colors"]["background"], 0.72) >= 3
    with pytest.raises(InvalidArgumentError, match="unknown theme"):
        add_theme("tall.png", pack, name="y", like="neon")


def test_main_prints_the_palette_and_the_name_last(pack, capsys):
    assert theme_builder.main(["night.svg", "--manifest", str(pack)]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out[0].startswith("saved theme night") and out[-1] == "night"
    assert theme_builder.main(["night.svg", "--manifest", str(pack)]) == 2
    assert "already exists" in capsys.readouterr().err


def test_add_theme_script_end_to_end(pack, tmp_path, engine_executable):
    env = {**os.environ, "SUDOKU_NO_BUILD": "1", "SUDOKU_CLI": str(engine_executable),
           "SUDOKU_THEME_PACK": str(pack), "SUDOKU_OUTPUT_DIR": str(tmp_path / "product")}
    done = subprocess.run([str(ROOT / "scripts" / "add_theme.sh"), "night.svg", "--veil", "0.8"], env=env, cwd=tmp_path,
                          capture_output=True, text=True, timeout=300)
    assert done.returncode == 0, done.stderr
    assert "saved theme night" in done.stdout and "./run.sh generate --theme night" in done.stdout
    assert load_theme_pack(pack)["night"].veil == 0.8
    assert (tmp_path / "product" / "night-preview.svg").is_file()
    done = subprocess.run([str(ROOT / "scripts" / "add_theme.sh"), "night.svg", "--no-preview"], env=env, cwd=tmp_path,
                          capture_output=True, text=True, timeout=300)
    assert done.returncode == 2 and "already exists" in done.stderr
