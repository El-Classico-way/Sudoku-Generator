"""The Python client against the REAL compiled engine.

Needs the built executable (cmake --preset dev && cmake --build --preset dev, or
SUDOKU_CLI=/path/to/sudoku_cli). If it is missing these tests FAIL with build
instructions instead of being skipped. All workloads are small and use fixed seeds.
Results are checked against Sudoku rules independently in this file (plain
constraint checks, not a solver).
"""
import time

import pytest

import sudoku_app
from sudoku_app import PuzzleReport, SudokuEngine, describe_puzzle, errors, generate_puzzle_report, models, protocol
from sudoku_app.models import PuzzleResult, SolutionClass
from sudoku_app.runner import EngineRunner

EASY = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
EASY_SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
MEDIUM = "5...2.8671...........45..1......7....6......443....2..81.6........179.5..79..5..1"
STALLED = ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4."
TWO_SOLUTIONS = "534008912672195348198342567859001423426853791713924856961537284287419635345286179"
DUPLICATE = "55" + "." * 79


@pytest.fixture(scope="module")
def engine(engine_executable):
    return SudokuEngine(engine_executable, timeout=120)


def digits(board):
    return [0 if c == "." else int(c) for c in board]


def is_valid_grid(board):
    """Independent Sudoku-rule check of a COMPLETE board (not a solver)."""
    g = digits(board)
    rows = [g[i * 9:(i + 1) * 9] for i in range(9)]
    cols = [g[i::9] for i in range(9)]
    boxes = [[g[(r + dr) * 9 + c + dc] for dr in range(3) for dc in range(3)] for r in (0, 3, 6) for c in (0, 3, 6)]
    return all(sorted(u) == list(range(1, 10)) for u in rows + cols + boxes)


def keeps_givens(puzzle, solution):
    """Every given of the puzzle ('.' and '0' both mean empty) is unchanged in the solution."""
    return all(p in ".0" or p == s for p, s in zip(puzzle, solution))


# ------------------------------------------------------------ capabilities

def test_capabilities(engine):
    caps = engine.capabilities(request_id="caps-1")
    assert caps.engine_version == sudoku_app.__version__
    assert set(caps.operations) == set(protocol.OPERATIONS)
    assert [p.name for p in caps.profiles] == ["Easy", "Medium", "Hard", "Expert", "Master"]
    assert caps.profiles[0].techniques == ("naked_single", "hidden_single")
    assert caps.response_schema == protocol.RESPONSE_SCHEMA and caps.request_schemas == (protocol.REQUEST_SCHEMA,)
    assert caps.board_length == 81 and caps.board_output_empty == "."
    # the client's early-validation ranges agree with what the engine reports
    assert caps.limits["max_attempts"] == protocol.MAX_ATTEMPTS and caps.limits["max_count_limit"] == protocol.MAX_COUNT_LIMIT
    assert caps.limits["max_node_budget"] == protocol.MAX_NODE_BUDGET and caps.limits["max_id_length"] == protocol.MAX_ID_LENGTH


# --------------------------------------------------------------- generation

@pytest.mark.parametrize("seed", [0, 1, 2**53 + 1, 2**64 - 1])
def test_generate_grid_is_valid_deterministic_and_keeps_the_seed(engine, seed):
    g = engine.generate_grid(seed)
    assert g.success and g.status is models.GridStatus.GENERATED
    assert g.seed == seed                                              # the full 64 bits survive the round trip
    assert is_valid_grid(g.grid) and "." not in g.grid
    assert engine.generate_grid(seed) == g                             # same seed, same grid
    assert g.stats.nodes >= 82


def test_different_seeds_give_different_grids(engine):
    assert engine.generate_grid(1).grid != engine.generate_grid(2).grid


def test_generate_puzzle_with_a_fixed_seed(engine):
    p = engine.generate_puzzle(7, target_clues=30, request_id="p7")
    assert p.success and p.status is models.PuzzleStatus.TARGET_REACHED and p.target_reached
    assert (p.seed, p.requested_clues, p.clue_count) == (7, 30, 30)
    assert is_valid_grid(p.solution) and keeps_givens(p.puzzle, p.solution)
    assert p.clue_count == sum(c != "." for c in p.puzzle)
    assert engine.generate_puzzle(7, target_clues=30, request_id="p7") == p     # reproducible, options preserved
    assert engine.generate_puzzle(8, target_clues=30).puzzle != p.puzzle
    # the engine's own exhaustive count says: exactly one solution, and it is p.solution
    c = engine.count_solutions(p.puzzle, limit=2)
    assert c.exact and c.count == 1 and c.classification is SolutionClass.UNIQUE and c.first_solution == p.solution


def test_default_clue_target_is_reported_honestly(engine):
    p = engine.generate_puzzle(7)            # the engine default (17) is not reachable by one pass
    assert p.success and not p.target_reached and p.status is models.PuzzleStatus.TARGET_NOT_REACHED
    assert p.requested_clues == 17 and p.clue_count > 17


def test_node_budget_is_passed_through(engine):
    starved = engine.generate_puzzle(7, node_budget=1)       # no removal can be proven unique: all clues are kept
    assert starved.clue_count == 81 and starved.removed == 0 and starved.rejected_inconclusive > 0


# ------------------------------------------------------------------ solving

def test_search_solve(engine):
    r = engine.solve_search(EASY)
    assert r.success and r.status is models.SearchStatus.SOLVED
    assert r.solution == EASY_SOLVED and is_valid_grid(r.solution) and keeps_givens(EASY, r.solution)
    multi = engine.solve_search(TWO_SOLUTIONS)
    assert multi.success and is_valid_grid(multi.solution) and keeps_givens(TWO_SOLUTIONS, multi.solution)   # a solution, not a uniqueness claim
    bad = engine.solve_search(DUPLICATE)                      # a non-success outcome is a typed result, not an exception
    assert not bad.success and bad.status is models.SearchStatus.INVALID_INPUT and bad.solution is None
    starved = engine.solve_search(EASY, node_budget=1)
    assert not starved.success and starved.status is models.SearchStatus.BUDGET_EXHAUSTED and starved.solution is None
    with pytest.raises(errors.OperationNotSuccessfulError):
        starved.require_success()


def test_count_solutions_semantics(engine):
    unique = engine.count_solutions(EASY)
    assert unique.status is models.CountStatus.EXHAUSTED and unique.exact and unique.count == 1
    assert unique.classification is SolutionClass.UNIQUE and unique.first_solution == EASY_SOLVED
    capped = engine.count_solutions(TWO_SOLUTIONS)                 # default limit 2: a lower bound
    assert capped.status is models.CountStatus.LIMIT_REACHED and capped.success and not capped.exact
    assert capped.count == 2 and capped.limit == 2 and capped.classification is SolutionClass.MULTIPLE
    exact = engine.count_solutions(TWO_SOLUTIONS, limit=10)
    assert exact.exact and exact.count == 2 and exact.status is models.CountStatus.EXHAUSTED
    one = engine.count_solutions(EASY, limit=1)                    # found one, but uniqueness is NOT established
    assert one.status is models.CountStatus.LIMIT_REACHED and not one.exact and one.classification is SolutionClass.UNKNOWN
    cut = engine.count_solutions(MEDIUM, node_budget=1)            # interrupted: inconclusive, and not a success
    assert cut.status is models.CountStatus.BUDGET_EXHAUSTED and not cut.success and cut.classification is SolutionClass.UNKNOWN
    assert engine.count_solutions(DUPLICATE).status is models.CountStatus.INVALID_INPUT
    empty = engine.count_solutions("." * 81, limit=5)              # bounded, not enumerated
    assert empty.count == 5 and not empty.exact


def test_logical_solve(engine):
    r = engine.solve_logical(EASY, include_trace=True, request_id="logic")
    assert r.success and r.status is models.LogicalStatus.SOLVED and r.board == EASY_SOLVED
    assert r.deductions == r.placements == 51 and r.eliminated_candidates == 0 and len(r.steps) == 51
    assert all(s.action == "place" and len(s.targets) == 1 for s in r.steps)
    # replaying the placements reproduces the solution: the trace is consistent with the board
    board = list(EASY)
    for step in r.steps:
        ((cell, digit),) = step.targets
        assert board[cell] == "."
        board[cell] = str(digit)
    assert "".join(board) == EASY_SOLVED
    assert engine.solve_logical(EASY).steps is None                # no trace unless requested


def test_logical_solver_stalls_without_guessing(engine):
    s = engine.solve_logical(STALLED)
    assert not s.success and s.status is models.LogicalStatus.STALLED and s.reason == "no_applicable_deduction"
    assert "." in s.board and keeps_givens(STALLED, s.board)       # progress is consistent with the givens, but it is not a solution
    only_singles = engine.solve_logical(MEDIUM, techniques=["naked_single", "hidden_single"])
    assert only_singles.status is models.LogicalStatus.STALLED and only_singles.by_technique["pointing"] == 0
    assert engine.solve_logical(MEDIUM).status is models.LogicalStatus.SOLVED
    assert engine.solve_logical(DUPLICATE).status is models.LogicalStatus.INVALID_INPUT


def test_verify(engine):
    ok = engine.verify(EASY, EASY_SOLVED)
    assert ok.success and ok.valid and ok.status is models.VerifyStatus.VALID
    hole = engine.verify(EASY, EASY_SOLVED[:40] + "." + EASY_SOLVED[41:])
    assert not hole.success and hole.status is models.VerifyStatus.EMPTY_CELL and hole.index == 40
    changed = engine.verify(EASY_SOLVED, "534768912672195348198342567859671423426853791713924856961537284287419635345286179")
    assert changed.status is models.VerifyStatus.GIVEN_CHANGED and changed.index == 3 and changed.digit == 6   # valid grid, wrong givens
    assert engine.verify("0" * 81, EASY_SOLVED).success                # '0' spells an empty given


# ----------------------------------------------------------- classification

def test_classification(engine):
    easy = engine.classify(EASY)
    assert easy.success and easy.status is models.ClassifyStatus.CLASSIFIED and easy.profile == "Easy" and easy.profile_index == 0
    assert easy.metrics.deductions == 51 and easy.metrics.hardest_level == 1 and easy.metrics.hardest_technique is not None
    assert engine.classify(MEDIUM).profile == "Medium"
    stalled = engine.classify(STALLED)
    assert not stalled.success and stalled.status is models.ClassifyStatus.UNSOLVED
    assert stalled.profile is None and stalled.metrics is None         # never labelled
    invalid = engine.classify(DUPLICATE)
    assert invalid.status is models.ClassifyStatus.INVALID_INPUT and invalid.profile is None


# --------------------------------------------------------------- targeted

def test_targeted_generation_exact_match(engine):
    t = engine.generate_targeted(1, "Easy", max_attempts=60, request_id="t-easy")
    assert t.success and t.status is models.TargetedStatus.GENERATED and t.target == "Easy" and t.seed == 1
    assert 1 <= t.attempts <= t.max_attempts == 60
    assert t.rejections.total() + 1 == t.attempts
    assert t.classification.profile == "Easy" and is_valid_grid(t.solution) and keeps_givens(t.puzzle, t.solution)
    assert t.clue_count == sum(c != "." for c in t.puzzle)
    # independent confirmation through other engine operations: exactly one solution, exactly this difficulty
    c = engine.count_solutions(t.puzzle)
    assert c.exact and c.count == 1 and c.first_solution == t.solution
    assert engine.classify(t.puzzle).profile == "Easy"
    assert engine.verify(t.puzzle, t.solution).success
    assert engine.generate_targeted(1, "Easy", max_attempts=60, request_id="t-easy") == t


def test_targeted_exhaustion_is_a_typed_outcome(engine):
    t = engine.generate_targeted(1, "Master", max_attempts=3)
    assert not t.success and t.status is models.TargetedStatus.EXHAUSTED
    assert t.puzzle is None and t.solution is None and t.classification is None and t.clue_count is None   # no fallback of another difficulty
    assert t.attempts == t.max_attempts == 3 and t.rejections.total() == 3 and t.last_rejection is not None
    assert t.attempt_seed is not None
    with pytest.raises(errors.OperationNotSuccessfulError) as info:
        t.require_success()
    assert info.value.result is t
    assert engine.generate_targeted(1, "Master", max_attempts=3) == t     # a failed bounded search repeats exactly


def test_targeted_full_range_seed(engine):
    t = engine.generate_targeted(2**64 - 1, "Easy", max_attempts=2)
    assert t.seed == 2**64 - 1 and t.attempt_seed is not None and 0 <= t.attempt_seed < 2**64


# ------------------------------------------------------------ real failures

def test_engine_rejects_invalid_requests(engine):
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.generate_targeted(1, "Legendary", request_id="bad-target")
    e = info.value
    assert (e.code, e.field, e.request_id) == ("invalid_field", "params.target", "bad-target")
    # the engine is authoritative even for things the Python side does not check
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.request("verify", {"puzzle": EASY, "solution": EASY_SOLVED, "extra": 1})
    assert (info.value.code, info.value.field) == ("unknown_field", "params.extra")
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.request("classify", {"puzzle": "x" * 81})
    assert info.value.code == "invalid_field" and info.value.field == "params.puzzle"
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.request("generate_grid", {"seed": 5})                  # a JSON number is not an accepted seed
    assert info.value.field == "params.seed"
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.request("generate_grid", {})
    assert info.value.code == "missing_field"


def test_unknown_operation_and_bad_json_through_the_real_engine(engine_executable):
    runner = EngineRunner(engine_executable)
    for raw, code in [(b'{"schema":"sudoku.request/1","op":"explode"}', "unknown_operation"),
                      (b'{"schema":"sudoku.request/9","op":"capabilities"}', "unsupported_schema"),
                      (b'{"schema"', "parse_error"), (b"", "empty_request")]:
        outcome = runner.run(raw)
        with pytest.raises(errors.RequestRejectedError) as info:
            protocol.parse_process_output(exit_code=outcome.exit_code, stdout=outcome.stdout, stderr=outcome.stderr,
                                          op="capabilities", request_id=None)
        assert info.value.code == code and outcome.exit_code == 2


def test_real_engine_timeout(engine_executable):
    slow = SudokuEngine(engine_executable, timeout=120)
    start = time.monotonic()
    with pytest.raises(errors.EngineTimeoutError):
        slow.generate_targeted(0, "Master", max_attempts=1_000_000, timeout=0.5)     # would run for minutes
    assert time.monotonic() - start < 15


def test_discovery_via_environment_variable(engine_executable, monkeypatch):
    monkeypatch.setenv("SUDOKU_CLI", str(engine_executable))
    assert SudokuEngine().argv == (str(engine_executable),)
    assert SudokuEngine().generate_grid(3).success


# ----------------------------------------------------------- orchestration

def test_puzzle_report_collects_the_engines_answers(engine):
    report = generate_puzzle_report(engine, 7, target_clues=30)
    assert isinstance(report, PuzzleReport)
    assert report.generation == engine.generate_puzzle(7, target_clues=30)       # seed and options preserved
    assert report.is_unique and report.solution_verified
    assert report.count.exact and report.count.count == 1
    assert report.profile == report.classification.profile and report.profile is not None
    assert report.puzzle == report.generation.puzzle and report.solution == report.generation.solution
    assert generate_puzzle_report(engine, 7, target_clues=30) == report         # reproducible end to end


def test_uniqueness_is_never_assumed(engine):
    # A hand-built "generation" for a puzzle with two solutions: only the engine's answers decide.
    fake_generation = PuzzleResult(models.PuzzleStatus.TARGET_REACHED, True, 0, 17, True, TWO_SOLUTIONS, EASY_SOLVED,
                                   sum(c != "." for c in TWO_SOLUTIONS), 0, 0, 0, 0)
    report = describe_puzzle(engine, fake_generation)
    assert report.count.count == 2 and not report.is_unique              # the engine found a second solution
    assert report.classification.profile is None or report.profile == report.classification.profile
    stalled = PuzzleResult(models.PuzzleStatus.TARGET_REACHED, True, 0, 17, True, STALLED, "529781634814623759736945218297316485341258976685479123178594362453162897962837541",
                           sum(c != "." for c in STALLED), 0, 0, 0, 0)
    report = describe_puzzle(engine, stalled)
    assert report.is_unique and report.profile is None                   # unique, but the engine gave it no difficulty label


# ----------------------------------------------------------- documentation

def test_readme_python_examples_run(engine_executable, monkeypatch, capsys):
    """Every ```python run block in the README and docs/REFERENCE.md executes without error against the real engine."""
    import re
    from pathlib import Path

    root = Path(__file__).resolve().parents[2]
    text = (root / "README.md").read_text() + "\n" + (root / "docs" / "REFERENCE.md").read_text()
    blocks = re.findall(r"```python run\n(.*?)\n```", text, re.S)
    assert len(blocks) >= 2, "the README examples were not found"
    monkeypatch.setenv("SUDOKU_CLI", str(engine_executable))      # the examples use default discovery
    for block in blocks:
        exec(compile(block, "<README example>", "exec"), {"__name__": "__readme__"})
    out = capsys.readouterr().out
    assert "0.1.0" in out and "invalid_field params.target" in out   # the documented output really appears
