"""Unit tests of the client's pure layers (no executable needed)."""
import copy
import json
import math

import pytest

from sudoku_app import errors, models, protocol
from sudoku_app.protocol import Envelope

EASY = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
STATS = {"nodes": 5, "assignments": 4, "backtracks": 3}
TECHNIQUES = ["naked_single", "hidden_single", "pointing", "claiming", "naked_pair", "hidden_pair", "naked_triple",
              "hidden_triple", "x_wing"]
BY_TECHNIQUE = {t: 0 for t in TECHNIQUES} | {"naked_single": 2}
METRICS = {"deductions": 2, "placements": 2, "eliminated_candidates": 0, "hardest_level": 1,
           "hardest_technique": "naked_single", "by_technique": BY_TECHNIQUE}


def envelope(op, status, success, result, request_id=None, exit_code=None):
    return Envelope(op=op, request_id=request_id, success=success, status=status, result=result,
                    exit_code=exit_code if exit_code is not None else (0 if success else 1))


# ------------------------------------------------------------------ seeds

@pytest.mark.parametrize("seed", [0, 1, 2**53, 2**53 + 1, 2**63, 2**64 - 1])
def test_seed_round_trip_is_exact(seed):
    text = protocol.encode_seed(seed)
    assert isinstance(text, str) and text == str(seed)
    assert protocol.decode_seed(text, "seed") == seed
    request = json.loads(protocol.serialize_request(protocol.build_request("generate_grid", {"seed": text})))
    assert request["params"]["seed"] == text                      # a string on the wire, never a float
    assert b'"seed":"%d"' % seed in protocol.serialize_request(protocol.build_request("generate_grid", {"seed": text}))


@pytest.mark.parametrize("seed", [-1, 2**64, 2**100, True, False, 1.0, 1.5, "1", None, [1], float("nan")])
def test_invalid_python_seeds(seed):
    with pytest.raises(errors.InvalidArgumentError):
        protocol.encode_seed(seed)


@pytest.mark.parametrize("text", ["", "01", "-1", "+1", " 1", "1.0", "18446744073709551616", "99999999999999999999", "x", 5, None, 1.5])
def test_invalid_response_seeds(text):
    with pytest.raises(errors.ResponseFormatError):
        protocol.decode_seed(text, "seed")


# ----------------------------------------------------------------- boards

def test_board_validation():
    assert protocol.check_board(EASY, "puzzle") == EASY
    assert protocol.check_board("0" * 81, "puzzle") == "0" * 81      # '0' and '.' both mean empty
    for bad in ["", "." * 80, "." * 82, "x" * 81, "5" * 40 + "a" + "." * 40, " " * 81, "." * 80 + "\n"]:
        with pytest.raises(errors.InvalidArgumentError):
            protocol.check_board(bad, "puzzle")
    for bad in [None, 5, b"." * 81, ["."] * 81]:
        with pytest.raises(errors.InvalidArgumentError, match="must be a str"):
            protocol.check_board(bad, "puzzle")
    with pytest.raises(errors.InvalidArgumentError, match="index 40"):
        protocol.check_board("." * 40 + "x" + "." * 40, "puzzle")


def test_response_boards_use_dot_for_empty():
    assert protocol.check_response_board(EASY, "b", complete=False) == EASY
    assert protocol.check_response_board(SOLVED, "b", complete=True) == SOLVED
    for bad, complete in [("0" * 81, False), (EASY, True), ("x" * 81, False), (EASY[:80], False), (5, False)]:
        with pytest.raises(errors.ResponseFormatError):
            protocol.check_response_board(bad, "b", complete=complete)


# --------------------------------------------------------------- requests

def test_build_request_includes_only_what_was_given():
    assert protocol.build_request("capabilities") == {"schema": "sudoku.request/1", "op": "capabilities"}
    r = protocol.build_request("generate_grid", {"seed": "7"}, "abc")
    assert r == {"schema": "sudoku.request/1", "op": "generate_grid", "id": "abc", "params": {"seed": "7"}}
    assert protocol.build_request("classify", {}) ["params"] == {}


def test_build_request_validation():
    with pytest.raises(errors.InvalidArgumentError, match="unknown operation"):
        protocol.build_request("explode")
    for bad_id in ["", "x" * 129, 5, b"x"]:
        with pytest.raises(errors.InvalidArgumentError, match="request_id"):
            protocol.build_request("capabilities", None, bad_id)
    assert protocol.build_request("capabilities", None, "x" * 128)["id"] == "x" * 128


def test_serialization_is_single_line_utf8_without_nan():
    data = protocol.serialize_request(protocol.build_request("capabilities", None, "id-é\n"))
    assert b"\n" not in data and data.decode("ascii")                  # escaped, one line
    assert json.loads(data)["id"] == "id-é\n"
    with pytest.raises(errors.InvalidArgumentError):
        protocol.serialize_request({"params": {"x": math.nan}})
    with pytest.raises(errors.InvalidArgumentError):
        protocol.serialize_request({"params": {"x": object()}})


# ------------------------------------------- exit codes and response mapping

def ok_body(op="generate_grid", success=True, status="generated", result=None, **extra):
    body = {"schema": "sudoku.response/1", "op": op, "ok": True, "success": success, "status": status,
            "result": result if result is not None else {}}
    body.update(extra)
    return body


def err_body(code, message="m", **error_extra):
    return {"schema": "sudoku.response/1", "ok": False, "error": {"code": code, "message": message, **error_extra}}


def parse(body, exit_code, *, op="generate_grid", request_id=None, stderr=b"", raw=None):
    stdout = raw if raw is not None else (json.dumps(body) + "\n").encode()
    return protocol.parse_process_output(exit_code=exit_code, stdout=stdout, stderr=stderr, op=op, request_id=request_id)


def test_success_and_non_success_outcomes_are_returned_not_raised():
    env = parse(ok_body(), 0, stderr=b"some diagnostics")
    assert (env.success, env.status, env.exit_code, env.op) == (True, "generated", 0, "generate_grid")
    assert env.stderr == "some diagnostics"
    env = parse(ok_body(op="generate_targeted", success=False, status="exhausted"), 1, op="generate_targeted")
    assert (env.success, env.status, env.exit_code) == (False, "exhausted", 1)


def test_protocol_error_maps_to_request_rejected():
    with pytest.raises(errors.RequestRejectedError) as info:
        parse(err_body("invalid_field", "bad seed", field="params.seed", position=7), 2, request_id="r1", stderr=b"diag")
    e = info.value
    assert (e.code, e.engine_message, e.field, e.position, e.request_id, e.stderr) == ("invalid_field", "bad seed", "params.seed", 7, "r1", "diag")
    assert "invalid_field" in str(e) and "params.seed" in str(e)


def test_internal_error_maps_to_engine_internal_error():
    with pytest.raises(errors.EngineInternalError) as info:
        parse(err_body("internal_error", "internal error; no result is available"), 3, stderr=b"detail from stderr")
    assert info.value.stderr == "detail from stderr" and "detail from stderr" in str(info.value)


@pytest.mark.parametrize("code", [4, 5, 7, 126, 127, 255, -9, -11, 139])
def test_undefined_exit_codes_are_process_errors_even_with_valid_json(code):
    with pytest.raises(errors.EngineProcessError) as info:
        parse(ok_body(), code, stderr=b"crashed")
    assert info.value.exit_code == code and "crashed" in str(info.value)


@pytest.mark.parametrize("exit_code", [1, 2, 3])
def test_nonzero_exit_without_valid_response_is_a_process_error(exit_code):
    for raw in [b"", b"oops\n", b"{not json}\n", b"\xff\xfe\n", b"[1,2]\n" * 2]:
        with pytest.raises(errors.EngineProcessError) as info:
            parse(None, exit_code, raw=raw, stderr=b"usage: ...")
        assert info.value.exit_code == exit_code


@pytest.mark.parametrize("raw", [b"", b"   \n", b"nonsense\n", b"{\n", b'{"a":1,"a":2}\n', b"NaN\n", b'{"x": Infinity}\n',
                                 b"\xff\n", b'{"schema":"sudoku.response/1"}\n{"schema":"sudoku.response/1"}\n',
                                 b"\n" + json.dumps(ok_body()).encode() + b"\n", json.dumps(ok_body()).encode() + b"\nextra\n",
                                 json.dumps(ok_body()).encode() + b"\r\n" + json.dumps(ok_body()).encode(), b"banner\n" + json.dumps(ok_body()).encode()])
def test_exit_zero_with_unusable_output_is_a_format_error(raw):
    with pytest.raises(errors.ResponseFormatError):
        parse(None, 0, raw=raw)


@pytest.mark.parametrize("mutate", [
    lambda b: b.update(schema="sudoku.response/2"),
    lambda b: b.pop("schema"),
    lambda b: b.update(ok="yes"),
    lambda b: b.pop("ok"),
    lambda b: b.update(success=1),
    lambda b: b.pop("success"),
    lambda b: b.update(status=5),
    lambda b: b.pop("status"),
    lambda b: b.update(result=[]),
    lambda b: b.pop("result"),
    lambda b: b.update(op="classify"),
    lambda b: b.pop("op"),
    lambda b: b.update(unexpected=1),
    lambda b: b.update(error={"code": "x", "message": "y"}),
    lambda b: b.update(id=5),
    lambda b: b.update(id="unexpected"),     # no id was sent
])
def test_schema_violations_in_ok_responses(mutate):
    body = ok_body()
    mutate(body)
    with pytest.raises(errors.ResponseFormatError):
        parse(body, 0)


@pytest.mark.parametrize("exit_code,success", [(1, True), (0, False)])
def test_exit_code_must_agree_with_success(exit_code, success):
    with pytest.raises(errors.ResponseFormatError, match="inconsistent"):
        parse(ok_body(success=success), exit_code)


@pytest.mark.parametrize("body,exit_code", [
    (err_body("internal_error"), 2),              # internal_error must exit 3
    (err_body("invalid_field"), 3),               # protocol errors must exit 2
    (err_body("invalid_field"), 0),
    (err_body("invalid_field"), 1),
    ({"schema": "sudoku.response/1", "ok": False}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": "text"}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": "x"}}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": 5, "message": "m"}}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": "x", "message": "m", "extra": 1}}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": "x", "message": "m", "field": 5}}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": "x", "message": "m", "position": "1"}}, 2),
    ({"schema": "sudoku.response/1", "ok": False, "error": {"code": "x", "message": "m"}, "status": "x"}, 2),
])
def test_malformed_or_inconsistent_error_responses(body, exit_code):
    with pytest.raises(errors.ResponseFormatError):
        parse(body, exit_code)


def test_request_ids_are_verified():
    assert parse(ok_body(id="r1"), 0, request_id="r1").request_id == "r1"
    with pytest.raises(errors.ResponseFormatError, match="does not match"):
        parse(ok_body(id="other"), 0, request_id="r1")
    with pytest.raises(errors.ResponseFormatError, match="does not match"):
        parse(ok_body(), 0, request_id="r1")                    # id was sent but not echoed
    with pytest.raises(errors.RequestRejectedError) as info:    # protocol errors echo the id when the engine could read it
        parse(dict(err_body("invalid_field"), id="r1"), 2, request_id="r1")
    assert info.value.request_id == "r1"
    with pytest.raises(errors.ResponseFormatError, match="does not match"):
        parse(dict(err_body("invalid_field"), id="zzz"), 2, request_id="r1")
    # a protocol error may omit the id (for example when the id itself was invalid); the sent id is still reported
    with pytest.raises(errors.RequestRejectedError) as info:
        parse(err_body("invalid_field"), 2, request_id="r1")
    assert info.value.request_id == "r1"


def test_error_context_is_bounded():
    huge = b"x" * 1_000_000
    with pytest.raises(errors.EngineProcessError) as info:
        parse(None, 2, raw=b"garbage " + huge, stderr=huge)
    assert len(str(info.value)) < 1000 and len(info.value.stderr) < 2200 and len(info.value.stdout) < 2200
    assert "truncated" in info.value.stderr
    assert errors.excerpt(None) == "" and errors.excerpt(b"ab\xff") == "ab�"


def test_exception_hierarchy():
    for cls in [errors.InvalidArgumentError, errors.RequestRejectedError, errors.EngineInternalError, errors.EngineProcessError,
                errors.ResponseFormatError, errors.EngineTimeoutError, errors.EngineLaunchError, errors.ExecutableNotFoundError,
                errors.OperationNotSuccessfulError]:
        assert issubclass(cls, errors.SudokuError)
    assert issubclass(errors.InvalidArgumentError, ValueError)


# ------------------------------------------------------------ typed results

def grid_result(**changes):
    return {"seed": "18446744073709551615", "grid": SOLVED, "stats": dict(STATS)} | changes


def test_grid_result():
    r = models.GridResult.from_envelope(envelope("generate_grid", "generated", True, grid_result()))
    assert r.seed == 2**64 - 1 and r.grid == SOLVED and r.stats.nodes == 5 and r.status is models.GridStatus.GENERATED
    assert r.require_success() is r


@pytest.mark.parametrize("change", [{"seed": 5}, {"seed": "01"}, {"grid": EASY}, {"grid": 5}, {"stats": {"nodes": 1}},
                                    {"stats": dict(STATS, extra=1)}, {"stats": dict(STATS, nodes=-1)},
                                    {"stats": dict(STATS, nodes=True)}, {"stats": dict(STATS, nodes=1.5)}, {"extra": 1}])
def test_grid_result_rejects_bad_fields(change):
    with pytest.raises(errors.ResponseFormatError):
        models.GridResult.from_envelope(envelope("generate_grid", "generated", True, grid_result(**change)))
    missing = grid_result()
    for key in ["seed", "grid", "stats"]:
        broken = {k: v for k, v in missing.items() if k != key}
        with pytest.raises(errors.ResponseFormatError, match="missing"):
            models.GridResult.from_envelope(envelope("generate_grid", "generated", True, broken))


def test_unknown_status_and_success_mismatch_are_rejected():
    with pytest.raises(errors.ResponseFormatError, match="unknown status"):
        models.GridResult.from_envelope(envelope("generate_grid", "weird", True, grid_result()))
    with pytest.raises(errors.ResponseFormatError, match="disagrees"):
        models.GridResult.from_envelope(envelope("generate_grid", "generated", False, grid_result()))
    with pytest.raises(errors.ResponseFormatError, match="disagrees"):
        models.SearchSolveResult.from_envelope(envelope("solve_search", "unsolvable", True, {"stats": dict(STATS)}))


def test_puzzle_result_and_consistency_checks():
    base = {"seed": "7", "requested_clues": 30, "target_reached": True, "puzzle": EASY, "solution": SOLVED,
            "clue_count": 30, "removal": {"cells_considered": 5, "removed": 51, "rejected_not_unique": 1, "rejected_inconclusive": 0}}
    r = models.PuzzleResult.from_envelope(envelope("generate_puzzle", "target_reached", True, base))
    assert r.puzzle == EASY and r.removed == 51 and r.target_reached
    nr = models.PuzzleResult.from_envelope(envelope("generate_puzzle", "target_not_reached", True,
                                                                         base | {"target_reached": False, "requested_clues": 25}))
    assert nr.success and not nr.target_reached                       # unique puzzle, clue goal not met
    for bad in [base | {"target_reached": False}, base | {"clue_count": 31}, base | {"solution": SOLVED[:-1] + "."},
                base | {"puzzle": SOLVED.replace("5", "1", 1)}]:
        with pytest.raises(errors.ResponseFormatError):
            models.PuzzleResult.from_envelope(envelope("generate_puzzle", "target_reached", True, bad))


def test_search_and_count_results():
    solved = models.SearchSolveResult.from_envelope(envelope("solve_search", "solved", True, {"solution": SOLVED, "stats": dict(STATS)}))
    assert solved.solution == SOLVED
    unsolvable = models.SearchSolveResult.from_envelope(envelope("solve_search", "unsolvable", False, {"stats": dict(STATS)}))
    assert unsolvable.solution is None and not unsolvable.success
    with pytest.raises(errors.OperationNotSuccessfulError) as info:
        unsolvable.require_success()
    assert info.value.result is unsolvable
    with pytest.raises(errors.ResponseFormatError):    # a solution without 'solved'
        models.SearchSolveResult.from_envelope(envelope("solve_search", "unsolvable", False, {"solution": SOLVED, "stats": dict(STATS)}))
    count = {"limit": 2, "count": 2, "exact": False, "classification": "multiple", "first_solution": SOLVED, "stats": dict(STATS)}
    r = models.CountResult.from_envelope(envelope("count_solutions", "limit_reached", True, count))
    assert not r.exact and r.classification is models.SolutionClass.MULTIPLE and r.count == 2
    for bad in [count | {"exact": True}, count | {"count": 3}, count | {"classification": "bogus"},
                count | {"classification": "unique"}]:         # 'unique' needs an exact count of one
        with pytest.raises(errors.ResponseFormatError):
            models.CountResult.from_envelope(envelope("count_solutions", "limit_reached", True, bad))
    with pytest.raises(errors.ResponseFormatError):
        models.CountResult.from_envelope(envelope("count_solutions", "exhausted", True, count))   # exact must be True


def test_logical_result_with_and_without_trace():
    stats = {"deductions": 2, "placements": 2, "eliminated_candidates": 0, "by_technique": dict(BY_TECHNIQUE)}
    body = {"reason": "puzzle_solved", "board": SOLVED, "stats": stats}
    r = models.LogicalResult.from_envelope(envelope("solve_logical", "solved", True, body))
    assert r.steps is None and r.deductions == 2 and r.by_technique["naked_single"] == 2
    steps = [{"technique": "naked_single", "action": "place", "targets": [{"cell": 0, "digit": 5}]},
             {"technique": "naked_single", "action": "place", "targets": [{"cell": 1, "digit": 3}]}]
    traced = models.LogicalResult.from_envelope(envelope("solve_logical", "solved", True, body | {"steps": steps}))
    assert traced.steps[0].targets == ((0, 5),) and len(traced.steps) == 2
    stalled = models.LogicalResult.from_envelope(envelope("solve_logical", "stalled", False, body | {"reason": "no_applicable_deduction", "board": EASY}))
    assert not stalled.success and "." in stalled.board
    for bad in [body | {"board": EASY}, {k: v for k, v in body.items() if k != "board"},                  # solved needs a complete board
                body | {"steps": steps[:1]}, body | {"steps": [dict(steps[0], action="teleport"), steps[1]]},
                body | {"steps": [dict(steps[0], targets=[{"cell": 81, "digit": 5}]), steps[1]]},
                body | {"steps": [dict(steps[0], targets=[{"cell": 0, "digit": 10}]), steps[1]]},
                body | {"stats": dict(stats, by_technique=dict(BY_TECHNIQUE, naked_single=1))}]:        # counts must add up
        with pytest.raises(errors.ResponseFormatError):
            models.LogicalResult.from_envelope(envelope("solve_logical", "solved", True, bad))


def test_verify_and_classify_results():
    ok = models.VerifyResult.from_envelope(envelope("verify", "valid", True, {"valid": True}))
    assert ok.valid and ok.index is None
    bad = models.VerifyResult.from_envelope(envelope("verify", "empty_cell", False, {"valid": False, "fault": "empty_cell", "index": 40}))
    assert not bad.valid and bad.index == 40 and bad.status is models.VerifyStatus.EMPTY_CELL
    for body, status, success in [({"valid": True, "fault": "empty_cell"}, "valid", True), ({"valid": False}, "empty_cell", False),
                                  ({"valid": True}, "empty_cell", False), ({"valid": False, "fault": "row_violation", "index": 0}, "empty_cell", False)]:
        with pytest.raises(errors.ResponseFormatError):
            models.VerifyResult.from_envelope(envelope("verify", status, success, body))
    done = {"solver_status": "solved", "solver_reason": "puzzle_solved", "profile": "Easy", "profile_index": 0, "metrics": METRICS}
    c = models.ClassifyResult.from_envelope(envelope("classify", "classified", True, done))
    assert c.profile == "Easy" and c.metrics.deductions == 2 and c.metrics.by_technique["naked_single"] == 2
    u = models.ClassifyResult.from_envelope(envelope("classify", "unsolved", False, {"solver_status": "stalled", "solver_reason": "no_applicable_deduction"}))
    assert u.profile is None and u.metrics is None
    for bad_body, status, success in [(done, "unsolved", False), ({"solver_status": "s", "solver_reason": "r"}, "classified", True),
                                      (done | {"profile": 5}, "classified", True)]:
        with pytest.raises(errors.ResponseFormatError):
            models.ClassifyResult.from_envelope(envelope("classify", status, success, bad_body))


def targeted_body(**changes):
    body = {"target": "Easy", "seed": "1", "attempts": 2, "max_attempts": 60, "attempt_seed": "99",
            "rejections": {"easier_than_target": 0, "harder_than_target": 0, "unsolved": 1, "degenerate": 0, "inconclusive": 0},
            "last_rejection": "unsolved", "puzzle": EASY, "solution": SOLVED, "clue_count": 30,
            "classification": {"profile": "Easy", "profile_index": 0, "metrics": METRICS}}
    return body | changes


def test_targeted_results_generated_and_exhausted():
    g = models.TargetedResult.from_envelope(envelope("generate_targeted", "generated", True, targeted_body()))
    assert g.success and g.puzzle == EASY and g.classification.profile == "Easy" and g.attempt_seed == 99 and g.rejections.total() == 1
    ex_body = {k: v for k, v in targeted_body(target="Master", attempts=3, max_attempts=3).items()
               if k not in ("puzzle", "solution", "clue_count", "classification")}
    ex_body["rejections"] = {"easier_than_target": 2, "harder_than_target": 0, "unsolved": 1, "degenerate": 0, "inconclusive": 0}
    ex = models.TargetedResult.from_envelope(envelope("generate_targeted", "exhausted", False, ex_body))
    assert not ex.success and ex.puzzle is None and ex.solution is None and ex.classification is None and ex.attempts == 3
    with pytest.raises(errors.OperationNotSuccessfulError):
        ex.require_success()
    bad_bodies = [
        ("generated", True, targeted_body(classification={"profile": "Hard", "profile_index": 2, "metrics": METRICS})),   # wrong profile
        ("generated", True, {k: v for k, v in targeted_body().items() if k != "puzzle"}),
        ("generated", True, targeted_body(attempts=70)),                                                                  # over the limit
        ("generated", True, targeted_body(attempts=5)),                                                                   # rejection counts disagree
        ("generated", True, targeted_body(attempt_seed="x")),
        ("exhausted", False, dict(ex_body, puzzle=EASY)),                                                                 # exhausted with a puzzle
        ("exhausted", False, {k: v for k, v in ex_body.items() if k != "attempt_seed"}),
        ("generated", True, targeted_body(rejections={"easier_than_target": 0})),
        ("generated", True, targeted_body(extra=1)),
    ]
    for status, success, body in bad_bodies:
        with pytest.raises(errors.ResponseFormatError):
            models.TargetedResult.from_envelope(envelope("generate_targeted", status, success, body))


def test_capabilities_result():
    body = {"engine_version": "0.1.0", "request_schemas": ["sudoku.request/1"], "response_schema": "sudoku.response/1",
            "operations": list(protocol.OPERATIONS), "techniques": TECHNIQUES,
            "profiles": [{"name": "Easy", "techniques": ["naked_single", "hidden_single"]}],
            "board": {"length": 81, "input_characters": "123456789.0", "output_empty": "."},
            "limits": {"max_request_bytes": 1048576, "max_attempts": 1000000}}
    c = models.Capabilities.from_envelope(envelope("capabilities", "ok", True, body))
    assert c.engine_version == "0.1.0" and c.profiles[0].name == "Easy" and c.limits["max_attempts"] == 1000000
    for bad in [body | {"extra": 1}, body | {"operations": [1]}, body | {"board": {"length": 81}}, {k: v for k, v in body.items() if k != "limits"}]:
        with pytest.raises(errors.ResponseFormatError):
            models.Capabilities.from_envelope(envelope("capabilities", "ok", True, bad))
    with pytest.raises(errors.ResponseFormatError):
        models.Capabilities.from_envelope(envelope("capabilities", "fine", True, body))
    assert copy.deepcopy(body) == body    # (the validators never mutate their input)


# --------------------------------------- gaps found by mutation testing

def test_duplicate_keys_in_an_otherwise_valid_response_are_rejected():
    valid = json.dumps(ok_body())
    assert parse(None, 0, raw=(valid + "\n").encode()).status == "generated"
    for duplicated in ['{"schema":"sudoku.response/1","op":"generate_grid","ok":true,"ok":true,"success":true,"status":"generated","result":{}}',
                       '{"schema":"sudoku.response/1","op":"generate_grid","ok":true,"success":true,"status":"generated","status":"generated","result":{}}',
                       '{"schema":"sudoku.response/1","op":"generate_grid","ok":true,"success":true,"status":"generated","result":{"a":1,"a":1}}']:
        with pytest.raises(errors.ResponseFormatError, match="valid JSON"):
            parse(None, 0, raw=(duplicated + "\n").encode())
    assert protocol.loads_strict('{"a":1}') == {"a": 1}
    for text in ['{"a":1,"a":1}', "NaN", "[Infinity]", '{"a":-Infinity}']:
        with pytest.raises(ValueError):
            protocol.loads_strict(text)


def test_uniqueness_is_derived_only_from_an_exact_count_of_one():
    from sudoku_app.orchestration import PuzzleReport

    generation = models.PuzzleResult(models.PuzzleStatus.TARGET_REACHED, True, 0, 30, True, EASY, SOLVED, 30, 0, 0, 0, 0)
    verification = models.VerifyResult(models.VerifyStatus.VALID, True, True, None, None)
    classification = models.ClassifyResult(models.ClassifyStatus.UNSOLVED, False, "stalled", "r", None, None, None)
    stats = models.SearchStats(1, 1, 1)

    def report(**count):
        base = dict(status=models.CountStatus.EXHAUSTED, success=True, limit=2, count=1, exact=True,
                    classification=models.SolutionClass.UNIQUE, first_solution=SOLVED, stats=stats)
        return PuzzleReport(generation, models.CountResult(**(base | count)), verification, classification)

    assert report().is_unique
    assert not report(status=models.CountStatus.LIMIT_REACHED, exact=False, classification=models.SolutionClass.UNKNOWN).is_unique   # one found, not proven
    assert not report(status=models.CountStatus.BUDGET_EXHAUSTED, success=False, exact=False, classification=models.SolutionClass.UNKNOWN).is_unique
    assert not report(count=2, exact=False, status=models.CountStatus.LIMIT_REACHED, classification=models.SolutionClass.MULTIPLE).is_unique
    assert not report(classification=models.SolutionClass.UNKNOWN).is_unique      # an exact one must also be classified unique
    assert report().profile is None and report().solution_verified     # no label invented for an unclassified puzzle
