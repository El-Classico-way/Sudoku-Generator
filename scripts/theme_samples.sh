#!/usr/bin/env bash
# One Easy puzzle per SVG theme, each with its own random seed (printed, so any sample can be reproduced).
#   scripts/theme_samples.sh [OUTPUT_DIR]        default: product/ in the repository (created if missing)
# Each puzzle is generated into a temporary directory and then moved to OUTPUT_DIR/<theme>_<seed>.svg,
# so repeated runs collect puzzles; an existing file is never replaced (that theme fails instead).
# The theme list is `./run.sh themes --names`: the built-in themes plus the picture themes of the theme
# pack ($SUDOKU_THEME_PACK, else themePictures/themes.json). Extra environment is passed on to run.sh.
set -eu

ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/product}"
mkdir -p "$OUT"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sudoku-samples.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

themes="$("$ROOT/run.sh" themes --names)"
failed=0
first=1
printf '%-22s %-20s %s\n' "theme" "seed" "file"
for theme in $themes; do
    draft="$WORK/$theme.svg"
    # the first call builds the engine if needed; the rest reuse it
    if output="$( [ "$first" = 1 ] || export SUDOKU_NO_BUILD=1
                  "$ROOT/run.sh" generate --difficulty easy --theme "$theme" --output "$draft" 2>&1 )"; then
        seed="$(printf '%s\n' "$output" | sed -n 's/^seed: *\([0-9]*\).*/\1/p')"
        file="$OUT/${theme}_${seed}.svg"
        if [ -e "$file" ]; then
            printf '%-22s FAILED: %s already exists\n' "$theme" "$file" >&2
            failed=1
        else
            mv "$draft" "$file"
            printf '%-22s %-20s %s\n' "$theme" "$seed" "$file"
        fi
    else
        printf '%-22s FAILED\n%s\n' "$theme" "$output" >&2
        failed=1
    fi
    first=0
done
exit "$failed"
