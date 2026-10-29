"""Picture themes: Picture validation, guaranteed contrast, rendering, theme packs, CLI and the sample script."""
from __future__ import annotations

import base64
import json
import os
import random
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from sudoku_app import Picture, Theme, cli, load_theme_pack, render_board_svg, resolve_theme
from sudoku_app.errors import ConfigurationError, InvalidArgumentError
from sudoku_app.svg import DEFAULT_VEIL, MAX_PICTURE_BYTES, PICTURE_FOCUS, worst_case_contrast

ROOT = Path(__file__).resolve().parents[2]
NS = "{http://www.w3.org/2000/svg}"
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 16          # recognised by signature; contents are not decoded
JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 16
SVG = b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10" fill="#ff0000"/></svg>'
COLORS = dict(background="#0b0b20", grid="#c59588", box="#ff7c1e", border="#ffc837", given="#fff6dc", filled="#ffb54b")
PALE = dict(COLORS, grid="#303050")                 # far too dark for a line over a bright picture


def picture_theme(**changes):
    options = dict(COLORS, digit=COLORS["given"], picture=Picture(PNG))
    options.update(changes)
    return Theme("pic", **options)


# ---- Picture -----------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("data, media_type", [(PNG, "image/png"), (JPEG, "image/jpeg"), (SVG, "image/svg+xml"),
                                              (b'<?xml version="1.0"?>\n' + SVG, "image/svg+xml"),
                                              (b"\xef\xbb\xbf  " + SVG, "image/svg+xml")])
def test_picture_media_type_comes_from_the_content(data, media_type):
    assert Picture(data).media_type == media_type


@pytest.mark.parametrize("data", [b"", b"GIF89a....", b"<html><body/></html>", b"<?xml version='1.0'?><note/>", "text"])
def test_picture_rejects_unsupported_data(data):
    with pytest.raises(InvalidArgumentError):
        Picture(data)


def test_picture_rejects_oversized_data_and_unknown_focus():
    with pytest.raises(InvalidArgumentError, match="bytes"):
        Picture(PNG + b"\x00" * MAX_PICTURE_BYTES)
    with pytest.raises(InvalidArgumentError, match="focus"):
        Picture(PNG, "middle")


def test_picture_from_file_and_short_repr(tmp_path):
    path = tmp_path / "a.svg"
    path.write_bytes(SVG)
    picture = Picture.from_file(path, "top")
    assert picture == Picture(SVG, "top") and picture.focus == "top"
    assert "bytes" in repr(picture) and len(repr(picture)) < 80
    with pytest.raises(OSError):
        Picture.from_file(tmp_path / "missing.png")


# ---- guaranteed contrast ------------------------------------------------------------------------------------------

def _contrast_over(color, background, veil, pixel):
    def lum(rgb):
        lin = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgb]
        return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
    fg = lum([int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)])
    bg = [int(background[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    under = lum([veil * b + (1 - veil) * p for b, p in zip(bg, pixel)])
    return (max(fg, under) + 0.05) / (min(fg, under) + 0.05)


def test_worst_case_contrast_is_a_lower_bound_for_any_pixel():
    rng = random.Random(20261029)
    for _ in range(300):
        color, background = (f"#{rng.randrange(1 << 24):06x}" for _ in range(2))
        veil = rng.random()
        bound = worst_case_contrast(color, background, veil)
        pixels = [[rng.random() for _ in range(3)] for _ in range(40)] + [[0.0] * 3, [1.0] * 3]
        ratios = [_contrast_over(color, background, veil, p) for p in pixels]
        assert bound <= min(ratios) + 1e-9
        if bound > 1:                                             # attained by a black or a white pixel
            assert bound == pytest.approx(min(ratios[-2:]))


def test_an_opaque_veil_is_plain_contrast_and_no_veil_is_the_harshest():
    assert worst_case_contrast("#ffffff", "#000000", 1.0) == pytest.approx(21.0)
    assert worst_case_contrast("#ffffff", "#000000", 0.0) == pytest.approx(1.0)


def test_a_colour_inside_the_reachable_range_has_no_guaranteed_contrast():
    # mid grey over a 30 % black veil: far from both a black and a white pixel, yet some grey pixel matches it
    assert worst_case_contrast("#777777", "#000000", 0.3) == 1.0
    assert _contrast_over("#777777", "#000000", 0.3, [0.0] * 3) > 3 and _contrast_over("#777777", "#000000", 0.3, [1.0] * 3) > 1.5


def test_picture_theme_must_be_readable_over_any_picture():
    picture_theme()                                              # the palette used by the "dragon" theme
    with pytest.raises(InvalidArgumentError, match=r"grid #303050 reaches only .*needs 3.0:1"):
        picture_theme(grid=PALE["grid"])
    with pytest.raises(InvalidArgumentError, match=r"digit .*needs 4.5:1"):
        picture_theme(veil=0.3)                                  # digits fail first
    Theme("plain", digit="#000000", **PALE)                      # without a picture contrast is not enforced


@pytest.mark.parametrize("veil", [-0.01, 1.01, True, "0.7", None, float("nan")])
def test_invalid_veil_is_rejected(veil):
    with pytest.raises(InvalidArgumentError, match="veil"):
        picture_theme(veil=veil)


def test_picture_must_be_a_picture():
    with pytest.raises(InvalidArgumentError, match="Picture"):
        picture_theme(picture=PNG)


# ---- rendering ----------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("focus", list(PICTURE_FOCUS))
def test_picture_is_embedded_under_a_veil_and_nothing_else_changes(focus):
    theme = picture_theme(picture=Picture(SVG, focus), veil=0.725)
    svg = render_board_svg(SOLVED, original=PUZZLE, theme=theme)
    root = ET.fromstring(svg)
    children = list(root)
    assert [c.tag for c in children[:3]] == [NS + "rect", NS + "image", NS + "rect"]
    background, image, veil = children[:3]
    assert background.get("class") == "background" and background.get("fill") == COLORS["background"]
    assert image.get("width") == image.get("height") == root.get("width")
    assert image.get("preserveAspectRatio") == f"{PICTURE_FOCUS[focus]} slice"
    media, data = image.get("href").split(",", 1)
    assert media == "data:image/svg+xml;base64" and base64.b64decode(data) == SVG
    assert veil.get("class") == "veil" and veil.get("fill") == COLORS["background"]
    assert float(veil.get("fill-opacity")) == theme.veil == 0.725
    # without the picture the document is exactly the plain-colour rendering of the same theme
    plain = render_board_svg(SOLVED, original=PUZZLE, theme=Theme("pic", digit=COLORS["given"], **COLORS))
    stripped = "\n".join(line for line in svg.split("\n") if 'class="picture"' not in line and 'class="veil"' not in line)
    assert stripped == plain


def test_veil_is_written_exactly_as_checked():
    assert picture_theme(veil=0.72).veil == DEFAULT_VEIL
    for veil, text in ((1, "1"), (0.8, "0.8"), (0.72004, "0.72"), (0.73336, "0.7334")):
        try:
            theme = picture_theme(veil=veil)
        except InvalidArgumentError:
            continue
        assert f'fill-opacity="{text}"' in render_board_svg(PUZZLE, theme=theme)
        assert float(text) == theme.veil


def test_picture_rendering_is_deterministic_and_has_no_scripts_or_external_references():
    theme = picture_theme(picture=Picture(JPEG))
    first = render_board_svg(PUZZLE, theme=theme)
    assert first == render_board_svg(PUZZLE, theme=theme)
    assert "<script" not in first and "http://" not in first.replace("http://www.w3.org/2000/svg", "")
    assert re.search(r'href="data:image/jpeg;base64,[A-Za-z0-9+/=]+"', first)


# ---- theme packs ----------------------------------------------------------------------------------------------------

def write_pack(directory: Path, themes=None, *, schema="sudoku.themes/1", raw=None) -> Path:
    (directory / "pics").mkdir(exist_ok=True)
    (directory / "pics" / "a.png").write_bytes(PNG)
    (directory / "pics" / "b.svg").write_bytes(SVG)
    if themes is None:
        themes = [{"name": "alpha", "picture": "pics/a.png", "colors": COLORS},
                  {"name": "beta", "picture": "pics/b.svg", "focus": "top", "veil": 0.8,
                   "colors": dict(COLORS, digit="#ffffff")}]
    path = directory / "themes.json"
    path.write_text(raw if raw is not None else json.dumps({"schema": schema, "themes": themes}), encoding="utf-8")
    return path


def test_pack_loads_in_order_with_defaults(tmp_path):
    pack = load_theme_pack(write_pack(tmp_path))
    assert list(pack) == ["alpha", "beta"]
    alpha, beta = pack["alpha"], pack["beta"]
    assert alpha.picture == Picture(PNG) and alpha.veil == DEFAULT_VEIL and alpha.digit == COLORS["given"]
    assert beta.picture == Picture(SVG, "top") and beta.veil == 0.8 and beta.digit == "#ffffff"
    with pytest.raises(TypeError):
        pack["gamma"] = alpha                                     # read-only


def test_pack_resolves_pictures_against_its_own_directory(tmp_path, monkeypatch):
    path = write_pack(tmp_path)
    monkeypatch.chdir(tmp_path / "pics")
    assert list(load_theme_pack(path)) == ["alpha", "beta"]


def entry(**changes):
    item = {"name": "alpha", "picture": "pics/a.png", "colors": dict(COLORS)}
    item.update(changes)
    return item


@pytest.mark.parametrize("themes, message", [
    ([entry(extra=1)], "unknown key"),
    ([{"name": "alpha", "colors": COLORS}], "missing key"),
    ([entry(colors=dict(COLORS, accent="#000000"))], "unknown key"),
    ([entry(colors={k: v for k, v in COLORS.items() if k != "box"})], "missing key"),
    ([entry(picture="/etc/passwd")], "relative path"),
    ([entry(picture="../outside.png")], "relative path"),
    ([entry(picture="")], "non-empty"),
    ([entry(picture="pics/missing.png")], "cannot read picture"),
    ([entry(picture="themes.json")], "PNG, JPEG or SVG"),
    ([entry(name="Bad Name")], "theme name"),
    ([entry(name="dark")], "already used"),
    ([entry(), entry()], "already used"),
    ([entry(colors=PALE)], "needs 3.0:1"),
    ([entry(veil=2)], "veil"),
    ([entry(focus="middle")], "focus"),
    ([entry(colors=dict(COLORS, given="white"))], "#rrggbb"),
    (["alpha"], "JSON object"),
    ({"alpha": {}}, "must be a list"),
])
def test_pack_rejects_invalid_entries(tmp_path, themes, message):
    with pytest.raises(ConfigurationError, match=message):
        load_theme_pack(write_pack(tmp_path, themes))


@pytest.mark.parametrize("raw, message", [
    ('{"schema": "sudoku.themes/1", "themes": [], "themes": []}', "not valid JSON"),
    ("{", "not valid JSON"),
    ('{"schema": "sudoku.themes/2", "themes": []}', "schema"),
    ('{"themes": []}', "missing key"),
    ('[]', "JSON object"),
])
def test_pack_rejects_malformed_documents(tmp_path, raw, message):
    with pytest.raises(ConfigurationError, match=message):
        load_theme_pack(write_pack(tmp_path, raw=raw))


def test_missing_pack_is_a_configuration_error(tmp_path):
    with pytest.raises(ConfigurationError, match="cannot read theme pack"):
        load_theme_pack(tmp_path / "nope.json")


def test_resolve_theme_prefers_built_ins_and_lists_choices(tmp_path):
    pack = load_theme_pack(write_pack(tmp_path))
    assert resolve_theme("dark", pack).name == "dark" and resolve_theme("dark").name == "dark"
    assert resolve_theme("beta", pack) is pack["beta"]
    with pytest.raises(InvalidArgumentError, match="available: default, light, dark, alpha, beta"):
        resolve_theme("neon", pack)
    with pytest.raises(InvalidArgumentError, match="unknown theme 'alpha'"):
        resolve_theme("alpha")


def test_the_projects_own_theme_pack_is_valid():
    """themePictures/ holds the user's pictures and is not part of the repository; check it when present."""
    path = ROOT / "themePictures" / "themes.json"
    if not path.is_file():
        pytest.skip("no themePictures/themes.json in this checkout")
    pack = load_theme_pack(path)
    assert pack and all(theme.picture is not None for theme in pack.values())


# ---- command line ----------------------------------------------------------------------------------------------------

def run(capsys, *args):
    code = cli.main(list(args))
    out = capsys.readouterr()
    return code, out.out, out.err


def test_themes_command_lists_built_ins_and_the_pack(tmp_path, capsys, monkeypatch):
    monkeypatch.delenv(cli.THEME_PACK_ENV, raising=False)
    code, out, _ = run(capsys, "themes")
    assert code == 0 and "default, light, dark" in out and "picture themes: none" in out
    pack = write_pack(tmp_path)
    code, out, _ = run(capsys, "--theme-pack", str(pack), "themes")
    assert code == 0 and "alpha" in out and "image/svg+xml" in out
    monkeypatch.setenv(cli.THEME_PACK_ENV, str(pack))
    code, out, _ = run(capsys, "themes", "--names")
    assert code == 0 and out.split() == ["default", "light", "dark", "alpha", "beta"]


def test_unknown_theme_or_broken_pack_is_a_usage_error(tmp_path, capsys, monkeypatch):
    monkeypatch.setenv(cli.THEME_PACK_ENV, str(write_pack(tmp_path)))
    with pytest.raises(SystemExit) as info:
        cli.main(["generate", "--theme", "neon"])
    assert info.value.code == 2 and "available: default, light, dark, alpha, beta" in capsys.readouterr().err
    broken = write_pack(tmp_path / "..", [entry(colors=PALE)])
    with pytest.raises(SystemExit) as info:
        cli.main(["--theme-pack", str(broken), "themes"])
    assert info.value.code == 2 and "needs 3.0:1" in capsys.readouterr().err


def test_built_in_themes_never_read_the_pack(tmp_path, capsys, monkeypatch, engine_executable):
    monkeypatch.setenv("SUDOKU_CLI", str(engine_executable))
    monkeypatch.setenv(cli.THEME_PACK_ENV, str(tmp_path / "missing.json"))
    monkeypatch.chdir(tmp_path)
    code, _, err = run(capsys, "generate", "--seed", "7", "--theme", "dark", "--output", "p.svg")
    assert code == 0, err


def test_generate_with_a_picture_theme(tmp_path, capsys, monkeypatch, engine_executable):
    monkeypatch.setenv("SUDOKU_CLI", str(engine_executable))
    monkeypatch.setenv(cli.THEME_PACK_ENV, str(write_pack(tmp_path)))
    monkeypatch.chdir(tmp_path)
    code, out, err = run(capsys, "generate", "--difficulty", "easy", "--seed", "3", "--theme", "beta",
                         "--output", "p.svg", "--solution-output", "s.svg")
    assert code == 0, err
    for name in ("p.svg", "s.svg"):
        root = ET.parse(tmp_path / name).getroot()
        image = root.find(NS + "image")
        assert base64.b64decode(image.get("href").split(",", 1)[1]) == SVG
        assert image.get("preserveAspectRatio") == "xMidYMin slice"


def test_sample_script_moves_one_easy_puzzle_per_theme_into_the_output_dir(tmp_path, engine_executable):
    pack = write_pack(tmp_path)
    out = tmp_path / "samples"
    env = {**os.environ, "SUDOKU_NO_BUILD": "1", "SUDOKU_CLI": str(engine_executable), cli.THEME_PACK_ENV: str(pack)}
    names = ["default", "light", "dark", "alpha", "beta"]
    files = set()
    for _ in range(2):                                            # a second run adds files, it replaces nothing
        result = subprocess.run([str(ROOT / "scripts" / "theme_samples.sh"), str(out)], env=env, cwd=tmp_path,
                                capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, result.stderr
        rows = [line.split() for line in result.stdout.splitlines()[1:]]
        assert [r[0] for r in rows] == names
        seeds = [r[1] for r in rows]
        assert all(s.isdigit() for s in seeds) and len(set(seeds)) == len(seeds)
        assert [r[2] for r in rows] == [str(out / f"{n}_{s}.svg") for n, s in zip(names, seeds)]
        files |= {f"{n}_{s}.svg" for n, s in zip(names, seeds)}
        assert {p.name for p in out.iterdir()} == files
    assert len(files) == 2 * len(names)
    alpha = next(out.glob("alpha_*.svg"))
    assert ET.parse(alpha).getroot().find(NS + "image") is not None
