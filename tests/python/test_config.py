"""Settings and settings-file tests (M23)."""
from __future__ import annotations

import dataclasses
import json
import os

import pytest

from sudoku_app import Settings, SudokuEngine, errors, load_settings
from sudoku_app.errors import ConfigurationError, InvalidArgumentError


def write(tmp_path, content, name="settings.json"):
    path = tmp_path / name
    path.write_text(content if isinstance(content, str) else json.dumps(content), encoding="utf-8")
    return path


def test_defaults():
    s = Settings()
    assert (s.executable, s.timeout, s.profile, s.max_attempts, s.theme, s.cell_size, s.margin, s.output_dir, s.fail_fast) == (
        None, 60.0, None, None, "default", 50, 10, None, False)


def test_settings_are_immutable_and_overrides_return_copies():
    s = Settings(profile="Hard")
    with pytest.raises(dataclasses.FrozenInstanceError):
        s.profile = "Easy"  # type: ignore[misc]
    t = s.with_overrides(profile=None, theme="dark", max_attempts=5)     # None = not given
    assert (t.profile, t.theme, t.max_attempts) == ("Hard", "dark", 5) and s.theme == "default"
    with pytest.raises(ConfigurationError, match="unknown"):
        s.with_overrides(colour="red")
    with pytest.raises(ConfigurationError):
        s.with_overrides(cell_size=3)                                     # overrides are validated too


@pytest.mark.parametrize("kwargs", [
    {"executable": ""}, {"executable": 5}, {"timeout": 0}, {"timeout": -1}, {"timeout": float("nan")}, {"timeout": float("inf")},
    {"timeout": True}, {"timeout": "5"}, {"profile": ""}, {"profile": 3}, {"max_attempts": 0}, {"max_attempts": 1000001},
    {"max_attempts": True}, {"max_attempts": 1.5}, {"theme": "neon"}, {"theme": "Dark"}, {"theme": None}, {"cell_size": 15},
    {"cell_size": 201}, {"cell_size": True}, {"margin": 3}, {"margin": 101}, {"output_dir": ""}, {"output_dir": 1},
    {"fail_fast": 1}, {"fail_fast": "yes"}])
def test_invalid_values_are_rejected(kwargs):
    with pytest.raises(ConfigurationError):
        Settings(**kwargs)


def test_boundaries_are_accepted():
    Settings(timeout=0.001, max_attempts=1, cell_size=16, margin=4)
    Settings(timeout=10**6, max_attempts=1000000, cell_size=200, margin=100, theme="dark")


def test_a_configuration_error_is_an_invalid_argument_error():
    assert issubclass(ConfigurationError, InvalidArgumentError) and issubclass(ConfigurationError, ValueError)
    assert issubclass(ConfigurationError, errors.SudokuError)


def test_valid_file_round_trip(tmp_path):
    path = write(tmp_path, {"version": 1, "timeout": 120, "profile": "Hard", "max_attempts": 200, "theme": "dark",
                            "cell_size": 40, "margin": 8, "fail_fast": True, "output_dir": "out", "executable": "bin/sudoku_cli"})
    s = load_settings(path)
    assert (s.timeout, s.profile, s.max_attempts, s.theme, s.cell_size, s.margin, s.fail_fast) == (120, "Hard", 200, "dark", 40, 8, True)
    assert s.output_dir == str(tmp_path / "out") and s.executable == str(tmp_path / "bin/sudoku_cli")   # relative to the file
    assert load_settings(str(path)) == s                                                                  # str paths work


def test_absolute_paths_are_kept(tmp_path):
    s = load_settings(write(tmp_path, {"version": 1, "output_dir": "/abs/out", "executable": "/abs/cli"}))
    assert (s.output_dir, s.executable) == ("/abs/out", "/abs/cli")


def test_minimal_file_gives_defaults(tmp_path):
    assert load_settings(write(tmp_path, {"version": 1})) == Settings()


@pytest.mark.parametrize("content,match", [
    ("", "not valid JSON"), ("{", "not valid JSON"), ("[1]", "JSON object"), ("null", "JSON object"), ("5", "JSON object"),
    ('{"version": 1, "timeout": 5, "timeout": 6}', "duplicate"), ('{"version": NaN}', "not valid JSON"),
    ('{"version": 1, "timeout": NaN}', "not valid JSON"), ("{}", "version"), ('{"timeout": 5}', "version"),
    ('{"version": 2}', "unsupported version"), ('{"version": "1"}', "unsupported version"), ('{"version": 1.0}', "unsupported version"),
    ('{"version": true}', "unsupported version"), ('{"version": null}', "unsupported version"),
    ('{"version": 1, "colour": "red"}', "unknown key"), ('{"version": 1, "Timeout": 5}', "unknown key"),
    ('{"version": 1, "timeout": "5"}', "timeout"), ('{"version": 1, "timeout": -3}', "timeout"),
    ('{"version": 1, "theme": "neon"}', "theme"), ('{"version": 1, "max_attempts": 0}', "max_attempts"),
    ('{"version": 1, "profile": 7}', "profile"), ('{"version": 1, "cell_size": 5}', "cell_size"),
    ('{"version": 1, "fail_fast": "no"}', "fail_fast"), ('{"version": 1, "output_dir": ""}', "output_dir"),
    ("[" * 100000, "not valid JSON")])
def test_malformed_or_invalid_files(tmp_path, content, match):
    path = write(tmp_path, content)
    with pytest.raises(ConfigurationError, match=match) as info:
        load_settings(path)
    assert str(path) in str(info.value)                                    # the message names the file


def test_missing_unreadable_and_binary_files(tmp_path):
    with pytest.raises(ConfigurationError, match="not found"):
        load_settings(tmp_path / "nope.json")
    with pytest.raises(ConfigurationError, match="cannot be read"):
        load_settings(tmp_path)                                            # a directory
    binary = tmp_path / "bin.json"
    binary.write_bytes(b"\xff\xfe\x00")
    with pytest.raises(ConfigurationError, match="cannot be read"):
        load_settings(binary)


def test_loading_does_not_touch_the_environment_or_filesystem(tmp_path, monkeypatch):
    monkeypatch.setenv("SUDOKU_CLI", "/should/not/matter")
    s = load_settings(write(tmp_path, {"version": 1, "output_dir": "does-not-exist-yet"}))
    assert s.executable is None and not (tmp_path / "does-not-exist-yet").exists()   # checked when used, not when loaded


def test_engine_precedence_explicit_then_settings_then_environment(tmp_path, monkeypatch):
    fake_a, fake_b, fake_env = (tmp_path / n for n in ("a", "b", "env"))
    for f in (fake_a, fake_b, fake_env):
        f.write_text("#!/bin/sh\n"); f.chmod(0o755)
    monkeypatch.setenv("SUDOKU_CLI", str(fake_env))
    assert Settings().engine().argv == (str(fake_env),)                                    # environment
    assert Settings(executable=str(fake_a)).engine().argv == (str(fake_a),)                # settings beat the environment
    assert Settings(executable=str(fake_a)).with_overrides(executable=str(fake_b)).engine().argv == (str(fake_b),)   # argument wins
    assert SudokuEngine(str(fake_b), timeout=Settings(timeout=5).timeout).argv == (str(fake_b),)


def test_an_unusable_configured_executable_is_an_actionable_error_without_fallback(tmp_path, monkeypatch):
    good = tmp_path / "good"
    good.write_text("#!/bin/sh\n"); good.chmod(0o755)
    monkeypatch.setenv("SUDOKU_CLI", str(good))
    with pytest.raises(errors.ExecutableNotFoundError) as info:
        Settings(executable=str(tmp_path / "missing")).engine()                            # not silently replaced by SUDOKU_CLI
    assert "executable" in str(info.value) and str(tmp_path / "missing") in str(info.value)


def test_engine_gets_the_configured_timeout(tmp_path):
    f = tmp_path / "cli"
    f.write_text("#!/bin/sh\n"); f.chmod(0o755)
    assert Settings(executable=str(f), timeout=7.5).engine()._runner.timeout == 7.5


def test_real_engine_from_a_settings_file(engine_executable, tmp_path):
    path = write(tmp_path, {"version": 1, "executable": str(engine_executable), "timeout": 120})
    engine = load_settings(path).engine()
    assert engine.capabilities().engine_version
