"""Documentation accuracy checks (M24): README.md and docs/REFERENCE.md - executable examples, links, and agreement
with the real engine and API. Content checks look at both files together (the README is the short guide, the
reference holds the details); anchors and relative links are checked per file."""
from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

import pytest

import sudoku_app
from sudoku_app import SudokuEngine, load_settings

ROOT = Path(__file__).resolve().parents[2]
DOC_FILES = (ROOT / "README.md", ROOT / "docs" / "REFERENCE.md")
README = DOC_FILES[0].read_text(encoding="utf-8")
REFERENCE = DOC_FILES[1].read_text(encoding="utf-8")
DOCS = README + "\n" + REFERENCE


def blocks(info: str, text: str = DOCS) -> list[str]:
    return re.findall(rf"```{re.escape(info)}\n(.*?)\n```", text, re.S)


def prose_and_headings(text: str):
    """(headings, text outside code fences)."""
    headings, outside, fenced = [], [], False
    for line in text.splitlines():
        if line.startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        outside.append(line)
        if re.match(r"#{1,6} ", line):
            headings.append(line.lstrip("#").strip())
    return headings, "\n".join(outside)


def slug(heading: str) -> str:
    text = re.sub(r"[`*]", "", heading).lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


# ---- executable examples -------------------------------------------------------------------------------------------

def test_shell_examples_run_against_the_built_engine(engine_executable, tmp_path):
    """Every ```sh run block is executed from a scratch directory that sees the repository's build/ (so output files land in tmp)."""
    found = blocks("sh run")
    assert found, "no `sh run` blocks found"
    build = ROOT / "build" / "dev"
    if Path(engine_executable).resolve() != (build / "sudoku_cli").resolve():
        pytest.skip("the shell examples use the documented build/dev location")
    (tmp_path / "build").symlink_to(ROOT / "build")
    for script in found:
        done = subprocess.run(["bash", "-e", "-c", script], cwd=tmp_path, capture_output=True, text=True, timeout=300)
        assert done.returncode == 0, (script, done.stdout, done.stderr)
        lines = done.stdout.splitlines()
        assert lines[0].startswith("sudoku_cli ")
        for line in lines[1:]:
            response = json.loads(line)
            assert response["schema"] == "sudoku.response/1" and response["ok"] is True and response["success"] is True
        report = json.loads((tmp_path / "smoke-report.json").read_text())
        assert report["schema"] == "sudoku.benchmark/1" and report["ok"] is True


def test_settings_example_is_accepted_by_the_real_parser(tmp_path):
    (example,) = blocks("json settings")
    path = tmp_path / "sudoku.json"
    path.write_text(example)
    settings = load_settings(path)
    assert settings.profile == "Hard" and settings.max_attempts == 200 and settings.theme == "dark"
    assert settings.output_dir == str(tmp_path / "puzzles")


def test_python_run_blocks_exist_and_compile():
    found = blocks("python run")
    assert len(found) >= 4
    for block in found:
        compile(block, "<README>", "exec")           # they are executed by test_client_engine.py::test_readme_python_examples_run


# ---- links, structure, wording -------------------------------------------------------------------------------------------

@pytest.mark.parametrize("path", DOC_FILES, ids=lambda p: p.name)
def test_internal_anchor_links_resolve(path):
    headings, text = prose_and_headings(path.read_text(encoding="utf-8"))
    slugs = {slug(h) for h in headings}
    links = re.findall(r"\]\(#([^)]+)\)", text)
    missing = sorted(set(links) - slugs)
    assert not missing, f"anchors without a heading: {missing}"
    assert re.findall(r"\]\(#([^)]+)\)", REFERENCE)                # the reference's table of contents


@pytest.mark.parametrize("path", DOC_FILES, ids=lambda p: p.name)
def test_relative_file_links_exist(path):
    _, text = prose_and_headings(path.read_text(encoding="utf-8"))
    for target in re.findall(r"\]\((?!https?:|#|mailto:)([^)\s]+)\)", text):
        file, _, anchor = target.partition("#")
        linked = path.parent / file
        assert linked.exists(), target
        if anchor and linked.suffix == ".md":                       # cross-file anchors must exist too
            headings, _ = prose_and_headings(linked.read_text(encoding="utf-8"))
            assert anchor in {slug(h) for h in headings}, target


def test_readme_links_to_the_reference():
    assert "](docs/REFERENCE.md)" in README


def test_repository_layout_block_matches_the_tree():
    block = re.search(r"## Repository structure\n\n```text\n(.*?)\n```", README, re.S).group(1)
    top = re.findall(r"^[├└]── ([\w.\-]+/?)", block, re.M)
    assert {"CMakeLists.txt", "cpp/", "python/", "tests/", "benchmarks/", "pyproject.toml"} <= set(top)
    for name in top:
        assert (ROOT / name).exists(), name
    for sub in ("cpp/include/sudoku", "cpp/src", "cpp/tests", "python/sudoku_app", "tests/python"):
        assert (ROOT / sub).is_dir()


def test_no_placeholders_or_obsolete_status_wording():
    for phrase in ("TODO", "TBD", "FIXME", "XXX", "lorem", "coming soon", "Initial project setup", "will be documented",
                   "a later milestone", "planned for a later", "to be chosen", "not implemented yet", "is not available yet"):
        assert phrase.lower() not in DOCS.lower(), phrase


def test_documented_python_version_matches_the_project():
    pyproject = (ROOT / "pyproject.toml").read_text()
    assert 'requires-python = ">=3.11"' in pyproject
    assert "Python | >= 3.11" in DOCS and "Python >= 3.11" in README


# ---- agreement with the implementation -------------------------------------------------------------------------------------

def test_every_public_python_name_is_documented():
    for name in sudoku_app.__all__:
        if name.startswith("_") or name in ("errors", "models", "__version__"):
            continue
        assert name in DOCS, f"{name} is exported but not documented in README.md or docs/REFERENCE.md"


def test_documented_operations_techniques_and_profiles_match_the_engine(engine_executable):
    caps = SudokuEngine(engine_executable, timeout=60).capabilities()
    table = DOCS.split("### Operations", 1)[1].split("Technique ids:", 1)[0]
    documented_ops = set(re.findall(r"^\| `(\w+)` \|", table, re.M)) - {"op"}      # the header row
    assert documented_ops == set(caps.operations)
    technique_line = re.search(r"Technique ids: (.*?)\. Engine semantics", DOCS, re.S).group(1)
    assert re.findall(r"`(\w+)`", technique_line) == list(caps.techniques)
    assert [p.name for p in caps.profiles] == ["Easy", "Medium", "Hard", "Expert", "Master"]
    for name in ("Easy", "Medium", "Hard", "Expert", "Master"):
        assert name in DOCS
    assert caps.board_length == 81


def test_documented_exit_codes_and_error_codes_match_the_client():
    from sudoku_app import protocol
    assert (protocol.EXIT_SUCCESS, protocol.EXIT_NON_SUCCESS, protocol.EXIT_PROTOCOL_ERROR, protocol.EXIT_INTERNAL_ERROR) == (0, 1, 2, 3)
    section = DOCS.split("### Exit codes", 1)[1].split("###", 1)[0]
    for code in "0123":
        assert re.search(rf"^\| {code} \|", section, re.M)


def test_documented_batch_and_settings_fields_exist():
    from dataclasses import fields
    from sudoku_app import Settings
    section = DOCS.split("### Settings", 1)[1]
    for f in fields(Settings):
        assert f"`{f.name}`" in section, f.name
    for name in ("success", "unsuccessful", "failed", "skipped"):
        assert f"`{name}`" in DOCS


def test_run_the_application_examples_work_through_run_sh(engine_executable, tmp_path):
    """Every ```sh runapp block is executed through the real launcher from a scratch directory (build step skipped)."""
    import os
    if Path(engine_executable).resolve() != (ROOT / "build" / "dev" / "sudoku_cli").resolve():
        pytest.skip("run.sh uses build/dev/sudoku_cli")
    found = blocks("sh runapp", README)
    assert found
    env = {**os.environ, "SUDOKU_NO_BUILD": "1", "SUDOKU_OUTPUT_DIR": str(tmp_path)}   # stands in for product/
    for script in found:
        done = subprocess.run(["bash", "-e", "-c", script.replace("./run.sh", str(ROOT / "run.sh"))], cwd=tmp_path, env=env,
                              capture_output=True, text=True, timeout=300)
        assert done.returncode == 0, (script, done.stdout, done.stderr)
    assert (tmp_path / "puzzle.svg").is_file()
    assert "valid:" in done.stdout or "difficulty:" in done.stdout


def test_documented_launcher_commands_are_known_cli_options():
    from sudoku_app import cli
    parser = cli.build_parser()
    subparsers = next(a for a in parser._actions if getattr(a, "choices", None) and isinstance(a.choices, dict))
    help_text = parser.format_help() + "".join(sub.format_help() for sub in subparsers.choices.values())
    section = README.split("## Run the application", 1)[1].split("\n## ", 1)[0]
    used = [line for line in section.splitlines() if "./run.sh" in line]
    assert used
    for line in used:
        for option in re.findall(r"(--[a-z][a-z-]+)", line):
            assert option in help_text, f"{option} is documented but not offered by the CLI"
        for command in re.findall(r"\./run\.sh ([a-z]+)\b", line):
            assert command in subparsers.choices, command
