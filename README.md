# Sudoku Studio

Generates Sudoku puzzles that are guaranteed to have exactly one solution, at a chosen difficulty, and draws them as SVG images,
optionally over a picture. A C++20 engine does all the Sudoku work; a small Python command line (`./run.sh`) drives it.

Licensed under the [MIT licence](LICENSE). Technical details (engine, JSON protocol, Python library, guarantees, measurements) are in
the [technical reference](docs/REFERENCE.md).

## Run the application

Needs `cmake` (>= 3.20), a C++20 compiler (`g++` 13) and Python >= 3.11. Verified on Ubuntu 24.04 x86_64 only.

```sh
sudo apt install build-essential cmake python3        # once, if these are missing
./run.sh generate --difficulty easy --output puzzle.svg
```

The first run builds the engine (15-60 seconds). `run.sh` works from any directory, installs nothing and uses no network.

| to ... | run |
| --- | --- |
| generate a puzzle (random seed, printed) | `./run.sh generate` |
| choose the level and the seed | `./run.sh generate --difficulty medium --seed 7` (levels: `easy`, `medium`, `hard`, `expert`, `master`) |
| save it as an image (to `product/`) | `./run.sh generate --difficulty easy --output puzzle.svg` |
| also save the solution, choose a theme | `... --output puzzle.svg --solution-output solution.svg --theme dragon` |
| see the solution in the terminal | `./run.sh generate --seed 7 --show-solution` |
| list the themes | `./run.sh themes` |
| one Easy puzzle for every theme | `scripts/theme_samples.sh` |
| add a picture as a new theme | `scripts/add_theme.sh PICTURE` ([details](#adding-a-picture)) |
| turn every SVG in `product/` into PNG in `product/png/` | `scripts/svg_to_png.sh` ([details](#png-images)) |
| solve, check, rate a board | `./run.sh solve --board "53..7....6..195....98..."`, `./run.sh verify --puzzle "..." --solution "..."`, `./run.sh classify --board "..."` |
| help | `./run.sh --help`, `./run.sh generate --help` |

Examples (executed by the tests):

```sh runapp
./run.sh generate --difficulty easy --seed 7 --output puzzle.svg
./run.sh solve --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79" --unique
./run.sh verify --puzzle "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79" --solution "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
./run.sh classify --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
```

**Where files go.** A plain file name (`--output puzzle.svg`) is saved in `product/` in the repository, which is created when needed.
A path with a directory (`./puzzle.svg`, `out/puzzle.svg`, `/tmp/puzzle.svg`) is used as given, relative to where you run the command.
`SUDOKU_OUTPUT_DIR=DIR ./run.sh ...` uses another folder instead of `product/`. An existing file is never replaced unless you add
`--overwrite`, and nothing is written when generation fails.

**What you can rely on.**
* Every puzzle has exactly one solution, proven by an exhaustive search.
* With `--difficulty` you get exactly that level or an error, never a different level. Expert and Master are rare: if the command
  says no puzzle was found, raise `--max-attempts` (for example 2000) or try another seed.
* The seed is always printed; the same seed gives the same puzzle on the same build.
* `--method logical` solves by deductions only. If it stops, the deductions ran out; that does not mean the puzzle is unsolvable.
* Exit codes: `0` success, `1` negative answer (nothing found, no solution, invalid solution), `2` wrong input, `3` engine problem,
  `4` file could not be written. Errors are one `error:` line.

## Picture themes

A picture theme draws the board over a picture. A darkened veil lies over the picture so that the digits and lines stay readable;
this is checked by calculation for every theme, whatever the picture shows.

```sh
./run.sh themes                                              # what is available
./run.sh generate --difficulty easy --theme rosette --output rosette.svg
scripts/theme_samples.sh                                     # product/<theme>_<seed>.svg for every theme
```

Built-in themes are `default`, `light` and `dark`. The picture themes come from `themePictures/themes.json`. That folder holds
your own pictures and is not part of the repository.

### Adding a picture

```sh
scripts/add_theme.sh ~/Pictures/sea.jpg          # or a file name already in themePictures/originals/
```

The script copies the picture into `themePictures/originals/`, reads its colours, picks a palette from them and lightens
each colour just enough to be readable. It then adds the theme to `themePictures/themes.json` and saves a preview puzzle as
`product/<name>-preview.svg`. The theme name comes from the file name (`sea.jpg` → `sea`). Portraits keep their top part
when they are cropped to a square.

| option | meaning |
| --- | --- |
| `--name NAME` | another theme name (lower-case letters, digits and `-`) |
| `--focus top\|center\|bottom\|left\|right` | which part of the picture stays visible after cropping |
| `--veil 0.8` | darken the picture more (calmer, default `0.72`); below about 0.7 often no colour is readable enough and the script says so |
| `--like THEME` | take the colours of an existing theme instead of the picture's |
| `--replace` | redo a theme that already exists (new colours, other focus or veil) |
| `--no-preview` | skip the preview puzzle |

Pictures can be PNG, JPEG or SVG (at most 10 MiB). Reading JPEG colours needs `gdk-pixbuf-csource`
(`sudo apt install libgdk-pixbuf2.0-bin`); without it use `--like THEME`.

**Adjusting by hand.** Every theme is one line in `themes.json` and can be edited:

```json
{"name": "sea", "picture": "originals/sea.jpg", "focus": "top", "veil": 0.72,
 "colors": {"background": "#0b0b20", "grid": "#7ea6bb", "box": "#4fc3e8", "border": "#bff6ff",
            "given": "#ffffff", "filled": "#8ff3ff"}}
```

`background` is the veil colour; `grid` the thin lines, `box` the lines between 3x3 boxes, `border` the outer frame,
`given` the printed clues (bold), `filled` the digits written into the solution (italic). After an edit, `./run.sh themes`
checks the file. A colour that is too dark is reported with its contrast, for example
`grid #5f7396 reaches only 1.67:1 over the picture (needs 3.0:1)`: lighten it or raise `veil`.

All entries are checked when the file is read: a typo, an unknown key, a missing picture or a duplicate name stops with an error
instead of being skipped. The SVG file embeds the picture, so it is about as large as the picture itself.
Full rules: [picture themes in the reference](docs/REFERENCE.md#picture-themes).

## PNG images

```sh
scripts/svg_to_png.sh                 # product/*.svg -> product/png/*.png, 940 x 940 pixels
scripts/svg_to_png.sh --scale 4       # sharper (1880 x 1880), e.g. for printing
scripts/svg_to_png.sh --force         # convert everything again
```

Only new or changed SVGs are converted, so it is cheap to run after every batch. A file that cannot be converted is
reported and the rest still are (exit code 1). The images are drawn by librsvg through `gdk-pixbuf-csource`
(`sudo apt install libgdk-pixbuf2.0-bin librsvg2-common`). The PNG matches the SVG pixel for pixel at the chosen scale;
the SVG stays the original.

## Developer quick start

```sh
cmake --preset dev && cmake --build --preset dev     # build into build/dev (Debug, with tests)
ctest --preset dev --output-on-failure               # C++ tests
python3 -m venv .venv && .venv/bin/pip install pytest # once (pytest is the only Python dependency)
.venv/bin/python -m pytest -q                        # Python tests; they need the build above
```

The Python package needs no installation (`PYTHONPATH=python`). Library examples, the JSON protocol of the engine and every
component contract are in the [technical reference](docs/REFERENCE.md).

## Repository structure

```text
.
├── CLAUDE.md           # instructions for the AI assistant
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
├── pyproject.toml
├── run.sh              # launcher: builds the engine, runs the Python command line
├── cpp/                # C++ engine: include/sudoku (headers), src, tests
├── benchmarks/         # sudoku_bench: fixed corpus, runner, JSON report
├── python/
│   └── sudoku_app/     # command line, engine client, SVG rendering, themes, theme builder, PNG export, batches, settings
├── scripts/            # add_theme.sh (new theme), theme_samples.sh (sample per theme), svg_to_png.sh (PNG copies)
├── docs/               # REFERENCE.md: technical reference
└── tests/
    └── python/         # pytest suites (unit, integration, documentation examples)
```

Not in the repository: `build/` (build output), `product/` (generated puzzles) and `themePictures/` (your pictures and `themes.json`).

## Status and limitations

All planned features are implemented and tested; the project is not released or tagged.

* Verified on Ubuntu 24.04 x86_64 with g++ 13 only; other systems and compilers are untested, Windows is not supported by `run.sh`.
* Expert and Master puzzles are rare and can need thousands of attempts.
* Picture themes have one layout (picture under a veil). Settings files and batches accept built-in theme names only.
* Output is SVG, with PNG copies through `scripts/svg_to_png.sh`; no PDF or GUI.

More: [known limitations and release status](docs/REFERENCE.md#known-limitations-and-release-status).
