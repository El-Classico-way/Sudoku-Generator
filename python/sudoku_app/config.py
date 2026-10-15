"""Typed, validated settings (M23).

``Settings`` collects the defaults a user would otherwise repeat in every call. It is plain data:
nothing here starts the engine or knows a Sudoku rule, and nothing is global or mutable.

Precedence, highest first - the same everywhere in :mod:`sudoku_app.batch`:

1. an explicit argument of the call (``None`` means "not given");
2. the settings object / settings file;
3. for the engine executable only: the ``SUDOKU_CLI`` variable, then the standard build location
   (the rules of :mod:`sudoku_app.runner`; a settings ``executable`` takes the place of the explicit argument);
4. the built-in defaults below.

A settings file is JSON::

    {"version": 1, "timeout": 120, "profile": "Hard", "max_attempts": 200, "theme": "dark",
     "output_dir": "puzzles"}

``version`` is required (only ``1`` exists). Unknown keys, duplicate keys, wrong types and out-of-range
values are rejected with :class:`ConfigurationError`; nothing is ignored or corrected. Relative
``executable`` and ``output_dir`` paths are relative to the directory of the settings file. The
profile name is not interpreted here (the engine is the authority); a name it does not know fails
at run time and is never replaced by another profile. No default profile or attempt budget is
invented either: when ``profile`` is absent, callers must pass one.
"""
from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, fields, replace
from pathlib import Path
from typing import Any

from .client import SudokuEngine
from .errors import ConfigurationError
from .protocol import MAX_ATTEMPTS, loads_strict
from .runner import DEFAULT_TIMEOUT_SECONDS
from .svg import DEFAULT_CELL_SIZE, DEFAULT_MARGIN, MAX_CELL_SIZE, MAX_MARGIN, MIN_CELL_SIZE, MIN_MARGIN, THEMES

__all__ = ["Settings", "load_settings", "SETTINGS_VERSION"]

SETTINGS_VERSION = 1


def _int(value: Any, key: str, lo: int, hi: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigurationError(f"{key} must be an integer, got {type(value).__name__}")
    if not lo <= value <= hi:
        raise ConfigurationError(f"{key} must be between {lo} and {hi}, got {value}")
    return value


@dataclass(frozen=True)
class Settings:
    """Immutable defaults. ``None`` means "not configured"."""

    executable: str | None = None          # engine path; None -> SUDOKU_CLI / standard build location
    timeout: float = DEFAULT_TIMEOUT_SECONDS
    profile: str | None = None             # default target profile of targeted batches
    max_attempts: int | None = None        # None -> the engine's own default
    theme: str = "default"                 # a built-in theme name
    cell_size: int = DEFAULT_CELL_SIZE
    margin: int = DEFAULT_MARGIN
    output_dir: str | None = None          # directory for exported SVG files; must exist
    fail_fast: bool = False                # stop a batch at the first item that is not a success

    def __post_init__(self) -> None:
        if self.executable is not None and (not isinstance(self.executable, str) or not self.executable):
            raise ConfigurationError("executable must be a non-empty string")
        if (isinstance(self.timeout, bool) or not isinstance(self.timeout, (int, float))
                or not math.isfinite(self.timeout) or self.timeout <= 0):
            raise ConfigurationError("timeout must be a positive, finite number of seconds")
        if self.profile is not None and (not isinstance(self.profile, str) or not self.profile):
            raise ConfigurationError("profile must be a non-empty string")
        if self.max_attempts is not None:
            _int(self.max_attempts, "max_attempts", 1, MAX_ATTEMPTS)
        if not isinstance(self.theme, str) or self.theme not in THEMES:
            raise ConfigurationError(f"theme must be one of {', '.join(THEMES)}, got {self.theme!r}")
        _int(self.cell_size, "cell_size", MIN_CELL_SIZE, MAX_CELL_SIZE)
        _int(self.margin, "margin", MIN_MARGIN, MAX_MARGIN)
        if self.output_dir is not None and (not isinstance(self.output_dir, str) or not self.output_dir):
            raise ConfigurationError("output_dir must be a non-empty string")
        if not isinstance(self.fail_fast, bool):
            raise ConfigurationError("fail_fast must be a boolean")

    def with_overrides(self, **overrides: Any) -> "Settings":
        """A copy with every override that is not ``None`` applied (and validated); unknown names are rejected."""
        known = {f.name for f in fields(self)}
        unknown = sorted(set(overrides) - known)
        if unknown:
            raise ConfigurationError(f"unknown setting(s): {', '.join(unknown)}")
        return replace(self, **{k: v for k, v in overrides.items() if v is not None})

    def engine(self) -> SudokuEngine:
        """A client for the configured executable and timeout (discovery rules of :mod:`sudoku_app.runner`)."""
        return SudokuEngine(self.executable, timeout=self.timeout)


def load_settings(path: str | os.PathLike[str]) -> Settings:
    """Reads a settings file (see the module docstring). Every problem is a :class:`ConfigurationError` naming the file."""
    file = Path(path)
    try:
        text = file.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise ConfigurationError(f"settings file not found: {file}") from None
    except (OSError, UnicodeDecodeError) as exc:
        raise ConfigurationError(f"settings file cannot be read: {file} ({exc})") from exc
    try:
        data = loads_strict(text)
    except (ValueError, RecursionError) as exc:
        raise ConfigurationError(f"settings file {file} is not valid JSON ({exc})") from exc
    if not isinstance(data, dict):
        raise ConfigurationError(f"settings file {file} must contain a JSON object")
    if "version" not in data:
        raise ConfigurationError(f"settings file {file} needs a \"version\" (supported: {SETTINGS_VERSION})")
    if type(data["version"]) is not int or data["version"] != SETTINGS_VERSION:
        raise ConfigurationError(f"settings file {file} has unsupported version {data['version']!r} (supported: {SETTINGS_VERSION})")
    values = {k: v for k, v in data.items() if k != "version"}
    unknown = sorted(set(values) - {f.name for f in fields(Settings)})
    if unknown:
        raise ConfigurationError(f"settings file {file} has unknown key(s): {', '.join(map(str, unknown))}")
    for key in ("executable", "output_dir"):
        if isinstance(values.get(key), str) and values[key]:
            values[key] = str((file.parent / values[key]) if not os.path.isabs(values[key]) else values[key])
    try:
        return Settings(**values)
    except ConfigurationError as exc:
        raise ConfigurationError(f"settings file {file}: {exc}") from exc
