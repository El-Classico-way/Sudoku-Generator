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

Picture themes
--------------
A theme may carry a :class:`Picture` (PNG, JPEG or SVG bytes). It is drawn over the whole
image, scaled to cover it, under a *veil*: a rectangle in the theme's background colour with
opacity ``veil``. The picture is embedded as a ``data:`` URI in an ``<image>`` element, so the
document stays self-contained, and viewers display an SVG picture in that position as a static
image (no scripts, no external loads). Geometry and digits are unchanged.

Readability is guaranteed, not hoped for: the colour under any line or digit is
``veil * background + (1 - veil) * pixel`` per sRGB channel, which lies between the values for a
black and a white pixel. A picture theme is accepted only if every digit colour reaches 4.5:1 and
every line colour 3:1 against *both* extremes, so the ratios hold for any picture.
"""

from __future__ import annotations

import base64
import os
import re
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping, Sequence, TextIO

from .errors import InvalidArgumentError
from .protocol import BOARD_LENGTH, check_board

__all__ = [
    "DEFAULT_CELL_SIZE", "DEFAULT_MARGIN", "MIN_CELL_SIZE", "MAX_CELL_SIZE", "MIN_MARGIN", "MAX_MARGIN",
    "Theme", "Picture", "PICTURE_FOCUS", "MAX_PICTURE_BYTES", "DEFAULT_VEIL", "worst_case_contrast", "LIGHT_THEME", "DARK_THEME", "DEFAULT_THEME", "THEMES", "THEME_NAMES", "get_theme",
    "render_board_svg", "write_board_svg",
]

SVG_NAMESPACE = "http://www.w3.org/2000/svg"

DEFAULT_CELL_SIZE = 50
DEFAULT_MARGIN = 10
MIN_CELL_SIZE, MAX_CELL_SIZE = 16, 200
MIN_MARGIN, MAX_MARGIN = 4, 100  # the minimum keeps the outer border fully inside the viewBox

_HEX_COLOR = re.compile(r"#[0-9a-fA-F]{6}")
_THEME_NAME = re.compile(r"[a-z][a-z0-9-]{0,31}")

MAX_PICTURE_BYTES = 10 * 1024 * 1024
DEFAULT_VEIL = 0.72
# focus -> preserveAspectRatio: which part of a picture stays visible when it is cropped to a square
PICTURE_FOCUS = MappingProxyType({"center": "xMidYMid", "top": "xMidYMin", "bottom": "xMidYMax",
                                  "left": "xMinYMid", "right": "xMaxYMid"})
_MIN_DIGIT_CONTRAST, _MIN_LINE_CONTRAST = 4.5, 3.0


def _media_type(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    head = data[:4096].lstrip(b"\xef\xbb\xbf \t\r\n")
    if head.startswith((b"<svg", b"<?xml", b"<!--")) and b"<svg" in data:
        return "image/svg+xml"
    return None


@dataclass(frozen=True)
class Picture:
    """An image drawn behind the board. Construct with bytes or :meth:`from_file`.

    ``data`` must be a PNG, JPEG or SVG file (recognised by its content, not a name) of at most
    ``MAX_PICTURE_BYTES``; ``focus`` (a key of ``PICTURE_FOCUS``) chooses the part kept when a
    non-square picture is cropped. The media type is derived, never supplied.
    """

    data: bytes
    focus: str = "center"

    def __post_init__(self) -> None:
        if not isinstance(self.data, bytes):
            raise InvalidArgumentError(f"picture data must be bytes, got {type(self.data).__name__}")
        if not 0 < len(self.data) <= MAX_PICTURE_BYTES:
            raise InvalidArgumentError(f"picture must be 1 to {MAX_PICTURE_BYTES} bytes, got {len(self.data)}")
        if _media_type(self.data) is None:
            raise InvalidArgumentError("picture must be a PNG, JPEG or SVG file")
        if self.focus not in PICTURE_FOCUS:
            raise InvalidArgumentError(f"picture focus must be one of {', '.join(PICTURE_FOCUS)}, got {self.focus!r}")

    @classmethod
    def from_file(cls, path: str | os.PathLike[str], focus: str = "center") -> "Picture":
        """Read a picture file. ``OSError`` propagates; an unusable file raises ``InvalidArgumentError``."""
        with open(path, "rb") as handle:
            data = handle.read(MAX_PICTURE_BYTES + 1)
        return cls(data, focus)

    @property
    def media_type(self) -> str:
        return _media_type(self.data)  # type: ignore[return-value]  # validated in __post_init__

    def __repr__(self) -> str:  # never dump megabytes of image data
        return f"Picture({self.media_type}, {len(self.data)} bytes, focus={self.focus!r})"


def _luminance(hex_color: str) -> float:
    return _relative_luminance([int(hex_color[i:i + 2], 16) / 255 for i in (1, 3, 5)])


def _relative_luminance(rgb: Sequence[float]) -> float:
    lin = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgb]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def worst_case_contrast(color: str, background: str, veil: float) -> float:
    """Lowest WCAG contrast of ``color`` over ``background`` at opacity ``veil`` above any picture.

    The composite is affine in the pixel and luminance is monotone in each channel, so the luminance
    under the colour ranges exactly over [black pixel, white pixel] (grey pixels fill the interval).
    A colour whose luminance falls inside that interval matches some pixel: contrast 1. Otherwise the
    nearer end of the interval is the worst case.
    """
    bg = [int(background[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    fg = _luminance(color)
    darkest, lightest = (_relative_luminance([veil * c + (1 - veil) * pixel for c in bg]) for pixel in (0.0, 1.0))
    if fg > lightest:
        return (fg + 0.05) / (lightest + 0.05)
    if fg < darkest:
        return (darkest + 0.05) / (fg + 0.05)
    return 1.0


@dataclass(frozen=True)
class Theme:
    """Colours of a rendering; geometry, stroke widths and the given/filled font styling are not themeable.

    Every colour is a ``#rrggbb`` hex string (case-insensitive, stored lower case); no other
    CSS/SVG value is accepted, so a theme cannot inject markup, scripts or references.
    ``name`` is 1-32 characters of ``a-z``, ``0-9`` and ``-``, starting with a letter. Invalid
    values raise :class:`InvalidArgumentError`. Contrast is NOT enforced for custom themes.

    ``digit`` colours digits when no given information is supplied, ``given`` and ``filled``
    colour clues and other digits (which differ in weight and slant as well, never colour alone).

    ``picture`` (a :class:`Picture`, default ``None``) is drawn behind the board under a veil of
    ``background`` at opacity ``veil`` (0..1, default ``DEFAULT_VEIL``). With a picture the theme
    must pass :func:`worst_case_contrast` (digits 4.5:1, lines 3:1) or construction fails; that is
    what keeps a picture theme readable whatever the picture shows.
    """

    name: str
    background: str
    grid: str
    box: str
    border: str
    digit: str
    given: str
    filled: str
    picture: Picture | None = None
    veil: float = DEFAULT_VEIL

    def __post_init__(self) -> None:
        if not isinstance(self.name, str) or not _THEME_NAME.fullmatch(self.name):
            raise InvalidArgumentError(f"theme name must match {_THEME_NAME.pattern!r}, got {self.name!r}")
        for field in ("background", "grid", "box", "border", "digit", "given", "filled"):
            value = getattr(self, field)
            if not isinstance(value, str) or not _HEX_COLOR.fullmatch(value):
                raise InvalidArgumentError(f"theme {field} must be a '#rrggbb' colour, got {value!r}")
            object.__setattr__(self, field, value.lower())
        if self.picture is not None and not isinstance(self.picture, Picture):
            raise InvalidArgumentError(f"theme picture must be a Picture or None, got {type(self.picture).__name__}")
        if isinstance(self.veil, bool) or not isinstance(self.veil, (int, float)) or not 0 <= self.veil <= 1:
            raise InvalidArgumentError(f"theme veil must be a number from 0 to 1, got {self.veil!r}")
        object.__setattr__(self, "veil", round(float(self.veil), 4))  # checked exactly as it is written out
        if self.picture is not None:
            for field, needed in (("digit", _MIN_DIGIT_CONTRAST), ("given", _MIN_DIGIT_CONTRAST),
                                  ("filled", _MIN_DIGIT_CONTRAST), ("grid", _MIN_LINE_CONTRAST),
                                  ("box", _MIN_LINE_CONTRAST), ("border", _MIN_LINE_CONTRAST)):
                ratio = worst_case_contrast(getattr(self, field), self.background, self.veil)
                if ratio < needed:
                    raise InvalidArgumentError(
                        f"theme {self.name!r}: {field} {getattr(self, field)} reaches only {ratio:.2f}:1 over the "
                        f"picture (needs {needed}:1); use a stronger veil or a colour further from the background")


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
    ]
    if theme.picture is not None:
        picture = theme.picture
        data = base64.b64encode(picture.data).decode("ascii")
        out.append(f'  <image class="picture" x="0" y="0" width="{total}" height="{total}" '
                   f'preserveAspectRatio="{PICTURE_FOCUS[picture.focus]} slice" '
                   f'href="data:{picture.media_type};base64,{data}"/>')
        opacity = f"{theme.veil:.4f}".rstrip("0").rstrip(".")  # the exact value the contrast check used
        out.append(f'  <rect class="veil" x="0" y="0" width="{total}" height="{total}" fill="{theme.background}" '
                   f'fill-opacity="{opacity}"/>')
    out.append(f'  <g class="thin-lines" stroke="{theme.grid}" stroke-width="{_THIN}">')
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
