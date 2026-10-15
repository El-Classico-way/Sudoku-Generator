"""SVG rendering tests (M19). Output is parsed with the standard XML parser; no browser, network or tools."""

from __future__ import annotations

import io
import xml.etree.ElementTree as ET

import pytest

from sudoku_app import SudokuEngine, render_board_svg, write_board_svg
from sudoku_app.errors import InvalidArgumentError
from sudoku_app.svg import (DEFAULT_CELL_SIZE, DEFAULT_MARGIN, MAX_CELL_SIZE, MAX_MARGIN, MIN_CELL_SIZE, MIN_MARGIN)

NS = "http://www.w3.org/2000/svg"
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
EMPTY = "." * 81


def q(name):
    return f"{{{NS}}}{name}"


def parse(svg):
    return ET.fromstring(svg)


def group(root, cls):
    found = [g for g in root.findall(q("g")) if g.get("class") == cls]
    assert len(found) == 1
    return found[0]


def digit_cells(root, margin=DEFAULT_MARGIN, cell=DEFAULT_CELL_SIZE):
    """(row, col, char, element) for every text element, derived from its x/y position."""
    cells = []
    for t in group(root, "digits").findall(q("text")):
        x, y = float(t.get("x")), float(t.get("y"))
        col, row = int((x - margin) // cell), int((y - margin) // cell)
        cells.append((row, col, t.text, t))
    return cells


# ---- structure -------------------------------------------------------------------------------------------------

def test_root_dimensions_and_viewbox():
    root = parse(render_board_svg(EMPTY))
    assert root.tag == q("svg")
    total = 9 * DEFAULT_CELL_SIZE + 2 * DEFAULT_MARGIN
    assert root.get("width") == root.get("height") == str(total)
    assert root.get("viewBox") == f"0 0 {total} {total}"


@pytest.mark.parametrize("board", [EMPTY, PUZZLE, SOLVED])
def test_fixed_element_vocabulary_and_no_external_references(board):
    svg = render_board_svg(board)
    root = parse(svg)
    allowed = {q("svg"), q("rect"), q("g"), q("line"), q("text")}
    assert {e.tag for e in root.iter()} <= allowed
    for e in root.iter():
        for attr in e.attrib:
            assert attr.split("}")[-1] not in ("href", "src") and not attr.startswith("on")
        assert "url(" not in "".join(e.attrib.values())
    for banned in ("<script", "<image", "<use", "<style", "<foreignObject", "http://www.w3.org/1999/xlink", "<!--", "<?xml"):
        assert banned not in svg
    assert svg.endswith("</svg>\n") and "\r" not in svg


def test_grid_lines_are_regular_and_box_lines_stronger():
    margin, cell = 10, 50
    root = parse(render_board_svg(EMPTY, cell_size=cell, margin=margin))
    thin, box = group(root, "thin-lines"), group(root, "box-lines")
    w_thin, w_box = int(thin.get("stroke-width")), int(box.get("stroke-width"))
    border = [r for r in root.findall(q("rect")) if r.get("class") == "border"][0]
    assert w_thin < w_box < int(border.get("stroke-width"))
    lo, hi = margin, margin + 9 * cell

    def positions(g):
        vertical = sorted(int(l.get("x1")) for l in g.findall(q("line")) if l.get("x1") == l.get("x2"))
        horizontal = sorted(int(l.get("y1")) for l in g.findall(q("line")) if l.get("y1") == l.get("y2"))
        for l in g.findall(q("line")):  # every line spans the whole grid
            if l.get("x1") == l.get("x2"):
                assert (int(l.get("y1")), int(l.get("y2"))) == (lo, hi)
            else:
                assert (int(l.get("x1")), int(l.get("x2"))) == (lo, hi)
        return vertical, horizontal

    expected_box = [lo + i * cell for i in (3, 6)]
    expected_thin = [lo + i * cell for i in (1, 2, 4, 5, 7, 8)]
    assert positions(box) == (expected_box, expected_box)
    assert positions(thin) == (expected_thin, expected_thin)
    assert (int(border.get("x")), int(border.get("y")), int(border.get("width")), int(border.get("height"))) \
        == (lo, lo, 9 * cell, 9 * cell)
    assert border.get("fill") == "none"


def test_empty_board_has_no_digit_elements():
    root = parse(render_board_svg(EMPTY))
    assert digit_cells(root) == []
    assert group(root, "digits").findall(q("text")) == []


@pytest.mark.parametrize("empty_char", [".", "0"])
def test_both_empty_conventions_render_identically(empty_char):
    assert render_board_svg(PUZZLE.replace(".", empty_char)) == render_board_svg(PUZZLE)


def test_partial_puzzle_digits_match_in_position_and_value_and_order():
    cells = digit_cells(parse(render_board_svg(PUZZLE)))
    assert [(r * 9 + c, ch) for r, c, ch, _ in cells] == [(i, ch) for i, ch in enumerate(PUZZLE) if ch != "."]


def test_completed_grid_has_81_digits_covering_every_row_column_and_box():
    cells = digit_cells(parse(render_board_svg(SOLVED)))
    assert len(cells) == 81
    grid = {(r, c): ch for r, c, ch, _ in cells}
    assert len(grid) == 81
    assert all(grid[(i // 9, i % 9)] == ch for i, ch in enumerate(SOLVED))
    for k in range(9):
        assert {grid[(k, c)] for c in range(9)} == set("123456789")
        assert {grid[(r, k)] for r in range(9)} == set("123456789")
        br, bc = divmod(k, 3)
        assert {grid[(3 * br + r, 3 * bc + c)] for r in range(3) for c in range(3)} == set("123456789")


def test_each_single_digit_is_centred_in_its_cell():
    margin, cell = 12, 40
    for i in (0, 4, 8, 40, 72, 80):
        board = "." * i + "7" + "." * (80 - i)
        root = parse(render_board_svg(board, cell_size=cell, margin=margin))
        (t,) = group(root, "digits").findall(q("text"))
        r, c = divmod(i, 9)
        assert float(t.get("x")) == margin + c * cell + cell / 2
        assert margin + r * cell < float(t.get("y")) < margin + (r + 1) * cell
        assert group(root, "digits").get("text-anchor") == "middle"


@pytest.mark.parametrize("cell,margin", [(MIN_CELL_SIZE, MIN_MARGIN), (MAX_CELL_SIZE, MAX_MARGIN), (33, 7), (50, 10)])
def test_all_geometry_lies_inside_viewbox(cell, margin):
    root = parse(render_board_svg(SOLVED, cell_size=cell, margin=margin))
    total = 9 * cell + 2 * margin
    assert root.get("viewBox") == f"0 0 {total} {total}"
    font = float(group(root, "digits").get("font-size"))
    for line in root.iter(q("line")):
        for a in ("x1", "y1", "x2", "y2"):
            assert 0 <= float(line.get(a)) <= total
    border = [r for r in root.findall(q("rect")) if r.get("class") == "border"][0]
    half = int(border.get("stroke-width")) / 2
    assert float(border.get("x")) - half >= 0
    assert float(border.get("x")) + float(border.get("width")) + half <= total
    for r, c, _, t in digit_cells(root, margin, cell):
        x, y = float(t.get("x")), float(t.get("y"))
        assert margin <= x - font / 2 and x + font / 2 <= margin + 9 * cell
        assert margin + r * cell <= y - font * 0.7 and y <= margin + (r + 1) * cell
    for e in root.iter(q("text")):
        assert e.text in set("123456789")


# ---- givens versus filled digits -------------------------------------------------------------------------------

def test_without_given_information_every_digit_is_neutral():
    cells = digit_cells(parse(render_board_svg(SOLVED)))
    assert {t.get("class") for *_, t in cells} == {"digit"}
    assert len({tuple(sorted(t.attrib.items() - {("x", t.get("x")), ("y", t.get("y"))})) for *_, t in cells}) == 1


def check_given_split(svg, given_indexes, board):
    cells = digit_cells(parse(svg))
    by_index = {r * 9 + c: t for r, c, _, t in cells}
    assert sorted(by_index) == [i for i, ch in enumerate(board) if ch not in ".0"]
    for i, t in by_index.items():
        if i in given_indexes:
            assert (t.get("class"), t.get("font-weight"), t.get("font-style")) == ("given", "bold", None)
        else:
            assert (t.get("class"), t.get("font-weight"), t.get("font-style")) == ("filled", "normal", "italic")
    given_attrs = {(t.get("font-weight"), t.get("font-style"), t.get("fill")) for i, t in by_index.items() if i in given_indexes}
    filled_attrs = {(t.get("font-weight"), t.get("font-style"), t.get("fill")) for i, t in by_index.items() if i not in given_indexes}
    # distinguishable by weight and slant, not by colour alone
    assert all(g[0] != f[0] and g[1] != f[1] for g in given_attrs for f in filled_attrs)


def test_original_board_marks_givens_on_a_completed_solution():
    givens = {i for i, ch in enumerate(PUZZLE) if ch != "."}
    check_given_split(render_board_svg(SOLVED, original=PUZZLE), givens, SOLVED)


def test_original_board_marks_givens_on_a_partially_filled_board():
    partial = SOLVED[:30] + "." * 51
    original = PUZZLE[:30] + "." * 51
    check_given_split(render_board_svg(partial, original=original), {i for i, ch in enumerate(original) if ch != "."}, partial)


@pytest.mark.parametrize("shape", ["flat", "nested"])
def test_mask_marks_givens_and_equals_original_rendering(shape):
    flags = [ch != "." for ch in PUZZLE]
    mask = flags if shape == "flat" else [flags[r * 9:(r + 1) * 9] for r in range(9)]
    svg = render_board_svg(SOLVED, given_mask=mask)
    check_given_split(svg, {i for i, f in enumerate(flags) if f}, SOLVED)
    assert svg == render_board_svg(SOLVED, original=PUZZLE)


def test_mask_accepts_tuples_and_all_false_means_all_filled():
    svg = render_board_svg(SOLVED, given_mask=tuple([False] * 81))
    assert {t.get("class") for *_, t in digit_cells(parse(svg))} == {"filled"}


def test_empty_cells_get_no_styling_with_given_information():
    root = parse(render_board_svg(PUZZLE, original=PUZZLE))
    assert len(digit_cells(root)) == sum(ch != "." for ch in PUZZLE)
    assert {t.get("class") for *_, t in digit_cells(root)} == {"given"}


# ---- determinism, purity -----------------------------------------------------------------------------------------

def test_repeated_rendering_is_identical_and_inputs_are_unchanged():
    mask = [ch != "." for ch in PUZZLE]
    mask_copy = list(mask)
    a = render_board_svg(SOLVED, given_mask=mask, cell_size=37, margin=9)
    b = render_board_svg(SOLVED, given_mask=mask, cell_size=37, margin=9)
    assert a == b and mask == mask_copy
    assert render_board_svg(PUZZLE) != render_board_svg(SOLVED)
    assert render_board_svg(EMPTY, cell_size=30) != render_board_svg(EMPTY, cell_size=31)


def test_odd_sizes_use_stable_short_numbers():
    svg = render_board_svg(SOLVED, cell_size=17, margin=5)
    for t in parse(svg).iter(q("text")):
        for a in ("x", "y"):
            v = t.get(a)
            if "." in v:
                assert len(v.split(".")[1]) <= 2 and not v.endswith("0")
    assert svg == render_board_svg(SOLVED, cell_size=17, margin=5)


# ---- validation --------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("board", [None, 5, b"." * 81, list(EMPTY), "", "." * 80, "." * 82, "x" * 81, "-" * 81,
                                   EMPTY[:-1] + "a", EMPTY[:-1] + " ", EMPTY[:-1] + "١"])
def test_bad_boards_are_rejected(board):
    with pytest.raises(InvalidArgumentError):
        render_board_svg(board)


def test_unsolvable_or_conflicting_boards_still_render():
    conflicting = "11" + "." * 79
    assert len(digit_cells(parse(render_board_svg(conflicting)))) == 2


@pytest.mark.parametrize("name,low,high", [("cell_size", MIN_CELL_SIZE, MAX_CELL_SIZE), ("margin", MIN_MARGIN, MAX_MARGIN)])
def test_dimension_boundaries(name, low, high):
    for ok in (low, low + 1, high - 1, high):
        parse(render_board_svg(PUZZLE, **{name: ok}))
    for bad in (low - 1, high + 1, 0, -1, 10**9):
        with pytest.raises(InvalidArgumentError):
            render_board_svg(PUZZLE, **{name: bad})
    for bad in (True, False, 50.0, "50", None, float("nan"), [50]):
        with pytest.raises(InvalidArgumentError):
            render_board_svg(PUZZLE, **{name: bad})


def test_largest_output_is_bounded():
    svg = render_board_svg(SOLVED, cell_size=MAX_CELL_SIZE, margin=MAX_MARGIN)
    assert len(svg) < 20_000 and parse(svg).get("width") == str(9 * MAX_CELL_SIZE + 2 * MAX_MARGIN)


def test_original_and_mask_together_are_rejected():
    with pytest.raises(InvalidArgumentError, match="at most one"):
        render_board_svg(SOLVED, original=PUZZLE, given_mask=[True] * 81)


@pytest.mark.parametrize("original", [5, "x" * 81, "." * 80, list(PUZZLE)])
def test_bad_original_is_rejected(original):
    with pytest.raises(InvalidArgumentError):
        render_board_svg(SOLVED, original=original)


def test_original_inconsistent_with_board_is_rejected():
    wrong = "4" + PUZZLE[1:]  # board has 5 there
    with pytest.raises(InvalidArgumentError, match="index 0"):
        render_board_svg(SOLVED, original=wrong)
    with pytest.raises(InvalidArgumentError):  # a given that the board leaves empty
        render_board_svg(".." + SOLVED[2:], original=PUZZLE)


def test_mask_given_on_an_empty_cell_is_rejected():
    with pytest.raises(InvalidArgumentError, match="empty"):
        render_board_svg(PUZZLE, given_mask=[True] * 81)


@pytest.mark.parametrize("mask", [
    [True] * 80, [True] * 82, [], [[True] * 9] * 8, [[True] * 9] * 10, [[True] * 8] * 9, [[True] * 9] * 8 + [[True] * 10],
    [[True] * 9] * 8 + ["x" * 9], "T" * 81, b"\x01" * 81, 7,
    [1] * 81, [0] * 81, [None] * 81, ["True"] * 81, [True] * 80 + [1],
    [[1] * 9] * 9, {i: True for i in range(81)},
])
def test_bad_masks_are_rejected(mask):
    with pytest.raises(InvalidArgumentError):
        render_board_svg(SOLVED, given_mask=mask)


def test_validation_errors_are_invalid_argument_and_value_errors():
    with pytest.raises(ValueError):
        render_board_svg("1")


# ---- writing -------------------------------------------------------------------------------------------------------

def test_write_to_path_and_stream_match_returned_text(tmp_path):
    target = tmp_path / "board.svg"
    text = write_board_svg(target, PUZZLE, original=PUZZLE)
    assert target.read_bytes() == text.encode("utf-8") == render_board_svg(PUZZLE, original=PUZZLE).encode("utf-8")
    assert b"\r" not in target.read_bytes()
    stream = io.StringIO()
    assert write_board_svg(stream, PUZZLE, original=PUZZLE) == text
    assert stream.getvalue() == text
    as_str = tmp_path / "again.svg"
    write_board_svg(str(as_str), SOLVED)
    assert as_str.read_text(encoding="utf-8") == render_board_svg(SOLVED)
    parse(target.read_text(encoding="utf-8"))


def test_invalid_input_never_creates_or_changes_a_file(tmp_path):
    target = tmp_path / "x.svg"
    with pytest.raises(InvalidArgumentError):
        write_board_svg(target, "bad")
    assert not target.exists()
    target.write_text("keep")
    with pytest.raises(InvalidArgumentError):
        write_board_svg(target, PUZZLE, cell_size=1)
    assert target.read_text() == "keep"
    stream = io.StringIO()
    with pytest.raises(InvalidArgumentError):
        write_board_svg(stream, PUZZLE, given_mask=[True] * 81)
    assert stream.getvalue() == ""


@pytest.mark.parametrize("destination", [None, 5, object(), b"x.svg"])
def test_bad_destination_is_rejected(destination):
    with pytest.raises(InvalidArgumentError):
        write_board_svg(destination, PUZZLE)


def test_write_into_a_missing_directory_raises_oserror(tmp_path):
    with pytest.raises(OSError):
        write_board_svg(tmp_path / "missing" / "b.svg", PUZZLE)


# ---- integration with the real engine --------------------------------------------------------------------------------

def test_renders_real_engine_output_without_mutating_it(engine_executable):
    engine = SudokuEngine(engine_executable, timeout=120)
    result = engine.generate_puzzle(12345)
    puzzle, solution = result.puzzle, result.solution
    before = (puzzle, solution)
    svg = render_board_svg(solution, original=puzzle)
    assert (puzzle, solution) == before
    cells = digit_cells(parse(svg))
    assert len(cells) == 81
    givens = {i for i, ch in enumerate(puzzle) if ch != "."}
    assert {r * 9 + c for r, c, _, t in cells if t.get("class") == "given"} == givens
    assert "".join(ch for _, _, ch, _ in cells) == solution
    assert len(digit_cells(parse(render_board_svg(puzzle)))) == len(givens)
