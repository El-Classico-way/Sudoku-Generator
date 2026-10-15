"""Solution export: an SVG of a completed solution with the original clues distinguished.

Composition only. The renderer (:mod:`sudoku_app.svg`) never talks to the engine and this module
implements no Sudoku rule: whether the grid is a valid solution is established by the engine's
``verify`` operation. What each check proves:

* clue preservation  - the solution repeats every clue of the puzzle (checked here, cell by cell);
* solution verification - the engine says the grid is a complete valid Sudoku that keeps the clues;
* uniqueness - NOT established by this function (that needs an exact ``count_solutions`` result).
"""
from __future__ import annotations

import os
from typing import TextIO

from .client import SudokuEngine
from .errors import InvalidArgumentError, OperationNotSuccessfulError, ResponseFormatError
from .models import VerifyResult, VerifyStatus
from .protocol import check_board
from .svg import DEFAULT_CELL_SIZE, DEFAULT_MARGIN, Theme, render_board_svg

__all__ = ["export_solution_svg"]


def export_solution_svg(
    engine: SudokuEngine,
    puzzle: str,
    solution: str,
    destination: str | os.PathLike[str] | TextIO | None = None,
    *,
    theme: str | Theme | None = None,
    cell_size: int = DEFAULT_CELL_SIZE,
    margin: int = DEFAULT_MARGIN,
    timeout: float | None = None,
) -> str:
    """Return (and optionally write) the SVG of ``solution`` with the clues of ``puzzle`` drawn as givens.

    Order of work: (1) Python-side validation of both boards, completeness of ``solution``, clue
    preservation, theme and dimensions - ``InvalidArgumentError``, nothing sent; (2) one engine
    ``verify`` call, never retried - engine errors propagate unchanged, and a result that is not a
    plain success raises ``OperationNotSuccessfulError`` (``.result`` holds the fault) while a
    self-contradictory result raises ``ResponseFormatError``; (3) only then is ``destination`` (a path
    or a text stream, same rules as :func:`write_board_svg`) written. On any failure nothing is
    written. An omitted ``destination`` writes nothing. The SVG text is returned either way.
    """
    puzzle = check_board(puzzle, "puzzle")
    solution = check_board(solution, "solution")
    empty = next((i for i, c in enumerate(solution) if c in ".0"), None)
    if empty is not None:
        raise InvalidArgumentError(f"solution is incomplete: cell {empty} is empty")
    for i, (p, s) in enumerate(zip(puzzle, solution)):
        if p not in ".0" and p != s:
            raise InvalidArgumentError(f"solution has {s!r} at index {i} but the puzzle clue is {p!r}")
    destination_is_path = isinstance(destination, (str, os.PathLike))
    if destination is not None and not destination_is_path and not callable(getattr(destination, "write", None)):
        raise InvalidArgumentError("destination must be None, a path or a text stream with write()")
    svg = render_board_svg(solution, original=puzzle, cell_size=cell_size, margin=margin, theme=theme)

    result = engine.verify(puzzle, solution, timeout=timeout)
    # The verdict authorizes the export only if it is unmistakably "valid": an injected or substituted engine
    # must return a VerifyResult whose flags and status all agree. Anything else is refused, never guessed at.
    if not isinstance(result, VerifyResult):
        raise ResponseFormatError(f"verify returned {type(result).__name__}, not a VerifyResult")
    if result.success != result.valid or result.valid != (result.status is VerifyStatus.VALID):
        raise ResponseFormatError(
            f"verify: contradictory verdict (success={result.success}, valid={result.valid}, status={result.status!s})")
    if not result.success:
        raise OperationNotSuccessfulError(result)
    if result.index is not None or result.digit is not None:
        raise ResponseFormatError("verify: a valid verdict must not report a faulty cell")

    if destination_is_path:
        with open(destination, "w", encoding="utf-8", newline="\n") as handle:  # type: ignore[arg-type]
            handle.write(svg)
    elif destination is not None:
        destination.write(svg)  # type: ignore[union-attr]
    return svg
