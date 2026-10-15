"""Batch workflow tests (M23). Scripted engines make every failure deterministic; the last section uses the real engine."""
from __future__ import annotations

import json
import os
import xml.etree.ElementTree as ET

import pytest

from sudoku_app import (SudokuEngine, Settings, classify_puzzles, errors, generate_puzzles, generate_targeted_puzzles, item_seed,
                        solve_puzzles, verify_solutions)
from sudoku_app import batch as batch_module
from sudoku_app.protocol import Envelope

NS = "{http://www.w3.org/2000/svg}"
P = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
S = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
CLUES = sum(c != "." for c in P)
ST = {"nodes": 1, "assignments": 1, "backtracks": 0}
METRICS = {"deductions": 1, "placements": 1, "eliminated_candidates": 0, "hardest_level": 1, "hardest_technique": "naked_single",
           "by_technique": {"naked_single": 1}}


def env(op, status, success, result):
    return Envelope(op, None, success, status, result, 0 if success else 1)


class Scripted(SudokuEngine):
    """A client whose transport is a Python function; records every request (op, params)."""

    def __init__(self, handler=None):
        self.handler = handler or default_handler
        self.calls = []

    def request(self, op, params=None, **kw):
        self.calls.append((op, dict(params or {}), kw.get("timeout")))
        return self.handler(op, dict(params or {}))


def default_handler(op, params):
    if op == "generate_puzzle":
        return env(op, "target_reached", True, {
            "seed": params["seed"], "requested_clues": CLUES, "target_reached": True, "puzzle": P, "solution": S,
            "clue_count": CLUES, "removal": {"cells_considered": 1, "removed": 1, "rejected_not_unique": 0, "rejected_inconclusive": 0}})
    if op == "generate_targeted":
        return env(op, "generated", True, {
            "target": params["target"], "seed": params["seed"], "attempts": 1, "max_attempts": params.get("max_attempts", 100),
            "attempt_seed": "7", "rejections": {"easier_than_target": 0, "harder_than_target": 0, "unsolved": 0, "degenerate": 0,
                                                 "inconclusive": 0},
            "puzzle": P, "solution": S, "clue_count": CLUES,
            "classification": {"profile": params["target"], "profile_index": 0, "metrics": METRICS}})
    if op == "classify":
        return env(op, "classified", True, {"solver_status": "solved", "solver_reason": "puzzle_solved", "profile": "Easy",
                                            "profile_index": 0, "metrics": METRICS})
    if op == "solve_search":
        return env(op, "solved", True, {"solution": S, "stats": ST})
    if op == "verify":
        return env(op, "valid", True, {"valid": True})
    raise AssertionError(op)


def exhausted(params):
    return env("generate_targeted", "exhausted", False, {
        "target": params["target"], "seed": params["seed"], "attempts": 2, "max_attempts": 2, "attempt_seed": "9",
        "rejections": {"easier_than_target": 2, "harder_than_target": 0, "unsolved": 0, "degenerate": 0, "inconclusive": 0},
        "last_rejection": "wrong_difficulty"})


def failing_on(index_to_error, base=default_handler):
    """Raises the given error on the n-th request (0-based), otherwise behaves normally."""
    state = {"n": 0}

    def handler(op, params):
        n = state["n"]; state["n"] += 1
        if n in index_to_error:
            raise index_to_error[n]
        return base(op, params)
    return handler


# ---- seeds, empty batches, ordering -----------------------------------------------------------------------------------

def test_item_seed_is_seed_plus_index_wrapping_at_64_bits():
    assert [item_seed(10, i) for i in range(3)] == [10, 11, 12]
    assert item_seed(2**64 - 1, 0) == 2**64 - 1 and item_seed(2**64 - 1, 1) == 0 and item_seed(2**64 - 1, 5) == 4
    for bad in ((-1, 0), (2**64, 0), (0, -1), (True, 0), (0, True), (1.5, 0), ("1", 0)):
        with pytest.raises(errors.InvalidArgumentError):
            item_seed(*bad)


def test_empty_batches_are_valid_and_make_no_request():
    engine = Scripted()
    for result in (generate_puzzles(engine, 1, 0), generate_targeted_puzzles(engine, 1, 0, target="Easy"),
                   classify_puzzles(engine, []), solve_puzzles(engine, ()), verify_solutions(engine, iter([]))):
        assert len(result) == 0 and result.items == () and result.all_succeeded and not result.stopped_early
        assert result.summary().total == 0
        assert result.to_dict()["items"] == []
    assert engine.calls == []


def test_generate_puzzles_uses_documented_seeds_in_input_order():
    engine = Scripted()
    result = generate_puzzles(engine, 2**64 - 2, 4, target_clues=30, node_budget=999, timeout=5)
    assert [c[1]["seed"] for c in engine.calls] == ["18446744073709551614", "18446744073709551615", "0", "1"]
    assert all(c[1]["target_clues"] == 30 and c[1]["node_budget"] == 999 and c[2] == 5 for c in engine.calls)
    assert [i.index for i in result] == [0, 1, 2, 3] and [i.seed for i in result] == [2**64 - 2, 2**64 - 1, 0, 1]
    assert [i.id for i in result] == [f"puzzle-000{k}" for k in range(4)]
    assert result.all_succeeded and result.summary().succeeded == 4
    assert [i.result.seed for i in result] == [i.seed for i in result]
    assert len({id(i.result) for i in result}) == 4                          # no result object is shared


def test_stable_custom_ids():
    result = generate_puzzles(Scripted(), 1, 2, ids=["a", "b.c-1_"])
    assert [i.id for i in result] == ["a", "b.c-1_"]


@pytest.mark.parametrize("ids", [["a", "a"], ["a"], ["a", "b", "c"], "ab", ["../x", "b"], ["a/b", "b"], [".hidden", "b"], ["", "b"],
                                 ["a" * 65, "b"], [1, 2], ["a b", "b"], ["a\\b", "b"], ["ok", None]])
def test_bad_or_duplicate_ids_are_rejected_before_any_request(ids):
    engine = Scripted()
    with pytest.raises(errors.InvalidArgumentError):
        generate_puzzles(engine, 1, 2, ids=ids)
    assert engine.calls == []


@pytest.mark.parametrize("count", [-1, True, 1.0, "2", None, 100001])
def test_bad_counts(count):
    with pytest.raises(errors.InvalidArgumentError):
        generate_puzzles(Scripted(), 1, count)


def test_bad_seed():
    for seed in (-1, 2**64, True, "1"):
        with pytest.raises(errors.InvalidArgumentError):
            generate_puzzles(Scripted(), seed, 1)


# ---- partial success and failures -------------------------------------------------------------------------------------------

@pytest.mark.parametrize("error,category", [
    (errors.EngineTimeoutError(1.0, ("x",)), "timeout"),
    (errors.RequestRejectedError("invalid_field", "no", field="params.seed"), "rejected"),
    (errors.EngineProcessError("boom", 9), "process"),
    (errors.EngineLaunchError("cannot start", ("x",)), "process"),
    (errors.ExecutableNotFoundError("missing"), "process"),
    (errors.ResponseFormatError("junk"), "response_format"),
    (errors.EngineInternalError("bug"), "engine_internal"),
    (errors.InvalidArgumentError("bad"), "invalid_input"),
])
def test_item_failures_are_isolated_and_categorized(error, category):
    engine = Scripted(failing_on({1: error}))
    result = generate_puzzles(engine, 5, 3)
    assert [i.outcome for i in result] == ["success", "failed", "success"]
    bad = result[1]
    assert bad.result is None and bad.error.category == category and bad.error.type == type(error).__name__
    assert bad.error.message
    assert result[0].result.seed == 5 and result[2].result.seed == 7        # later items are unaffected
    s = result.summary()
    assert (s.total, s.succeeded, s.unsuccessful, s.failed, s.skipped) == (3, 2, 0, 1, 0)
    assert not result.all_succeeded and not result.stopped_early
    assert len(engine.calls) == 3                                           # no retry


def test_non_sudoku_exceptions_are_defects_and_propagate():
    with pytest.raises(RuntimeError):
        generate_puzzles(Scripted(failing_on({0: RuntimeError("bug")})), 1, 2)
    with pytest.raises(KeyError):
        generate_puzzles(Scripted(failing_on({1: KeyError("bug")})), 1, 2)


def test_exhaustion_is_unsuccessful_not_failed_and_has_no_puzzle():
    def handler(op, params):
        return exhausted(params) if params["seed"] == "11" else default_handler(op, params)
    result = generate_targeted_puzzles(Scripted(handler), 10, 3, target="Master", max_attempts=2)
    assert [i.outcome for i in result] == ["success", "unsuccessful", "success"]
    item = result[1]
    assert item.error is None and item.result.puzzle is None and item.result.solution is None and item.status == "exhausted"
    assert result.summary().unsuccessful == 1 and not result.all_succeeded
    assert result.successes() == (result[0], result[2])


def test_mixed_valid_and_invalid_boards():
    engine = Scripted()
    boards = [P, "x" * 81, None, P[:-1], P.replace(".", "0"), 5]
    result = classify_puzzles(engine, boards)
    assert [i.outcome for i in result] == ["success", "failed", "failed", "failed", "success", "failed"]
    assert {result[k].error.category for k in (1, 2, 3, 5)} == {"invalid_input"}
    assert len(engine.calls) == 2                                           # invalid boards never reach the engine
    assert [i.index for i in result] == list(range(6))


def test_classify_unsolved_is_unsuccessful_with_no_label():
    def handler(op, params):
        return env(op, "unsolved", False, {"solver_status": "stalled", "solver_reason": "no_applicable_deduction"})
    result = classify_puzzles(Scripted(handler), ["." * 81])
    assert result[0].outcome == "unsuccessful" and result[0].result.profile is None and result[0].error is None


def test_solve_and_verify_batches():
    result = solve_puzzles(Scripted(), [P, P], node_budget=100, ids=["x", "y"])
    assert result.all_succeeded and result[0].result.solution == S
    pairs = verify_solutions(Scripted(), [(P, S), [P, S], (P,), "nope", None])
    assert [i.outcome for i in pairs] == ["success", "success", "failed", "failed", "failed"]
    fault = lambda op, params: env(op, "row_violation", False, {"valid": False, "fault": "row_violation", "index": 3, "digit": 4})
    bad = verify_solutions(Scripted(fault), [(P, S)])
    assert bad[0].outcome == "unsuccessful" and bad[0].result.index == 3


@pytest.mark.parametrize("boards", ["x" * 81, 5, None, b"abc"])
def test_boards_argument_must_be_a_collection(boards):
    with pytest.raises(errors.InvalidArgumentError):
        classify_puzzles(Scripted(), boards)


# ---- fail-fast ---------------------------------------------------------------------------------------------------------------

def test_fail_fast_stops_after_the_current_failed_item_and_reports_the_rest_as_skipped():
    engine = Scripted(failing_on({1: errors.EngineTimeoutError(1.0, ("x",))}))
    result = generate_puzzles(engine, 0, 4, fail_fast=True)
    assert [i.outcome for i in result] == ["success", "failed", "skipped", "skipped"]
    assert len(engine.calls) == 2 and result.stopped_early and not result.all_succeeded
    assert [i.index for i in result] == [0, 1, 2, 3] and [i.seed for i in result] == [0, 1, 2, 3]
    assert result[2].result is None and result[2].error is None
    s = result.summary()
    assert (s.total, s.succeeded, s.failed, s.skipped) == (4, 1, 1, 2)


def test_fail_fast_also_stops_on_an_unsuccessful_item():
    result = generate_targeted_puzzles(Scripted(lambda op, p: exhausted(p)), 0, 3, target="Master", fail_fast=True)
    assert [i.outcome for i in result] == ["unsuccessful", "skipped", "skipped"]


def test_continue_is_the_default_and_settings_can_enable_fail_fast_but_an_argument_wins():
    engine = Scripted(failing_on({0: errors.EngineTimeoutError(1.0, ("x",))}))
    assert [i.outcome for i in generate_puzzles(engine, 0, 2)] == ["failed", "success"]
    on = Settings(fail_fast=True)
    engine = Scripted(failing_on({0: errors.EngineTimeoutError(1.0, ("x",))}))
    assert [i.outcome for i in generate_puzzles(engine, 0, 2, settings=on)] == ["failed", "skipped"]
    engine = Scripted(failing_on({0: errors.EngineTimeoutError(1.0, ("x",))}))
    assert [i.outcome for i in generate_puzzles(engine, 0, 2, settings=on, fail_fast=False)] == ["failed", "success"]


def test_summary_and_report_always_agree_with_the_items():
    engine = Scripted(failing_on({1: errors.EngineTimeoutError(1.0, ("x",))}, base=lambda op, p: exhausted(p) if p["seed"] == "3" else default_handler(op, p)))
    result = generate_targeted_puzzles(engine, 1, 5, target="Easy")
    report = result.to_dict()
    json.dumps(report)
    outcomes = [i["outcome"] for i in report["items"]]
    s = report["summary"]
    assert s["total"] == len(outcomes) == 5
    for name, key in (("success", "succeeded"), ("unsuccessful", "unsuccessful"), ("failed", "failed"), ("skipped", "skipped")):
        assert s[key] == outcomes.count(name)
    assert s["all_succeeded"] is (outcomes.count("success") == 5)
    assert report["items"][1]["error"]["category"] == "timeout" and report["items"][0]["seed"] == "1"


# ---- targeted batches and settings ------------------------------------------------------------------------------------------

def test_targeted_needs_an_explicit_profile_and_never_invents_one():
    engine = Scripted()
    with pytest.raises(errors.InvalidArgumentError, match="profile"):
        generate_targeted_puzzles(engine, 1, 2)
    assert engine.calls == []


def test_settings_supply_defaults_and_arguments_override_them():
    settings = Settings(profile="Hard", max_attempts=77, fail_fast=False)
    engine = Scripted()
    generate_targeted_puzzles(engine, 1, 1, settings=settings)
    assert engine.calls[0][1]["target"] == "Hard" and engine.calls[0][1]["max_attempts"] == 77
    engine = Scripted()
    generate_targeted_puzzles(engine, 1, 1, target="Expert", max_attempts=5, settings=settings)
    assert engine.calls[0][1]["target"] == "Expert" and engine.calls[0][1]["max_attempts"] == 5
    engine = Scripted()
    generate_targeted_puzzles(engine, 1, 1, target="Easy")
    assert "max_attempts" not in engine.calls[0][1]                             # no budget is invented


def test_an_unknown_profile_is_rejected_by_the_engine_per_item_and_never_replaced():
    def handler(op, params):
        raise errors.RequestRejectedError("invalid_field", "unknown profile", field="params.target")
    engine = Scripted(handler)
    result = generate_targeted_puzzles(engine, 1, 2, target="Legendary")
    assert [i.outcome for i in result] == ["failed", "failed"]
    assert {i.error.category for i in result} == {"rejected"}
    assert {c[1]["target"] for c in engine.calls} == {"Legendary"}


# ---- export ------------------------------------------------------------------------------------------------------------------

def texts(path):
    return list(ET.parse(path).getroot().iter(NS + "text"))


def test_export_puzzle_writes_one_file_per_success(tmp_path):
    engine = Scripted(failing_on({1: errors.EngineTimeoutError(1.0, ("x",))}))
    result = generate_puzzles(engine, 1, 3, export="puzzle", output_dir=tmp_path, ids=["a", "b", "c"], theme="dark")
    assert [i.outcome for i in result] == ["success", "failed", "success"]
    assert sorted(p.name for p in tmp_path.iterdir()) == ["a.svg", "c.svg"]     # failed item: no file
    assert result[0].output_path == tmp_path / "a.svg" and result[1].output_path is None
    assert "".join(t.text for t in texts(tmp_path / "a.svg")) == P.replace(".", "")
    assert {t.get("class") for t in texts(tmp_path / "a.svg")} == {"given"}


def test_export_solution_is_verified_by_the_engine_first(tmp_path):
    engine = Scripted()
    result = generate_puzzles(engine, 1, 2, export="solution", output_dir=tmp_path, cell_size=30, margin=6)
    assert result.all_succeeded
    assert [c[0] for c in engine.calls] == ["generate_puzzle", "verify", "generate_puzzle", "verify"]
    svg = (tmp_path / "puzzle-0000.svg").read_text()
    assert ET.fromstring(svg).get("width") == str(9 * 30 + 12) and "".join(t.text for t in texts(tmp_path / "puzzle-0000.svg")) == S
    assert [t.get("class") for t in texts(tmp_path / "puzzle-0000.svg")] == ["given" if c != "." else "filled" for c in P]


def test_refused_verification_writes_nothing_and_keeps_the_result(tmp_path):
    def handler(op, params):
        if op == "verify":
            return env(op, "row_violation", False, {"valid": False, "fault": "row_violation", "index": 3, "digit": 4})
        return default_handler(op, params)
    result = generate_puzzles(Scripted(handler), 1, 1, export="solution", output_dir=tmp_path)
    item = result[0]
    assert item.outcome == "failed" and item.error.category == "verification" and item.result.solution == S
    assert list(tmp_path.iterdir()) == []


def test_existing_files_are_not_overwritten_unless_asked(tmp_path):
    (tmp_path / "puzzle-0000.svg").write_text("keep me")
    result = generate_puzzles(Scripted(), 1, 2, export="puzzle", output_dir=tmp_path)
    assert [i.outcome for i in result] == ["failed", "success"]
    assert result[0].error.category == "output" and result[0].error.type == "FileExistsError" and result[0].result is not None
    assert (tmp_path / "puzzle-0000.svg").read_text() == "keep me"
    result = generate_puzzles(Scripted(), 1, 2, export="puzzle", output_dir=tmp_path, overwrite=True)
    assert result.all_succeeded and (tmp_path / "puzzle-0000.svg").read_text().startswith("<svg ")
    assert sorted(p.name for p in tmp_path.iterdir()) == ["puzzle-0000.svg", "puzzle-0001.svg"]   # no temp files left


@pytest.mark.parametrize("overwrite", [False, True])
def test_a_failing_write_leaves_no_partial_file(tmp_path, monkeypatch, overwrite):
    (tmp_path / "puzzle-0000.svg").write_text("old") if overwrite else None
    real_fdopen = os.fdopen

    class Broken:
        def __init__(self, handle):
            self.handle = handle

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.handle.close()

        def write(self, data):
            self.handle.write(data[:10])
            raise OSError(28, "No space left on device")

    monkeypatch.setattr(batch_module.os, "fdopen", lambda fd, mode: Broken(real_fdopen(fd, mode)))
    result = generate_puzzles(Scripted(), 1, 1, export="puzzle", output_dir=tmp_path, overwrite=overwrite)
    assert result[0].outcome == "failed" and result[0].error.category == "output"
    names = sorted(p.name for p in tmp_path.iterdir())
    assert names == (["puzzle-0000.svg"] if overwrite else [])
    if overwrite:
        assert (tmp_path / "puzzle-0000.svg").read_text() == "old"               # the old file is intact


def test_output_directory_problems_are_reported_before_any_request(tmp_path):
    engine = Scripted()
    for kwargs in ({"export": "puzzle"}, {"export": "puzzle", "output_dir": tmp_path / "missing"},
                   {"export": "puzzle", "output_dir": __file__}, {"export": "bogus", "output_dir": tmp_path}):
        with pytest.raises(errors.InvalidArgumentError):
            generate_puzzles(engine, 1, 2, **kwargs)
    assert engine.calls == [] and list(tmp_path.iterdir()) == []


def test_output_dir_comes_from_settings_and_an_argument_overrides_it(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir(); b.mkdir()
    generate_puzzles(Scripted(), 1, 1, export="puzzle", settings=Settings(output_dir=str(a)))
    generate_puzzles(Scripted(), 1, 1, export="puzzle", output_dir=b, settings=Settings(output_dir=str(a)))
    assert [p.name for p in a.iterdir()] == ["puzzle-0000.svg"] and [p.name for p in b.iterdir()] == ["puzzle-0000.svg"]


def test_exhausted_targeted_items_export_nothing(tmp_path):
    result = generate_targeted_puzzles(Scripted(lambda op, p: exhausted(p)), 1, 2, target="Master", export="puzzle",
                                       output_dir=tmp_path)
    assert [i.outcome for i in result] == ["unsuccessful", "unsuccessful"] and list(tmp_path.iterdir()) == []


def test_default_svg_theme_settings_flow_into_exports(tmp_path):
    generate_puzzles(Scripted(), 1, 1, export="puzzle", output_dir=tmp_path, settings=Settings(theme="dark"))
    assert 'fill="#121212"' in (tmp_path / "puzzle-0000.svg").read_text()


# ---- real engine -----------------------------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def engine(engine_executable):
    return SudokuEngine(engine_executable, timeout=120)


def test_real_generation_batch_matches_single_item_calls_and_exports(engine, tmp_path):
    result = generate_puzzles(engine, 100, 3, target_clues=32, export="solution", output_dir=tmp_path)
    assert result.all_succeeded and len(result) == 3
    for i, item in enumerate(result):
        assert item.result == engine.generate_puzzle(100 + i, target_clues=32)      # documented seed derivation
        assert engine.count_solutions(item.result.puzzle).classification.value == "unique"
        glyphs = texts(item.output_path)
        assert "".join(t.text for t in glyphs) == item.result.solution
        assert [t.get("class") == "given" for t in glyphs] == [c != "." for c in item.result.puzzle]
    again = generate_puzzles(engine, 100, 3, target_clues=32)
    assert [i.result for i in again] == [i.result for i in result]                   # deterministic


def test_real_targeted_batch_with_exhaustion_and_report(engine):
    easy = generate_targeted_puzzles(engine, 5, 2, target="Easy", max_attempts=50)
    assert easy.all_succeeded and all(i.result.classification.profile == "Easy" for i in easy)
    assert easy[0].result == engine.generate_targeted(5, "Easy", max_attempts=50)
    master = generate_targeted_puzzles(engine, 1, 2, target="Master", max_attempts=2)
    assert [i.outcome for i in master] == ["unsuccessful", "unsuccessful"]
    assert all(i.result.puzzle is None and i.result.attempts == 2 for i in master)
    json.dumps(master.to_dict())


def test_real_classify_solve_verify_batches(engine):
    unsolvable = "12345678." + "........9" + "." * 63
    boards = [P, "." * 81, "11" + "." * 79, "bad", unsolvable]
    classified = classify_puzzles(engine, boards)
    assert [i.outcome for i in classified] == ["success", "unsuccessful", "unsuccessful", "failed", "unsuccessful"]
    assert classified[0].result.profile == "Easy" and classified[1].result.profile is None
    solved = solve_puzzles(engine, boards)
    assert [i.outcome for i in solved] == ["success", "success", "unsuccessful", "failed", "unsuccessful"]
    assert solved[0].result.solution == S and solved[4].status == "unsolvable"
    swapped = S[:2] + S[3] + S[2] + S[4:]
    verified = verify_solutions(engine, [(P, S), ("." * 81, swapped), (P, P)])
    assert [i.outcome for i in verified] == ["success", "unsuccessful", "unsuccessful"]
    assert verified[2].status == "empty_cell"


def test_an_invalid_explicit_budget_is_never_replaced_by_the_settings_value():
    engine = Scripted()
    result = generate_targeted_puzzles(engine, 1, 2, target="Easy", max_attempts=0, settings=Settings(max_attempts=50))
    assert [i.outcome for i in result] == ["failed", "failed"] and {i.error.category for i in result} == {"invalid_input"}
    assert engine.calls == []
