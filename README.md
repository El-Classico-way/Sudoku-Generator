# Sudoku Studio

A customizable Sudoku puzzle generator focused on correctness,
reproducibility, logical solving, and print-ready visual output.

The project combines a modern C++ Sudoku engine with a Python
orchestration and SVG rendering layer.

> Development status: Initial project setup.
>
> Sudoku-solving, generation, and rendering capabilities are implemented
> incrementally. Features should not be considered available until they
> are implemented, tested, and documented.

## Goals

The project aims to provide:

- Valid, complete 9x9 Sudoku grids.
- Puzzles with exactly one solution.
- A fast search solver and solution counter.
- A logical solver that explains deductions without guessing.
- Configurable difficulty profiles based on solving techniques.
- Reproducible puzzle generation using explicit seeds.
- Detailed generation diagnostics and performance measurements.
- Customizable, print-ready SVG puzzle designs.
- Separate puzzle and solution exports.
- Automated C++ and Python tests.

Correctness takes priority over generation speed and visual complexity.

## Architecture

The application is divided into two primary components.

### C++ engine

The C++20 engine is responsible for computationally intensive
Sudoku operations:

- Board representation and validation.
- Search solving and solution counting.
- Complete-grid generation.
- Uniqueness-preserving clue removal.
- Logical solving and deduction traces.
- Difficulty evaluation.
- Difficulty-targeted puzzle generation.

The engine is designed to remain independent of the visual layer.

### Python application

The Python application is responsible for:

- Command-line orchestration.
- Configuration.
- Communication with the C++ executable.
- Request and response validation.
- Error reporting.
- SVG rendering.
- Themes and visual configuration.
- Puzzle and solution export.
- Batch generation workflows.

The initial integration strategy uses JSON over standard input
and output rather than Python bindings.

### Communication

The C++ executable and Python application communicate through a
documented JSON protocol.

The protocol will define supported operations, request formats,
response formats, error reporting, and versioning.

Standard output is reserved for protocol responses. Diagnostic
messages should be written to standard error.

The exact interface will be documented when implemented.

## Repository Structure

Current layout (directories such as `benchmarks/`,
`examples/`, and `docs/` will be added when they hold real content):

```text
.
├── CLAUDE.md
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── pyproject.toml
├── .gitignore
├── cpp/
│   ├── include/sudoku/ # public headers (board, solver, generator, puzzle)
│   ├── src/            # board.cpp, solver.cpp, generator.cpp, puzzle.cpp, search.hpp (private), sudoku_cli placeholder entry point
│   └── tests/          # CTest tests (plain executables, check.hpp)
├── python/
│   └── sudoku_app/
└── tests/
    └── python/
```

Build output goes to `build/` (Git-ignored).

## Requirements

Verified on Ubuntu 24.04 x86_64 only; other platforms are untested.

- C++20 compiler (verified: g++ 13.3.0)
- CMake >= 3.20 (verified: 3.28.3)
- Python >= 3.11 (verified: 3.12.3)
- pytest (installed into the project-local `.venv`, see below)
- Ninja is optional; the presets do not require a specific generator.

## Build and test

C++ (from the repository root):

```sh
cmake --preset dev            # configure into build/dev
cmake --build --preset dev
ctest --preset dev            # prints output of failing tests
./build/dev/sudoku_cli --version
```

Python:

```sh
python3 -m venv .venv                  # once, if .venv does not exist
.venv/bin/pip install pytest           # once; or: pip install -e ".[test]"
.venv/bin/python -m pytest             # config in pyproject.toml
```

pytest finds the package through `pythonpath = ["python"]` in
`pyproject.toml`, so no install step is needed to run the tests.

## Current status

Build/test foundation (M01), the board model (M02), the search
solver/solution counter (M03), complete-grid generation (M04),
uniqueness-preserving clue removal (M05), and test hardening (M06) exist.

- `sudoku::Board` (`cpp/include/sudoku/board.hpp`): fixed 81-cell board with
  range-checked access and row/column/box conflict checks, completeness,
  consistency, and valid-solution validation. "Consistent" means no repeated
  digit in a unit; it does not imply the board is solvable.

- `sudoku::find_solution` / `count_solutions` / `classify_uniqueness`
  (`cpp/include/sudoku/solver.hpp`): MRV backtracking search with a
  solution limit and optional node budget. Results distinguish invalid
  input, exhaustive (exact) counts, and stopped searches (lower bounds);
  one found solution is never reported as unique unless the search was
  exhaustive. Not yet exposed through `sudoku_cli`.
- `sudoku::generate_complete_grid(seed, options)`
  (`cpp/include/sudoku/generator.hpp`): seeded, randomized MRV backtracking
  that produces a complete valid 9x9 grid. Same seed => same grid with this
  implementation; no uniform-sampling claim. Complete grids only: clue
  removal and puzzle generation are **not** implemented.
- `sudoku::remove_clues` / `generate_puzzle`
  (`cpp/include/sudoku/puzzle.hpp`): greedy clue removal from a complete grid
  in a seeded random order. A removal is kept only if
  `count_solutions(puzzle, 2)` is exhaustive and finds exactly one solution;
  zero, two-or-more, and budget-interrupted counts all revert it. The final
  puzzle is re-counted and compared with the source grid. `target_clues`
  (17..81) is a goal, not a guarantee: if one pass cannot reach it the result
  is `TargetNotReached` with a verified unique puzzle that has more clues.
  Same solution + seed + options => same puzzle (this implementation).
  No difficulty rating or logical solving yet.
- Hardening (M06): `cpp/tests/hardening_test.cpp` cross-checks the validators,
  solver and counter against naive independent references
  (`cpp/tests/reference.hpp`) on fixed-seed pseudo-random boards, runs the
  generate -> remove clues -> count -> solve pipeline over 25 seeds, and
  checks that clue-removal results are minimal when the target is not
  reached. Failure messages include the trial number and input grid.
- `sudoku_cli` only supports `--version`.
- `sudoku_app` only exposes `__version__`.
- Logical solving, puzzle (clue-removal) generation, difficulty evaluation, the JSON
  protocol, and rendering are **not implemented yet**.

Optional sanitizer check (not wired into the repo; scratch build dir):

```sh
cmake -S . -B /path/to/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cmake --build /path/to/asan && ctest --test-dir /path/to/asan --output-on-failure
```

## Decisions and known limitations

- **C++ tests:** no test framework (Catch2, GoogleTest, doctest) is
  installed. Current tests are plain executables/commands evaluated by
  CTest through exit status and output matching. This is a stopgap, not
  a framework. Network access to PyPI was observed, so adopting a
  framework (via `find_package` or CMake `FetchContent`) is an open
  decision for M02.
- **JSON library:** none installed (no nlohmann/json found). Not added
  yet; to be chosen when the JSON protocol is implemented.
- **clang++:** installed (18.1.3) but fails to link in this environment
  (`cannot find -lstdc++`), so builds were verified with g++ only.
- Warnings are enabled per target (`-Wall -Wextra -Wpedantic`, `/W4` on
  MSVC) and are not treated as errors.
- No benchmarks exist; no performance claims are made.

