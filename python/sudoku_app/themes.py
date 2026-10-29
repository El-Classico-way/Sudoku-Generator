"""Theme packs: picture themes described by a JSON manifest next to their pictures.

A pack is a ``themes.json`` file::

    {"schema": "sudoku.themes/1",
     "themes": [
       {"name": "dragon", "picture": "originals/dragon.svg", "focus": "center", "veil": 0.72,
        "colors": {"background": "#0b0b20", "grid": "#c59588", "box": "#ff7c1e", "border": "#ffc837",
                   "given": "#fff6dc", "filled": "#ffb54b"}}]}

``name``, ``picture`` and ``colors`` are required; ``focus`` (default ``center``), ``veil`` (default
``DEFAULT_VEIL``) and ``colors.digit`` (default: the ``given`` colour) are optional. ``picture`` is a
relative path, resolved against the manifest's directory, that may not leave it. Names follow the
:class:`Theme` rules and may not repeat or shadow a built-in theme. Unknown or duplicate keys, wrong
types and unreadable pictures are rejected with :class:`ConfigurationError`; every theme must pass
the picture-theme contrast check of :class:`Theme`. Nothing is repaired or skipped.
"""
from __future__ import annotations

import os
from pathlib import Path, PurePath
from types import MappingProxyType
from typing import Any, Mapping

from .errors import ConfigurationError, InvalidArgumentError
from .protocol import loads_strict
from .svg import DEFAULT_VEIL, THEME_NAMES, THEMES, Picture, Theme

__all__ = ["THEME_PACK_SCHEMA", "load_theme_pack", "resolve_theme"]

THEME_PACK_SCHEMA = "sudoku.themes/1"
_MAX_MANIFEST_BYTES = 1024 * 1024
_ENTRY_KEYS = {"name", "picture", "focus", "veil", "colors"}
_REQUIRED_ENTRY_KEYS = {"name", "picture", "colors"}
_COLOR_KEYS = {"background", "grid", "box", "border", "given", "filled"}


def _object(value: Any, where: str, allowed: set[str], required: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigurationError(f"{where} must be a JSON object, got {type(value).__name__}")
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ConfigurationError(f"{where} has unknown key(s): {', '.join(unknown)}")
    missing = sorted(required - set(value))
    if missing:
        raise ConfigurationError(f"{where} is missing key(s): {', '.join(missing)}")
    return value


def _picture_path(base: Path, value: Any, where: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ConfigurationError(f"{where}.picture must be a non-empty string")
    relative = PurePath(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ConfigurationError(f"{where}.picture must be a relative path inside the pack directory, got {value!r}")
    return base / relative


def _theme(base: Path, entry: Any, where: str) -> Theme:
    entry = _object(entry, where, _ENTRY_KEYS, _REQUIRED_ENTRY_KEYS)
    colors = _object(entry["colors"], f"{where}.colors", _COLOR_KEYS | {"digit"}, _COLOR_KEYS)
    path = _picture_path(base, entry["picture"], where)
    veil = entry.get("veil", DEFAULT_VEIL)
    try:
        picture = Picture.from_file(path, entry.get("focus", "center"))
        return Theme(entry["name"], digit=colors.get("digit", colors["given"]), picture=picture, veil=veil,
                     **{k: colors[k] for k in _COLOR_KEYS})
    except OSError as exc:
        raise ConfigurationError(f"{where}: cannot read picture {str(path)!r}: {exc.strerror or exc}") from None
    except InvalidArgumentError as exc:
        raise ConfigurationError(f"{where}: {exc}") from None


def load_theme_pack(path: str | os.PathLike[str]) -> Mapping[str, Theme]:
    """Read and validate a pack; returns name -> :class:`Theme` in manifest order (read-only mapping)."""
    manifest = Path(path)
    try:
        with open(manifest, "rb") as handle:
            raw = handle.read(_MAX_MANIFEST_BYTES + 1)
    except OSError as exc:
        raise ConfigurationError(f"cannot read theme pack {str(manifest)!r}: {exc.strerror or exc}") from None
    if len(raw) > _MAX_MANIFEST_BYTES:
        raise ConfigurationError(f"theme pack {str(manifest)!r} is larger than {_MAX_MANIFEST_BYTES} bytes")
    try:
        document = loads_strict(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ConfigurationError(f"theme pack {str(manifest)!r} is not valid JSON: {exc}") from None
    document = _object(document, "theme pack", {"schema", "themes"}, {"schema", "themes"})
    if document["schema"] != THEME_PACK_SCHEMA:
        raise ConfigurationError(f"theme pack schema must be {THEME_PACK_SCHEMA!r}, got {document['schema']!r}")
    entries = document["themes"]
    if not isinstance(entries, list):
        raise ConfigurationError("theme pack 'themes' must be a list")
    themes: dict[str, Theme] = {}
    for i, entry in enumerate(entries):
        theme = _theme(manifest.parent, entry, f"themes[{i}]")
        if theme.name in THEMES or theme.name in themes:
            raise ConfigurationError(f"themes[{i}]: the name {theme.name!r} is already used")
        themes[theme.name] = theme
    return MappingProxyType(themes)


def resolve_theme(name: str, pack: Mapping[str, Theme] | None = None) -> Theme:
    """A built-in theme by name, else a theme of ``pack``; an unknown name raises ``InvalidArgumentError``."""
    if name in THEMES:
        return THEMES[name]
    if pack is not None and name in pack:
        return pack[name]
    available = list(THEME_NAMES) + list(pack or ())
    raise InvalidArgumentError(f"unknown theme {name!r} (available: {', '.join(available)})")
