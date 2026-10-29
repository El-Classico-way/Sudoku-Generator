#!/usr/bin/env bash
# Convert every SVG in product/ to PNG in product/png/.
#   scripts/svg_to_png.sh [--scale N] [--force]
# --scale N   pixels per SVG unit, 1-8 (default 2: a standard board becomes 940 x 940 pixels)
# --force     convert again even if the PNG is newer than its SVG (by default only new or changed SVGs are converted)
# The folder is $SUDOKU_OUTPUT_DIR if set, else product/ in the repository. Needs gdk-pixbuf-csource with
# SVG support (Ubuntu: sudo apt install libgdk-pixbuf2.0-bin librsvg2-common). Exit code 1 if a file failed.
set -eu

ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${SUDOKU_OUTPUT_DIR:-$ROOT/product}"
PYTHON=python3
[ -x "$ROOT/.venv/bin/python" ] && PYTHON="$ROOT/.venv/bin/python"

PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$ROOT/python" exec "$PYTHON" -m sudoku_app.png_export "$SOURCE" "$@"
