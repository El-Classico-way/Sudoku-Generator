import re

import sudoku_app


def test_package_exposes_semver_version():
    assert re.fullmatch(r"\d+\.\d+\.\d+", sudoku_app.__version__)


def test_version_matches_pyproject():
    import tomllib
    from pathlib import Path

    pyproject = Path(__file__).resolve().parents[2] / "pyproject.toml"
    with pyproject.open("rb") as f:
        declared = tomllib.load(f)["project"]["version"]
    assert sudoku_app.__version__ == declared
