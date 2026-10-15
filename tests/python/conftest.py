"""Shared fixtures for the engine-client tests."""
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FAKE_ENGINE = Path(__file__).with_name("fake_engine.py")


@pytest.fixture(scope="session")
def engine_executable() -> Path:
    """The built sudoku_cli, for the real-engine tests.

    Located like the client does it: SUDOKU_CLI, else build/dev/sudoku_cli. If it is
    missing the tests FAIL with build instructions rather than being skipped, so a
    missing build can never look like a passing run. Set SUDOKU_ALLOW_MISSING_ENGINE=1
    to skip them deliberately (for example on a machine without a C++ toolchain).
    """
    name = "sudoku_cli.exe" if sys.platform == "win32" else "sudoku_cli"
    path = Path(os.environ.get("SUDOKU_CLI") or ROOT / "build" / "dev" / name)
    if path.is_file() and os.access(path, os.X_OK):
        return path
    message = (f"the engine executable is not built: {path}\n"
               "Build it first:  cmake --preset dev && cmake --build --preset dev\n"
               "or point SUDOKU_CLI at a built sudoku_cli "
               "(SUDOKU_ALLOW_MISSING_ENGINE=1 skips the real-engine tests on purpose).")
    if os.environ.get("SUDOKU_ALLOW_MISSING_ENGINE") == "1":
        pytest.skip(message)
    pytest.fail(message, pytrace=False)


@pytest.fixture()
def fake(tmp_path):
    """Factory for a SudokuEngine that talks to the fake executable.

    ``fake(exit=..., stdout_text=..., ...)`` returns (engine, count_file); the count file
    gets one line per process started.
    """
    from sudoku_app import SudokuEngine

    def make(*, timeout=30.0, **options):
        count_file = tmp_path / "invocations.txt"
        argv = [sys.executable, str(FAKE_ENGINE), "--count-file", str(count_file)]
        for key, value in options.items():
            flag = "--" + key.replace("_", "-")
            if value is True:
                argv.append(flag)
            else:
                argv += [flag, str(value)]
        return SudokuEngine(argv, timeout=timeout), count_file

    return make


def invocations(count_file) -> list[bytes]:
    return count_file.read_bytes().splitlines() if count_file.exists() else []
