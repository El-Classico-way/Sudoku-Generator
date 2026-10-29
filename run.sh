#!/usr/bin/env bash
# Sudoku Studio launcher: builds the C++ engine when needed and runs the Python command line.
#   ./run.sh                      show help
#   ./run.sh generate --difficulty easy --seed 7
# Works from any directory. Output files: a bare name goes to product/ (or $SUDOKU_OUTPUT_DIR), a path with a
# directory is relative to where you call it.
# Environment: SUDOKU_NO_BUILD=1 skips the build step and uses the existing engine;
#              SUDOKU_THEME_PACK=FILE selects the picture themes (default: themePictures/themes.json if present).
set -u

SOURCE="${BASH_SOURCE[0]}"
while [ -L "$SOURCE" ]; do                       # follow symlinks to find the real location
    DIR="$(cd -P "$(dirname "$SOURCE")" && pwd)"
    SOURCE="$(readlink "$SOURCE")"
    case "$SOURCE" in /*) ;; *) SOURCE="$DIR/$SOURCE" ;; esac
done
ROOT="$(cd -P "$(dirname "$SOURCE")" && pwd)"
ENGINE="$ROOT/build/dev/sudoku_cli"

die() { echo "run.sh: error: $*" >&2; exit 3; }

# --- Python (3.11+, standard library only; no packages are installed or downloaded) -------------------------
PYTHON=""
for candidate in "$ROOT/.venv/bin/python" python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 \
        && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
[ -n "$PYTHON" ] || die "Python 3.11 or newer is required but was not found. On Ubuntu: sudo apt install python3"

# Picture themes: the pack in themePictures/ is used unless SUDOKU_THEME_PACK is already set.
if [ -z "${SUDOKU_THEME_PACK:-}" ] && [ -f "$ROOT/themePictures/themes.json" ]; then
    export SUDOKU_THEME_PACK="$ROOT/themePictures/themes.json"
fi

# Output files given as a bare name (generate --output puzzle.svg) go to product/ unless SUDOKU_OUTPUT_DIR is set.
export SUDOKU_OUTPUT_DIR="${SUDOKU_OUTPUT_DIR:-$ROOT/product}"

run_python() {
    PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" SUDOKU_CLI="$ENGINE" \
        exec "$PYTHON" -m sudoku_app "$@"
}

# Help and the theme list need no engine, so they work even before the first build.
if [ "$#" -eq 0 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ] || [ "$1" = "themes" ]; then
    run_python "$@"
fi

# --- Build the engine (incremental: a no-op when nothing changed) ---------------------------------------------
if [ "${SUDOKU_NO_BUILD:-0}" != "1" ]; then
    if command -v cmake >/dev/null 2>&1; then
        command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 \
            || die "no C++ compiler found. On Ubuntu: sudo apt install g++ cmake"
        [ -x "$ENGINE" ] || echo "run.sh: building the C++ engine (first run only; this can take a minute)..." >&2
        LOG="$(mktemp "${TMPDIR:-/tmp}/sudoku-build.XXXXXX")" || die "cannot create a temporary file"
        if (cd "$ROOT" && cmake --preset dev && cmake --build --preset dev --target sudoku_cli) >"$LOG" 2>&1; then
            rm -f "$LOG"
        else
            cat "$LOG" >&2
            rm -f "$LOG"
            die "building the engine failed (output above). Needs CMake >= 3.20 and a C++20 compiler such as g++ 13."
        fi
    elif [ ! -x "$ENGINE" ]; then
        die "cmake was not found and the engine is not built. On Ubuntu: sudo apt install cmake g++"
    fi
fi

[ -x "$ENGINE" ] || die "the engine was not found at $ENGINE. Run ./run.sh without SUDOKU_NO_BUILD=1 to build it."

run_python "$@"
