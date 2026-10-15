"""Theme tests (M20): built-in themes, validation, XML safety, M19 backward compatibility."""
from __future__ import annotations

import dataclasses
import hashlib
import xml.etree.ElementTree as ET

import pytest

from sudoku_app import DARK_THEME, DEFAULT_THEME, LIGHT_THEME, THEME_NAMES, Theme, render_board_svg, write_board_svg
from sudoku_app.errors import InvalidArgumentError
from sudoku_app.svg import MAX_CELL_SIZE, MAX_MARGIN, MIN_CELL_SIZE, MIN_MARGIN, THEMES, get_theme

NS = "{http://www.w3.org/2000/svg}"
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
GOOD = dict(background="#ffffff", grid="#808080", box="#000000", border="#000000", digit="#000000", given="#000000",
            filled="#1a56c4")

# SHA-256 of the M19 renderer's output, recorded before themes existed.
M19_GOLDEN = {
    "5f9bd80bd3d5acbb3b9d2c8e47147ef615899a2bfadf1f645f5e794faa809adc": ("." * 81, {}),
    "df8cc9ec009c7cd311ab00684beebffdb57acb6e0630a66937df715ed327e18b": (PUZZLE, {}),
    "c00cb0c8bec8add8a91925901290475505eb756c0ba755cfd78b32c458c11a91": (SOLVED, {"original": PUZZLE}),
    "ee6f7d6cd1021affb288044e2e9b094000b3fcf2be293bc000de165eb651a7bc": (SOLVED, {"cell_size": 16, "margin": 4}),
    "30f4d1f2b96386395ea7b03bffbe6f7b4dc52aeac780d213d1a911748396346e": (PUZZLE, {"original": PUZZLE, "cell_size": 200, "margin": 100}),
    "fa2498048a62c06a0cba289b47bda8ede9a1277de0904258f96bf09a57e29837": (SOLVED, {"original": PUZZLE, "cell_size": 37, "margin": 9}),
}


def parse(svg):
    return ET.fromstring(svg)


def luminance(hex_color):
    def channel(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


# ---- compatibility ----------------------------------------------------------------------------------------------

@pytest.mark.parametrize("digest", list(M19_GOLDEN))
def test_default_output_is_byte_identical_to_m19(digest):
    board, options = M19_GOLDEN[digest]
    assert hashlib.sha256(render_board_svg(board, **options).encode()).hexdigest() == digest
    for theme in (None, "default", "light", DEFAULT_THEME):
        assert render_board_svg(board, theme=theme, **options) == render_board_svg(board, **options)


def test_default_theme_is_the_light_theme():
    assert DEFAULT_THEME is LIGHT_THEME is get_theme(None) is get_theme("default") is get_theme("light")


def test_stable_theme_names():
    assert THEME_NAMES == ("default", "light", "dark")
    assert set(THEMES) == set(THEME_NAMES)
    assert LIGHT_THEME.name == "light" and DARK_THEME.name == "dark"


# ---- built-in themes ----------------------------------------------------------------------------------------------

@pytest.mark.parametrize("name", ["light", "dark"])
def test_builtin_theme_colours_reach_the_svg_and_nothing_else_changes(name):
    theme = THEMES[name]
    svg = render_board_svg(SOLVED, original=PUZZLE, theme=name)
    root = parse(svg)
    groups = {g.get("class"): g for g in root.findall(NS + "g")}
    assert root.find(NS + "rect").get("fill") == theme.background
    assert groups["thin-lines"].get("stroke") == theme.grid
    assert groups["box-lines"].get("stroke") == theme.box
    assert [r for r in root.findall(NS + "rect") if r.get("class") == "border"][0].get("stroke") == theme.border
    texts = groups["digits"].findall(NS + "text")
    assert {t.get("fill") for t in texts if t.get("class") == "given"} == {theme.given}
    assert {t.get("fill") for t in texts if t.get("class") == "filled"} == {theme.filled}
    assert render_board_svg(SOLVED, original=PUZZLE, theme=name) == svg
    plain = parse(render_board_svg(SOLVED, theme=name))
    assert {t.get("fill") for t in plain.iter(NS + "text")} == {theme.digit}


def skeleton(svg):
    """Everything except colours: tags, positions, sizes, text, weights and slants."""
    out = []
    for e in parse(svg).iter():
        attrs = {k: v for k, v in e.attrib.items() if k not in ("fill", "stroke")}
        if e.tag == NS + "rect" and e.get("class") == "background":
            attrs["fill"] = None
        out.append((e.tag, tuple(sorted(attrs.items())), e.text and e.text.strip()))
    return out


@pytest.mark.parametrize("cell,margin", [(MIN_CELL_SIZE, MIN_MARGIN), (50, 10), (MAX_CELL_SIZE, MAX_MARGIN)])
def test_geometry_and_content_are_identical_across_themes(cell, margin):
    kw = dict(original=PUZZLE, cell_size=cell, margin=margin)
    base = skeleton(render_board_svg(SOLVED, **kw))
    assert skeleton(render_board_svg(SOLVED, theme="dark", **kw)) == base
    assert skeleton(render_board_svg(SOLVED, theme="light", **kw)) == base
    assert render_board_svg(SOLVED, theme="dark", **kw) != render_board_svg(SOLVED, theme="light", **kw)


@pytest.mark.parametrize("theme", [LIGHT_THEME, DARK_THEME])
def test_builtin_themes_are_legible(theme):
    for digit_colour in (theme.digit, theme.given, theme.filled):
        assert contrast(digit_colour, theme.background) >= 4.5
    assert contrast(theme.grid, theme.background) >= 3.0
    assert contrast(theme.box, theme.background) >= 4.5 and contrast(theme.border, theme.background) >= 4.5
    assert theme.grid != theme.box  # ordinary lines differ from 3x3 boundaries (colour, plus width 1 vs 2)
    assert theme.given != theme.filled


@pytest.mark.parametrize("name", ["light", "dark"])
def test_given_vs_filled_not_colour_only_in_every_theme(name):
    texts = parse(render_board_svg(SOLVED, original=PUZZLE, theme=name)).iter(NS + "text")
    style = {}
    for t in texts:
        style.setdefault(t.get("class"), set()).add((t.get("font-weight"), t.get("font-style")))
    assert style == {"given": {("bold", None)}, "filled": {("normal", "italic")}}


@pytest.mark.parametrize("name", ["light", "dark"])
def test_minimum_and_maximum_sizes_with_themes_stay_inside_viewbox(name):
    for cell, margin in ((MIN_CELL_SIZE, MIN_MARGIN), (MAX_CELL_SIZE, MAX_MARGIN)):
        root = parse(render_board_svg(SOLVED, original=PUZZLE, cell_size=cell, margin=margin, theme=name))
        total = 9 * cell + 2 * margin
        assert root.get("viewBox") == f"0 0 {total} {total}"
        assert len(list(root.iter(NS + "text"))) == 81


def test_write_board_svg_accepts_a_theme(tmp_path):
    target = tmp_path / "d.svg"
    text = write_board_svg(target, SOLVED, original=PUZZLE, theme="dark")
    assert text == render_board_svg(SOLVED, original=PUZZLE, theme=DARK_THEME) and target.read_text() == text
    with pytest.raises(InvalidArgumentError):
        write_board_svg(tmp_path / "e.svg", SOLVED, theme="nope")
    assert not (tmp_path / "e.svg").exists()


# ---- validation -------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("theme", ["", "Dark", "LIGHT", " dark", "dark ", "solarized", 5, b"dark", ["dark"], {"name": "x"}, True])
def test_unknown_or_malformed_theme_selectors_are_rejected(theme):
    with pytest.raises(InvalidArgumentError):
        render_board_svg(PUZZLE, theme=theme)


@pytest.mark.parametrize("field", ["background", "grid", "box", "border", "digit", "given", "filled"])
@pytest.mark.parametrize("bad", [
    "", "red", "#fff", "#ffff", "#fffffff", "ffffff", "#gggggg", "#ff ff0", "#ffffff ", " #ffffff", "#ffffff\n",
    "url(#x)", "#ffffff\" onload=\"x", "#ffffff\"/><script/>", "#ffffff;", "rgb(0,0,0)", "#ffffffff", "none", None, 0xffffff, b"#ffffff",
    "#ffffff<", "&amp;", "#ＦＦＦＦＦＦ"])
def test_bad_colours_are_rejected(field, bad):
    with pytest.raises(InvalidArgumentError):
        Theme("custom", **{**GOOD, field: bad})


@pytest.mark.parametrize("name", ["", "Custom", "1x", "-x", "a b", "a_b", "a" * 33, "x\"y", "x<y", None, 5, "é"])
def test_bad_theme_names_are_rejected(name):
    with pytest.raises(InvalidArgumentError):
        Theme(name, **GOOD)


def test_missing_theme_fields_are_a_type_error():
    with pytest.raises(TypeError):
        Theme("custom", background="#ffffff")  # type: ignore[call-arg]


def test_custom_theme_works_normalises_case_and_is_xml_safe():
    custom = Theme("my-theme-1", **{**GOOD, "background": "#FFEEDD", "filled": "#00AA00"})
    assert custom.background == "#ffeedd" and custom.filled == "#00aa00"
    svg = render_board_svg(SOLVED, original=PUZZLE, theme=custom)
    root = parse(svg)
    assert root.find(NS + "rect").get("fill") == "#ffeedd"
    assert {e.tag for e in root.iter()} <= {NS + t for t in ("svg", "rect", "g", "line", "text")}
    assert "my-theme-1" not in svg  # the name is metadata, never written into the SVG
    assert svg == render_board_svg(SOLVED, original=PUZZLE, theme=custom)


def test_themes_are_immutable_and_registry_is_read_only():
    with pytest.raises(dataclasses.FrozenInstanceError):
        LIGHT_THEME.background = "#000000"  # type: ignore[misc]
    with pytest.raises(TypeError):
        THEMES["evil"] = LIGHT_THEME  # type: ignore[index]
    with pytest.raises(TypeError):
        del THEMES["dark"]  # type: ignore[attr-defined]
    before = render_board_svg(PUZZLE, theme="dark")
    dataclasses.replace(DARK_THEME, background="#000000")  # a copy never affects the built-in
    assert render_board_svg(PUZZLE, theme="dark") == before and DARK_THEME.background == "#121212"


def test_an_invalid_copy_of_a_theme_is_rejected():
    with pytest.raises(InvalidArgumentError):
        dataclasses.replace(DARK_THEME, border="blue")


def test_theme_does_not_bypass_board_or_mask_validation():
    for theme in ("light", "dark", None):
        with pytest.raises(InvalidArgumentError):
            render_board_svg("x" * 81, theme=theme)
        with pytest.raises(InvalidArgumentError):
            render_board_svg(SOLVED, original=PUZZLE, given_mask=[True] * 81, theme=theme)
        with pytest.raises(InvalidArgumentError):
            render_board_svg(SOLVED, original="4" + PUZZLE[1:], theme=theme)
        with pytest.raises(InvalidArgumentError):
            render_board_svg(SOLVED, cell_size=MIN_CELL_SIZE - 1, theme=theme)


def test_inputs_are_not_mutated():
    mask = [c != "." for c in PUZZLE]
    copy = list(mask)
    render_board_svg(SOLVED, given_mask=mask, theme="dark")
    assert mask == copy


def test_every_theme_colour_lands_on_its_own_element_kind():
    distinct = Theme("distinct", background="#010101", grid="#020202", box="#030303", border="#040404", digit="#050505",
                     given="#060606", filled="#070707")
    root = parse(render_board_svg(SOLVED, original=PUZZLE, theme=distinct))
    groups = {g.get("class"): g for g in root.findall(NS + "g")}
    assert root.find(NS + "rect").get("fill") == "#010101"
    assert groups["thin-lines"].get("stroke") == "#020202" and groups["box-lines"].get("stroke") == "#030303"
    assert [r for r in root.findall(NS + "rect") if r.get("class") == "border"][0].get("stroke") == "#040404"
    fills = {t.get("class"): t.get("fill") for t in root.iter(NS + "text")}
    assert fills == {"given": "#060606", "filled": "#070707"}
    assert {t.get("fill") for t in parse(render_board_svg(SOLVED, theme=distinct)).iter(NS + "text")} == {"#050505"}
