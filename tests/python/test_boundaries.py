"""M21 boundary-contract tests: the client must not accept responses that contradict its own request or the
engine's documented invariants. Envelopes are fabricated (fake engine); the last section uses the real engine."""
from __future__ import annotations

import pytest

from sudoku_app import SudokuEngine, protocol
from sudoku_app.errors import EngineProcessError, ResponseFormatError
from sudoku_app.protocol import Envelope

P = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
S = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
OTHER = "123456789456789123789123456231674895875912364694538217317265948542897631968341572"
ST = {"nodes": 1, "assignments": 1, "backtracks": 0}
CLUES = sum(c != "." for c in P)


class Canned(SudokuEngine):
    """A client whose transport returns one prepared envelope (no process)."""

    def __init__(self, env):
        self.env = env

    def request(self, op, params=None, **kw):
        return self.env


def env(op, status, success, result):
    return Envelope(op, None, success, status, result, 0 if success else 1)


def rejected(call):
    with pytest.raises(ResponseFormatError):
        call()


# ---- echoed request values --------------------------------------------------------------------------------------

def grid(seed="5"):
    return {"seed": seed, "grid": S, "stats": ST}


def test_grid_echo_must_match_the_requested_seed():
    assert Canned(env("generate_grid", "generated", True, grid())).generate_grid(5).seed == 5
    rejected(lambda: Canned(env("generate_grid", "generated", True, grid("6"))).generate_grid(5))
    big = 2**64 - 1
    assert Canned(env("generate_grid", "generated", True, grid(str(big)))).generate_grid(big).seed == big
    rejected(lambda: Canned(env("generate_grid", "generated", True, grid(str(big - 1)))).generate_grid(big))


def puzzle_result(**k):
    return {"seed": "5", "requested_clues": CLUES, "target_reached": True, "puzzle": P, "solution": S, "clue_count": CLUES,
            "removal": {"cells_considered": 1, "removed": 1, "rejected_not_unique": 0, "rejected_inconclusive": 0}} | k


def test_generate_puzzle_echo_and_target_semantics():
    ok = Canned(env("generate_puzzle", "target_reached", True, puzzle_result())).generate_puzzle(5, target_clues=CLUES)
    assert ok.target_reached
    rejected(lambda: Canned(env("generate_puzzle", "target_reached", True, puzzle_result(seed="6"))).generate_puzzle(5))
    rejected(lambda: Canned(env("generate_puzzle", "target_reached", True, puzzle_result())).generate_puzzle(5, target_clues=40))
    # "reached" means exactly the requested number of clues; "not reached" means MORE clues than requested
    rejected(lambda: Canned(env("generate_puzzle", "target_reached", True, puzzle_result(requested_clues=20))).generate_puzzle(5))
    rejected(lambda: Canned(env("generate_puzzle", "target_not_reached", True,
                                puzzle_result(target_reached=False, requested_clues=CLUES))).generate_puzzle(5))
    rejected(lambda: Canned(env("generate_puzzle", "target_not_reached", True,
                                puzzle_result(target_reached=False, requested_clues=CLUES + 5))).generate_puzzle(5))
    assert Canned(env("generate_puzzle", "target_not_reached", True,
                      puzzle_result(target_reached=False, requested_clues=20))).generate_puzzle(5, target_clues=20).requested_clues == 20


def targeted(**k):
    classification = {"profile": "Easy", "profile_index": 0, "metrics": {
        "deductions": 1, "placements": 1, "eliminated_candidates": 0, "hardest_level": 1, "hardest_technique": "naked_single",
        "by_technique": {"naked_single": 1}}}
    return {"target": "Easy", "seed": "1", "attempts": 1, "max_attempts": 5, "attempt_seed": "7",
            "rejections": {"easier_than_target": 0, "harder_than_target": 0, "unsolved": 0, "degenerate": 0, "inconclusive": 0},
            "puzzle": P, "solution": S, "clue_count": CLUES, "classification": classification} | k


def test_generate_targeted_echo_and_internal_consistency():
    call = lambda result, **kw: Canned(env("generate_targeted", "generated", True, result)).generate_targeted(1, "Easy", **kw)
    assert call(targeted(), max_attempts=5).puzzle == P
    assert call(targeted()).max_attempts == 5                       # omitted option: nothing to compare with
    rejected(lambda: call(targeted(seed="2")))
    rejected(lambda: call(targeted(), max_attempts=6))
    rejected(lambda: Canned(env("generate_targeted", "generated", True, targeted())).generate_targeted(1, "Hard"))
    rejected(lambda: call(targeted(clue_count=CLUES + 1)))
    rejected(lambda: call(targeted(puzzle=OTHER[:80] + ".")))        # puzzle contradicts its solution
    exhausted = targeted(attempts=5, puzzle=None, solution=None, clue_count=None, classification=None,
                         rejections={"easier_than_target": 5, "harder_than_target": 0, "unsolved": 0, "degenerate": 0, "inconclusive": 0},
                         last_rejection="wrong_difficulty")
    for k in ("puzzle", "solution", "clue_count", "classification"):
        exhausted.pop(k)
    result = Canned(env("generate_targeted", "exhausted", False, exhausted)).generate_targeted(1, "Easy", max_attempts=5)
    assert result.puzzle is None and result.solution is None and not result.success
    rejected(lambda: Canned(env("generate_targeted", "exhausted", False, exhausted)).generate_targeted(2, "Easy"))


# ---- boards must keep the submitted givens -------------------------------------------------------------------------------

def test_search_solution_must_keep_the_givens():
    ok = Canned(env("solve_search", "solved", True, {"solution": S, "stats": ST})).solve_search(P)
    assert ok.solution == S
    rejected(lambda: Canned(env("solve_search", "solved", True, {"solution": OTHER, "stats": ST})).solve_search(P))


def logical(board, **k):
    return {"reason": "puzzle_solved", "board": board,
            "stats": {"deductions": 1, "placements": 1, "eliminated_candidates": 0, "by_technique": {"naked_single": 1}}} | k


def test_logical_board_must_keep_the_givens_in_every_status():
    rejected(lambda: Canned(env("solve_logical", "solved", True, logical(OTHER))).solve_logical(P))
    stalled_bad = OTHER.replace("1", ".")
    rejected(lambda: Canned(env("solve_logical", "stalled", False, logical(stalled_bad, reason="no_applicable_deduction"))).solve_logical(P))
    first_empty = P.index(".")
    stalled_ok = P[:first_empty] + S[first_empty] + P[first_empty + 1:]      # one placement, as the stats say
    assert Canned(env("solve_logical", "stalled", False, logical(stalled_ok, reason="no_applicable_deduction"))).solve_logical(P).board == stalled_ok


def count(**k):
    doc = {"limit": 2, "count": 1, "exact": True, "classification": "unique", "first_solution": S, "stats": ST} | k
    return {a: b for a, b in doc.items() if b is not None}      # the engine omits absent fields


@pytest.mark.parametrize("status,success,result", [
    ("exhausted", True, count(first_solution=OTHER)),                               # first solution contradicts the puzzle
    ("exhausted", True, count(count=0, classification="no_solution")),             # a solution although count is 0
    ("exhausted", True, count(first_solution=None)),                               # count 1 without a solution
    ("exhausted", True, count(classification="multiple")),                         # exact 1 is not 'multiple'
    ("exhausted", True, count(classification="no_solution")),
    ("exhausted", True, count(classification="unknown")),                          # exact counts are never 'unknown'
    ("exhausted", True, count(classification="invalid_input")),
    ("exhausted", True, count(count=2, limit=5, classification="unique")),
    ("exhausted", True, count(limit=1)),                                           # exhausted must be below the limit
    ("limit_reached", True, count(limit=5, count=2, exact=False, classification="multiple")),   # limit not reached
    ("limit_reached", True, count(limit=2, count=2, exact=False, classification="unique")),
    ("limit_reached", True, count(limit=2, count=2, exact=False, classification="unknown")),
    ("limit_reached", True, count(limit=1, count=1, exact=False, classification="multiple")),
    ("budget_exhausted", False, count(count=0, exact=False, classification="no_solution", first_solution=None)),
    ("budget_exhausted", False, count(count=1, exact=False, classification="unique")),
    ("budget_exhausted", False, count(count=0, exact=False, classification="unknown")),     # first_solution with count 0
    ("invalid_input", False, count(count=0, exact=False, classification="unknown", first_solution=None)),
    ("exhausted", True, count(count=0, classification="no_solution", first_solution=None, limit=0)),
])
def test_impossible_count_results_are_rejected(status, success, result):
    rejected(lambda: Canned(env("count_solutions", status, success, result)).count_solutions(P))


@pytest.mark.parametrize("status,success,result", [
    ("exhausted", True, count()),
    ("exhausted", True, count(count=0, classification="no_solution", first_solution=None)),
    ("exhausted", True, count(count=3, limit=5, classification="multiple")),
    ("limit_reached", True, count(limit=1, count=1, exact=False, classification="unknown")),
    ("limit_reached", True, count(limit=2, count=2, exact=False, classification="multiple")),
    ("budget_exhausted", False, count(count=0, exact=False, classification="unknown", first_solution=None)),
    ("budget_exhausted", False, count(count=1, exact=False, classification="unknown")),
    ("invalid_input", False, count(count=0, exact=False, classification="invalid_input", first_solution=None)),
])
def test_legitimate_count_outcomes_are_accepted(status, success, result):
    r = Canned(env("count_solutions", status, success, result)).count_solutions(P, limit=result["limit"])
    assert r.exact == (status == "exhausted")
    assert r.classification.value == result["classification"]


def test_count_limit_echo():
    rejected(lambda: Canned(env("count_solutions", "exhausted", True, count())).count_solutions(P, limit=5))
    assert Canned(env("count_solutions", "exhausted", True, count())).count_solutions(P, limit=2).limit == 2
    assert Canned(env("count_solutions", "exhausted", True, count())).count_solutions(P).limit == 2


# ---- raw protocol edge cases ---------------------------------------------------------------------------------------------

def parse(stdout, *, exit_code=0, request_id=None, op="capabilities"):
    return protocol.parse_process_output(exit_code=exit_code, stdout=stdout, stderr=b"", op=op, request_id=request_id)


def test_deeply_nested_output_is_a_format_error_not_a_crash():
    deep = b"[" * 200000 + b"\n"
    with pytest.raises(ResponseFormatError):
        parse(deep)
    with pytest.raises(EngineProcessError):
        parse(deep, exit_code=2)
    with pytest.raises(ResponseFormatError):
        parse(b'{"a":' * 100000 + b"\n")


def error_doc(**k):
    import json
    doc = {"schema": "sudoku.response/1", "op": "capabilities", "ok": False,
           "error": {"code": "invalid_field", "message": "m", "field": "f"}} | k
    return (json.dumps({a: b for a, b in doc.items() if b is not ...}) + "\n").encode()


def test_error_responses_must_agree_with_the_request():
    from sudoku_app.errors import RequestRejectedError
    with pytest.raises(RequestRejectedError):
        parse(error_doc(), exit_code=2)
    with pytest.raises(RequestRejectedError):
        parse(error_doc(id="a"), exit_code=2, request_id="a")
    with pytest.raises(RequestRejectedError):
        parse(error_doc(op=...), exit_code=2)                       # engine may omit op (e.g. parse errors)
    with pytest.raises(ResponseFormatError):
        parse(error_doc(id="zzz"), exit_code=2)                     # the request carried no id
    with pytest.raises(ResponseFormatError):
        parse(error_doc(id="b"), exit_code=2, request_id="a")
    with pytest.raises(ResponseFormatError):
        parse(error_doc(op="verify"), exit_code=2)                  # another operation's error


def test_logical_placements_must_match_the_digits_gained():
    ok = logical(S, stats={"deductions": 1, "placements": 51, "eliminated_candidates": 0, "by_technique": {"naked_single": 1}})
    assert Canned(env("solve_logical", "solved", True, ok)).solve_logical(P).board == S
    for wrong in (50, 52, 0):
        bad = logical(S, stats={"deductions": 1, "placements": wrong, "eliminated_candidates": 0, "by_technique": {"naked_single": 1}})
        rejected(lambda: Canned(env("solve_logical", "solved", True, bad)).solve_logical(P))


# ---- solution export: the verdict must be unmistakable, independent of the renderer ---------------------------------------

def test_export_refuses_every_verdict_that_is_not_plainly_valid(tmp_path):
    from sudoku_app import export_solution_svg
    from sudoku_app.errors import OperationNotSuccessfulError
    from sudoku_app.models import VerifyResult, VerifyStatus

    class Stub:
        def __init__(self, result):
            self.result = result

        def verify(self, puzzle, solution, *, timeout=None):
            return self.result

    target = tmp_path / "x.svg"
    contradictions = [
        VerifyResult(VerifyStatus.ROW_VIOLATION, True, True, None, None),     # flags say valid, status says fault
        VerifyResult(VerifyStatus.VALID, True, True, 4, 2),                   # valid but names a faulty cell
        VerifyResult(VerifyStatus.VALID, False, True, None, None),
        VerifyResult(VerifyStatus.VALID, True, False, None, None),
        VerifyResult(VerifyStatus.ROW_VIOLATION, False, True, 3, 1),
        VerifyResult(VerifyStatus.VALID, False, False, None, None),
    ]
    for verdict in contradictions:
        with pytest.raises(ResponseFormatError):
            export_solution_svg(Stub(verdict), P, S, target)
    for not_a_verdict in (None, True, {"valid": True}, "valid", object()):
        with pytest.raises(ResponseFormatError):
            export_solution_svg(Stub(not_a_verdict), P, S, target)
    with pytest.raises(OperationNotSuccessfulError):
        export_solution_svg(Stub(VerifyResult(VerifyStatus.BOX_VIOLATION, False, False, 3, 4)), P, S, target)
    assert not target.exists()


def test_export_checks_clues_itself_without_relying_on_the_renderer(tmp_path, monkeypatch):
    """The contract (clue mismatch -> InvalidArgumentError, engine untouched, nothing written) must hold even if the
    renderer were permissive; the export layer keeps its own check."""
    from sudoku_app import export, export_solution_svg
    from sudoku_app.errors import InvalidArgumentError

    calls = []

    class Stub:
        def verify(self, *a, **k):
            calls.append(a)
            raise AssertionError("must not be reached")

    monkeypatch.setattr(export, "render_board_svg", lambda *a, **k: "<svg/>")   # a renderer that accepts anything
    target = tmp_path / "x.svg"
    with pytest.raises(InvalidArgumentError, match="clue"):
        export_solution_svg(Stub(), "4" + P[1:], S, target)
    with pytest.raises(InvalidArgumentError, match="incomplete"):
        export_solution_svg(Stub(), P, S[:40] + "." + S[41:], target)
    assert calls == [] and not target.exists()


def test_request_id_limit_is_measured_in_utf8_bytes_like_the_engine(engine_executable):
    from sudoku_app.errors import InvalidArgumentError
    engine = SudokuEngine(engine_executable)
    for ok in ("a" * 128, "é" * 64, "😀" * 32, "x"):
        assert engine.capabilities(request_id=ok).engine_version                  # the real engine accepts it and echoes it
    for too_long in ("a" * 129, "é" * 65, "😀" * 33, "", "\ud800"):
        with pytest.raises(InvalidArgumentError):
            engine.capabilities(request_id=too_long)                              # refused before a process starts


def test_targeted_puzzle_that_contradicts_its_solution_is_rejected_on_its_own():
    """Same clue count, one clue digit changed: only the puzzle-vs-solution check can catch it."""
    i = P.index("5")
    forged = P[:i] + "4" + P[i + 1:]
    assert sum(c != "." for c in forged) == CLUES
    rejected(lambda: Canned(env("generate_targeted", "generated", True, targeted(puzzle=forged))).generate_targeted(1, "Easy"))


def test_logical_board_that_changes_a_given_is_rejected_on_its_own():
    """The digit count still matches the reported placements, so only the givens check can catch it."""
    i = P.index("5")
    first_empty = P.index(".")
    board = list(P)
    board[i] = "4"                              # a given changed
    board[first_empty] = S[first_empty]         # one placement, as reported
    result = logical("".join(board))
    with pytest.raises(ResponseFormatError, match="puzzle given there"):
        Canned(env("solve_logical", "stalled", False, result | {"reason": "no_applicable_deduction"})).solve_logical(P)


def test_a_unique_claim_without_an_exact_count_of_one_is_called_out_specifically():
    for result in (count(exact=False, limit=1), count(count=2, limit=5)):
        status = "limit_reached" if result["limit"] == 1 else "exhausted"
        with pytest.raises(ResponseFormatError, match="'unique' without an exact count of one"):
            Canned(env("count_solutions", status, True, result)).count_solutions(P)
