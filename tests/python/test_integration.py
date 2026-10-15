"""M21 cross-component flows. Sections A-E use the real engine (fixed seeds, bounded work); F uses the fake
executable for deterministic failure paths and always checks that nothing is retried."""
from __future__ import annotations

import json
import xml.etree.ElementTree as ET

import pytest

from sudoku_app import (PuzzleReport, SudokuEngine, errors, export_solution_svg, models, render_board_svg)
from sudoku_app.models import CountStatus, LogicalStatus, SearchStatus, SolutionClass, TargetedStatus, VerifyStatus
from conftest import invocations

NS = "{http://www.w3.org/2000/svg}"
EASY = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
EASY_SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
EMPTY = "." * 81
# consistent (no repeated digit in a unit) yet unsolvable: row 0 holds 1-8 so cell 8 needs 9, but column 8 already has a 9
UNSOLVABLE = "12345678." + "........9" + "." * 63


@pytest.fixture(scope="module")
def engine(engine_executable):
    return SudokuEngine(engine_executable, timeout=120)


def clues(board):
    return {i: c for i, c in enumerate(board) if c not in ".0"}


# ---- A: generation -> Python validation -> engine verification -------------------------------------------------------------

@pytest.mark.parametrize("seed", [0, 7, 2**64 - 1])
def test_a_generated_puzzle_is_valid_unique_and_keeps_its_clues(engine, seed):
    gen = engine.generate_puzzle(seed, target_clues=30)
    assert gen.seed == seed and gen.success
    assert clues(gen.puzzle).items() <= clues(gen.solution).items() and len(gen.solution) == 81 and "." not in gen.solution
    assert gen.clue_count == len(clues(gen.puzzle))
    verdict = engine.verify(gen.puzzle, gen.solution)
    assert verdict.success and verdict.valid and verdict.status is VerifyStatus.VALID
    count = engine.count_solutions(gen.puzzle, limit=2)
    assert count.exact and count.count == 1 and count.classification is SolutionClass.UNIQUE and count.first_solution == gen.solution
    assert engine.generate_puzzle(seed, target_clues=30) == gen          # reproducible, including 64-bit seeds


def test_a_grid_seed_round_trip_is_exact(engine):
    for seed in (0, 2**53 + 1, 2**64 - 1):
        grid = engine.generate_grid(seed)
        assert grid.seed == seed and engine.verify(EMPTY, grid.grid).valid


# ---- B: search and logical solving ----------------------------------------------------------------------------------------

def test_b_search_and_logical_solve_a_known_puzzle(engine):
    searched = engine.solve_search(EASY)
    assert searched.status is SearchStatus.SOLVED and searched.solution == EASY_SOLVED
    logical = engine.solve_logical(EASY, include_trace=True)
    assert logical.status is LogicalStatus.SOLVED and logical.board == EASY_SOLVED and logical.steps
    assert all(step.technique in logical.by_technique for step in logical.steps)


def test_b_a_stalled_logical_solve_is_not_a_success_and_never_guesses(engine):
    stalled = engine.solve_logical(EMPTY)
    assert stalled.status is LogicalStatus.STALLED and not stalled.success and stalled.board == EMPTY
    assert stalled.deductions == stalled.placements == 0
    assert engine.solve_search(EMPTY).success                           # the search solver can; the logical one must not
    limited = engine.solve_logical(EASY, techniques=["hidden_single"])
    assert limited.status in (LogicalStatus.SOLVED, LogicalStatus.STALLED)
    assert limited.success == (limited.status is LogicalStatus.SOLVED)
    with pytest.raises(errors.OperationNotSuccessfulError):
        stalled.require_success()


def test_b_bad_input_is_never_reinterpreted_as_success(engine):
    conflict = "11" + "." * 79
    for result in (engine.solve_search(conflict), engine.solve_logical(conflict), engine.count_solutions(conflict),
                   engine.classify(conflict)):
        assert not result.success and result.status.value in ("invalid_input", "contradiction")
    assert engine.solve_search(UNSOLVABLE).status is SearchStatus.UNSOLVABLE
    assert engine.solve_search(UNSOLVABLE).solution is None
    assert not engine.solve_logical(UNSOLVABLE).success
    assert not engine.classify(UNSOLVABLE).success and engine.classify(UNSOLVABLE).profile is None
    with pytest.raises(errors.InvalidArgumentError):
        engine.solve_search("x" * 81)                                    # rejected before any process starts
    with pytest.raises(errors.RequestRejectedError):
        engine.request("solve_search", {"puzzle": "x" * 81})             # raw layer: the engine itself rejects it


# ---- C: counting and uniqueness -------------------------------------------------------------------------------------------

def test_c_exact_versus_bounded_counts(engine):
    exact = engine.count_solutions(EASY)
    assert exact.exact and exact.count == 1 and exact.classification is SolutionClass.UNIQUE
    bounded = engine.count_solutions(EMPTY, limit=3)
    assert bounded.status is CountStatus.LIMIT_REACHED and not bounded.exact and bounded.count == bounded.limit == 3
    assert bounded.classification is SolutionClass.MULTIPLE
    only_one = engine.count_solutions(EASY, limit=1)                     # the single solution found hides uniqueness
    assert only_one.count == 1 and not only_one.exact and only_one.classification is SolutionClass.UNKNOWN
    none = engine.count_solutions(UNSOLVABLE)
    assert none.exact and none.count == 0 and none.classification is SolutionClass.NO_SOLUTION and none.first_solution is None
    cut = engine.count_solutions(EMPTY, node_budget=5)
    assert cut.status is CountStatus.BUDGET_EXHAUSTED and not cut.exact and cut.classification is SolutionClass.UNKNOWN


def test_c_an_inconclusive_count_never_becomes_a_uniqueness_claim(engine):
    gen = engine.generate_puzzle(3, target_clues=30)
    inconclusive = [engine.count_solutions(gen.puzzle, limit=1), engine.count_solutions(gen.puzzle, node_budget=1)]
    for count in inconclusive:
        assert not count.exact
        report = PuzzleReport(gen, count, engine.verify(gen.puzzle, gen.solution), engine.classify(gen.puzzle))
        assert not report.is_unique                                      # only an exact count of one counts
    full = PuzzleReport(gen, engine.count_solutions(gen.puzzle), engine.verify(gen.puzzle, gen.solution), engine.classify(gen.puzzle))
    assert full.is_unique and full.solution_verified


def test_c_export_does_not_claim_uniqueness(engine, tmp_path):
    svg = export_solution_svg(engine, EMPTY, engine.generate_grid(1).grid, tmp_path / "grid.svg")   # a puzzle with no clues
    assert "unique" not in svg.lower()
    assert engine.count_solutions(EMPTY, limit=2).classification is SolutionClass.MULTIPLE         # and it is not unique


# ---- D: targeted generation and classification ----------------------------------------------------------------------------

def test_d_targeted_puzzle_passes_the_documented_checks(engine):
    first = engine.generate_targeted(5, "Easy", max_attempts=50)
    assert first.status is TargetedStatus.GENERATED and first.success and first.classification.profile == "Easy"
    assert first.attempts >= 1 and first.attempt_seed is not None
    assert engine.generate_targeted(5, "Easy", max_attempts=50) == first        # deterministic: same seed, same attempts
    assert engine.classify(first.puzzle).profile == "Easy"
    assert engine.classify(first.puzzle).metrics == models.ClassifyResult.from_envelope(
        engine.request("classify", {"puzzle": first.puzzle})).metrics
    count = engine.count_solutions(first.puzzle)
    assert count.exact and count.count == 1 and count.first_solution == first.solution
    assert engine.verify(first.puzzle, first.solution).valid
    assert first.clue_count == len(clues(first.puzzle))


def test_d_exhaustion_returns_no_substitute(engine):
    result = engine.generate_targeted(1, "Master", max_attempts=2)
    assert result.status is TargetedStatus.EXHAUSTED and not result.success
    assert result.attempts == 2 == result.max_attempts
    assert result.puzzle is None and result.solution is None and result.classification is None and result.clue_count is None
    assert result.rejections.total() == result.attempts and result.attempt_seed is not None
    with pytest.raises(errors.OperationNotSuccessfulError):
        result.require_success()
    assert engine.generate_targeted(1, "Master", max_attempts=2) == result       # exhaustion is reproducible too


# ---- E: rendering and export through the real engine -----------------------------------------------------------------------

def test_e_export_end_to_end(engine, tmp_path):
    gen = engine.generate_puzzle(11, target_clues=32)
    target = tmp_path / "solution.svg"
    svg = export_solution_svg(engine, gen.puzzle, gen.solution, target, theme="dark")
    assert target.read_text(encoding="utf-8") == svg
    texts = list(ET.fromstring(svg).iter(NS + "text"))
    assert "".join(t.text for t in texts) == gen.solution
    assert [t.get("class") for t in texts] == ["given" if c != "." else "filled" for c in gen.puzzle]
    assert {t.get("font-weight") for t in texts if t.get("class") == "given"} == {"bold"}
    assert svg == render_board_svg(gen.solution, original=gen.puzzle, theme="dark")


def test_e_rule_breaking_grids_are_refused_and_nothing_is_written(engine, tmp_path):
    target = tmp_path / "bad.svg"
    swapped = EASY_SOLVED[:2] + EASY_SOLVED[3] + EASY_SOLVED[2] + EASY_SOLVED[4:]
    cases = [(EMPTY, swapped, {VerifyStatus.ROW_VIOLATION, VerifyStatus.COLUMN_VIOLATION, VerifyStatus.BOX_VIOLATION}),
             (EMPTY, "1" * 81, {VerifyStatus.ROW_VIOLATION})]
    for puzzle, grid, faults in cases:
        with pytest.raises(errors.OperationNotSuccessfulError) as info:
            export_solution_svg(engine, puzzle, grid, target)
        assert info.value.result.status in faults and info.value.result.index is not None
    assert not target.exists()


# ---- F: process and protocol failures through the public API ---------------------------------------------------------------

def count_runs(counts):
    return len(invocations(counts))


def test_f_invalid_requests_start_no_process(fake):
    engine, counts = fake(exit=0, stdout_text="{}")
    for call in (lambda: engine.generate_grid(-1), lambda: engine.generate_grid(2**64), lambda: engine.generate_grid(True),
                 lambda: engine.solve_search("x"), lambda: engine.count_solutions(EASY, limit=0),
                 lambda: engine.generate_targeted(1, ""), lambda: engine.capabilities(request_id="")):
        with pytest.raises(errors.InvalidArgumentError):
            call()
    assert count_runs(counts) == 0


GOOD = {"schema": "sudoku.response/1", "op": "generate_grid", "ok": True, "success": True, "status": "generated",
        "result": {"seed": "5", "grid": EASY_SOLVED, "stats": {"nodes": 1, "assignments": 1, "backtracks": 0}}}


@pytest.mark.parametrize("text,error", [
    ("not json\n", errors.ResponseFormatError),
    ("", errors.ResponseFormatError),
    ('{"a":1,"a":2}\n', errors.ResponseFormatError),                                          # duplicate key
    (json.dumps(GOOD)[:-1] + ',"ok":false}\n', errors.ResponseFormatError),                   # duplicate key inside a response
    (json.dumps(GOOD | {"status": "mystery"}) + "\n", errors.ResponseFormatError),            # unknown status
    (json.dumps(GOOD | {"success": False}) + "\n", errors.ResponseFormatError),               # success flag vs exit code/status
    (json.dumps(GOOD | {"id": "other"}) + "\n", errors.ResponseFormatError),                  # id the request never had
    (json.dumps(GOOD) + "\n" + json.dumps(GOOD) + "\n", errors.ResponseFormatError),          # two responses
    (json.dumps(GOOD | {"extra": 1}) + "\n", errors.ResponseFormatError),                     # unknown field
    (json.dumps(GOOD | {"result": GOOD["result"] | {"seed": "6"}}) + "\n", errors.ResponseFormatError),   # seed of another request
    (json.dumps(GOOD | {"result": GOOD["result"] | {"seed": 5}}) + "\n", errors.ResponseFormatError),     # seed as JSON number
])
def test_f_unusable_output_is_a_format_error_and_is_not_retried(fake, text, error):
    engine, counts = fake(exit=0, stdout_text=text)
    with pytest.raises(error):
        engine.generate_grid(5)
    assert count_runs(counts) == 1


def test_f_request_id_mismatch(fake):
    engine, counts = fake(exit=0, stdout_text=json.dumps(GOOD | {"id": "b"}) + "\n")
    with pytest.raises(errors.ResponseFormatError, match="does not match"):
        engine.generate_grid(5, request_id="a")
    assert count_runs(counts) == 1
    engine, _ = fake(exit=0, stdout_text=json.dumps(GOOD) + "\n")                              # id dropped by the engine
    with pytest.raises(errors.ResponseFormatError):
        engine.generate_grid(5, request_id="a")


def test_f_exit_codes_and_engine_errors(fake):
    err = {"schema": "sudoku.response/1", "op": "generate_grid", "ok": False,
           "error": {"code": "invalid_field", "message": "bad", "field": "params.seed"}}
    engine, counts = fake(exit=2, stdout_text=json.dumps(err) + "\n")
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.generate_grid(5)
    assert (info.value.code, info.value.field) == ("invalid_field", "params.seed")
    engine, _ = fake(exit=3, stdout_text=json.dumps({**err, "error": {"code": "internal_error", "message": "boom"}}) + "\n")
    with pytest.raises(errors.EngineInternalError):
        engine.generate_grid(5)
    engine, _ = fake(exit=2, stdout_text=json.dumps(err | {"error": {"code": "internal_error", "message": "x"}}) + "\n")
    with pytest.raises(errors.ResponseFormatError):                                           # code and exit code disagree
        engine.generate_grid(5)


@pytest.mark.parametrize("code", [4, 9, 255])
def test_f_undefined_exit_codes_are_process_errors(fake, code):
    engine, counts = fake(exit=code, stdout_text="")
    with pytest.raises(errors.EngineProcessError) as info:
        engine.generate_grid(5)
    assert info.value.exit_code == code and count_runs(counts) == 1


def test_f_non_success_outcomes_are_results_not_exceptions(fake):
    stalled = {"schema": "sudoku.response/1", "op": "solve_logical", "ok": True, "success": False, "status": "stalled",
               "result": {"reason": "no_applicable_deduction", "board": EMPTY,
                          "stats": {"deductions": 0, "placements": 0, "eliminated_candidates": 0, "by_technique": {"naked_single": 0}}}}
    engine, counts = fake(exit=1, stdout_text=json.dumps(stalled) + "\n")
    result = engine.solve_logical(EMPTY)
    assert not result.success and result.status is LogicalStatus.STALLED and count_runs(counts) == 1
    engine, _ = fake(exit=0, stdout_text=json.dumps(stalled) + "\n")                           # success=False with exit 0
    with pytest.raises(errors.ResponseFormatError):
        engine.solve_logical(EMPTY)


def test_f_timeout_and_missing_executable(fake, tmp_path):
    engine, counts = fake(sleep=30, timeout=0.5)
    with pytest.raises(errors.EngineTimeoutError):
        engine.generate_grid(5)
    assert count_runs(counts) == 1
    with pytest.raises(errors.ExecutableNotFoundError):
        SudokuEngine(tmp_path / "does-not-exist")


def test_f_stdout_and_stderr_stay_separate(fake):
    engine, _ = fake(exit=0, stdout_text=json.dumps(GOOD) + "\n", stderr="warning on stderr")
    assert engine.generate_grid(5).seed == 5                                                   # stderr noise is not output
    engine, _ = fake(exit=0, stdout_text="warning\n" + json.dumps(GOOD) + "\n")
    with pytest.raises(errors.ResponseFormatError):                                           # stdout noise is a violation
        engine.generate_grid(5)
