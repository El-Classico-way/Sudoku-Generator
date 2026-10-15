"""Process-level behaviour of the client, tested with a controlled fake engine.

The fake executable (fake_engine.py) can be told to exit with any code, print any
output, write to stderr, hang, kill itself, and it records every invocation, so each
failure path is exercised deterministically and the absence of retries is provable.
"""
import json
import os
import stat
import sys
import time

import pytest

from sudoku_app import SudokuEngine, errors, models
from sudoku_app.runner import EngineRunner, find_executable

from conftest import invocations

POSIX = sys.platform != "win32"
EXHAUSTED = {"schema": "sudoku.response/1", "op": "@OP@", "ok": True, "success": False, "status": "exhausted", "result": {
    "target": "Master", "seed": "1", "attempts": 3, "max_attempts": 3, "attempt_seed": "17911839290282890590",
    "rejections": {"easier_than_target": 2, "harder_than_target": 0, "unsolved": 1, "degenerate": 0, "inconclusive": 0},
    "last_rejection": "wrong_difficulty"}}
GRID_OK = {"schema": "sudoku.response/1", "op": "@OP@", "ok": True, "success": True, "status": "generated", "result": {
    "seed": "5", "grid": "534678912672195348198342567859761423426853791713924856961537284287419635345286179",
    "stats": {"nodes": 1, "assignments": 1, "backtracks": 0}}}


def text_of(doc):
    return json.dumps(doc) + "\n"


# ----------------------------------------------------- discovery and launch

def test_missing_executable(tmp_path):
    with pytest.raises(errors.ExecutableNotFoundError) as info:
        SudokuEngine(tmp_path / "nope")
    assert str(tmp_path / "nope") in str(info.value) and info.value.searched == (str(tmp_path / "nope"),)


def test_directory_is_not_an_executable(tmp_path):
    with pytest.raises(errors.ExecutableNotFoundError):
        SudokuEngine(tmp_path)


@pytest.mark.skipif(not POSIX, reason="execute permission bits are POSIX-specific")
def test_file_without_execute_permission(tmp_path):
    path = tmp_path / "sudoku_cli"
    path.write_text("#!/bin/sh\n")
    path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    with pytest.raises(errors.ExecutableNotFoundError):
        SudokuEngine(path)


@pytest.mark.skipif(not POSIX, reason="exec-format errors are POSIX-specific")
def test_executable_that_cannot_be_launched(tmp_path):
    path = tmp_path / "sudoku_cli"
    path.write_bytes(b"\x00\x01 not a program")        # executable bit, but no valid format
    path.chmod(0o755)
    engine = SudokuEngine(path)
    with pytest.raises(errors.EngineLaunchError) as info:
        engine.capabilities()
    assert isinstance(info.value.cause, OSError) and info.value.argv == (str(path),)


def test_discovery_rules(tmp_path, monkeypatch):
    fake = tmp_path / "fake"
    fake.write_text("")
    fake.chmod(0o755)
    other = tmp_path / "other"
    other.write_text("")
    other.chmod(0o755)
    env = {"SUDOKU_CLI": str(other)}
    assert find_executable(fake, env) == (str(fake),)                    # 1. explicit argument wins over the environment
    assert find_executable(None, env) == (str(other),)                   # 2. environment variable
    with pytest.raises(errors.ExecutableNotFoundError):                   # an explicit but bad choice never falls back
        find_executable(tmp_path / "missing", env)
    with pytest.raises(errors.ExecutableNotFoundError, match="SUDOKU_CLI"):
        find_executable(None, {"SUDOKU_CLI": str(tmp_path / "missing")})  # nor does a bad environment value
    assert find_executable([sys.executable, "-c", "pass"], {}) == (sys.executable, "-c", "pass")   # argv prefix
    for bad in ["", [], [""], [5], 5, [sys.executable, 5], 3.5]:
        with pytest.raises(errors.InvalidArgumentError):
            find_executable(bad, {})
    assert find_executable([sys.executable, ""], {}) == (sys.executable, "")   # empty ARGUMENTS are fine
    # 3. standard build location; PATH is never searched
    monkeypatch.setattr("sudoku_app.runner._default_location", lambda: tmp_path / "build" / "dev" / "sudoku_cli")
    with pytest.raises(errors.ExecutableNotFoundError, match="cmake --preset dev"):
        find_executable(None, {})
    monkeypatch.setattr("sudoku_app.runner._default_location", lambda: None)
    with pytest.raises(errors.ExecutableNotFoundError, match="SUDOKU_CLI"):
        find_executable(None, {})


def test_timeout_values_are_validated(fake):
    for bad in [0, -1, float("nan"), float("inf"), True, "5", None]:
        if bad is None:
            continue   # per-call None means "use the engine's default"
        with pytest.raises(errors.InvalidArgumentError):
            EngineRunner([sys.executable], timeout=bad)
    engine, counts = fake(stdout_text=text_of(GRID_OK))
    with pytest.raises(errors.InvalidArgumentError):
        engine.generate_grid(5, timeout=0)
    assert invocations(counts) == []                                      # rejected before any process started


# -------------------------------------------------------------- timeouts

def test_timeout_kills_the_process_and_raises(fake):
    engine, counts = fake(sleep=30, timeout=0.5)
    start = time.monotonic()
    with pytest.raises(errors.EngineTimeoutError) as info:
        engine.generate_grid(5)
    assert time.monotonic() - start < 10                                  # it did not wait for the 30 s sleep
    assert info.value.timeout == 0.5 and "0.5" in str(info.value)
    assert len(invocations(counts)) == 1                                  # and did not retry


def test_per_call_timeout_overrides_the_default(fake):
    engine, _ = fake(sleep=30, timeout=100)
    start = time.monotonic()
    with pytest.raises(errors.EngineTimeoutError) as info:
        engine.capabilities(timeout=0.4)
    assert info.value.timeout == 0.4 and time.monotonic() - start < 10


# ------------------------------------------------------ exit codes / output

def test_protocol_error_with_valid_json(fake, tmp_path):
    body = {"schema": "sudoku.response/1", "id": "@ID@", "ok": False,
            "error": {"code": "invalid_field", "message": "must be a decimal string", "field": "params.seed", "position": 3}}
    (tmp_path / "out.json").write_text(text_of(body))
    engine, counts = fake(exit=2, stdout_file=tmp_path / "out.json", stderr="hint")
    with pytest.raises(errors.RequestRejectedError) as info:
        engine.generate_grid(5, request_id="req-1")
    e = info.value
    assert (e.code, e.field, e.position, e.request_id, e.stderr) == ("invalid_field", "params.seed", 3, "req-1", "hint")
    assert len(invocations(counts)) == 1


def test_internal_error_with_valid_json(fake, tmp_path):
    body = {"schema": "sudoku.response/1", "ok": False, "error": {"code": "internal_error", "message": "internal error; no result is available"}}
    (tmp_path / "out.json").write_text(text_of(body))
    engine, counts = fake(exit=3, stdout_file=tmp_path / "out.json", stderr="engine said: disk on fire")
    with pytest.raises(errors.EngineInternalError) as info:
        engine.generate_grid(5)
    assert "disk on fire" in info.value.stderr and "disk on fire" in str(info.value)
    assert len(invocations(counts)) == 1


@pytest.mark.parametrize("exit_code", [1, 2, 3, 4, 7])
def test_nonzero_exit_with_malformed_output(fake, exit_code):
    engine, counts = fake(exit=exit_code, stdout_text="this is not json\n", stderr="usage: sudoku_cli ...")
    with pytest.raises(errors.EngineProcessError) as info:
        engine.generate_grid(5)
    assert info.value.exit_code == exit_code and "not json" in info.value.stdout and "usage" in info.value.stderr
    assert len(invocations(counts)) == 1


@pytest.mark.skipif(not POSIX, reason="death by signal is POSIX-specific")
def test_process_killed_by_a_signal(fake):
    engine, _ = fake(signal_self=True)
    with pytest.raises(errors.EngineProcessError) as info:
        engine.generate_grid(5)
    assert info.value.exit_code < 0


def test_exit_zero_with_empty_stdout(fake):
    engine, counts = fake(exit=0, stdout_text="", stderr="warning: nothing to say")
    with pytest.raises(errors.ResponseFormatError, match="no output") as info:
        engine.generate_grid(5)
    assert "nothing to say" in info.value.stderr and len(invocations(counts)) == 1


@pytest.mark.parametrize("text", ["{not json}\n", "[1, 2]\n", '"just a string"\n', "NaN\n", '{"a":1,"a":2}\n', "42\n"])
def test_exit_zero_with_malformed_json(fake, text):
    engine, _ = fake(exit=0, stdout_text=text)
    with pytest.raises(errors.ResponseFormatError):
        engine.generate_grid(5)


def test_invalid_utf8_output(fake):
    engine, _ = fake(exit=0, stdout_hex="fffe0a")
    with pytest.raises(errors.ResponseFormatError, match="UTF-8"):
        engine.generate_grid(5)


def test_extra_output_and_multiple_lines(fake, tmp_path):
    line = json.dumps(GRID_OK)
    for name, content in {"two": line + "\n" + line + "\n", "banner": "building...\n" + line + "\n",
                          "trailing": line + "\nfinished\n", "crlf": line + "\r\n" + line + "\r\n"}.items():
        (tmp_path / f"{name}.json").write_text(content, newline="")
        engine, counts = fake(exit=0, stdout_file=tmp_path / f"{name}.json")
        with pytest.raises(errors.ResponseFormatError, match="one JSON line"):
            engine.generate_grid(5)
        counts.unlink()


def test_response_violating_the_schema(fake, tmp_path):
    bad = json.loads(json.dumps(GRID_OK))
    bad["result"]["grid"] = "not a grid"
    (tmp_path / "bad.json").write_text(text_of(bad))
    engine, _ = fake(exit=0, stdout_file=tmp_path / "bad.json")
    with pytest.raises(errors.ResponseFormatError, match="grid"):
        engine.generate_grid(5)
    bad = json.loads(json.dumps(GRID_OK))
    bad["op"] = "classify"
    (tmp_path / "bad2.json").write_text(text_of(bad))
    engine, _ = fake(exit=0, stdout_file=tmp_path / "bad2.json")
    with pytest.raises(errors.ResponseFormatError, match="op"):
        engine.generate_grid(5)


def test_stderr_diagnostics_do_not_break_a_valid_response(fake, tmp_path):
    (tmp_path / "ok.json").write_text(text_of(GRID_OK))
    engine, _ = fake(exit=0, stdout_file=tmp_path / "ok.json", stderr="note: something harmless\n")
    result = engine.generate_grid(5)
    assert result.success and result.seed == 5
    assert engine.request("generate_grid", {"seed": "5"}).stderr == "note: something harmless\n"   # available, not mixed in


def test_huge_stderr_is_bounded_in_errors(fake):
    engine, _ = fake(exit=2, stdout_text="oops\n", stderr_bytes=2_000_000)
    with pytest.raises(errors.EngineProcessError) as info:
        engine.generate_grid(5)
    assert len(str(info.value)) < 1000 and len(info.value.stderr) < 2200 and "truncated" in info.value.stderr


# ------------------------------------------------------ non-success outcomes

def test_targeted_exhaustion_is_a_typed_outcome_not_an_exception(fake, tmp_path):
    (tmp_path / "ex.json").write_text(text_of(EXHAUSTED))
    engine, counts = fake(exit=1, stdout_file=tmp_path / "ex.json")
    result = engine.generate_targeted(1, "Master", max_attempts=3)
    assert isinstance(result, models.TargetedResult)
    assert result.status is models.TargetedStatus.EXHAUSTED and result.success is False
    assert result.puzzle is None and result.solution is None and result.classification is None   # no substitute
    assert (result.attempts, result.max_attempts, result.attempt_seed) == (3, 3, 17911839290282890590)
    assert result.rejections.easier_than_target == 2 and result.rejections.total() == 3
    with pytest.raises(errors.OperationNotSuccessfulError):
        result.require_success()                                          # only on explicit request
    assert len(invocations(counts)) == 1


# ------------------------------------------------- requests and no retries

def test_exactly_one_request_is_sent_and_matches_the_protocol(fake, tmp_path):
    (tmp_path / "ok.json").write_text(text_of(GRID_OK | {"result": GRID_OK["result"] | {"seed": str(2**64 - 1)}}))  # a response echoes its seed
    engine, counts = fake(exit=0, stdout_file=tmp_path / "ok.json")
    engine.generate_grid(2**64 - 1)
    (line,) = invocations(counts)
    assert json.loads(line) == {"schema": "sudoku.request/1", "op": "generate_grid", "params": {"seed": "18446744073709551615"}}
    assert b"\n" not in line


def test_optional_parameters_are_sent_only_when_given(fake, tmp_path):
    def answer_for(seed):                                       # a response echoes the seed of its request
        (tmp_path / "ex.json").write_text(text_of(EXHAUSTED | {"result": EXHAUSTED["result"] | {"seed": str(seed)}}))

    answer_for(0)
    engine, counts = fake(exit=1, stdout_file=tmp_path / "ex.json")
    engine.generate_targeted(0, "Master", max_attempts=3)
    answer_for(2**53 + 1)
    engine.generate_targeted(2**53 + 1, "Master", max_attempts=3, target_clues=30, node_budget=7)
    first, second = (json.loads(x) for x in invocations(counts))
    assert first["params"] == {"seed": "0", "target": "Master", "max_attempts": 3}           # nothing the caller did not give
    assert second["params"] == {"seed": "9007199254740993", "target": "Master", "max_attempts": 3, "target_clues": 30, "node_budget": 7}


def test_omitted_options_stay_omitted_for_every_operation(fake, tmp_path):
    """Only required arguments are sent when nothing else is given: the engine's defaults apply, none are injected."""
    (tmp_path / "any.json").write_text("{}\n")
    engine, counts = fake(exit=2, stdout_file=tmp_path / "any.json")
    board = "." * 81
    calls = [lambda: engine.capabilities(), lambda: engine.generate_grid(1), lambda: engine.generate_puzzle(1),
             lambda: engine.solve_search(board), lambda: engine.count_solutions(board), lambda: engine.solve_logical(board),
             lambda: engine.verify(board, board), lambda: engine.classify(board), lambda: engine.generate_targeted(1, "Easy")]
    for call in calls:
        with pytest.raises(errors.SudokuError):
            call()
    sent = [json.loads(x) for x in invocations(counts)]
    assert [r["op"] for r in sent] == ["capabilities", "generate_grid", "generate_puzzle", "solve_search", "count_solutions",
                                       "solve_logical", "verify", "classify", "generate_targeted"]
    assert "params" not in sent[0]
    assert sent[1]["params"] == {"seed": "1"} and sent[2]["params"] == {"seed": "1"}
    assert sent[3]["params"] == {"puzzle": board} and sent[4]["params"] == {"puzzle": board} and sent[5]["params"] == {"puzzle": board}
    assert sent[6]["params"] == {"puzzle": board, "solution": board} and sent[7]["params"] == {"puzzle": board}
    assert sent[8]["params"] == {"seed": "1", "target": "Easy"}


def test_invalid_arguments_never_start_a_process(fake):
    engine, counts = fake(exit=0, stdout_text="")
    calls = [lambda: engine.generate_grid(-1), lambda: engine.generate_grid(2**64), lambda: engine.generate_grid("1"),
             lambda: engine.generate_puzzle(1, target_clues=16), lambda: engine.generate_puzzle(1, target_clues=82),
             lambda: engine.generate_puzzle(1, node_budget=0), lambda: engine.solve_search("x"), lambda: engine.classify(None),
             lambda: engine.verify("." * 81, "short"), lambda: engine.count_solutions("." * 81, limit=0),
             lambda: engine.count_solutions("." * 81, limit=100001), lambda: engine.solve_logical("." * 81, techniques=[]),
             lambda: engine.solve_logical("." * 81, techniques="naked_single"), lambda: engine.solve_logical("." * 81, include_trace=1),
             lambda: engine.generate_targeted(1, ""), lambda: engine.generate_targeted(1, "Easy", max_attempts=0),
             lambda: engine.generate_targeted(-1, "Easy"), lambda: engine.generate_targeted(2**64, "Easy"),
             lambda: engine.generate_targeted("1", "Easy"), lambda: engine.generate_targeted(True, "Easy"),
             lambda: engine.generate_puzzle(-1), lambda: engine.generate_puzzle(2**64), lambda: engine.generate_puzzle(1.0),
             lambda: engine.generate_grid(True), lambda: engine.generate_grid(None),
             lambda: engine.generate_targeted(1, "Easy", max_attempts=1000001), lambda: engine.capabilities(request_id=""),
             lambda: engine.request("explode")]
    for call in calls:
        with pytest.raises(errors.InvalidArgumentError):
            call()
    assert invocations(counts) == []


@pytest.mark.parametrize("options,exception", [
    (dict(exit=0, stdout_text=""), errors.ResponseFormatError),
    (dict(exit=2, stdout_text="boom\n"), errors.EngineProcessError),
    (dict(exit=9, stdout_text=""), errors.EngineProcessError),
    (dict(exit=0, stdout_text="{bad\n"), errors.ResponseFormatError),
])
def test_failures_are_never_retried(fake, options, exception):
    engine, counts = fake(**options)
    with pytest.raises(exception):
        engine.generate_grid(5)
    assert len(invocations(counts)) == 1


def test_a_successful_call_runs_one_process_per_request(fake, tmp_path):
    (tmp_path / "ok.json").write_text(text_of(GRID_OK))
    engine, counts = fake(exit=0, stdout_file=tmp_path / "ok.json")
    engine.generate_grid(5)
    engine.generate_grid(5)
    assert len(invocations(counts)) == 2          # one process per request: no persistent process, no batching


def test_request_id_is_echoed_and_checked(fake, tmp_path):
    echoing = dict(GRID_OK, id="@ID@")
    (tmp_path / "echo.json").write_text(text_of(echoing))
    engine, _ = fake(exit=0, stdout_file=tmp_path / "echo.json")
    assert engine.request("generate_grid", {"seed": "5"}, request_id="abc").request_id == "abc"
    wrong = dict(GRID_OK, id="somebody-else")
    (tmp_path / "wrong.json").write_text(text_of(wrong))
    engine, _ = fake(exit=0, stdout_file=tmp_path / "wrong.json")
    with pytest.raises(errors.ResponseFormatError, match="does not match"):
        engine.request("generate_grid", {"seed": "5"}, request_id="abc")
