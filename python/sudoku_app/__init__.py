"""Python orchestration and rendering layer for Sudoku Studio.

The engine client is :class:`sudoku_app.SudokuEngine`; errors are in
:mod:`sudoku_app.errors`, typed results in :mod:`sudoku_app.models`. SVG board
rendering is in :mod:`sudoku_app.svg`, picture-theme packs in :mod:`sudoku_app.themes`.
"""

__version__ = "0.1.0"

from . import errors, models  # noqa: E402
from .client import SudokuEngine  # noqa: E402
from .errors import SudokuError  # noqa: E402
from .batch import (BatchItem, BatchResult, BatchSummary, classify_puzzles, generate_puzzles,  # noqa: E402
                    generate_targeted_puzzles, item_seed, solve_puzzles, verify_solutions)
from .config import Settings, load_settings  # noqa: E402
from .export import export_solution_svg  # noqa: E402
from .svg import (DARK_THEME, DEFAULT_THEME, LIGHT_THEME, THEME_NAMES, Picture, Theme, render_board_svg,  # noqa: E402
                  write_board_svg)
from .themes import load_theme_pack, resolve_theme  # noqa: E402
from .orchestration import PuzzleReport, describe_puzzle, generate_puzzle_report  # noqa: E402

__all__ = ["SudokuEngine", "SudokuError", "PuzzleReport", "describe_puzzle", "generate_puzzle_report", "render_board_svg",
           "write_board_svg", "export_solution_svg", "Settings", "load_settings", "BatchItem", "BatchResult", "BatchSummary",
           "item_seed", "generate_puzzles", "generate_targeted_puzzles", "classify_puzzles", "solve_puzzles", "verify_solutions", "Theme", "Picture", "load_theme_pack", "resolve_theme", "THEME_NAMES", "DEFAULT_THEME", "LIGHT_THEME", "DARK_THEME", "errors", "models",
           "__version__"]
