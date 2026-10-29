"""Integration tests of the real sudoku_cli executable and its JSON protocol.

Build first (cmake --preset dev && cmake --build --preset dev); set SUDOKU_CLI to
test another binary. If the binary is missing the tests are skipped, never
silently passed. Python's json module is the independent, established parser
that validates everything the executable prints.
"""
import json
import os
import random
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CLI = Path(os.environ.get("SUDOKU_CLI", ROOT / "build" / "dev" / "sudoku_cli"))
BENCH = Path(os.environ.get("SUDOKU_BENCH", ROOT / "build" / "dev" / "benchmarks" / "sudoku_bench"))

pytestmark = pytest.mark.skipif(not CLI.exists(), reason=f"sudoku_cli not built: {CLI}")

EASY = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
EASY_SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
STALLED = ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4."
TWO_SOLUTIONS = "534008912672195348198342567859001423426853791713924856961537284287419635345286179"


def strict_loads(text):
    """json.loads that also rejects duplicate keys and NaN/Infinity."""
    def no_dups(pairs):
        keys = [k for k, _ in pairs]
        if len(keys) != len(set(keys)):
            raise ValueError("duplicate key")
        return dict(pairs)

    def no_const(name):
        raise ValueError(f"nonstandard constant {name}")

    return json.loads(text, object_pairs_hook=no_dups, parse_constant=no_const)


def run(stdin_text="", *args, raw=None, timeout=300):
    data = raw if raw is not None else stdin_text.encode()
    return subprocess.run([str(CLI), *args], input=data, capture_output=True, timeout=timeout)


def request(op, params=None, **extra):
    doc = {"schema": "sudoku.request/1", "op": op, **extra}
    if params is not None:
        doc["params"] = params
    return json.dumps(doc)


def call(op, params=None, **extra):
    """Runs one request; returns (exit_code, parsed response). Checks the output contract."""
    proc = run(request(op, params, **extra))
    return check_output(proc)


def check_output(proc):
    out = proc.stdout.decode("utf-8")           # must be valid UTF-8
    assert out.endswith("\n") and out.count("\n") == 1, f"stdout must be exactly one line: {out!r}"
    response = strict_loads(out)                # exactly one valid JSON document
    assert isinstance(response, dict) and response["schema"] == "sudoku.response/1"
    assert isinstance(response["ok"], bool)
    if response["ok"]:
        assert isinstance(response["success"], bool) and "status" in response and "result" in response
        assert "error" not in response
        assert proc.returncode == (0 if response["success"] else 1)
        assert proc.stderr == b""               # normal operation writes nothing to stderr
    else:
        assert "error" in response and "status" not in response and "result" not in response
        assert proc.returncode in (2, 3)
        assert isinstance(response["error"]["code"], str) and isinstance(response["error"]["message"], str)
    return proc.returncode, response


# ------------------------------------------------------------------ basics

def test_version_and_help_are_plain_text():
    version = run("", "--version")
    assert version.returncode == 0 and version.stdout.decode().startswith("sudoku_cli 0.1.0")
    help_ = run("", "--help")
    assert help_.returncode == 0 and b"sudoku.request/1" in help_.stdout and help_.stderr == b""
    for proc in (version, help_):
        with pytest.raises(ValueError):
            json.loads(proc.stdout)             # documented as not JSON


@pytest.mark.parametrize("args", [["--bogus"], ["--input"], ["--output"], ["extra"], ["--version", "--help"], ["--input", "a", "--bogus"]])
def test_bad_command_line_exits_2_with_empty_stdout(args):
    proc = run("", *args)
    assert proc.returncode == 2 and proc.stdout == b"" and proc.stderr != b""


def test_capabilities():
    code, r = call("capabilities")
    assert code == 0 and r["ok"] and r["success"] and r["status"] == "ok" and r["op"] == "capabilities"
    caps = r["result"]
    assert caps["response_schema"] == "sudoku.response/1" and caps["request_schemas"] == ["sudoku.request/1"]
    assert set(caps["operations"]) == {"capabilities", "generate_grid", "generate_puzzle", "solve_search", "count_solutions",
                                       "solve_logical", "verify", "classify", "generate_targeted"}
    assert [p["name"] for p in caps["profiles"]] == ["Easy", "Medium", "Hard", "Expert", "Master"]
    assert caps["limits"]["max_request_bytes"] == 1048576 and caps["engine_version"] == "0.1.0"


def test_request_id_is_echoed():
    code, r = call("capabilities", id="req-ü-1")
    assert r["id"] == "req-ü-1"
    code, r = call("verify", {}, id="bad")
    assert code == 2 and r["id"] == "bad" and r["op"] == "verify"


# --------------------------------------------------------------- exit codes

def test_exit_code_0_success():
    code, r = call("generate_grid", {"seed": "1"})
    assert code == 0 and r["status"] == "generated"


def test_exit_code_1_valid_request_non_success():
    code, r = call("generate_targeted", {"seed": "1", "target": "Master", "max_attempts": 3})
    assert code == 1 and r["ok"] is True and r["success"] is False and r["status"] == "exhausted"
    assert "puzzle" not in r["result"] and "solution" not in r["result"]   # no fallback puzzle
    code, r = call("solve_logical", {"puzzle": STALLED})
    assert code == 1 and r["status"] == "stalled"
    code, r = call("classify", {"puzzle": STALLED})
    assert code == 1 and r["status"] == "unsolved" and "profile" not in r["result"]
    code, r = call("count_solutions", {"puzzle": EASY, "node_budget": 1})
    assert code == 1 and r["status"] == "budget_exhausted" and r["result"]["classification"] == "unknown"


@pytest.mark.parametrize("text,code_name", [
    ("", "empty_request"), ("   \n", "empty_request"), ("{", "parse_error"), ("nonsense", "parse_error"),
    ("[]", "invalid_request"), ('{"schema":"sudoku.request/2","op":"capabilities"}', "unsupported_schema"),
    ('{"schema":"sudoku.request/1","op":"nope"}', "unknown_operation"),
    ('{"schema":"sudoku.request/1","op":"capabilities","x":1}', "unknown_field"),
    ('{"schema":"sudoku.request/1","op":"verify"}', "missing_field"),
])
def test_exit_code_2_protocol_errors(text, code_name):
    proc = run(text)
    code, r = check_output(proc)
    assert code == 2 and r["ok"] is False and r["error"]["code"] == code_name


def test_trailing_input_and_two_requests_are_rejected():
    one = request("capabilities")
    for text in (one + " x", one + one, one + "\n" + one):
        code, r = check_output(run(text))
        assert code == 2 and r["error"]["code"] == "parse_error"
    code, r = check_output(run(one + "  \n\t\r\n"))   # trailing whitespace is fine
    assert code == 0


def test_oversized_request_is_rejected_without_hanging():
    proc = run(raw=b" " * (1048576 + 1))
    code, r = check_output(proc)
    assert code == 2 and r["error"]["code"] == "request_too_large"
    ok = request("capabilities").encode()
    proc = run(raw=ok + b" " * (1048576 - len(ok)))        # exactly at the limit is accepted
    assert check_output(proc)[0] == 0


def test_input_and_output_files(tmp_path):
    src, dst = tmp_path / "in.json", tmp_path / "out.json"
    src.write_text(request("generate_grid", {"seed": "9"}))
    proc = run("", "--input", str(src), "--output", str(dst))
    assert proc.returncode == 0 and proc.stdout == b"" and proc.stderr == b""
    assert strict_loads(dst.read_text())["status"] == "generated"
    missing = run("", "--input", str(tmp_path / "missing.json"))
    code, r = missing.returncode, strict_loads(missing.stdout.decode())
    assert code == 3 and r["ok"] is False and r["error"]["code"] == "internal_error"
    assert b"cannot open input file" in missing.stderr and "cannot open" not in json.dumps(r)   # detail only on stderr
    bad_out = run(request("capabilities"), "--output", str(tmp_path / "no" / "dir" / "o.json"))
    assert bad_out.returncode == 3 and bad_out.stdout == b"" and bad_out.stderr != b""


# ------------------------------------------------------------- seeds / boards

@pytest.mark.parametrize("seed", ["0", "1", "9007199254740993", "18446744073709551615"])
def test_full_range_seeds_are_preserved(seed):
    code, r = call("generate_grid", {"seed": seed})
    assert code == 0 and r["result"]["seed"] == seed


@pytest.mark.parametrize("seed", ["18446744073709551616", "-1", "01", "", "1.0", " 1", "+1", 5, 1.5, None, True, [], {}])
def test_invalid_seeds(seed):
    code, r = call("generate_grid", {"seed": seed})
    assert code == 2 and r["error"]["code"] == "invalid_field" and r["error"]["field"] == "params.seed"


@pytest.mark.parametrize("board", ["", "." * 80, "." * 82, "x" * 81, "5" * 40 + "a" + "." * 40, 5, None])
def test_invalid_boards(board):
    code, r = call("classify", {"puzzle": board})
    assert code == 2 and r["error"]["code"] == "invalid_field" and r["error"]["field"] == "params.puzzle"


def test_empty_cell_spellings():
    for empty in ".0":
        code, r = call("classify", {"puzzle": empty * 81})
        assert code == 1 and r["status"] == "unsolved"


# -------------------------------------------------------------- operations

def digits(board):
    return [int(c) if c != "." else 0 for c in board]


def valid_grid(g):
    rows = [g[i * 9:(i + 1) * 9] for i in range(9)]
    cols = [g[i::9] for i in range(9)]
    boxes = [[g[(r + dr) * 9 + c + dc] for dr in range(3) for dc in range(3)] for r in (0, 3, 6) for c in (0, 3, 6)]
    return all(sorted(u) == list(range(1, 10)) for u in rows + cols + boxes)


def test_generate_grid_is_valid_and_deterministic():
    code, a = call("generate_grid", {"seed": "42"})
    assert valid_grid(digits(a["result"]["grid"]))
    assert call("generate_grid", {"seed": "42"})[1] == a
    assert call("generate_grid", {"seed": "43"})[1]["result"]["grid"] != a["result"]["grid"]


def test_generate_puzzle_is_unique_and_consistent():
    code, r = call("generate_puzzle", {"seed": "7", "target_clues": 30})
    res = r["result"]
    assert code == 0 and r["status"] == "target_reached" and res["clue_count"] == 30
    puzzle, solution = res["puzzle"], res["solution"]
    assert valid_grid(digits(solution)) and all(p in (".", s) for p, s in zip(puzzle, solution))
    # independent uniqueness: exactly one solution according to the engine's counter
    code, c = call("count_solutions", {"puzzle": puzzle, "limit": 2})
    assert c["result"]["count"] == 1 and c["result"]["exact"] and c["result"]["first_solution"] == solution
    assert call("generate_puzzle", {"seed": "7", "target_clues": 30})[1] == r


def test_search_and_count_semantics():
    code, r = call("solve_search", {"puzzle": EASY})
    assert code == 0 and r["result"]["solution"] == EASY_SOLVED
    code, r = call("count_solutions", {"puzzle": TWO_SOLUTIONS})
    assert code == 0 and r["status"] == "limit_reached" and r["result"]["exact"] is False and r["result"]["count"] == 2
    assert r["result"]["classification"] == "multiple"
    code, r = call("count_solutions", {"puzzle": TWO_SOLUTIONS, "limit": 10})
    assert r["status"] == "exhausted" and r["result"]["exact"] is True and r["result"]["count"] == 2
    code, r = call("count_solutions", {"puzzle": EASY, "limit": 1})
    assert r["status"] == "limit_reached" and r["result"]["classification"] == "unknown" and r["result"]["exact"] is False
    code, r = call("count_solutions", {"puzzle": "55" + "." * 79})
    assert code == 1 and r["status"] == "invalid_input"


def test_logical_solving_does_not_guess():
    code, r = call("solve_logical", {"puzzle": EASY, "include_trace": True})
    assert code == 0 and r["result"]["board"] == EASY_SOLVED
    assert len(r["result"]["steps"]) == r["result"]["stats"]["deductions"] == 51
    code, r = call("solve_logical", {"puzzle": STALLED})
    assert code == 1 and r["status"] == "stalled" and "." in r["result"]["board"]
    code, r = call("solve_logical", {"puzzle": EASY, "techniques": ["naked_single"]})
    assert r["status"] in ("solved", "stalled") and r["result"]["stats"]["by_technique"]["hidden_single"] == 0


def test_verify_and_classify():
    code, r = call("verify", {"puzzle": EASY, "solution": EASY_SOLVED})
    assert code == 0 and r["result"]["valid"] is True
    code, r = call("verify", {"puzzle": EASY, "solution": EASY_SOLVED[:40] + "." + EASY_SOLVED[41:]})
    assert code == 1 and r["status"] == "empty_cell" and r["result"]["index"] == 40
    code, r = call("classify", {"puzzle": EASY})
    assert code == 0 and r["result"]["profile"] == "Easy" and r["result"]["metrics"]["deductions"] == 51


def test_targeted_generation_exact_match_and_determinism():
    code, r = call("generate_targeted", {"seed": "1", "target": "Easy", "max_attempts": 60})
    res = r["result"]
    assert code == 0 and r["status"] == "generated" and res["classification"]["profile"] == "Easy"
    assert 1 <= res["attempts"] <= 60 and sum(res["rejections"].values()) + 1 == res["attempts"]
    code, c = call("classify", {"puzzle": res["puzzle"]})
    assert c["result"]["profile"] == "Easy"                                      # independent classification agrees
    code, u = call("count_solutions", {"puzzle": res["puzzle"]})
    assert u["result"]["classification"] == "unique"
    assert call("generate_targeted", {"seed": "1", "target": "Easy", "max_attempts": 60})[1] == r
    for bad in ("easy", "Legendary", 5):
        code, e = call("generate_targeted", {"seed": "1", "target": bad})
        assert code == 2 and e["error"]["field"] == "params.target"


def test_full_range_seed_in_targeted_response():
    code, r = call("generate_targeted", {"seed": "18446744073709551615", "target": "Easy", "max_attempts": 2})
    assert r["result"]["seed"] == "18446744073709551615" and isinstance(r["result"].get("attempt_seed", ""), str)


def test_diagnostics_schema_is_not_the_protocol():
    code, r = call("generate_targeted", {"seed": "1", "target": "Master", "max_attempts": 2})
    assert "sudoku.targeted-diagnostics" not in json.dumps(r) and r["schema"] == "sudoku.response/1"


# ------------------------------------------------- reader vs Python's parser

def python_accepts(raw: bytes) -> bool:
    try:
        text = raw.decode("utf-8")
        strict_loads(text)
        return text.strip(" \t\n\r") != ""
    except (ValueError, RecursionError):
        return False


def cli_says_parse_error(raw: bytes) -> bool:
    proc = run(raw=raw)
    response = strict_loads(proc.stdout.decode())
    return (not response["ok"]) and response["error"]["code"] in ("parse_error", "empty_request")


def test_reader_agrees_with_python_json_on_curated_inputs():
    valid = ['{}', '[]', 'null', 'true', '0', '-0', '1.5e3', '"x"', ' [1, 2 ] ', '{"a":{"b":[1,{"c":null}]}}',
             '"\\u00e9"', '"\\ud83d\\ude00"', '"é€"', '1E5', '-1e-2', '{"schema":"sudoku.request/1","op":"capabilities"}']
    invalid = ['', ' ', '{', '}', '[1,]', '{"a":1,}', '[1 2]', '01', '1.', '.5', '+1', '-', '1e', 'NaN', 'Infinity', '-Infinity',
               'tru', 'nul', "'a'", '{a:1}', '"abc', '"\\q"', '"\\u12"', '[1] x', '{} {}', '{"a":1,"a":2}', '"a\nb"',
               '{"a":}', '[,1]', '{"a" 1}', '--1', '0x10', '1,2', '"\\ud800"', '"\\udc00"']
    for text in valid:
        assert python_accepts(text.encode()), text
        assert not cli_says_parse_error(text.encode()), f"CLI rejected valid JSON: {text!r}"
    for text in invalid:
        assert not python_accepts(text.encode()) or text == '"\\ud800"' or text == '"\\udc00"'
        assert cli_says_parse_error(text.encode()), f"CLI accepted invalid JSON: {text!r}"
    for raw in (b'"\xff"', b'"\xc0\x80"', b'"\xed\xa0\x80"', b'"\xe2\x82"', b'["\x01"]', b'\xef\xbb\xbf{}'):
        assert not python_accepts(raw) or raw.startswith(b"\xef\xbb\xbf")
        assert cli_says_parse_error(raw), raw


def test_reader_agrees_with_python_json_on_mutated_inputs():
    rng = random.Random(20260101)
    bases = [request("generate_grid", {"seed": "5"}), request("verify", {"puzzle": EASY, "solution": EASY_SOLVED}, id="x"),
             '[1, 2.5e3, "s", true, null, {"k": [false]}]']
    alphabet = '{}[]",:\\ \n0123456789.-eEtrufalsn+'
    checked = disagreements = python_valid = 0
    for _ in range(250):
        text = rng.choice(bases)
        for _ in range(rng.randint(1, 3)):
            if not text:
                break
            pos = rng.randrange(len(text))
            kind = rng.randrange(4)
            if kind == 0:
                text = text[:pos] + text[pos + 1:]
            elif kind == 1:
                text = text[:pos] + rng.choice(alphabet) + text[pos:]
            elif kind == 2:
                text = text[:pos] + rng.choice(alphabet) + text[pos + 1:]
            else:
                text = text[:pos]
        raw = text.encode()
        expected_bad = not python_accepts(raw)
        got_bad = cli_says_parse_error(raw)
        checked += 1
        python_valid += 0 if expected_bad else 1
        if expected_bad != got_bad:
            disagreements += 1
            print("DISAGREE", repr(text), "python_bad=", expected_bad, "cli_bad=", got_bad)
    assert checked == 250 and disagreements == 0
    # The sample must contain both outcomes in quantity, or agreement would prove little.
    assert 40 <= python_valid <= 210, python_valid


def test_nesting_limit_is_documented_behaviour():
    deep_ok = "[" * 32 + "]" * 32
    deep_bad = "[" * 33 + "]" * 33
    assert not cli_says_parse_error(deep_ok.encode())          # valid JSON, rejected later as not an object
    assert cli_says_parse_error(deep_bad.encode())             # valid JSON, but over the documented depth limit
    assert cli_says_parse_error(("[" * 200000).encode())       # no stack exhaustion


# -------------------------------------------------------- compatibility

@pytest.mark.skipif(not BENCH.exists(), reason="benchmark tool not built")
def test_benchmark_tool_is_unaffected():
    proc = subprocess.run([str(BENCH), "--tier", "smoke"], capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0
    report = strict_loads(proc.stdout)
    assert report["schema"] == "sudoku.benchmark/1" and report["ok"] is True
    diag = next(r for r in report["records"] if r["case"] == "targeted.easy.seed1")["detail"]
    assert diag["schema"] == "sudoku.targeted-diagnostics/1" and isinstance(diag["seed"], int)   # diagnostics keep numeric seeds


# ----------------------------------------------------------- documentation

def readme_examples():
    """(request text, expected exit code, expected response text) from the README and docs/REFERENCE.md."""
    import re
    text = (ROOT / "README.md").read_text() + "\n" + (ROOT / "docs" / "REFERENCE.md").read_text()
    pattern = re.compile(r"```json request\n(.*?)\n```\s*```json response exit=(\d+)\n(.*?)\n```", re.S)
    return [(req, int(code), resp) for req, code, resp in pattern.findall(text)]


def test_readme_examples():
    """Every documented request/response pair is exactly what the executable prints."""
    examples = readme_examples()
    assert len(examples) >= 7, "the README examples were not found"
    for request_text, exit_code, expected in examples:
        proc = run(request_text)
        assert proc.returncode == exit_code, request_text
        assert proc.stdout.decode().rstrip("\n") == expected, request_text
        strict_loads(expected)                  # and the documented text is itself valid JSON
