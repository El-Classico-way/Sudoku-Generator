"""Command-line interface, launcher and licence tests."""
from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from sudoku_app import cli

ROOT = Path(__file__).resolve().parents[2]
RUN_SH = ROOT / "run.sh"
PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
EMPTY = "." * 81
NS = "{http://www.w3.org/2000/svg}"


@pytest.fixture()
def real(engine_executable, monkeypatch, tmp_path):
    """Run the CLI in-process against the real engine from an empty scratch directory."""
    monkeypatch.setenv("SUDOKU_CLI", str(engine_executable))
    monkeypatch.delenv(cli.OUTPUT_DIR_ENV, raising=False)       # bare output names: current directory
    monkeypatch.chdir(tmp_path)
    return tmp_path


def run(capsys, *args):
    code = cli.main(list(args))
    out = capsys.readouterr()
    return code, out.out, out.err


def fake_engine(tmp_path, *, stdout="", exit_code=0, sleep=0, stderr=""):
    """An executable shell script standing in for sudoku_cli."""
    reply = tmp_path / "reply.txt"
    reply.write_text(stdout)
    script = tmp_path / "fake_engine.sh"
    script.write_text(f"#!/bin/sh\ncat >/dev/null\n[ {sleep} -gt 0 ] && sleep {sleep}\ncat '{reply}'\necho '{stderr}' >&2\nexit {exit_code}\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


# ---- help and usage ---------------------------------------------------------------------------------------------------

def test_no_arguments_shows_help_successfully(capsys):
    code, out, err = run(capsys)
    assert code == 0 and "usage:" in out and err == ""
    for word in ("generate", "solve", "verify", "classify", "examples:", "exit codes"):
        assert word in out


@pytest.mark.parametrize("args", [["--help"], ["-h"], ["generate", "--help"], ["solve", "--help"], ["verify", "--help"], ["classify", "--help"]])
def test_help_exits_zero(capsys, args):
    with pytest.raises(SystemExit) as info:
        cli.main(args)
    assert info.value.code == 0 and "usage:" in capsys.readouterr().out


@pytest.mark.parametrize("args", [
    ["bogus"], ["generate", "--difficulty", "legendary"], ["generate", "--seed", "-1"], ["generate", "--seed", "x"],
    ["generate", "--seed", str(2**64)], ["generate", "--target-clues", "16"], ["generate", "--target-clues", "82"],
    ["generate", "--max-attempts", "0"], ["generate", "--theme", "neon"], ["--timeout", "0", "generate"], ["--timeout", "abc", "generate"],
    ["solve"], ["solve", "--board", "123"], ["solve", "--board", "x" * 81], ["solve", "--board", EMPTY, "--method", "guess"],
    ["verify", "--puzzle", EMPTY], ["verify", "--solution", SOLVED], ["classify"], ["classify", "--board", EMPTY[:-1]],
    ["generate", "--nonsense"]])
def test_invalid_commands_and_arguments_exit_2_with_a_short_message(capsys, args):
    with pytest.raises(SystemExit) as info:
        cli.main(args)
    assert info.value.code == 2
    err = capsys.readouterr().err
    assert "error:" in err and "Traceback" not in err


def test_board_parsing_accepts_both_empty_conventions_and_surrounding_whitespace():
    assert cli._board(f"  {PUZZLE}\n") == PUZZLE and cli._board(PUZZLE.replace(".", "0")) == PUZZLE.replace(".", "0")
    for bad in ("", PUZZLE[:-1], PUZZLE + "1", "x" * 81, PUZZLE.replace("5", " ", 1)):
        with pytest.raises(Exception):
            cli._board(bad)


def test_format_board_is_a_conventional_grid():
    lines = cli.format_board(PUZZLE.replace(".", "0")).splitlines()
    assert len(lines) == 11 and lines[0] == "5 3 . | . 7 . | . . ." and lines[3].startswith("------+")


# ---- generate -------------------------------------------------------------------------------------------------------------

def test_generate_plain(real, capsys):
    code, out, err = run(capsys, "generate", "--seed", "3")
    assert code == 0 and err == ""
    assert "seed:       3" in out and "unique:     yes (exhaustive" in out and "hidden" in out
    grid = out.split("\n\n")[0]
    cells = [c for c in grid.replace("|", " ").replace("-", " ").replace("+", " ").split()]
    assert len(cells) == 81 and set(cells) <= set("123456789.") and "." in cells
    assert f"clues:      {sum(c != '.' for c in cells)}" in out
    assert run(capsys, "generate", "--seed", "3")[1] == out                              # reproducible


def test_generate_without_seed_prints_the_random_seed(real, capsys):
    code, out, _ = run(capsys, "generate")
    assert code == 0 and "random; use --seed to reproduce" in out


def test_generate_with_difficulty_and_solution_reveal(real, capsys):
    code, out, _ = run(capsys, "generate", "--difficulty", "EASY", "--seed", "7", "--show-solution")
    assert code == 0 and "difficulty: Easy (requested and verified)" in out and "solution:\n" in out and "hidden" not in out
    code, hidden, _ = run(capsys, "generate", "--difficulty", "easy", "--seed", "7")
    assert "hidden (add --show-solution" in hidden and hidden.split("solution:")[0].rstrip() == out.split("solution:")[0].rstrip()


def test_generation_exhaustion_is_an_error_and_prints_no_puzzle(real, capsys):
    code, out, err = run(capsys, "generate", "--difficulty", "master", "--seed", "1", "--max-attempts", "2")
    assert code == 1 and out == "" and "error: no Master puzzle found in 2 attempt(s)" in err and "does not mean none exists" in err


def test_max_attempts_requires_a_difficulty(real, capsys):
    code, out, err = run(capsys, "generate", "--max-attempts", "5")
    assert code == 2 and out == "" and "--difficulty" in err


def test_generate_writes_svgs_with_givens_and_verified_solution(real, capsys):
    code, out, _ = run(capsys, "generate", "--difficulty", "easy", "--seed", "7", "--output", "puzzle.svg",
                       "--solution-output", "solution.svg", "--theme", "dark")
    assert code == 0 and "wrote puzzle image: puzzle.svg" in out and "wrote solution image: solution.svg" in out
    puzzle_cells = list(ET.parse(real / "puzzle.svg").getroot().iter(NS + "text"))
    solution_cells = list(ET.parse(real / "solution.svg").getroot().iter(NS + "text"))
    assert {t.get("class") for t in puzzle_cells} == {"given"} and len(solution_cells) == 81
    assert 'fill="#121212"' in (real / "puzzle.svg").read_text()
    assert [t.get("class") for t in solution_cells].count("given") == len(puzzle_cells)


def test_output_problems_are_reported_before_generating(real, capsys):
    (real / "exists.svg").write_text("keep")
    (real / "dir").mkdir()
    for args, needle in ((["--output", "exists.svg"], "already exists"), (["--output", "missing/x.svg"], "does not exist"),
                         (["--output", "dir"], "is a directory"), (["--output", "a.svg", "--solution-output", "a.svg"], "same file")):
        code, out, err = run(capsys, "generate", "--seed", "1", *args)
        assert code == 2 and out == "" and needle in err
    assert (real / "exists.svg").read_text() == "keep" and not (real / "a.svg").exists()
    assert run(capsys, "generate", "--seed", "1", "--output", "exists.svg", "--overwrite")[0] == 0
    assert (real / "exists.svg").read_text().startswith("<svg ")


# ---- solve / verify / classify ----------------------------------------------------------------------------------------------

def test_search_solve(real, capsys):
    code, out, _ = run(capsys, "solve", "--board", PUZZLE, "--unique")
    assert code == 0 and "5 3 4 | 6 7 8 | 9 1 2" in out and "solved (search)" in out and "unique: yes" in out
    code, out, _ = run(capsys, "solve", "--board", PUZZLE.replace(".", "0"))
    assert code == 0 and "use --unique" in out                                         # no uniqueness claim without a count


def test_search_solve_reports_several_solutions_without_claiming_uniqueness(real, capsys):
    code, out, _ = run(capsys, "solve", "--board", EMPTY, "--unique")
    assert code == 0 and "unique: no (at least two solutions exist" in out


def test_unsolvable_and_invalid_boards_are_distinguished(real, capsys):
    unsolvable = "12345678." + "........9" + "." * 63
    code, out, err = run(capsys, "solve", "--board", unsolvable)
    assert code == 1 and out == "" and "no solution" in err
    code, out, err = run(capsys, "solve", "--board", "11" + "." * 79)
    assert code == 2 and "invalid board" in err
    code, out, err = run(capsys, "solve", "--method", "logical", "--board", "11" + "." * 79)
    assert code == 2 and "invalid board" in err


def test_logical_solve_and_a_stall_that_is_not_called_unsolvable(real, capsys):
    code, out, _ = run(capsys, "solve", "--method", "logical", "--board", PUZZLE)
    assert code == 0 and "solved (logical deductions only" in out and "no guessing" in out
    code, out, err = run(capsys, "solve", "--method", "logical", "--board", EMPTY)
    assert code == 1 and "stalled" in err and "NOT mean the puzzle is unsolvable" in err
    assert "unsolvable" not in err.replace("NOT mean the puzzle is unsolvable", "")
    assert run(capsys, "solve", "--board", EMPTY)[0] == 0                              # the search solver can


def test_verify(real, capsys):
    code, out, _ = run(capsys, "verify", "--puzzle", PUZZLE, "--solution", SOLVED)
    assert code == 0 and out.startswith("valid:")
    swapped = SOLVED[:2] + SOLVED[3] + SOLVED[2] + SOLVED[4:]
    code, out, _ = run(capsys, "verify", "--board", EMPTY, "--solution", swapped)
    assert code == 1 and out.startswith("invalid:") and "does not contain each digit exactly once" in out
    code, out, _ = run(capsys, "verify", "--puzzle", PUZZLE, "--solution", PUZZLE)
    assert code == 1 and "incomplete" in out and "row 1, column 3" in out
    changed = "4" + SOLVED[1:]                                      # the first given (5) replaced by 4: the grid is still complete
    code, out, _ = run(capsys, "verify", "--puzzle", PUZZLE, "--solution", changed)
    assert code == 1 and out.startswith("invalid:") and ("given" in out or "exactly once" in out)


def test_classify(real, capsys):
    code, out, _ = run(capsys, "classify", "--board", PUZZLE)
    assert code == 0 and "difficulty: Easy" in out and "steps: 51" in out and "naked_single" in out
    code, out, err = run(capsys, "classify", "--board", EMPTY)
    assert code == 1 and "stall" in err and "does not mean it is unsolvable" in err
    code, out, err = run(capsys, "classify", "--board", "11" + "." * 79)
    assert code == 2 and "invalid board" in err


# ---- engine problems ---------------------------------------------------------------------------------------------------------

def test_missing_engine(capsys, monkeypatch, tmp_path):
    monkeypatch.setenv("SUDOKU_CLI", str(tmp_path / "nope"))
    code, out, err = run(capsys, "solve", "--board", PUZZLE)
    assert code == 3 and out == "" and "error:" in err and "run.sh" in err and "Traceback" not in err
    code, _, err = run(capsys, "--engine", str(tmp_path / "other"), "solve", "--board", PUZZLE)
    assert code == 3 and "other" in err


def test_engine_failures_have_short_messages_and_no_traceback(capsys, monkeypatch, tmp_path):
    cases = [
        dict(stdout="not json\n", exit_code=0),
        dict(stdout="", exit_code=9, stderr="boom"),
        dict(stdout='{"schema":"sudoku.response/1","ok":false,"error":{"code":"internal_error","message":"x"}}\n', exit_code=3),
        dict(stdout='{"schema":"sudoku.response/1","op":"verify","ok":false,"error":{"code":"invalid_field","message":"no","field":"params.puzzle"}}\n',
             exit_code=2),
    ]
    for case in cases:
        engine = fake_engine(tmp_path, **case)
        code, out, err = run(capsys, "--engine", str(engine), "verify", "--puzzle", PUZZLE, "--solution", SOLVED)
        assert code == 3 and out == "" and err.startswith("error: engine problem:") and "Traceback" not in err, case


def test_engine_timeout(capsys, tmp_path):
    engine = fake_engine(tmp_path, stdout="x", sleep=5)
    code, out, err = run(capsys, "--engine", str(engine), "--timeout", "0.3", "classify", "--board", PUZZLE)
    assert code == 3 and "did not answer within 0.3 seconds" in err


def test_an_engine_that_says_a_valid_solution_is_valid_is_trusted_only_through_the_client(capsys, tmp_path):
    ok = '{"schema":"sudoku.response/1","op":"verify","ok":true,"success":true,"status":"valid","result":{"valid":true}}\n'
    engine = fake_engine(tmp_path, stdout=ok, exit_code=0)
    code, out, _ = run(capsys, "--engine", str(engine), "verify", "--puzzle", PUZZLE, "--solution", SOLVED)
    assert code == 0 and out.startswith("valid:")


def test_unexpected_exceptions_are_not_swallowed(monkeypatch, capsys):
    monkeypatch.setitem(cli.COMMANDS, "classify", lambda engine, args: 1 / 0)
    monkeypatch.setenv("SUDOKU_CLI", sys.executable)
    with pytest.raises(ZeroDivisionError):
        cli.main(["classify", "--board", PUZZLE])


# ---- module entry point and launcher -------------------------------------------------------------------------------------------

def test_python_dash_m_entry_point(engine_executable, tmp_path):
    env = {**os.environ, "PYTHONPATH": str(ROOT / "python"), "SUDOKU_CLI": str(engine_executable)}
    done = subprocess.run([sys.executable, "-m", "sudoku_app", "verify", "--puzzle", PUZZLE, "--solution", SOLVED],
                          cwd=tmp_path, env=env, capture_output=True, text=True)
    assert done.returncode == 0 and done.stdout.startswith("valid:")
    done = subprocess.run([sys.executable, "-m", "sudoku_app"], cwd=tmp_path, env=env, capture_output=True, text=True)
    assert done.returncode == 0 and "usage:" in done.stdout


def launcher(args, cwd, **env):
    full = {**os.environ, "SUDOKU_NO_BUILD": "1", **env}
    return subprocess.run([str(RUN_SH), *args], cwd=cwd, env=full, capture_output=True, text=True, timeout=300)


def test_run_sh_is_executable_and_shows_help_from_any_directory(tmp_path):
    assert os.access(RUN_SH, os.X_OK)
    for args in ([], ["--help"]):
        done = launcher(args, tmp_path)
        assert done.returncode == 0 and "usage:" in done.stdout and "generate" in done.stdout


def test_run_sh_runs_commands_from_another_directory_and_places_output_files(engine_executable, tmp_path):
    if Path(engine_executable).resolve() != (ROOT / "build/dev/sudoku_cli").resolve():
        pytest.skip("run.sh uses build/dev/sudoku_cli")
    done = launcher(["solve", "--board", PUZZLE], tmp_path)
    assert done.returncode == 0 and "solved (search)" in done.stdout
    product = tmp_path / "product"                                 # stands in for the repository's product/
    done = launcher(["generate", "--difficulty", "easy", "--seed", "7", "--output", "p.svg"], tmp_path,
                    SUDOKU_OUTPUT_DIR=str(product))
    assert done.returncode == 0 and (product / "p.svg").is_file() and not (tmp_path / "p.svg").exists()
    (tmp_path / "sub").mkdir()
    done = launcher(["generate", "--seed", "7", "--output", "./q.svg", "--solution-output", "sub/s.svg"], tmp_path,
                    SUDOKU_OUTPUT_DIR=str(product))
    assert done.returncode == 0 and (tmp_path / "q.svg").is_file() and (tmp_path / "sub" / "s.svg").is_file()
    assert sorted(p.name for p in product.iterdir()) == ["p.svg"] and not (ROOT / "p.svg").exists()


def test_run_sh_defaults_bare_output_names_to_product():
    text = RUN_SH.read_text()
    assert 'export SUDOKU_OUTPUT_DIR="${SUDOKU_OUTPUT_DIR:-$ROOT/product}"' in text


def test_bare_output_names_go_to_the_output_directory(real, capsys, monkeypatch):
    out = real / "deep" / "product"                                # created on demand, parents included
    monkeypatch.setenv(cli.OUTPUT_DIR_ENV, str(out))
    code, stdout, err = run(capsys, "generate", "--seed", "7", "--output", "p.svg", "--solution-output", "s.svg")
    assert code == 0, err
    assert sorted(p.name for p in out.iterdir()) == ["p.svg", "s.svg"] and f"wrote puzzle image: {out / 'p.svg'}" in stdout
    code, _, err = run(capsys, "generate", "--seed", "7", "--output", "p.svg")
    assert code == 2 and "already exists" in err                   # the existing-file rule applies there too
    monkeypatch.delenv(cli.OUTPUT_DIR_ENV)
    code, _, err = run(capsys, "generate", "--seed", "7", "--output", "p.svg")
    assert code == 0 and (real / "p.svg").is_file()                # without the variable: current directory


def test_an_unusable_output_directory_is_a_usage_error(real, capsys, monkeypatch):
    (real / "file").write_text("x")
    monkeypatch.setenv(cli.OUTPUT_DIR_ENV, str(real / "file" / "product"))
    code, _, err = run(capsys, "generate", "--seed", "7", "--output", "p.svg")
    assert code == 2 and "cannot create the output directory" in err


def test_run_sh_propagates_exit_codes(engine_executable, tmp_path):
    if Path(engine_executable).resolve() != (ROOT / "build/dev/sudoku_cli").resolve():
        pytest.skip("run.sh uses build/dev/sudoku_cli")
    assert launcher(["verify", "--puzzle", PUZZLE, "--solution", PUZZLE], tmp_path).returncode == 1
    assert launcher(["solve", "--board", "bad"], tmp_path).returncode == 2
    assert launcher(["nonsense"], tmp_path).returncode == 2
    assert launcher(["generate", "--difficulty", "master", "--seed", "1", "--max-attempts", "2"], tmp_path).returncode == 1


def copy_tree(tmp_path):
    """A scratch 'repository' holding run.sh and the Python sources but no build."""
    root = tmp_path / "repo"
    root.mkdir()
    shutil.copy(RUN_SH, root / "run.sh")
    (root / "python").symlink_to(ROOT / "python")
    return root


def test_run_sh_without_a_build_explains_how_to_get_one(tmp_path):
    root = copy_tree(tmp_path)
    done = subprocess.run([str(root / "run.sh"), "solve", "--board", PUZZLE], cwd=tmp_path, capture_output=True, text=True,
                          env={**os.environ, "SUDOKU_NO_BUILD": "1"})
    assert done.returncode == 3 and "engine was not found" in done.stderr and "Traceback" not in done.stderr


def test_run_sh_reports_a_failing_build_and_a_build_that_produces_nothing(tmp_path):
    root = copy_tree(tmp_path)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    fake = bin_dir / "cmake"
    fake.write_text("#!/bin/sh\necho 'fake cmake output: configure failed' >&2\nexit 1\n")
    fake.chmod(0o755)
    env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}"}
    env.pop("SUDOKU_NO_BUILD", None)
    done = subprocess.run([str(root / "run.sh"), "classify", "--board", PUZZLE], cwd=tmp_path, capture_output=True, text=True, env=env)
    assert done.returncode == 3 and "fake cmake output" in done.stderr and "building the engine failed" in done.stderr
    fake.write_text("#!/bin/sh\nexit 0\n")                                  # "succeeds" but builds nothing
    done = subprocess.run([str(root / "run.sh"), "classify", "--board", PUZZLE], cwd=tmp_path, capture_output=True, text=True, env=env)
    assert done.returncode == 3 and "engine was not found" in done.stderr
    assert not (root / "build").exists()                                   # the launcher never touches anything else


def test_run_sh_reports_a_missing_cmake(tmp_path):
    root = copy_tree(tmp_path)
    tools = tmp_path / "tools"
    tools.mkdir()
    for name in ("bash", "env", "dirname", "readlink", "mktemp", "rm", "cat", "sh"):
        found = shutil.which(name)
        if found:
            (tools / name).symlink_to(found)
    py = tools / "python3"
    py.symlink_to(sys.executable)
    env = {k: v for k, v in os.environ.items() if k != "SUDOKU_NO_BUILD"}
    env["PATH"] = str(tools)
    done = subprocess.run(["/bin/bash", str(root / "run.sh"), "classify", "--board", PUZZLE], cwd=tmp_path, capture_output=True,
                          text=True, env=env)
    assert done.returncode == 3 and "cmake was not found" in done.stderr


# ---- licence -----------------------------------------------------------------------------------------------------------------------

def test_mit_licence_text_and_copyright_line():
    text = (ROOT / "LICENSE").read_bytes().decode("utf-8")
    assert unicodedata.is_normalized("NFC", text)
    lines = text.splitlines()
    assert lines[0] == "MIT License"
    assert "Copyright (c) 2026 Chovaneček Štěpán Adam" in lines
    assert "Permission is hereby granted, free of charge, to any person obtaining a copy" in text
    assert "The above copyright notice and this permission notice shall be included in all" in text
    assert 'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND' in text
    assert text.rstrip().endswith("SOFTWARE.")
    assert 'license = {text = "MIT"}' in (ROOT / "pyproject.toml").read_text()


def test_random_seeds_differ_between_runs_and_are_printed(real, capsys):
    seeds = set()
    for _ in range(3):
        _, out, _ = run(capsys, "generate")
        line = next(l for l in out.splitlines() if l.startswith("seed:"))
        seeds.add(int(line.split()[1]))
    assert len(seeds) == 3 and 0 not in seeds


def test_the_solution_image_goes_through_the_engine_verified_export(real, capsys, monkeypatch):
    calls = []
    original = cli.export_solution_svg

    def spy(engine, puzzle, solution, destination, **kw):
        calls.append((puzzle, solution, destination))
        return original(engine, puzzle, solution, destination, **kw)

    monkeypatch.setattr(cli, "export_solution_svg", spy)
    assert run(capsys, "generate", "--seed", "7", "--solution-output", "s.svg")[0] == 0
    assert len(calls) == 1 and calls[0][2] is None and (real / "s.svg").is_file()


def test_uniqueness_is_never_claimed_from_an_inconclusive_count():
    from sudoku_app.models import CountResult
    from sudoku_app.protocol import Envelope
    ST = {"nodes": 1, "assignments": 1, "backtracks": 0}
    stopped = CountResult.from_envelope(Envelope("count_solutions", None, False, "budget_exhausted", {
        "limit": 2, "count": 1, "exact": False, "classification": "unknown", "first_solution": SOLVED, "stats": ST}, 1))
    exact = CountResult.from_envelope(Envelope("count_solutions", None, True, "exhausted", {
        "limit": 2, "count": 1, "exact": True, "classification": "unique", "first_solution": SOLVED, "stats": ST}, 0))

    class Stub:
        def __init__(self, result):
            self.result = result

        def count_solutions(self, puzzle):
            return self.result

    assert cli._claim_uniqueness(Stub(stopped), PUZZLE) == cli.EXIT_ENGINE
    assert cli._claim_uniqueness(Stub(exact), PUZZLE).startswith("yes")
