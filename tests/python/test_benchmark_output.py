"""Validates the sudoku_bench tool's real output with Python's own JSON parser.

The tool must be built first (cmake --preset dev && cmake --build --preset dev).
Set SUDOKU_BENCH to use a different binary. If the binary does not exist the
tests are skipped, never silently passed.
"""
import json
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BENCH = Path(os.environ.get("SUDOKU_BENCH", ROOT / "build" / "dev" / "benchmarks" / "sudoku_bench"))

pytestmark = pytest.mark.skipif(not BENCH.exists(), reason=f"benchmark binary not built: {BENCH}")


def run(*args, check=False):
    return subprocess.run([str(BENCH), *args], capture_output=True, text=True, timeout=300, check=check)


def smoke_report(repetitions="2"):
    proc = run("--tier", "smoke", "--repetitions", repetitions)
    assert proc.returncode == 0, proc.stderr
    return json.loads(proc.stdout)


def algorithmic(report):
    """Everything except wall-clock fields and the environment."""
    out = []
    for r in report["records"]:
        r = dict(r)
        r.pop("elapsed_ns")
        out.append(r)
    return out


def test_smoke_report_schema():
    report = smoke_report()
    assert report["schema"] == "sudoku.benchmark/1"
    assert report["corpus_version"] == "m16.1"
    assert report["ok"] is True
    assert report["config"] == {"tier": "smoke", "case_ids": [], "repetitions": 2, "warmup": 0}
    env = report["environment"]
    assert env["timer"] == "steady_clock" and env["timer_is_steady"] is True
    assert isinstance(env["optimization_enabled"], bool) and env["cxx_standard"] >= 202002

    assert len(report["records"]) == 12 and len(report["summary"]) == 6
    for r in report["records"]:
        assert isinstance(r["elapsed_ns"], int) and r["elapsed_ns"] >= 0
        assert isinstance(r["repetition"], int) and isinstance(r["success"], bool)
        assert r["matches_expected"] is True
        assert all(isinstance(v, int) and not isinstance(v, bool) for v in r["counters"].values())
        assert isinstance(r["params"], dict) and isinstance(r["detail"], dict)
    for s in report["summary"]:
        assert s["runs"] == 2 and s["results_stable"] is True and s["unexpected"] == 0
        t = s["timing"]
        assert t["samples"] == 2 and t["distribution_reliable"] is False   # fewer than 5 samples
        assert t["min_ns"] <= t["median_ns"] <= t["max_ns"]


def test_failures_and_exhaustion_are_represented():
    report = smoke_report()
    exhausted = [r for r in report["records"] if r["case"] == "targeted.master.exhaust.seed1"]
    assert len(exhausted) == 2
    for r in exhausted:
        assert r["status"] == "exhausted" and r["success"] is False
        diag = r["detail"]
        assert diag["schema"] == "sudoku.targeted-diagnostics/1"
        assert diag["status"] == "exhausted" and "result" not in diag
        assert diag["attempts"] == diag["max_attempts"] == 3
        assert sum(diag["rejections"].values()) == 3
    summary = {s["case"]: s for s in report["summary"]}
    assert summary["targeted.master.exhaust.seed1"]["successes"] == 0
    assert "attempts_per_success" not in summary["targeted.master.exhaust.seed1"]   # absent, not 0
    ok = summary["targeted.easy.seed1"]
    assert ok["successes"] == 2 and ok["attempts_per_success"] >= 1


def test_generated_diagnostics_are_consistent():
    report = smoke_report("1")
    rec = next(r for r in report["records"] if r["case"] == "targeted.easy.seed1")
    diag = rec["detail"]
    assert diag["status"] == "generated" and diag["target"] == "Easy"
    assert sum(diag["rejections"].values()) + 1 == diag["attempts"] <= diag["max_attempts"]
    result = diag["result"]
    assert len(result["puzzle"]) == 81 and len(result["solution"]) == 81 and "." not in result["solution"]
    assert all(p == "." or p == s for p, s in zip(result["puzzle"], result["solution"]))
    assert result["clue_count"] == sum(c != "." for c in result["puzzle"])
    assert result["classification"]["profile"] == "Easy"
    assert len(result["classification"]["metrics"]["by_technique"]) == 9
    assert isinstance(diag["seed"], int) and isinstance(diag["attempt_seed"], int)


def test_algorithmic_output_is_deterministic():
    assert algorithmic(smoke_report()) == algorithmic(smoke_report())


def test_case_selection_and_list():
    proc = run("--case", "grid.seed1", "--case", "classify.easy", "--repetitions", "3", "--warmup", "1")
    assert proc.returncode == 0, proc.stderr
    report = json.loads(proc.stdout)
    assert [s["case"] for s in report["summary"]] == ["grid.seed1", "classify.easy"]
    assert len(report["records"]) == 6 and report["config"]["warmup"] == 1
    listing = run("--list").stdout.splitlines()
    assert listing[0].split("\t")[0] == "grid.seed1" and len(listing) == 35
    assert len({line.split("\t")[0] for line in listing}) == 35


def test_output_file(tmp_path):
    out = tmp_path / "report.json"
    proc = run("--case", "grid.seed1", "--output", str(out))
    assert proc.returncode == 0 and proc.stdout == ""
    assert json.loads(out.read_text())["ok"] is True


@pytest.mark.parametrize("args", [
    ["--tier", "huge"], ["--case", "no.such.case"], ["--repetitions", "0"], ["--repetitions", "1001"],
    ["--repetitions", "abc"], ["--warmup", "-1"], ["--warmup", "101"], ["--bogus"], ["--tier"],
])
def test_invalid_configuration_is_rejected(args):
    proc = run(*args)
    assert proc.returncode == 2
    assert proc.stdout == "" and proc.stderr.strip() != ""     # nothing is run, no partial report
