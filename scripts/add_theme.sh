#!/usr/bin/env bash
# Add a picture theme: derive a readable palette from the picture, add it to themes.json, render a preview.
#   scripts/add_theme.sh PICTURE [--name NAME] [--focus top|center|...] [--veil 0.72] [--like THEME] [--replace] [--no-preview]
# PICTURE is a file name in themePictures/originals/ or a path to a PNG, JPEG or SVG file (copied there).
# The pack is themePictures/themes.json (or $SUDOKU_THEME_PACK). The preview is product/<name>-preview.svg,
# an Easy puzzle with a random seed, replaced on every run. JPEG needs gdk-pixbuf-csource
# (Ubuntu: sudo apt install libgdk-pixbuf2.0-bin) unless --like is given.
set -eu

ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${SUDOKU_THEME_PACK:-$ROOT/themePictures/themes.json}"
PYTHON=python3
[ -x "$ROOT/.venv/bin/python" ] && PYTHON="$ROOT/.venv/bin/python"

preview=1
args=()
for arg in "$@"; do
    if [ "$arg" = "--no-preview" ]; then preview=0; else args+=("$arg"); fi
done

builder() {
    PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$ROOT/python" "$PYTHON" -m sudoku_app.theme_builder --manifest "$MANIFEST" "$@"
}
case " $* " in *" -h "*|*" --help "*) builder --help; exit 0 ;; esac

output="$(builder "${args[@]}")" || exit $?
printf '%s\n' "$output" | sed '$d'                 # the palette; the last line is the theme name
name="$(printf '%s\n' "$output" | tail -n 1)"

if [ "$preview" = 1 ]; then
    SUDOKU_THEME_PACK="$MANIFEST" "$ROOT/run.sh" generate --difficulty easy --theme "$name" \
        --output "$name-preview.svg" --overwrite | tail -n 1
fi
echo "use it: ./run.sh generate --theme $name --output puzzle.svg"
