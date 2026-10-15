"""Solution-export tests (M20): controlled stub engines, the fake executable, and the real engine."""
from __future__ import annotations

import io
import json
import xml.etree.ElementTree as ET

import pytest

from sudoku_app import SudokuEngine, export_solution_svg, render_board_svg
from sudoku_app.errors import (EngineProcessError, EngineTimeoutError, InvalidArgumentError, OperationNotSuccessfulError,
                               RequestRejectedError, ResponseFormatError)
from sudoku_app.models import VerifyResult, VerifyStatus

NS = "{http://www.w3.org/2000/svg}"
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
# complete, keeps the clues of PUZZLE in cells 0..1 but breaks row/column rules (independent of the engine under test)
BROKEN = "5" + "3" + "4" * 79


class StubEngine:
    """Records calls; answers with a canned result or raises a canned error."""

    def __init__(self, result=None, error=None):
        self.result, self.error, self.calls = result, error, []

    def verify(self, puzzle, solution, *, timeout=None, **kw):
        self.calls.append((puzzle, solution, timeout))
        if self.error:
            raise self.error
        return self.result


VALID = VerifyResult(VerifyStatus.VALID, True, True, None, None)


def fault(status, index=3, digit=4):
    return VerifyResult(status, False, False, index, digit)


def texts(svg):
    return list(ET.fromstring(svg).iter(NS + "text"))


# ---- success -------------------------------------------------------------------------------------------------------

def test_valid_export_matches_the_renderer_and_distinguishes_givens():
    engine = StubEngine(VALID)
    svg = export_solution_svg(engine, PUZZLE, SOLVED)
    assert svg == render_board_svg(SOLVED, original=PUZZLE)
    assert engine.calls == [(PUZZLE, SOLVED, None)]
    ts = texts(svg)
    assert "".join(t.text for t in ts) == SOLVED
    for i, t in enumerate(ts):
        assert t.get("class") == ("given" if PUZZLE[i] != "." else "filled")
        assert t.get("font-weight") == ("bold" if PUZZLE[i] != "." else "normal")


def test_theme_size_and_timeout_are_passed_through_and_output_is_deterministic():
    engine = StubEngine(VALID)
    a = export_solution_svg(engine, PUZZLE, SOLVED, theme="dark", cell_size=30, margin=6, timeout=7.5)
    b = export_solution_svg(engine, PUZZLE, SOLVED, theme="dark", cell_size=30, margin=6, timeout=7.5)
    assert a == b == render_board_svg(SOLVED, original=PUZZLE, theme="dark", cell_size=30, margin=6)
    assert engine.calls[0][2] == 7.5


def test_zero_is_accepted_as_empty_in_the_puzzle():
    assert export_solution_svg(StubEngine(VALID), PUZZLE.replace(".", "0"), SOLVED) == render_board_svg(SOLVED, original=PUZZLE)


def test_destination_path_stream_and_omitted(tmp_path):
    target = tmp_path / "solution.svg"
    text = export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, target)
    assert target.read_bytes() == text.encode() and b"\r" not in target.read_bytes()
    stream = io.StringIO()
    assert export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, stream) == text and stream.getvalue() == text
    export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, str(tmp_path / "s2.svg"))
    assert (tmp_path / "s2.svg").read_text() == text
    assert sorted(p.name for p in tmp_path.iterdir()) == ["s2.svg", "solution.svg"]  # no implicit default output


def test_inputs_strings_are_not_modified_and_stub_not_asked_twice():
    engine = StubEngine(VALID)
    p, s = str(PUZZLE), str(SOLVED)
    export_solution_svg(engine, p, s)
    assert (p, s) == (PUZZLE, SOLVED) and len(engine.calls) == 1


# ---- Python-side rejection: engine never contacted, nothing written -----------------------------------------------------

@pytest.mark.parametrize("puzzle,solution", [
    ("x" * 81, SOLVED), (PUZZLE[:-1], SOLVED), (None, SOLVED), (PUZZLE, "x" * 81), (PUZZLE, SOLVED[:-1]), (PUZZLE, None),
    (PUZZLE, PUZZLE),                                   # incomplete solution
    (PUZZLE, SOLVED[:40] + "." + SOLVED[41:]),
    (PUZZLE, SOLVED[:40] + "0" + SOLVED[41:]),
    ("." * 81, "." * 81),
    ("4" + PUZZLE[1:], SOLVED),                         # solution conflicts with a clue
    (PUZZLE[:-1] + "1", SOLVED),
])
def test_invalid_inputs_are_rejected_before_the_engine_and_before_writing(tmp_path, puzzle, solution):
    engine, target = StubEngine(VALID), tmp_path / "out.svg"
    with pytest.raises(InvalidArgumentError):
        export_solution_svg(engine, puzzle, solution, target)
    assert engine.calls == [] and not target.exists()


@pytest.mark.parametrize("options", [{"theme": "nope"}, {"cell_size": 1}, {"margin": 10**6}, {"cell_size": True}])
def test_bad_render_options_are_rejected_before_the_engine(tmp_path, options):
    engine, target = StubEngine(VALID), tmp_path / "out.svg"
    with pytest.raises(InvalidArgumentError):
        export_solution_svg(engine, PUZZLE, SOLVED, target, **options)
    assert engine.calls == [] and not target.exists()


@pytest.mark.parametrize("destination", [5, object(), b"x.svg", 3.5])
def test_bad_destination_is_rejected_before_the_engine(destination):
    engine = StubEngine(VALID)
    with pytest.raises(InvalidArgumentError):
        export_solution_svg(engine, PUZZLE, SOLVED, destination)
    assert engine.calls == []


# ---- engine verdicts -------------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("status", [s for s in VerifyStatus if s is not VerifyStatus.VALID])
def test_engine_fault_is_raised_and_nothing_is_written(tmp_path, status):
    target, stream = tmp_path / "out.svg", io.StringIO()
    result = fault(status)
    for dest in (target, stream):
        with pytest.raises(OperationNotSuccessfulError) as info:
            export_solution_svg(StubEngine(result), PUZZLE, SOLVED, dest)
        assert info.value.result is result
    assert not target.exists() and stream.getvalue() == ""


def test_contradictory_verification_results_are_not_ignored(tmp_path):
    target = tmp_path / "out.svg"
    for bad in (VerifyResult(VerifyStatus.VALID, True, False, None, None), VerifyResult(VerifyStatus.VALID, False, True, None, None)):
        with pytest.raises(ResponseFormatError):
            export_solution_svg(StubEngine(bad), PUZZLE, SOLVED, target)
    assert not target.exists()


@pytest.mark.parametrize("error", [
    EngineTimeoutError(1.0, ("x",)), EngineProcessError("boom", 9), RequestRejectedError("bad", "nope"),
    ResponseFormatError("junk"), OSError("pipe")])
def test_engine_errors_propagate_unchanged_without_retry(tmp_path, error):
    engine, target = StubEngine(error=error), tmp_path / "out.svg"
    with pytest.raises(type(error)) as info:
        export_solution_svg(engine, PUZZLE, SOLVED, target)
    assert info.value is error and len(engine.calls) == 1 and not target.exists()


# ---- real client against the fake executable (no retry, nothing written) ----------------------------------------------------

def test_timeout_through_the_client(fake, tmp_path):
    engine, counts = fake(sleep=30, timeout=0.5)
    target = tmp_path / "out.svg"
    with pytest.raises(EngineTimeoutError):
        export_solution_svg(engine, PUZZLE, SOLVED, target)
    assert not target.exists() and len(counts.read_bytes().splitlines()) == 1


def test_process_failure_through_the_client(fake, tmp_path):
    engine, counts = fake(exit=7, stdout_text="garbage", stderr="oops")
    target = tmp_path / "out.svg"
    with pytest.raises(EngineProcessError):
        export_solution_svg(engine, PUZZLE, SOLVED, target)
    assert not target.exists() and len(counts.read_bytes().splitlines()) == 1


def test_garbage_success_output_through_the_client(fake, tmp_path):
    engine, _ = fake(exit=0, stdout_text="not json\n")
    with pytest.raises(ResponseFormatError):
        export_solution_svg(engine, PUZZLE, SOLVED, tmp_path / "out.svg")
    assert not (tmp_path / "out.svg").exists()


def canned(status, success, result):
    return json.dumps({"schema": "sudoku.response/1", "op": "verify", "ok": True, "success": success, "status": status,
                       "result": result}) + "\n"


def test_valid_response_through_the_client(fake, tmp_path):
    engine, counts = fake(exit=0, stdout_text=canned("valid", True, {"valid": True}))
    target = tmp_path / "out.svg"
    text = export_solution_svg(engine, PUZZLE, SOLVED, target)
    assert text == render_board_svg(SOLVED, original=PUZZLE) == target.read_text()
    assert len(counts.read_bytes().splitlines()) == 1


def test_fault_response_through_the_client_writes_nothing(fake, tmp_path):
    engine, _ = fake(exit=1, stdout_text=canned("row_violation", False,
                                                 {"valid": False, "fault": "row_violation", "index": 4, "digit": 3}))
    target = tmp_path / "out.svg"
    with pytest.raises(OperationNotSuccessfulError) as info:
        export_solution_svg(engine, PUZZLE, SOLVED, target)
    assert info.value.result.status is VerifyStatus.ROW_VIOLATION and info.value.result.index == 4
    assert not target.exists()


# ---- destination errors -------------------------------------------------------------------------------------------------------

class BrokenStream:
    def write(self, text):
        raise OSError("disk full")


def test_stream_that_raises_propagates():
    with pytest.raises(OSError, match="disk full"):
        export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, BrokenStream())


def test_unwritable_path_propagates_and_existing_file_survives_validation_failure(tmp_path):
    with pytest.raises(OSError):
        export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, tmp_path / "missing" / "o.svg")
    with pytest.raises(OSError):
        export_solution_svg(StubEngine(VALID), PUZZLE, SOLVED, tmp_path)  # a directory
    keep = tmp_path / "keep.svg"
    keep.write_text("keep")
    with pytest.raises(InvalidArgumentError):
        export_solution_svg(StubEngine(VALID), PUZZLE, PUZZLE, keep)
    with pytest.raises(OperationNotSuccessfulError):
        export_solution_svg(StubEngine(fault(VerifyStatus.ROW_VIOLATION)), PUZZLE, SOLVED, keep)
    assert keep.read_text() == "keep"


# ---- real engine ------------------------------------------------------------------------------------------------------------

def test_real_engine_export(engine_executable, tmp_path):
    engine = SudokuEngine(engine_executable, timeout=120)
    generated = engine.generate_puzzle(2024)
    target = tmp_path / "solution.svg"
    svg = export_solution_svg(engine, generated.puzzle, generated.solution, target, theme="dark")
    assert target.read_text(encoding="utf-8") == svg
    root = ET.fromstring(svg)
    ts = list(root.iter(NS + "text"))
    assert "".join(t.text for t in ts) == generated.solution
    assert [t.get("class") == "given" for t in ts] == [c != "." for c in generated.puzzle]
    assert export_solution_svg(engine, generated.puzzle, generated.solution, theme="dark") == svg


def test_real_engine_rejects_a_grid_that_breaks_the_rules(engine_executable, tmp_path):
    engine = SudokuEngine(engine_executable, timeout=120)
    target = tmp_path / "bad.svg"
    with pytest.raises(OperationNotSuccessfulError) as info:
        export_solution_svg(engine, "53" + "." * 79, BROKEN, target)
    assert info.value.result.status in (VerifyStatus.ROW_VIOLATION, VerifyStatus.COLUMN_VIOLATION, VerifyStatus.BOX_VIOLATION)
    assert not target.exists()
    # swapping two digits in a valid grid keeps all clues yet breaks the rules
    swapped = SOLVED[:2] + SOLVED[3] + SOLVED[2] + SOLVED[4:]
    with pytest.raises(OperationNotSuccessfulError):
        export_solution_svg(engine, "." * 81, swapped, target)
    assert not target.exists()
