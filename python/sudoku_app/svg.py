"""Deterministic SVG rendering of a 9x9 Sudoku board (M19 foundation).

Pure text generation: nothing here starts the engine, solves, generates or
classifies. The same arguments always give the same string. The output is a
fixed, self-contained SVG 1.1 document (no external references, scripts,
style sheets, timestamps or caller-supplied text).

Boards use the project-wide representation: a ``str`` of 81 characters, row
major, ``1``-``9`` for digits and ``.`` or ``0`` for empty cells.

Givens versus filled digits
---------------------------
A board string alone cannot say which digits were original clues, so by default
every digit is drawn in one neutral style (``class="digit"``) and nothing is
claimed about givens. Pass exactly one of

* ``original`` - the 81-character puzzle the board was derived from; a cell is a
  given iff the puzzle has a digit there, or
* ``given_mask`` - 81 booleans (flat) or 9 rows of 9 booleans,

and givens are drawn bold black (``class="given"``) while the other digits are
drawn in normal-weight italic blue (``class="filled"``). The distinction
does not rely on colour alone (weight and slant differ too).

Colours come from a :class:`Theme` (built-ins ``default``/``light``/``dark``); the
default reproduces the M19 output byte for byte.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping, Sequence, TextIO

from .errors import InvalidArgumentError
from .protocol import BOARD_LENGTH, check_board

__all__ = [
    "DEFAULT_CELL_SIZE", "DEFAULT_MARGIN", "MIN_CELL_SIZE", "MAX_CELL_SIZE", "MIN_MARGIN", "MAX_MARGIN",
    "Theme", "LIGHT_THEME", "DARK_THEME", "DEFAULT_THEME", "THEMES", "THEME_NAMES", "get_theme",
    "render_board_svg", "write_board_svg",
]

SVG_NAMESPACE = "http://www.w3.org/2000/svg"

DEFAULT_CELL_SIZE = 50
DEFAULT_MARGIN = 10
MIN_CELL_SIZE, MAX_CELL_SIZE = 16, 200
MIN_MARGIN, MAX_MARGIN = 4, 100  # the minimum keeps the outer border fully inside the viewBox

_HEX_COLOR = re.compile(r"#[0-9a-fA-F]{6}")
_THEME_NAME = re.compile(r"[a-z][a-z0-9-]{0,31}")


@dataclass(frozen=True)
class Theme:
    """Colours of a rendering; geometry, stroke widths and the given/filled font styling are not themeable.

    Every colour is a ``#rrggbb`` hex string (case-insensitive, stored lower case); no other
    CSS/SVG value is accepted, so a theme cannot inject markup, scripts or references.
    ``name`` is 1-32 characters of ``a-z``, ``0-9`` and ``-``, starting with a letter. Invalid
    values raise :class:`InvalidArgumentError`. Contrast is NOT enforced for custom themes.

    ``digit`` colours digits when no given information is supplied, ``given`` and ``filled``
    colour clues and other digits (which differ in weight and slant as well, never colour alone).
    """

    name: str
    background: str
    grid: str
    box: str
    border: str
    digit: str
    given: str
    filled: str

    def __post_init__(self) -> None:
        if not isinstance(self.name, str) or not _THEME_NAME.fullmatch(self.name):
            raise InvalidArgumentError(f"theme name must match {_THEME_NAME.pattern!r}, got {self.name!r}")
        for field in ("background", "grid", "box", "border", "digit", "given", "filled"):
            value = getattr(self, field)
            if not isinstance(value, str) or not _HEX_COLOR.fullmatch(value):
                raise InvalidArgumentError(f"theme {field} must be a '#rrggbb' colour, got {value!r}")
            object.__setattr__(self, field, value.lower())


LIGHT_THEME = Theme("light", background="#ffffff", grid="#808080", box="#000000", border="#000000",
                    digit="#000000", given="#000000", filled="#1a56c4")
DARK_THEME = Theme("dark", background="#121212", grid="#6b6b6b", box="#e0e0e0", border="#ffffff",
                   digit="#ffffff", given="#ffffff", filled="#7fb2ff")
DEFAULT_THEME = LIGHT_THEME  # exactly the M19 appearance

THEMES: Mapping[str, Theme] = MappingProxyType({"default": DEFAULT_THEME, "light": LIGHT_THEME, "dark": DARK_THEME})
THEME_NAMES = tuple(THEMES)


def get_theme(theme: str | Theme | None) -> Theme:
    """``None`` -> the default theme, a built-in name -> that theme, a :class:`Theme` -> itself."""
    if theme is None:
        return DEFAULT_THEME
    if isinstance(theme, Theme):
        return theme
    if isinstance(theme, str):
        try:
            return THEMES[theme]
        except KeyError:
            raise InvalidArgumentError(f"unknown theme {theme!r} (built-in: {', '.join(THEME_NAMES)})") from None
    raise InvalidArgumentError(f"theme must be None, a theme name or a Theme, got {type(theme).__name__}")


_THIN, _BOX, _OUTER = 1, 2, 4  # stroke widths in user units (outer is centred on the grid edge: margin >= 2)
_SIZE = 9
_EMPTY = ".0"


def _num(value: float) -> str:
    """Fixed numeric format: integers bare, otherwise at most two decimals, no trailing zeros."""
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return "0" if text in ("", "-0") else text


def _check_int(value: Any, name: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise InvalidArgumentError(f"{name} must be an int, got {type(value).__name__}")
    if not low <= value <= high:
        raise InvalidArgumentError(f"{name} must be between {low} and {high}, got {value}")
    return value


def _flatten_mask(mask: Any) -> list[bool]:
    if isinstance(mask, (str, bytes)) or not isinstance(mask, Sequence):
        raise InvalidArgumentError(f"given_mask must be a sequence of bools, got {type(mask).__name__}")
    if len(mask) == BOARD_LENGTH:
        flat = list(mask)
    elif len(mask) == _SIZE:
        flat = []
        for r, row in enumerate(mask):
            if isinstance(row, (str, bytes)) or not isinstance(row, Sequence) or len(row) != _SIZE:
                raise InvalidArgumentError(f"given_mask row {r} must be a sequence of {_SIZE} bools")
            flat.extend(row)
    else:
        raise InvalidArgumentError(
            f"given_mask must have {BOARD_LENGTH} entries (flat) or {_SIZE} rows of {_SIZE}, got {len(mask)} entries")
    for i, v in enumerate(flat):
        if not isinstance(v, bool):
            raise InvalidArgumentError(f"given_mask entry {i} must be a bool, got {type(v).__name__}")
    return flat


def _givens(board: str, original: Any, given_mask: Any) -> list[bool] | None:
    if original is not None and given_mask is not None:
        raise InvalidArgumentError("pass at most one of original and given_mask")
    if original is not None:
        original = check_board(original, "original")
        flags = []
        for i, (o, b) in enumerate(zip(original, board)):
            given = o not in _EMPTY
            if given and o != b:
                raise InvalidArgumentError(
                    f"original has {o!r} at index {i} but board has {b!r}; a given must match the board")
            flags.append(given)
        return flags
    if given_mask is not None:
        flags = _flatten_mask(given_mask)
        for i, (g, b) in enumerate(zip(flags, board)):
            if g and b in _EMPTY:
                raise InvalidArgumentError(f"given_mask marks index {i} as a given but the board cell is empty")
        return flags
    return None


def render_board_svg(
    board: str,
    *,
    original: str | None = None,
    given_mask: Sequence[Any] | None = None,
    cell_size: int = DEFAULT_CELL_SIZE,
    margin: int = DEFAULT_MARGIN,
    theme: str | Theme | None = None,
) -> str:
    """Return the board as an SVG document (``str``, ``\\n`` line ends, trailing newline).

    ``theme`` is ``None`` (default theme, identical to the M19 output), a built-in
    name (see ``THEME_NAMES``) or a :class:`Theme`; it changes colours only.
    ``cell_size`` (``MIN_CELL_SIZE``..``MAX_CELL_SIZE``) and ``margin``
    (``MIN_MARGIN``..``MAX_MARGIN``) are integers in SVG user units; the image is
    ``9 * cell_size + 2 * margin`` square. Raises :class:`InvalidArgumentError`
    for any invalid argument; nothing is repaired. The board need not be
    solvable, unique or even consistent. Inputs are never modified.
    """
    board = check_board(board, "board")
    cell_size = _check_int(cell_size, "cell_size", MIN_CELL_SIZE, MAX_CELL_SIZE)
    margin = _check_int(margin, "margin", MIN_MARGIN, MAX_MARGIN)
    givens = _givens(board, original, given_mask)
    theme = get_theme(theme)

    side = _SIZE * cell_size
    total = side + 2 * margin
    lo, hi = margin, margin + side
    font = cell_size * 3 // 5
    baseline_shift = font * 0.35  # alphabetic baseline: centres digit glyphs vertically without dominant-baseline

    out = [
        f'<svg xmlns="{SVG_NAMESPACE}" width="{total}" height="{total}" viewBox="0 0 {total} {total}" '
        f'role="img" aria-label="Sudoku board">',
        f'  <rect class="background" x="0" y="0" width="{total}" height="{total}" fill="{theme.background}"/>',
        f'  <g class="thin-lines" stroke="{theme.grid}" stroke-width="{_THIN}">',
    ]
    for i in range(1, _SIZE):
        if i % 3:
            p = margin + i * cell_size
            out.append(f'    <line x1="{p}" y1="{lo}" x2="{p}" y2="{hi}"/>')
            out.append(f'    <line x1="{lo}" y1="{p}" x2="{hi}" y2="{p}"/>')
    out.append("  </g>")
    out.append(f'  <g class="box-lines" stroke="{theme.box}" stroke-width="{_BOX}">')
    for i in (3, 6):
        p = margin + i * cell_size
        out.append(f'    <line x1="{p}" y1="{lo}" x2="{p}" y2="{hi}"/>')
        out.append(f'    <line x1="{lo}" y1="{p}" x2="{hi}" y2="{p}"/>')
    out.append("  </g>")
    out.append(f'  <rect class="border" x="{lo}" y="{lo}" width="{side}" height="{side}" fill="none" '
               f'stroke="{theme.border}" stroke-width="{_OUTER}"/>')
    out.append(f'  <g class="digits" font-family="sans-serif" font-size="{font}" text-anchor="middle">')
    for i, ch in enumerate(board):
        if ch in _EMPTY:
            continue
        r, c = divmod(i, _SIZE)
        x = _num(margin + c * cell_size + cell_size / 2)
        y = _num(margin + r * cell_size + cell_size / 2 + baseline_shift)
        if givens is None:
            style = f'class="digit" font-weight="normal" fill="{theme.digit}"'
        elif givens[i]:
            style = f'class="given" font-weight="bold" fill="{theme.given}"'
        else:
            style = f'class="filled" font-weight="normal" font-style="italic" fill="{theme.filled}"'
        out.append(f'    <text x="{x}" y="{y}" {style}>{ch}</text>')
    out.append("  </g>")
    out.append("</svg>")
    return "\n".join(out) + "\n"


def write_board_svg(
    destination: str | os.PathLike[str] | TextIO,
    board: str,
    **options: Any,
) -> str:
    """Render like :func:`render_board_svg` and write the text to ``destination``.

    ``destination`` is a path (written as UTF-8, ``\\n`` line ends, replacing an
    existing file) or an open text stream with ``write``. Rendering and all
    validation happen before anything is written, so invalid input never leaves a
    partial file. Returns the SVG text. File-system errors propagate as
    :class:`OSError`.
    """
    is_path = isinstance(destination, (str, os.PathLike))
    if not is_path and not callable(getattr(destination, "write", None)):
        raise InvalidArgumentError("destination must be a path or a text stream with write()")
    text = render_board_svg(board, **options)
    if is_path:
        with open(destination, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    else:
        destination.write(text)  # type: ignore[union-attr]
    return text
