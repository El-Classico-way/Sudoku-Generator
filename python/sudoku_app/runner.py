"""Executable discovery and process execution.

The engine is the C++ executable ``sudoku_cli``. It handles ONE request per
process: it reads one JSON request from standard input and writes one JSON
response line to standard output (see the README). This module starts it with an
argument list (never through a shell), sends the request, enforces a timeout and
returns the raw outcome. It never retries.

Discovery rules (deterministic; the first applicable rule decides, and a rule that
names a location that is not usable is an error: there is no fallback to a later
rule, so an explicit choice cannot silently turn into a different executable):

1. the ``executable`` argument, if given;
2. the ``SUDOKU_CLI`` environment variable, if set and non-empty;
3. the standard build location ``<repository>/build/dev/sudoku_cli``
   (``.exe`` on Windows), used only when this package sits in a source checkout
   (``<repository>/python/sudoku_app`` next to ``pyproject.toml``).

``PATH`` is deliberately not searched. ``executable`` may also be an argv prefix
(a sequence of strings), for example ``[sys.executable, "wrapper.py"]``.
"""
from __future__ import annotations

import math
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from .errors import (
    EngineLaunchError, EngineTimeoutError, ExecutableNotFoundError, InvalidArgumentError, ResponseFormatError,
)

ENV_VARIABLE = "SUDOKU_CLI"
DEFAULT_TIMEOUT_SECONDS = 60.0
# Responses are small; a runaway process must not make us hold unbounded output.
MAX_OUTPUT_BYTES = 32 * 1024 * 1024


@dataclass(frozen=True)
class ProcessResult:
    exit_code: int
    stdout: bytes
    stderr: bytes


def _default_location() -> Path | None:
    repo = Path(__file__).resolve().parents[2]
    if not (repo / "pyproject.toml").is_file() or not (repo / "cpp").is_dir():
        return None   # installed somewhere else: no standard build location
    name = "sudoku_cli.exe" if sys.platform == "win32" else "sudoku_cli"
    return repo / "build" / "dev" / name


def _usable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def find_executable(executable: str | os.PathLike[str] | Sequence[str] | None = None,
                    environ: Mapping[str, str] | None = None) -> tuple[str, ...]:
    """Selects the engine command (an argv prefix) by the rules in the module docstring."""
    env = os.environ if environ is None else environ
    if executable is not None:
        parts = [executable] if isinstance(executable, (str, os.PathLike)) else executable
        try:
            items = list(parts)
        except TypeError:
            raise InvalidArgumentError("executable must be a path or a sequence of strings") from None
        if not items or not all(isinstance(a, (str, os.PathLike)) for a in items):
            raise InvalidArgumentError("executable must be a non-empty path or a sequence of strings")
        argv = tuple(os.fspath(a) for a in items)
        if not argv[0]:
            raise InvalidArgumentError("the program to run (the first element) must not be empty")
        source = "the 'executable' argument"
    elif env.get(ENV_VARIABLE):
        argv, source = (env[ENV_VARIABLE],), f"the {ENV_VARIABLE} environment variable"
    else:
        default = _default_location()
        if default is None:
            raise ExecutableNotFoundError(
                f"no engine executable specified: pass executable=..., or set {ENV_VARIABLE}",
                ("executable argument", f"${ENV_VARIABLE}"))
        argv, source = (str(default),), "the standard build location"
    if not _usable(Path(argv[0])):
        raise ExecutableNotFoundError(
            f"the engine executable given by {source} is missing or not executable: {argv[0]}"
            + ("  (build it with: cmake --preset dev && cmake --build --preset dev)" if source.startswith("the standard") else ""),
            (argv[0],))
    return argv


def check_timeout(timeout: object) -> float:
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        raise InvalidArgumentError("timeout must be a positive, finite number of seconds")
    return float(timeout)


class EngineRunner:
    """Starts the engine once per request."""

    def __init__(self, executable: str | os.PathLike[str] | Sequence[str] | None = None, *,
                 timeout: float = DEFAULT_TIMEOUT_SECONDS) -> None:
        self.argv: tuple[str, ...] = find_executable(executable)
        self.timeout = check_timeout(timeout)

    def run(self, request: bytes, *, timeout: float | None = None) -> ProcessResult:
        """Sends ``request`` on stdin and returns exit code and both output streams.

        Raises EngineTimeoutError (the child is killed) or EngineLaunchError. It does
        not interpret the output and never retries.
        """
        limit = self.timeout if timeout is None else check_timeout(timeout)
        try:
            completed = subprocess.run(list(self.argv), input=request, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       timeout=limit, check=False)
        except subprocess.TimeoutExpired as exc:
            raise EngineTimeoutError(limit, self.argv, exc.stderr) from exc
        except OSError as exc:   # FileNotFoundError, PermissionError, exec format error, ...
            raise EngineLaunchError(f"could not start the engine: {exc}", self.argv, exc) from exc
        if len(completed.stdout) > MAX_OUTPUT_BYTES or len(completed.stderr) > MAX_OUTPUT_BYTES:
            raise ResponseFormatError(f"engine output exceeds {MAX_OUTPUT_BYTES} bytes")
        return ProcessResult(completed.returncode, completed.stdout, completed.stderr)
