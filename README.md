# Sudoku Studio

A Sudoku puzzle generator built for correctness and reproducibility: a C++20 engine (search solver, exact
solution counting, uniqueness-preserving generation, a deduction-only logical solver, difficulty profiles) behind a
strict JSON command-line protocol, plus a Python layer with a typed engine client, SVG rendering with themes,
verified solution export, batch workflows, and a command line (`./run.sh`).

Status: all planned features are implemented and tested on one platform (see [Requirements](#requirements-and-platform-coverage));
the project has not been tagged or published (see [Known limitations and release status](#known-limitations-and-release-status)).
Licensed under the [MIT licence](LICENSE).

## Run the application

On **Ubuntu 24.04 (x86_64)** with `git`, `cmake` (>= 3.20), a C++20 compiler (`g++` 13) and `python3` (>= 3.11):

```sh
sudo apt install build-essential cmake python3        # once, if these are missing
./run.sh                                              # help and examples
./run.sh generate --difficulty easy --seed 7          # a puzzle you can solve
```

`run.sh` builds the C++ engine the first time (typically 15-60 seconds; it needs the C++ toolchain, there is no prebuilt binary), then runs
the Python command line. It works from any directory, installs nothing, uses no network, and needs no virtual environment or pip package.

| to ... | run |
| --- | --- |
| generate a puzzle (random seed, printed) | `./run.sh generate` |
| choose the level (`easy`, `medium`, `hard`, `expert`, `master`) and a seed | `./run.sh generate --difficulty medium --seed 7` |
| give a hard level more tries (default 100 attempts) | `./run.sh generate --difficulty expert --max-attempts 2000` |
| also see the solution | `./run.sh generate --seed 7 --show-solution` |
| export an SVG image (`--solution-output` for the solved one, `--theme dark`) | `./run.sh generate --difficulty medium --seed 7 --output puzzle.svg` |
| solve a board (81 characters, `.` or `0` for empty) | `./run.sh solve --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"` |
| solve by logical deductions only, or check uniqueness | `./run.sh solve --method logical --board "..."` / `./run.sh solve --unique --board "..."` |
| check a solution | `./run.sh verify --puzzle "..." --solution "..."` |
| rate a board | `./run.sh classify --board "..."` |
| help | `./run.sh --help`, `./run.sh generate --help` |

Real examples (these are executed by the tests; `sh runapp` blocks use a scratch directory):

```sh runapp
./run.sh generate --difficulty easy --seed 7 --output puzzle.svg
./run.sh solve --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79" --unique
./run.sh verify --puzzle "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79" --solution "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
./run.sh classify --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
```

**Behaviour you can rely on.** A puzzle is printed only if the engine produced it and an exhaustive search confirmed that it has exactly one
solution. With `--difficulty` the puzzle is of exactly that level or the command fails (it never hands out another level); the solution stays
hidden unless you ask. The seed is random unless given, and always printed; the same seed gives the same puzzle on the same build. Solving by
`--method logical` can stall, which is reported as a stall, not as "unsolvable". Exit codes: `0` success; `1` a negative answer (generation
exhausted, no solution, a stall, an invalid solution); `2` invalid input or usage; `3` engine problem (missing, timeout, crash); `4` output
file problem. Errors are one `error:` line on stderr.

**Files.** Relative paths (`--output`, `--solution-output`) are relative to the directory you run the command from, not the repository. The
directory must already exist, and an existing file is not replaced unless you add `--overwrite`. Files are written only after the puzzle has
been generated and checked; nothing is written on failure.

**Common errors**

| message | remedy |
| --- | --- |
| `Python 3.11 or newer is required` | `sudo apt install python3` (or put a newer `python3` on `PATH`) |
| `no C++ compiler found` / `cmake was not found` | `sudo apt install build-essential cmake` |
| `building the engine failed` | read the compiler output printed above it; the build needs CMake >= 3.20 and a C++20 compiler |
| `no Master puzzle found in N attempt(s)` | exhaustion is a normal outcome: raise `--max-attempts` or try another `--seed` (Expert and Master are rare) |
| `board must have exactly 81 characters` | pass the board as one quoted string: digits `1`-`9`, `.` or `0` for empty |
| `invalid board: a digit repeats ...` | the board already breaks the rules |
| `output file ... already exists` | choose another name or add `--overwrite` |
| `the engine did not answer within ... seconds` | raise `--timeout` (default 300) |

Other platforms are untested (Windows is not supported by `run.sh`; the equivalent on Linux is `cmake --preset dev && cmake --build --preset dev --target sudoku_cli`
followed by `SUDOKU_CLI=build/dev/sudoku_cli PYTHONPATH=python python3 -m sudoku_app ...`). `SUDOKU_NO_BUILD=1 ./run.sh ...` skips the build step.
The Python CLI (`python/sudoku_app/cli.py`) is a thin layer over the library described below; it contains no Sudoku logic.

## Contents

* [Run the application](#run-the-application)
* [Developer quick start](#developer-quick-start)
* [Requirements and platform coverage](#requirements-and-platform-coverage)
* [Architecture](#architecture) and [Correctness contracts](#correctness-contracts)
* [Repository structure](#repository-structure) and [Build and test](#build-and-test)
* Reference: [C++ engine](#c-engine-reference), [Diagnostics and benchmarks](#diagnostics-and-benchmarks),
  [JSON protocol and CLI](#json-protocol-and-cli), [Python engine client](#python-engine-client),
  [SVG rendering](#svg-rendering), [Themes and solution export](#themes-and-solution-export),
  [Component contracts](#component-contracts-and-reliability-audit), [Performance analysis](#performance-analysis),
  [Batch operations and settings](#batch-operations-and-settings)
* [Known limitations and release status](#known-limitations-and-release-status)

## Developer quick start

Build, test and use the libraries directly. Every command is run from the repository root.

```sh
# 1. Build the engine and run the C++ tests (C++20 compiler and CMake >= 3.20)
cmake --preset dev                        # Debug build into build/dev, tests on
cmake --build --preset dev
ctest --preset dev --output-on-failure

# 2. Python >= 3.11; pytest is the only dependency, the package itself needs no installation
python3 -m venv .venv                     # once
.venv/bin/pip install pytest              # once (needs network access to a package index)
.venv/bin/python -m pytest -q             # needs the build from step 1 (real-engine tests fail loudly without it)
```

The C++ command line takes one JSON request on standard input and prints one JSON line (exit code 0 = success,
1 = valid request, non-success outcome, 2 = invalid request, 3 = internal failure):

```sh run
build/dev/sudoku_cli --version
echo '{"schema":"sudoku.request/1","op":"generate_puzzle","params":{"seed":"7","target_clues":30}}' | build/dev/sudoku_cli
echo '{"schema":"sudoku.request/1","op":"generate_targeted","params":{"seed":"1","target":"Easy","max_attempts":50}}' | build/dev/sudoku_cli
build/dev/benchmarks/sudoku_bench --tier smoke --output smoke-report.json    # six cheap cases; exit 0 = outcomes as expected
```

Python (run scripts with `PYTHONPATH=python .venv/bin/python script.py`, or from the repository root
inside pytest; the engine is found through `SUDOKU_CLI` or `build/dev/sudoku_cli`). This generates a puzzle, lets the engine
prove it has exactly one solution, and exports the engine-verified solution as SVG (given digits bold, filled digits italic):

```python run
import tempfile
from pathlib import Path
from sudoku_app import SudokuEngine, export_solution_svg

engine = SudokuEngine(timeout=60)
puzzle = engine.generate_puzzle(seed=7, target_clues=30)            # same seed, same puzzle (this version, this platform)
count = engine.count_solutions(puzzle.puzzle)                       # limit 2, exhaustive
assert count.exact and count.count == 1                             # uniqueness is established by the engine, never assumed
with tempfile.TemporaryDirectory() as out:                          # use a real directory in practice
    target = Path(out) / "solution.svg"
    svg = export_solution_svg(engine, puzzle.puzzle, puzzle.solution, target, theme="dark")   # one extra engine request: verify
    assert target.read_text(encoding="utf-8") == svg
print(puzzle.clue_count, "clues; unique:", count.classification.value)
```

More Python workflows: [batches and settings](#batch-operations-and-settings), [themes](#themes-and-solution-export),
[typed errors](#component-contracts-and-reliability-audit).

## Requirements and platform coverage

Verified on **Ubuntu 24.04 x86_64 only**; other operating systems are untested and unsupported until tested.

| component | requirement | verified with |
| --- | --- | --- |
| C++ compiler | C++20 | g++ 13.3.0 (clang++ 18.1.3 is installed but cannot link in the development environment, so clang is untested) |
| CMake | >= 3.20 (presets) | 3.28.3; the generator is not prescribed |
| Python | >= 3.11, standard library only | 3.12.3 |
| tests | pytest (Python); plain executables via CTest (C++) | pytest 9 |

No third-party C++ library is used (the JSON reader and writer are part of the project). Optional environment variables:
`SUDOKU_CLI` (engine executable for the Python client and the tests), `SUDOKU_BENCH` (benchmark executable for the benchmark tests),
`SUDOKU_ALLOW_MISSING_ENGINE=1` (skip, instead of fail, the real-engine tests on a machine without a build).

## Architecture

```text
 caller ── Python library ───────────────────────────────┐   one process per request
   batch ─ settings                                      │   stdin: one JSON request
   export_solution_svg ─ svg (themes)                    ├──────────────►  sudoku_cli ── C++ engine
   SudokuEngine (typed client, runner, protocol) ────────┘   stdout: one JSON response line
```

| layer | responsibility | does not |
| --- | --- | --- |
| **C++ engine** (`cpp/`) | Every Sudoku fact: board validation, search solving and exact counting, grid generation, uniqueness-preserving clue removal, logical solving, independent solution verification, difficulty metrics and profiles, targeted generation, diagnostics | know about JSON transport or SVG |
| **Protocol and `sudoku_cli`** | Strict validation of requests, one response per process, exit codes; re-verifies every success-shaped result before reporting it | keep state between requests |
| **Python client** (`client`, `runner`, `protocol`, `models`) | Start the executable (never through a shell), enforce a timeout, parse and cross-check the response, map failures to typed exceptions | solve, count, classify or generate anything; retry |
| **Orchestration, batch, settings** | Compose existing client calls; per-item outcomes; validated defaults | add Sudoku logic; hide failures |
| **SVG rendering and export** (`svg`, `export`) | Deterministic SVG from a board string; solution export asks the engine to verify before writing | solve or repair boards |
| **Diagnostics and benchmarks** | Reproducible JSON reports of what the engine did (`sudoku.targeted-diagnostics/1`, `sudoku.benchmark/1`) | change results (timing is taken outside the engine) |

Rendering never depends on solver internals, and the engine never depends on the Python side.

## Correctness contracts

These hold for the implementation and are covered by tests; the reference sections give the details.

**Boards, search and counting**
* A board is 81 characters, row-major: `1`-`9`, with `.` or `0` for empty cells on input (responses use `.`). Malformed input (length, characters, types) is rejected, never repaired.
* The engine separates: *invalid input* (a repeated digit in a unit, or malformed), *no solution* (consistent but unsolvable; established only by an exhaustive search), *solved*, and *interrupted* searches (node budget exhausted, or the solution `limit` reached).
* A solution count is **exact** only if the search finished (`exact: true`, status `exhausted`). Reaching the `limit` or the node budget is a lower bound: it never establishes uniqueness (`limit` 1 on a unique puzzle reports classification `unknown`). **Unique means an exact count of exactly one.**
* Every returned solution is complete, valid, and keeps all givens; the protocol layer re-verifies it, and the Python client checks it again against the submitted puzzle.

**Generation**
* A clue is removed only if an exhaustive count with limit 2 then finds exactly one solution; zero, two-or-more and budget-interrupted counts all revert the removal. `target_clues` is a goal: `target_not_reached` still returns a verified-unique puzzle with more clues.
* Targeted generation returns a puzzle only after independent exhaustive uniqueness, solution verification and an exact profile match. It makes at most `max_attempts` attempts (default 100); otherwise the result is `exhausted` with **no puzzle** (exit code 1, `ok: true`, `success: false`). Exhaustion does not mean the difficulty is impossible. Nothing of another difficulty is ever substituted.
* Seeds are 64-bit and given as decimal strings in the protocol. For a fixed implementation (same source, compiler and platform) the same request always gives the same answer: there is no clock, hidden state or unordered iteration. Results are **not** promised to be identical across versions, compilers or platforms, and no uniform-sampling claim is made.

**Logical solving and difficulty**
* The logical solver applies explicit deductions only; it never guesses, backtracks or calls the search solver. It reports `solved`, `stalled` (consistent but no applicable deduction under the enabled techniques: not a verdict on solvability), `contradiction` (the board provably cannot be completed) or `invalid_input`.
* Difficulty labels (Easy, Medium, Hard, Expert, Master) are this project's operational classification: the easiest standard technique set that solves the puzzle by deduction alone. They are not a universal human-difficulty scale. A stalled puzzle gets no label.

**Protocol and Python**
* Requests are strict (unknown fields, duplicate keys, wrong types, oversized or too deeply nested documents are rejected). One request per process, one response line on stdout, diagnostics on stderr.
* Failures are typed Python exceptions (`SudokuError` subclasses); valid engine outcomes that are not successes (exhausted, stalled, unsolvable, inconclusive) are returned as results with `success == False`. Nothing is retried.

**Rendering, export, batches**
* The default SVG output is byte-for-byte stable; themes change colours only. Export writes only after Python-side validation and a successful engine `verify`.
* A batch item is `success`, `unsuccessful` (valid non-success outcome), `failed` (with a category) or `skipped` (fail-fast); the summary is derived from the items. Item `i` uses seed `seed + i`. Exports go to an existing directory and never overwrite unless asked.

## Repository structure

```text
.
├── CLAUDE.md
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── pyproject.toml
├── .gitignore
├── cpp/
│   ├── include/sudoku/ # public headers (board, solver, generator, puzzle, candidates, deduction, singles, locked, pairs, triples, xwing, logical, verify, difficulty, targeted, diagnostics, json_writer, json_reader, protocol)
│   ├── src/            # implementations, search.hpp (private), main.cpp (sudoku_cli), protocol.cpp, json_reader.cpp
│   └── tests/          # CTest tests (plain executables, check.hpp)
├── benchmarks/         # sudoku_bench: fixed corpus, runner, JSON report
├── python/
│   └── sudoku_app/     # errors, protocol, models, runner, client, orchestration, svg, export, batch, config
└── tests/
    └── python/         # pytest suites (unit, subprocess fakes, real-engine integration, README examples)
```

Build output goes to `build/` (Git-ignored); generated `*.svg`, `*.png` and `benchmarks/results/` are ignored too.

## Build and test

```sh
cmake --preset dev            # configure into build/dev (Debug; tests on)
cmake --build --preset dev
ctest --preset dev --output-on-failure
.venv/bin/python -m pytest -q # config in pyproject.toml (pythonpath = ["python"]); run from any directory
```

The `dev` preset is an unoptimized Debug build. For representative timings use a separate Release build directory,
for example `cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release`.
Real-engine Python tests locate the executable like the client does (`SUDOKU_CLI`, else `build/dev/sudoku_cli`) and **fail** with build
instructions if it is missing; the protocol and benchmark test files (`test_cli_protocol.py`, `test_benchmark_output.py`) instead *skip*
when their binaries (`SUDOKU_CLI`/`SUDOKU_BENCH`, default `build/dev/...`) are absent, so always check the summary line for skips.
The `python run` and `sh run` blocks in this README are executed by `tests/python/test_docs.py` and `test_client_engine.py`, so the examples stay correct.

## C++ engine reference

Component-by-component reference of the C++ library. The `Mxx` tags are the development milestones in which each piece was
added; they are kept only for traceability. Headers are in `cpp/include/sudoku/`.

- `sudoku::Board` (`cpp/include/sudoku/board.hpp`): fixed 81-cell board with
  range-checked access and row/column/box conflict checks, completeness,
  consistency, and valid-solution validation. "Consistent" means no repeated
  digit in a unit; it does not imply the board is solvable.

- `sudoku::find_solution` / `count_solutions` / `classify_uniqueness`
  (`cpp/include/sudoku/solver.hpp`): MRV backtracking search with a
  solution limit and optional node budget. Results distinguish invalid
  input, exhaustive (exact) counts, and stopped searches (lower bounds);
  one found solution is never reported as unique unless the search was
  exhaustive. Exposed through `sudoku_cli` as `solve_search` / `count_solutions`.
- `sudoku::generate_complete_grid(seed, options)`
  (`cpp/include/sudoku/generator.hpp`): seeded, randomized MRV backtracking
  that produces a complete valid 9x9 grid. Same seed => same grid with this
  implementation; no uniform-sampling claim.
- `sudoku::remove_clues` / `generate_puzzle`
  (`cpp/include/sudoku/puzzle.hpp`): greedy clue removal from a complete grid
  in a seeded random order. A removal is kept only if
  `count_solutions(puzzle, 2)` is exhaustive and finds exactly one solution;
  zero, two-or-more, and budget-interrupted counts all revert it. The final
  puzzle is re-counted and compared with the source grid. `target_clues`
  (17..81) is a goal, not a guarantee: if one pass cannot reach it the result
  is `TargetNotReached` with a verified unique puzzle that has more clues.
  Same solution + seed + options => same puzzle (this implementation).
  Difficulty is evaluated separately (logical solver and difficulty sections below).
- Hardening (M06): `cpp/tests/hardening_test.cpp` cross-checks the validators,
  solver and counter against naive independent references
  (`cpp/tests/reference.hpp`) on fixed-seed pseudo-random boards, runs the
  generate -> remove clues -> count -> solve pipeline over 25 seeds, and
  checks that clue-removal results are minimal when the target is not
  reached. Failure messages include the trial number and input grid.
- Candidate model (M07), infrastructure only:
  - `sudoku/candidates.hpp`: `DigitSet` (9-bit set, digit d <-> bit d-1,
    invalid digits throw), unit helpers (`cell_row/column/box`, `unit_of`,
    `unit_cells`), and `CandidateGrid`. `CandidateGrid::from_board` returns an
    explicit `CandidateInitResult` (`InconsistentBoard` plus the conflicts for
    a board with repeated digits; no search, so a consistent but unsolvable
    board still initializes). The grid stores its own copy of the Board
    (`board()`, `matches()`), so later changes to the caller's Board are never
    silently reflected. The only mutations are `eliminate()` and `place()`, which returns
    `Eliminated`, `AlreadyAbsent`, `FilledCell` or `WouldEmptyCell` (the last
    two change nothing); candidates are never added back. `find_contradictions()`
    reports empty cells with no candidates and digits missing from a unit
    with no possible location. This is a local check and does not prove
    solvability.
  - `sudoku/deduction.hpp`: `Technique` ids (all nine techniques are implemented;
    `is_implemented()` is true for each),
    `Deduction` (an immutable proposal
    with typed targets, `Evidence` and an explanation) and `apply()`, which
    transactionally applies elimination deductions and returns an
    `AppliedDeduction` (`Applied`/`NoChange`/`Rejected`/`Unsupported`/`WouldContradict` plus
    the candidates actually removed). Original givens are not tracked by
    Board and are not modelled.

- Naked and hidden singles (M08), `sudoku/singles.hpp`:
  - `find_naked_singles`, `find_hidden_singles`, `find_singles` inspect a
    `CandidateGrid` (or a `Board`) and return proposed `Deduction`s without
    changing anything. A naked single is an empty cell with exactly one
    candidate; a hidden single is a digit not yet in a row/column/box with
    exactly one candidate cell there (zero cells is a contradiction, not a
    single). No search or guessing is used.
  - `DetectResult::status` separates `Ok` (deductions may be empty),
    `Contradiction` (local contradictions listed, no deductions) and
    `InvalidBoard` (Board overload only: repeated digits, conflicts listed).
  - Order: naked singles by cell; hidden singles by row/column/box, unit
    index, digit; `find_singles` is naked then hidden. Results are not merged:
    a placement that is both naked and hidden appears once per technique, and
    a hidden single found in two units appears twice (different evidence).
  - Evidence: naked = the cell and its one-digit candidate set; hidden = the
    unit, the single cell, and the digit.
  - Applying: `apply(grid, deduction)` re-validates the deduction against the
    grid (cell empty, digit a candidate, naked cell really has one candidate,
    hidden unit really has one location) and then calls
    `CandidateGrid::place()`, which writes the digit to the grid's Board
    snapshot and removes it from the peers' candidates, atomically. Stale or
    foreign deductions give `Rejected`/`Unsupported`; a placement that would
    leave a peer with no candidates gives `WouldContradict`. Nothing is
    changed on failure, and earlier eliminations are never undone. A detected
    list is a snapshot: re-detect after applying.
  - Not a solver by itself; see the logical solver (M13) below.
- Locked candidates (M09), `sudoku/locked.hpp`:
  - `find_pointing`, `find_claiming`, `find_locked_candidates` (grid or Board
    overload) return elimination `Deduction`s; detection is pure and uses no
    search or guessing.
  - Pointing (`Technique::Pointing`): digit d not placed in box B, and every
    cell of B that has d as a candidate lies in one row (or column); d is
    eliminated from the cells of that line outside B that have it. Claiming
    (`Technique::Claiming`): digit d not placed in a row/column L, and every
    cell of L that has d lies in one box; d is eliminated from the cells of
    that box outside L that have it. A single candidate position is aligned in
    both directions, so pointing yields a row and a column deduction. A
    deduction is only emitted if it has at least one target; filled cells
    are never targets.
  - Evidence: `unit` = source (box, or the row/column), `target_unit` = the
    line or box holding the targets, `cells` = all candidate positions
    examined, `digits` = {d}. (`Evidence::target_unit` is new in M09, and
    `Technique::LockedCandidates` was split into `Pointing`/`Claiming`.)
  - Status/ordering as for singles (`DetectResult` now lives in
    `deduction.hpp`): `Ok` (possibly empty), `Contradiction` (a digit missing
    from a unit with no location is never a locked candidate), `InvalidBoard`.
    Pointing by box, digit, row-before-column; then claiming by rows then
    columns, digit; targets ascending. Deductions are not merged: two may
    remove the same candidate; applying the second is `NoChange`.
  - Applying (`apply()`): Pointing/Claiming deductions are re-validated against
    the current grid (digit not placed in the source, its current candidate
    cells in the source still inside the target unit, targets in the target
    unit, outside the source, matching digit), else `Unsupported`. Candidate
    positions may have shrunk since detection and stay supported; targets
    already gone are skipped. Eliminating a cell's last candidate is
    `WouldContradict` (a filled target is `Rejected`); application is
    all-or-nothing and the Board snapshot is never changed by an elimination.
    Re-detect after applying; a detected list is only a snapshot.
- Naked and hidden pairs (M10), `sudoku/pairs.hpp`:
  - `find_naked_pairs`, `find_hidden_pairs`, `find_pairs` (grid or Board
    overload) return elimination `Deduction`s for every row, column and box;
    detection is pure and uses no search or guessing.
  - Naked pair (`Technique::NakedPair`): two empty cells of a unit with the
    identical two-digit candidate set {a, b}; a and b are eliminated from the
    other cells of the unit that have them. Hidden pair
    (`Technique::HiddenPair`): two digits not placed in the unit that have
    exactly the same two candidate cells; every other candidate is eliminated
    from those two cells. Pair cells/digits are never eliminated, and a
    deduction is only emitted if it has a target.
  - Ambiguous patterns are skipped, not guessed at: three cells sharing the
    same {a, b}, or three digits sharing the same two cells, produce no
    deduction (the local contradiction checks do not flag them).
  - Evidence: `unit` = source unit, `cells` = the two pair cells ascending,
    `digits` = the pair digits; targets are exact candidates currently
    present, ordered by cell then digit. Status/ordering as for the other
    techniques; units rows, columns, boxes; pairs by ascending digits; naked
    pairs before hidden pairs. The same pair seen from its row and its box
    gives two deductions (not merged); overlapping eliminations are
    idempotent.
  - `apply()` re-validates pair evidence: exactly two distinct pair cells in
    the source unit and two pair digits; a naked pair needs both cells to
    still have exactly the pair digits, a hidden pair needs each digit's
    current candidate cells in the unit to be exactly the two pair cells;
    targets must be pair digits in other cells (naked) or other digits in the
    pair cells (hidden). Otherwise `Unsupported`; eliminating a cell's last
    candidate is `WouldContradict`. All-or-nothing; re-detect after applying.
- Naked and hidden triples (M11), `sudoku/triples.hpp`:
  - `find_naked_triples`, `find_hidden_triples`, `find_triples` (grid or Board
    overload); pure detection, no search or guessing; rows, columns and boxes.
  - Naked triple (`Technique::NakedTriple`): three empty cells of a unit whose
    candidate sets each have 2-3 digits, all inside one set T of three digits,
    with union exactly T (e.g. {2,5},{5,9},{2,9}); T is eliminated from the
    unit's other cells. Hidden triple (`Technique::HiddenTriple`): three
    digits unplaced in the unit, each with 2-3 candidate cells, whose cells
    together are exactly three cells; every other candidate is removed from
    those cells. A deduction is only emitted if it has a target.
  - Skipped as impossible/ambiguous (no deduction, no guess): a fourth cell
    confined to T, a single-candidate cell inside T, a union smaller than T,
    a fourth digit confined to the three cells, or a digit with one location.
  - Evidence model (unchanged type, reused for pairs and triples):
    `Evidence{unit, cells, digits}` holds the logical *pattern* (source unit,
    the N pattern cells ascending, the N pattern digits) with N = 2 for pairs
    and N = 3 for triples; the proposed eliminations are separate
    (`Deduction::targets()`, ordered by cell then digit). The "exactly N"
    rule lives in `apply()`'s validation, not in the type.
  - `apply()` re-validates triples against the current grid: N distinct cells
    inside the unit and N digits; naked needs three empty cells with non-empty
    candidate sets inside T and union exactly T; hidden needs the digits
    unplaced with every candidate cell inside the three cells and their union
    exactly those cells; targets must be T-digits in other cells (naked) or
    non-T digits in the triple cells (hidden). Candidates may have shrunk since
    detection (for pairs the match must stay exact); a grid that no longer
    shows the pattern gives `Unsupported`. Removing a cell's last candidate is
    `WouldContradict`. Nothing is changed on any failure, and re-detecting after
    applying is required because a detected list is a snapshot.
- X-Wing (M12), `sudoku/xwing.hpp`:
  - `find_x_wings(grid | board)`: pure detection (no search or guessing) for a
    digit d in both orientations. Row-oriented: two rows where d is unplaced
    and has exactly two candidate cells, in the same two columns; d is
    eliminated from the other cells of those columns outside the two rows.
    Column-oriented is the same with rows and columns swapped. Base units with
    one, three or more positions, different position pairs, or d already
    placed never qualify; the corners need not be the only candidates in the
    cross units. Three or more base units sharing the same two positions are
    skipped as impossible/ambiguous. A deduction is emitted only with a target.
  - Evidence: `unit` = first base unit, `second_unit` = second (new field; the
    orientation is `unit->kind`), `cells` = the four corners ascending,
    `digits` = {d}; targets (separate) ascending by cell. Order: digit, row
    before column, then base unit indices. Status/ordering/overlap policy as
    for the other techniques. The new `Evidence::second_unit` is unused by every
    other technique and does not change their validation.
  - `apply()` re-validates X-Wing evidence against the current grid: two
    distinct rows or two distinct columns (never boxes), one digit unplaced in
    both, EXACTLY two current candidate cells in each, the same two cross
    positions, corners exactly matching, targets of that digit in the cross
    units and outside both base units; otherwise `Unsupported`. Filled targets
    are `Rejected`, emptying a cell is `WouldContradict`, absent targets are
    skipped, and nothing changes on failure.
  - All nine techniques exist as detectors.
- Logical solver (M13), `sudoku/logical.hpp` and `sudoku/verify.hpp`:
  - `apply()` no longer has an "apply as given" path: an elimination whose
    technique is `NakedSingle`, `HiddenSingle` or any value outside the enum is
    `Unsupported` and changes nothing (before M13 such a deduction could mutate
    the candidates); placements by any technique other than the two singles are
    `Rejected`.
  - `solve_logically(board, options)` applies one deduction per iteration from
    freshly detected candidates, using the techniques in priority order:
    naked single, hidden single, pointing, claiming, naked pair, hidden pair,
    naked triple, hidden triple, X-Wing (`LogicalOptions::techniques` may give a
    subset/other order; empty means this default; duplicates or unknown values
    are `InvalidInput`). It never guesses, backtracks or calls the search
    solver. The first deduction of the first technique that finds anything is
    applied through `apply()`; the candidate grid is kept incrementally, so
    eliminations are never undone by a rebuild.
  - Results: `Solved` (board complete and `verify_solution` passes against the
    input's clues), `Stalled` (consistent, nothing more to find; not a verdict
    on solvability), `Contradiction` (initial or later candidate contradiction,
    or a sound deduction that cannot be carried out without emptying a cell),
    `InvalidInput` (repeated digit in a unit, or bad options), `Error` (an
    internal inconsistency: verification failed, a detected deduction was
    refused, no progress, detector failure). `reason` gives the cause; the result
    also carries the final board and candidates (not a solution unless `Solved`),
    the trace of applied deductions, counts per technique, placements and
    eliminated candidates, and the failing application when there is one.
  - Termination: every applied deduction removes at least one candidate and none
    are ever added, so at most 729 deductions are applied; the loop checks the
    decrease and returns `Error` (`NoProgress`) instead of spinning.
  - `verify_solution(solution, givens)` is an independent check (plain counting
    over `Board::get`, no candidates or other solvers) reporting the first fault:
    empty cell, changed given, or a row/column/box that does not hold each digit
    exactly once.
- Difficulty metrics and profiles (M14), `sudoku/difficulty.hpp`:
  - These are this project's *operational* classification, defined only by
    which implemented logical techniques are needed. They are not a universal
    or standard measure of human difficulty, and puzzles within one profile can
    feel very different. A label means something only relative to the documented
    technique set.
  - Metrics (`compute_metrics`, a pure function of a solver trace; only
    `Applied` steps count): `deductions` (each applied deduction once),
    `placements`, `eliminated_candidates` (candidates removed by *elimination*
    deductions only, so a placement's side-effect removals are never counted
    twice), per-technique counts, `hardest_technique`/`hardest_level` (levels:
    1 singles, 2 locked candidates, 3 pairs, 4 triples, 5 X-Wing; ties go to the
    later technique). Counts describe this solver's deterministic path
    (easiest technique first); a different valid path could differ.
  - Standard profiles, each adding to the previous: Easy (singles), Medium
    (+ pointing, claiming), Hard (+ naked/hidden pairs), Expert (+ naked/hidden
    triples), Master (+ X-Wing). There are no numeric thresholds: the only
    criterion is that the solver *solves* the puzzle (independently verified)
    using just the profile's techniques.
  - `classify_difficulty(board, profiles)` runs the solver per profile in order
    and returns the easiest profile that solves it. `DifficultyStatus`:
    `Classified`, `Unsolved` (valid but every profile stalls; never labelled and
    not a claim about solvability; metrics are the last profile's partial
    progress), `Contradiction`, `InvalidInput` (repeated digit, or an invalid
    profile list: it must be a non-empty ladder of distinct non-empty names with
    valid technique lists, each a superset of the previous), `Error`.
    `evaluate_profile` tests a single profile. A complete board is Easy with zero
    deductions. No search or guessing is used.
  - Because a richer technique set behaves exactly like a poorer one until the
    poorer one would stall, the hardest level used equals the classified
    profile's level for the standard ladder; the tests check this.
- Difficulty-targeted generation (M15), `sudoku/targeted.hpp`:
  - Request: `generate_targeted(seed, TargetedOptions{target, profiles,
    max_attempts, puzzle})`. `target` is a profile *name* from the ladder
    (default ladder: the standard Easy..Master); `max_attempts` must be at least
    1 (default 100); `puzzle` is the existing `PuzzleOptions` used for clue
    removal in every attempt (defaults unchanged).
  - Exact match only. A puzzle is returned only after (1) clue removal produced
    it, (2) its uniqueness was re-established independently and *exhaustively*
    (`count_solutions` with a limit of 2; a budget-limited, inconclusive count
    is never accepted), (3) `verify_solution` confirmed the solution against the
    clues, and (4) `classify_difficulty` returned exactly the requested profile.
    A candidate classified easier or harder (even adjacent), a stalled
    candidate, and a complete grid with no clue removed are all rejected; there
    is no fallback to another difficulty.
  - Attempt: one pass of seed derivation, `generate_puzzle`, and the candidate
    check. `attempts` counts passes made (including the successful one) and never
    exceeds `max_attempts`; an invalid request makes 0. Attempt `i` uses
    `attempt_seed(seed, i)` (a documented splitmix64 mix), so
    `generate_puzzle(result.attempt_seed, options.puzzle)` replays the reported
    candidate.
  - Statuses: `Generated` (verified exact match; `puzzle`, `solution`,
    `classification` set), `Exhausted` (budget used up with no match; carries
    counts of rejections: easier, harder, unsolved, degenerate, inconclusive, and
    the last rejection reason), `InvalidRequest` (+ `fault`: empty/unknown
    target, invalid profile ladder, zero attempts, clue target outside 17..81,
    zero node budget), `Contradiction` and `Error` (a candidate that clue removal
    had just verified turned out contradictory, non-unique or invalid, or an
    exception: these indicate a defect and stop the request immediately, with
    `detail`). `Exhausted` does not mean the difficulty is impossible.
  - `check_candidate(puzzle, solution, target, profiles, budget)` exposes the
    per-candidate check (verdicts Matches, WrongDifficulty, Unsolved, NotUnique,
    Inconclusive, Degenerate, Invalid, Contradiction, Error) for validating any
    puzzle against a target.
  - Reproducibility: the whole result (status, puzzle, solution, attempts,
    attempt seed, classification, metrics, rejection counts) is a pure function
    of `(seed, options)` for this implementation; there is no hidden state, clock
    or unordered iteration. No cross-compiler guarantee is made beyond that of
    `generate_puzzle`.
  - Rarity of the targets with the existing clue removal. Sample 1 (300 puzzles at
    the default `target_clues=17`, g++ Debug): Easy 138, Medium 30, Hard 19,
    Expert 1, Master 2, Unsolved 110. Sample 2 (the M16 extended benchmark: six
    Expert and six Master requests of up to 400 attempts each, optimized build):
    Expert 3 of 6 requests succeeded (1956 attempts in total, about 650 attempts
    per success), Master 4 of 6 (1561 attempts, about 390 per success). The two
    samples are small and noisy, so treat Expert as roughly one candidate in
    300-650 and Master one in 150-400; Easy, Medium and Hard need about 2, 10 and
    16 attempts. Give Expert and Master a large `max_attempts`; at about 50 ms per
    attempt (Debug, before the later speed-up) or 9 ms (Release, before it); after the performance pass (see "Performance analysis") roughly
    17 ms (Debug) and 5 ms (Release) per attempt, which is still seconds. Raising
    `puzzle.target_clues` makes puzzles easier (at 38 clues, 299 of 300 were
    Easy), so it does not help hard targets; the default is already the best of
    the strategies measured, and the clue-removal policy was not changed.
  - The labels remain this project's operational classification, not a universal
    human-difficulty scale. Exposed through the JSON protocol (`generate_targeted`) and the Python client.
- `sudoku_cli` speaks the JSON protocol described under "JSON protocol and CLI" (one request per process) and answers `--version` and `--help`.
- `sudoku_app` is the Python engine client (see "Python engine client") plus the SVG board renderer, themes and solution export (see "SVG rendering" and "Themes and solution export").
- SVG is the only export format; PNG/raster export, a GUI and interactive SVG are not implemented. The logical solver does not fall back to search when
  it stalls.

Optional sanitizer check (not wired into the repo; scratch build dir):

```sh
cmake -S . -B /path/to/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cmake --build /path/to/asan && ctest --test-dir /path/to/asan --output-on-failure
```

## Diagnostics and benchmarks

### Targeted-generation diagnostics

`targeted_diagnostics_json(result, options[, elapsed_ns])` (`sudoku/diagnostics.hpp`)
renders what `generate_targeted()` reported, plus the request, as JSON schema
`sudoku.targeted-diagnostics/1`. It never invents data: unavailable fields are
omitted, and no timing is taken inside the engine.

| field | meaning |
|---|---|
| `schema` | `sudoku.targeted-diagnostics/1` |
| `status` | `generated`, `exhausted`, `invalid_request`, `contradiction`, `error` |
| `fault` | only for `invalid_request`: `empty_target`, `unknown_target`, `invalid_profiles`, `zero_attempts`, `invalid_clue_target`, `zero_node_budget` |
| `target`, `seed` | the request (64-bit seeds are exact integers; consumers that parse JSON numbers as doubles lose precision above 2^53) |
| `max_attempts` | configured limit (from the options, not the result) |
| `attempts` | attempts made, never above `max_attempts`; 0 for an invalid request; includes the successful or fatal attempt |
| `attempt_seed` | seed of the last attempt (the match when generated); omitted when `attempts` is 0 |
| `rejections` | `easier_than_target`, `harder_than_target`, `unsolved`, `degenerate`, `inconclusive`. Sum = `attempts - 1` when generated, `attempts` when exhausted, `attempts - 1` for `contradiction`/`error` (the fatal attempt is reported through `status`/`detail`, never as a rejection) |
| `last_rejection` | verdict of the last rejected candidate; omitted if none |
| `detail` | what failed; only for `error`/`contradiction` |
| `result` | only when `generated`: `clue_count`, `puzzle` (81 chars, `.` = empty), `solution`, and `classification` (`profile`, `profile_index`, `metrics` with `deductions`, `placements`, `eliminated_candidates`, `hardest_level`, `hardest_technique` when any, and `by_technique` for all nine techniques) |
| `elapsed_ns` | only if the caller measured it |

The classification of a non-generated request is deliberately never emitted: the
engine leaves it unassigned (or describes a defective candidate), so it is not a
measurement. `sudoku/json_writer.hpp` is a small writer (escaping, exact
integers, non-finite doubles written as `null`, misuse throws); it is not the
application JSON protocol described below.

### Benchmark tool

`sudoku_bench` (`benchmarks/`, built by default; `-DSUDOKU_BUILD_BENCHMARKS=OFF`
disables it) runs a fixed, version-controlled corpus and prints a JSON report.
It times calls to the public API from outside the engine with
`std::chrono::steady_clock`; timing cannot change any result, RNG sequence or
attempt count.

```sh
build/dev/benchmarks/sudoku_bench --tier smoke                   # 6 cheap cases, one per operation (also a CTest test)
build/dev/benchmarks/sudoku_bench --tier standard --repetitions 5 --warmup 1 --output report.json
build/dev/benchmarks/sudoku_bench --tier extended --repetitions 1 --output extended.json   # expensive; prefer an optimized build
build/dev/benchmarks/sudoku_bench --case targeted.expert.seed13 --case grid.seed1 --repetitions 20
build/dev/benchmarks/sudoku_bench --list                          # id, tier, operation
```

Options: `--tier smoke|standard|extended` (smoke is a subset of standard, which is a
subset of extended; default standard), `--case ID` (repeatable; overrides the
tier), `--repetitions N` (1..1000, default 1), `--warmup N` (unrecorded runs per
case before measuring, 0..100, default 0), `--output FILE`, `--list`, `--help`.
Exit status: 0 all outcomes as expected; 1 the run finished but an outcome was
unexpected, a case threw, or repeated runs disagreed (the report is still
written); 2 invalid command line or configuration (nothing is run). Timings are
never a pass/fail criterion.

Corpus `m16.1` (bump `kCorpusVersion` when cases, seeds, options or fixtures
change, so reports stay comparable). Operations: `complete_grid`
(`generate_complete_grid`), `puzzle_generation` (`generate_puzzle`),
`logical_solve` (`solve_logically` on fixtures), `classification`
(`classify_difficulty` on fixtures), `targeted_generation` (`generate_targeted`).
The extended tier adds Expert and Master requests (seeds 0..5, 400 attempts)
whose outcome (`generated` or `exhausted`) is not pinned. Every other case pins
the status it must produce (`expected_status`); a different status is reported
as `matches_expected: false`.

Report (`sudoku.benchmark/1`): `schema`, `corpus_version`, `environment`
(`compiler`, `build_type` as configured, `optimization_enabled` from the
compiler, `cxx_standard`, `pointer_bits`, `os`, `timer`, `timer_is_steady`; no
hardware identification is attempted), `config` (`tier`, `case_ids`,
`repetitions`, `warmup`), `records`, `summary`, `ok`.
* a **record** is one execution: `case`, `operation`, `params` (the inputs),
  `repetition` (0-based), `status`, `success`, `expected_status`,
  `matches_expected`, `elapsed_ns` (integer nanoseconds, wall clock), `counters`
  (deterministic integers), `detail` (deterministic JSON; for targeted generation
  the full diagnostics object above, without timing), and `error` only if the case
  threw (`status` is then `exception`; the run continues).
* a **summary** aggregates ONE case over its repetitions, never across cases:
  `runs`, `outcomes` (status -> count), `successes`, `unexpected`,
  `results_stable` (all repetitions produced identical status, counters and
  detail), `timing` (`samples`, `min_ns`, `median_ns`, `mean_ns`, `max_ns`,
  `distribution_reliable`), `counter_totals`, and for cases with an `attempts`
  counter `attempts_per_success` = total attempts of ALL requests (failed ones
  included) / number of successes, omitted when there are no successes.

Timing aggregates cover **all** repetitions, successful or not; they are absent
(not zero) when there are no samples; the median of an even count is the mean of
the two middle samples; `distribution_reliable` is true only with at least 5
samples, so a one-repetition run is a single observation, not a distribution.
Counters (attempts, rejection counts, search nodes, deductions, technique counts,
clue counts) are deterministic for a given corpus version and implementation;
timings are not. Success rates are always a numerator over a denominator
(`successes` of `runs`); a handful of fixed seeds is not an estimate of general
success probability. Timings depend on build type, compiler, hardware, OS and
load: compare only reports with the same corpus version, build type and
machine, and prefer the median over the mean. The `dev` preset is an
unoptimized Debug build; use `-DCMAKE_BUILD_TYPE=Release` in a separate build
directory for representative timings.

Omitted measurements: the engine records no per-phase timing inside a targeted
request (grid generation, clue removal, solving, classification inside one
attempt), because that would need instrumentation of the hot path. Per-phase
costs are available from the separate `complete_grid`, `puzzle_generation`,
`logical_solve` and `classification` cases, and time per attempt is
`elapsed_ns / attempts` (a mean over attempts of different cost).

## JSON protocol and CLI

`sudoku_cli` exposes the engine through a small, versioned JSON protocol:
**one request per process**, read from standard input (or `--input FILE`), **one
response** written to standard output (or `--output FILE`) as a single line of
JSON followed by a newline. Standard output carries nothing else; diagnostics for
operational failures go to standard error. This is the contract the Python client uses. It is separate from the diagnostics and
benchmark schemas above.

```sh
echo '{"schema":"sudoku.request/1","op":"generate_grid","id":"g1","params":{"seed":"18446744073709551615"}}' | build/dev/sudoku_cli
sudoku_cli --input request.json --output response.json
sudoku_cli --help        # plain text, exit 0
sudoku_cli --version     # "sudoku_cli 0.1.0", exit 0
```

### Request (`sudoku.request/1`)

```json
{"schema": "sudoku.request/1", "op": "<operation>", "id": "<optional>", "params": {}}
```

* `schema` (required string) must be exactly `sudoku.request/1`; any other value is `unsupported_schema`.
* `op` (required string): one of the operations below (case-sensitive).
* `id` (optional string, 1..128 bytes of UTF-8 - the same as characters for ASCII; the engine's error text still says "characters"): echoed in the response, including error responses.
* `params` (object; required fields depend on the operation).
* **Unknown fields are rejected** at the top level and inside `params` (`unknown_field`), so typos cannot be silently ignored. Duplicate keys are a `parse_error`.
* The request must be exactly one JSON document of at most 1 MiB (1048576 bytes); nothing but whitespace may follow it. Nesting deeper than 32 levels is a `parse_error`. Text must be valid UTF-8.

**Board** (`puzzle`, `solution`): a string of exactly 81 characters, row-major. Input digits are `1`-`9`; empty cells are `.` or `0`. Anything else (wrong length, other characters, non-string) is `invalid_field`. Boards in responses always use `.` for empty. A board that is well-formed but has a repeated digit is a valid request: the engine reports it (`invalid_input`) instead of the protocol rejecting it.

**Seed** (`seed`): a **decimal string** `"0"` .. `"18446744073709551615"`, no sign, no leading zeros. JSON numbers are not accepted (an IEEE-754 double cannot hold all 64 bits), and seeds are always returned as strings. Seeds are required wherever randomness is used; there is no hidden default or clock seeding.

**Integers** (`target_clues`, `node_budget`, `limit`, `max_attempts`) must be JSON integers (not `"17"`, `17.0` or `1e1`) within their range.

### Response (`sudoku.response/1`)

```json
{"schema":"sudoku.response/1","op":"…","id":"…","ok":true,"success":false,"status":"exhausted","result":{…}}
{"schema":"sudoku.response/1","op":"…","id":"…","ok":false,"error":{"code":"…","message":"…","field":"params.seed","position":12}}
```

* `ok` is true iff the request was valid and executed. Then `success` says whether the engine achieved the operation's goal, `status` is an operation-specific identifier, and `result` holds the details. A valid request can legitimately have `success: false` (for example `exhausted`, `stalled`, `unsolvable`, `budget_exhausted`, `invalid_input`).
* `ok` is false for protocol errors and internal errors: there is then an `error` object and **no** `status`/`result`/`success`. `error.code` is the stable machine-readable value; `message` is for humans; `field` (a path such as `params.seed`) and `position` (byte offset, parse errors) appear when applicable. `op` is echoed only when it is a known operation.
* **Error codes**: `empty_request`, `request_too_large`, `parse_error`, `invalid_request` (not an object), `missing_field`, `invalid_field`, `unknown_field`, `unsupported_schema`, `unknown_operation` (all protocol errors, exit 2), and `internal_error` (exit 3; the message is generic and details go to standard error only).

### Operations

| `op` | `params` | `status` values (`success` is true for the first) | `result` |
|---|---|---|---|
| `capabilities` | none | `ok` | `engine_version`, `request_schemas`, `response_schema`, `operations`, `techniques`, `profiles`, `board`, `limits` |
| `generate_grid` | `seed` | `generated` | `seed`, `grid`, `stats` (search `nodes`, `assignments`, `backtracks`) |
| `generate_puzzle` | `seed`; `target_clues` 17..81 (default 17); `node_budget` 1..2^53-1 | `target_reached`, `target_not_reached` (both succeed: a unique puzzle was produced; `target_reached` says whether the requested clue count was achieved) | `seed`, `requested_clues`, `target_reached`, `puzzle`, `solution`, `clue_count`, `removal` |
| `solve_search` | `puzzle`; `node_budget` | `solved`; `unsolvable`, `invalid_input`, `budget_exhausted` | `solution` (only when solved), `stats` |
| `count_solutions` | `puzzle`; `limit` 1..100000 (default 2); `node_budget` | `exhausted` (exact count), `limit_reached` (a lower bound) both succeed; `budget_exhausted`, `invalid_input` do not | `limit`, `count`, `exact`, `classification` (`no_solution`, `unique`, `multiple`, `unknown`, `invalid_input`), `first_solution` when found, `stats` |
| `solve_logical` | `puzzle`; `techniques` (non-empty array of technique ids, priority order; default all); `include_trace` (default false) | `solved`; `stalled`, `contradiction`, `invalid_input` | `reason`, `board` (a solution only when `solved`), `stats`, `steps` (when requested: `technique`, `action` `place`/`eliminate`, `targets` of `cell` 0..80 row-major and `digit`) |
| `verify` | `puzzle`, `solution` | `valid`; otherwise the fault: `empty_cell`, `given_changed`, `row_violation`, `column_violation`, `box_violation` | `valid`, and for a fault `fault`, `index`, `digit` (when applicable) |
| `classify` | `puzzle` | `classified`; `unsolved`, `contradiction`, `invalid_input` | `solver_status`, `solver_reason`; `profile`, `profile_index`, `metrics` only when classified |
| `generate_targeted` | `seed`, `target` (`Easy`, `Medium`, `Hard`, `Expert`, `Master`); `max_attempts` 1..1000000 (default 100); `target_clues`; `node_budget` | `generated`; `exhausted` | `target`, `seed`, `attempts`, `max_attempts`, `attempt_seed` (string; omitted if no attempt), `rejections`, `last_rejection`; when generated `puzzle`, `solution`, `clue_count`, `classification` |

Technique ids: `naked_single`, `hidden_single`, `pointing`, `claiming`, `naked_pair`, `hidden_pair`, `naked_triple`, `hidden_triple`, `x_wing`. Engine semantics are unchanged from the earlier sections: reaching a count `limit` is a lower bound, never proof of uniqueness (`limit` 1 on a unique puzzle reports `limit_reached` with classification `unknown`); a logical solver that stalls is never presented as solved and does not fall back to search; `generate_targeted` returns only an exact profile match and never a substitute, and its exhaustion says nothing about whether the difficulty is possible. Every success-shaped result is independently re-verified by the protocol layer before it is reported (grids, solutions, uniqueness of generated puzzles, exact target match); a failed re-check is an `internal_error`, never a success. A targeted generation that ends in the engine's own `contradiction`/`error` status (a defect) is also an `internal_error`.

Intentionally omitted: a combined "logical, then search" solve (the two solvers stay separate so no guessing is ever implicit), custom difficulty profiles (only the standard ladder), batch/newline-delimited input (one request per process is simpler and robust; run the process once per request), and any option to enable timing or tracing beyond `include_trace`. (Python offers batches on top of single requests; see "Batch operations and settings".)

### Exit codes

| code | meaning |
|---|---|
| 0 | valid request; the engine achieved the operation's goal (`ok` and `success`) |
| 1 | valid request; non-success engine outcome (`ok`, not `success`: exhausted, stalled, unsolvable, inconclusive, invalid board, ...). The response is still the full documented JSON |
| 2 | invalid request (any protocol error, response on stdout) or invalid command line (message on stderr, nothing on stdout) |
| 3 | internal or operational failure (`internal_error` response; for example an unreadable `--input` file or a failed write); details on stderr |

A caller distinguishes protocol errors from engine outcomes by `ok` (or exit code 2 vs 1). A valid but exhausted generation request is exit 1 with `ok: true`, not an error.

### Examples (real output)

Each pair below is replayed against the built executable by `tests/python/test_cli_protocol.py::test_readme_examples`.

Success: a complete grid for the largest seed (the id is echoed; seeds are strings).

```json request
{"schema":"sudoku.request/1","op":"generate_grid","id":"g1","params":{"seed":"18446744073709551615"}}
```

```json response exit=0
{"schema":"sudoku.response/1","op":"generate_grid","id":"g1","ok":true,"success":true,"status":"generated","result":{"seed":"18446744073709551615","grid":"326894715458176329719253846965381274241567938873942561192635487687429153534718692","stats":{"nodes":87,"assignments":86,"backtracks":86}}}
```

Exhausted targeted generation: a valid request with a non-success outcome (exit 1). There is no puzzle in the result.

```json request
{"schema":"sudoku.request/1","op":"generate_targeted","params":{"seed":"1","target":"Master","max_attempts":3}}
```

```json response exit=1
{"schema":"sudoku.response/1","op":"generate_targeted","ok":true,"success":false,"status":"exhausted","result":{"target":"Master","seed":"1","attempts":3,"max_attempts":3,"attempt_seed":"17911839290282890590","rejections":{"easier_than_target":2,"harder_than_target":0,"unsolved":1,"degenerate":0,"inconclusive":0},"last_rejection":"wrong_difficulty"}}
```

A valid puzzle that the logical solver cannot finish (exit 1). `board` is where the solver got to, not a solution.

```json request
{"schema":"sudoku.request/1","op":"solve_logical","params":{"puzzle":".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4."}}
```

```json response exit=1
{"schema":"sudoku.response/1","op":"solve_logical","ok":true,"success":false,"status":"stalled","result":{"reason":"no_applicable_deduction","board":".2.78.6.48.46237.97.6..5...2.73.6...34.2....6..5....2.178594.6.4..1..8.....8...4.","stats":{"deductions":18,"placements":12,"eliminated_candidates":16,"by_technique":{"naked_single":3,"hidden_single":9,"pointing":2,"claiming":3,"naked_pair":1,"hidden_pair":0,"naked_triple":0,"hidden_triple":0,"x_wing":0}}}}
```

An inconclusive count: one solution was found but the limit was reached, so uniqueness is not established (`exact` false, classification `unknown`).

```json request
{"schema":"sudoku.request/1","op":"count_solutions","params":{"puzzle":"53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79","limit":1}}
```

```json response exit=0
{"schema":"sudoku.response/1","op":"count_solutions","ok":true,"success":true,"status":"limit_reached","result":{"limit":1,"count":1,"exact":false,"classification":"unknown","first_solution":"534678912672195348198342567859761423426853791713924856961537284287419635345286179","stats":{"nodes":52,"assignments":51,"backtracks":51}}}
```

Protocol error: unknown operation (exit 2).

```json request
{"schema":"sudoku.request/1","op":"nope"}
```

```json response exit=2
{"schema":"sudoku.response/1","ok":false,"error":{"code":"unknown_operation","message":"unknown operation","field":"op"}}
```

Protocol error: malformed JSON (exit 2); `position` is a byte offset.

```json request
{"schema"
```

```json response exit=2
{"schema":"sudoku.response/1","ok":false,"error":{"code":"parse_error","message":"expected ':'","position":9}}
```

Protocol error: a seed must be a string (exit 2).

```json request
{"schema":"sudoku.request/1","op":"generate_grid","params":{"seed":5}}
```

```json response exit=2
{"schema":"sudoku.response/1","op":"generate_grid","ok":false,"error":{"code":"invalid_field","message":"must be a decimal string (a JSON number is not accepted)","field":"params.seed"}}
```

### Reproducibility, limits and tests

For a fixed request every algorithmic field of the response is identical on every run (no clock, no hidden state); the guarantee is that of the underlying operations (same implementation, and the cross-build/compiler caveats stated for generation). Resource protection is limited to the documented bounds: request size, nesting depth, `limit` (100000), `max_attempts` (1000000), integer ranges, and the caller's `node_budget`; there is no wall-clock limit, so callers that need one should apply a process timeout. Operations such as `count_solutions` on very open boards stay bounded by `limit`.

The reader is a small strict parser (`sudoku/json_reader.hpp`) written because no JSON library is a project dependency; it rejects duplicate keys, invalid UTF-8 and lone surrogates, and is cross-checked against Python's `json` module through the real executable (curated and mutated inputs).

Run the protocol tests with `ctest --preset dev -R protocol` (C++ handler and reader tests) and `.venv/bin/python -m pytest tests/python/test_cli_protocol.py` (executable-level tests: stdout is exactly one JSON line, exit codes, stderr hygiene, file options, size limit, full-range seeds, determinism, parser cross-check, benchmark compatibility). The Python protocol tests skip, rather than pass, if the binary is not built (see "Build and test").

## Python engine client

`sudoku_app.SudokuEngine` is a typed Python interface to the C++ engine. It starts
`sudoku_cli` once per request (an argument list, never a shell), speaks the JSON
protocol described above, enforces a timeout and returns typed results. It
implements **no** Sudoku logic: solving, counting, classification, uniqueness and
generation all come from the engine; the client only validates the traffic. It
uses the standard library only (Python 3.11+).

### Build and locate the engine

```sh
cmake --preset dev && cmake --build --preset dev        # builds build/dev/sudoku_cli
```

The executable is chosen by the first applicable rule; a rule that names an unusable
location is an error (there is no silent fallback to a later rule), and `PATH` is never
searched:

1. `SudokuEngine("/path/to/sudoku_cli")` (an argv prefix such as `[sys.executable, "wrapper.py"]` also works);
2. the `SUDOKU_CLI` environment variable;
3. `<repository>/build/dev/sudoku_cli` (`.exe` on Windows), when running from a source checkout.

Otherwise `ExecutableNotFoundError` explains what was missing and how to build it.

### Minimal use

```python run
from sudoku_app import SudokuEngine

engine = SudokuEngine(timeout=30)                  # seconds per call; default 60
caps = engine.capabilities()
print(caps.engine_version, [p.name for p in caps.profiles])

grid = engine.generate_grid(seed=42)               # same seed, same grid
assert grid.success and "." not in grid.grid

puzzle = engine.generate_puzzle(seed=7, target_clues=30)
count = engine.count_solutions(puzzle.puzzle)      # exhaustive, limit 2
assert count.exact and count.count == 1            # uniqueness is established by the engine, not assumed

result = engine.generate_targeted(seed=1, target="Master", max_attempts=3)
if not result.success:                             # exhaustion is an outcome, not an exception
    print("no Master puzzle in", result.attempts, "attempts:", result.rejections)
    assert result.puzzle is None                   # and no substitute of another difficulty is returned
```

### Operations

Boards are 81-character strings (`1`-`9`; `.` or `0` for empty on input, `.` on output).
Seeds are Python ints in `0..2**64-1`, sent as decimal strings, so all 64 bits survive.
Options are sent only when you give them, so the engine's documented defaults apply to the rest.
Every method also takes `request_id=` (echoed by the engine and verified) and `timeout=` (seconds).

| method | arguments | typed result (`success` is the engine's verdict) |
|---|---|---|
| `capabilities()` | | `Capabilities`: version, operations, techniques, profiles, board format, limits |
| `generate_grid(seed)` | | `GridResult`: `grid`, `seed`, `stats` |
| `generate_puzzle(seed, target_clues=, node_budget=)` | clues 17..81 | `PuzzleResult`: unique `puzzle`, `solution`, `clue_count`, `target_reached` |
| `solve_search(puzzle, node_budget=)` | | `SearchSolveResult`: `status` `SOLVED`, `UNSOLVABLE`, `INVALID_INPUT`, `BUDGET_EXHAUSTED`; `solution` only when solved |
| `count_solutions(puzzle, limit=, node_budget=)` | limit 1..100000 | `CountResult`: `count`, `exact`, `classification` (`UNIQUE` only with an exact count of one), `first_solution` |
| `solve_logical(puzzle, techniques=, include_trace=)` | technique ids | `LogicalResult`: `SOLVED`/`STALLED`/`CONTRADICTION`/`INVALID_INPUT`, `board`, counts, optional `steps` |
| `verify(puzzle, solution)` | | `VerifyResult`: `valid`, or the first fault (`status`, `index`, `digit`) |
| `classify(puzzle)` | | `ClassifyResult`: `profile`, `profile_index`, `metrics` only when classified |
| `generate_targeted(seed, target, max_attempts=, target_clues=, node_budget=)` | target `Easy`..`Master` | `TargetedResult`: `GENERATED` with `puzzle`/`solution`/`classification`, or `EXHAUSTED` with none of them |

`engine.request(op, params, ...)` is the raw layer (one validated response envelope). Responses are
validated strictly against `sudoku.response/1`: unknown or missing fields, wrong types, unknown statuses
and inconsistencies (for example `success` disagreeing with `status`) raise `ResponseFormatError`; nothing is
filled with a default.

`sudoku_app.generate_puzzle_report(engine, seed, ...)` / `describe_puzzle(engine, generation)` are the one piece
of orchestration: a generated puzzle together with the engine's own exhaustive solution count, solution
verification and classification (`PuzzleReport.is_unique` is true only if that count was exact and found one
solution). They compose existing operations and claim nothing the engine did not establish.

### Outcomes versus failures

**Engine outcomes are results, not exceptions.** A targeted generation that exhausts its attempts, a logical solve that
stalls, a puzzle that has no solution or an inconclusive count come back as typed results with `success == False`
(and the optional fields `None`). Call `result.require_success()` if you prefer an exception
(`OperationNotSuccessfulError`); the client never raises it on its own.

**Failures are exceptions** (all derive from `SudokuError`):

| exception | cause |
|---|---|
| `InvalidArgumentError` (also a `ValueError`) | an argument is invalid; nothing was sent |
| `RequestRejectedError` | the engine rejected the request (exit code 2): `code`, `field`, `position`, `request_id`, `stderr` |
| `EngineInternalError` | the engine reported an internal failure (exit code 3) |
| `ExecutableNotFoundError` | no usable executable could be selected |
| `EngineLaunchError` | the executable could not be started (OS error in `cause`) |
| `EngineTimeoutError` | the process exceeded the timeout and was killed |
| `EngineProcessError` | an exit code outside 0-3, or a nonzero exit without a valid response; has `exit_code`, `stdout`, `stderr` |
| `ResponseFormatError` | a normal exit with an unusable response: empty, malformed or multi-line output, schema or consistency violations, response id mismatch |

Process output in exceptions is bounded (about 2000 characters). Nothing is retried, ever: a failed call ran at most one
process, so a seeded request can never silently turn into a different one.

```python run
from sudoku_app import SudokuEngine, errors

engine = SudokuEngine()
try:
    engine.generate_targeted(1, "Legendary")       # not a profile name: the engine is the authority
except errors.RequestRejectedError as exc:
    print(exc.code, exc.field)                     # invalid_field params.target
```

### Running the Python tests

```sh
cmake --preset dev && cmake --build --preset dev       # the real-engine tests need the executable
.venv/bin/python -m pytest -q                          # everything
.venv/bin/python -m pytest tests/python/test_client_unit.py tests/python/test_client_subprocess.py -q   # no engine needed
```

`test_client_unit.py` (pure protocol and model code) and `test_client_subprocess.py` (failure paths against a
controllable fake executable, `tests/python/fake_engine.py`) never need the C++ build. `test_client_engine.py` runs
against the real executable and **fails with build instructions if it is missing** instead of silently skipping;
set `SUDOKU_ALLOW_MISSING_ENGINE=1` to skip it on purpose. The `python run` blocks in this README are executed by
that file, so the examples stay correct.

## SVG rendering

`sudoku_app.svg` turns a board into a self-contained SVG document. It is pure text generation: it never starts the
engine and does not solve, generate or classify anything, so any well-formed board renders (even an unsolvable one).

```python
from sudoku_app import render_board_svg, write_board_svg

PUZZLE = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
SOLVED = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"

svg = render_board_svg(SOLVED, original=PUZZLE)        # str
write_board_svg("board.svg", SOLVED, original=PUZZLE)  # also writes UTF-8 to a path (or a text stream)
```

* `render_board_svg(board, *, original=None, given_mask=None, cell_size=50, margin=10) -> str`
* `write_board_svg(destination, board, **same options) -> str` - `destination` is a path or a text stream with
  `write()`. Everything is validated before anything is written; file-system errors propagate as `OSError`.

**Boards** are the project's 81-character strings, row major: `1`-`9` are digits, `.` and `0` are empty cells
(both draw nothing). Anything else raises `InvalidArgumentError` (a `ValueError`); input is never repaired or modified.

**Size.** `cell_size` is 16-200 (default 50) and `margin` is 4-100 (default 10), both `int` (not `bool`) in user
units, so the default image is 470 x 470 with an explicit `width`, `height` and matching `viewBox`.

**Givens versus filled digits.** A board string cannot say which digits were clues, so by default every digit is
drawn in one neutral style (`class="digit"`) and nothing is claimed about givens. To distinguish them pass exactly one of

* `original` - the puzzle string; its digits are the givens (they must equal the board's digits), or
* `given_mask` - 81 `bool`s or 9 rows of 9 `bool`s (ints and strings are rejected; a given cell must hold a digit).

Givens are bold black (`class="given"`); other digits are normal-weight italic blue (`class="filled"`), so the
difference does not depend on colour alone, and it stays visible when the board is a completed solution.

**Structure** (fixed, in this order): background `rect`; `g.thin-lines` (12 lines, width 1); `g.box-lines` (the 4
box separators, width 2); `rect.border` (outer border, width 4, fully inside the `viewBox`); `g.digits` with one
`text` per non-empty cell in row-major order. No scripts, external references, style sheets, comments or timestamps;
identical arguments give identical text. Colours come from a theme (next section).

Run the rendering tests (they parse the output with `xml.etree.ElementTree`; no browser or network):

```sh
.venv/bin/python -m pytest tests/python/test_svg.py -q
```

## Themes and solution export

### Themes

`render_board_svg(..., theme=...)` and `write_board_svg(..., theme=...)` take `theme=None` (default), a built-in
name, or a `Theme`. A theme changes **colours only**; geometry, stroke widths, the board content and the
given/filled font styling (bold vs. italic) are identical in every theme, so changing a theme never changes puzzle values.

| name | use |
| --- | --- |
| `"default"` | the M19 appearance; the very same object as `"light"`. Calls without `theme` produce byte-identical M19 output (tested against recorded hashes). |
| `"light"` | white background, black lines/clues, blue filled digits |
| `"dark"` | near-black background, light lines and clues, light-blue filled digits |

`THEME_NAMES == ("default", "light", "dark")`; the theme objects are exported as `LIGHT_THEME`, `DARK_THEME` and `DEFAULT_THEME` (the same object as `LIGHT_THEME`). Unknown names (names are case-sensitive) raise `InvalidArgumentError`.

A custom theme is a frozen dataclass of seven `#rrggbb` hex colours (case-insensitive, stored lower case) and a
name (`a-z`, `0-9`, `-`, 1-32 characters, starting with a letter; it is never written into the SVG):

```python
from sudoku_app import Theme, render_board_svg

sepia = Theme("sepia", background="#f4ecd8", grid="#8a7a5a", box="#3b2f1a", border="#3b2f1a",
              digit="#2b2110", given="#2b2110", filled="#8b3a0e")
svg = render_board_svg("." * 81, theme=sepia)
```

Anything but `#rrggbb` (named colours, `rgb()`, `url(...)`, quotes, markup) is rejected, so a theme cannot inject
XML, scripts or references. `digit` colours digits when no given information was passed. Legibility (contrast of the
built-in themes: digits and box/border lines at least 4.5:1, ordinary lines at least 3:1 against the background) is
tested for the built-ins only; this is a contrast calculation, not a claim of formal accessibility compliance, and
custom themes are not checked.

### Exporting a solution

```python
export_solution_svg(engine, puzzle, solution, destination=None, *, theme=None, cell_size=50, margin=10, timeout=None) -> str
```

It draws `solution` with the clues of `puzzle` as givens and returns the SVG; a path or a text stream as
`destination` also receives it (a path is written as UTF-8 and replaced if it exists; with no destination nothing
is written, there is no implicit default file). Steps, in order:

1. **Clue preservation** (Python, structural only): both boards must be valid, `solution` must be complete, and each puzzle clue must equal the solution digit. Failure: `InvalidArgumentError`, the engine is not called.
2. **Solution verification** (engine, one `verify` call, never retried): the engine confirms a complete valid Sudoku that keeps the clues. A fault raises `OperationNotSuccessfulError` (`.result` has the status and cell index); a self-contradictory answer raises `ResponseFormatError`; client errors (`EngineTimeoutError`, `EngineProcessError`, ...) propagate unchanged.
3. **Write**, only after both checks passed. `OSError` from a path or stream propagates; invalid input never creates or changes the destination.

Clue preservation is not Sudoku correctness (a grid can keep every clue and still break a rule), and neither
establishes **uniqueness**: the export never claims the puzzle has one solution. Use `engine.count_solutions` (an
exact result with count 1) for that. Rendering itself (`svg.py`) never talks to the engine.

```python run
import io
from sudoku_app import SudokuEngine, export_solution_svg

engine = SudokuEngine(timeout=60)
generated = engine.generate_puzzle(seed=2024)
out = io.StringIO()                                # or a path such as "solution.svg"
svg = export_solution_svg(engine, generated.puzzle, generated.solution, out, theme="dark")
assert out.getvalue() == svg and svg.startswith("<svg ")
print(svg.count("<text "), "digits,", svg.count('class="given"'), "givens drawn bold")
```

Tests: `.venv/bin/python -m pytest tests/python/test_svg.py tests/python/test_svg_themes.py tests/python/test_export.py -q`
(`test_export.py` uses stub engines and the fake executable for failure paths, and the real engine for two end-to-end cases).

## Component contracts and reliability audit

Who is authoritative for what, and what each boundary checks. Rule: the C++ engine decides every Sudoku fact
(validity, solutions, counts, uniqueness, difficulty); Python only transports, type-checks and refuses answers that
contradict the request or the engine's own documented invariants. It never repairs, retries or substitutes.

| boundary | input checked by | output / invariants checked by | non-success and errors |
| --- | --- | --- | --- |
| request JSON -> `sudoku_cli` | engine (strict reader: duplicate keys, UTF-8/surrogates, nesting, 1 MiB, unknown/missing fields, number vs string types, 64-bit seed strings); Python pre-checks ranges for early errors | one `sudoku.response/1` line on stdout, nothing else; stderr separate | exit 0 success, 1 valid non-success, 2 protocol error, 3 internal |
| process -> `protocol.parse_process_output` | exit code in 0..3; one UTF-8 JSON line; duplicate keys, NaN and absurd nesting rejected | schema, `ok`, `op`, `id` echo (also in error responses), `success` vs exit code | `RequestRejectedError` (2), `EngineInternalError` (3), `EngineProcessError`, `ResponseFormatError` |
| typed results (`models`) | strict field/type/unknown-field checks | status vs `success`; internal consistency, e.g. a count's classification must follow the engine's own definition (`exact` only after exhaustion, `limit_reached` exactly when count == limit, a solution present exactly when count >= 1, `unique` only for an exact 1) | non-success outcomes are results with `success == False`, never exceptions |
| client methods (`SudokuEngine`) | arguments validated before any process starts (`InvalidArgumentError`) | the response must echo the request (seed, target, `max_attempts`, `limit`, `target_clues`) and every returned board must keep the submitted givens; `solve_logical` placements must equal the digits gained | no retry, no defaults after failure |
| orchestration | - | restates engine answers only; `is_unique` needs an exact count of 1 | - |
| `svg` / themes | board, masks, sizes, colours validated before any output | deterministic, self-contained SVG; colours only from `#rrggbb` | `InvalidArgumentError`, `OSError` from the destination |
| `export_solution_svg` | Python: both boards valid, solution complete, clues preserved, theme/sizes valid | engine `verify` verdict must be plainly valid (success, valid and status agree, no faulty cell); anything contradictory is refused; the export keeps its own clue check so the contract does not depend on the renderer | engine errors propagate unchanged; nothing is written unless every check passed |
| diagnostics / `sudoku_bench` | fixed corpus | `sudoku.benchmark/1`; counters satisfy `attempts == rejections + successes` | `ok` / `unexpected` fields |

Audit findings fixed in M21 (each with a regression test in `tests/python/test_boundaries.py`):

* the client accepted a response whose seed (or `target`, `max_attempts`, `limit`, `requested_clues`) differed from the request;
* solutions and boards returned by `solve_search`, `count_solutions` (`first_solution`) and `solve_logical` were not checked against the submitted givens;
* `count_solutions` accepted impossible combinations (an exact count labelled `unknown`, `limit_reached` below the limit, a solution with count 0, ...);
* `generate_puzzle` accepted `target_reached` with a clue count different from the request, and `generate_targeted` a puzzle that contradicted its solution or a wrong `clue_count`;
* absurdly deeply nested engine output raised `RecursionError` instead of a `ResponseFormatError`;
* error responses were not checked for an unexpected `id` or another operation's `op`;
* a request `id` is limited to 128 **bytes** of UTF-8 by the engine, but the client counted characters and the docs said characters;
* `export_solution_svg` accepted a `VerifyResult` whose status contradicted its flags, or a non-`VerifyResult` object from a substituted engine.

The engine/protocol side was probed with hostile input (duplicate keys, invalid UTF-8, lone surrogates, deep nesting, a 3 MB request,
seed forms, number types, unknown fields, multiple requests) and no defect was found; the protocol is unchanged. The audit covers the reviewed
paths and fixed seeds only; it does not prove correctness for all inputs.

Cross-component flows are tested in `tests/python/test_integration.py` (real engine: generation, search/logical solving, exact versus bounded
counts, targeted generation and exhaustion, export; fake executable: malformed output, duplicate keys, unknown status, id mismatch, exit codes,
timeout, missing executable, no retry).

## Performance analysis

Method: the existing `sudoku_bench` harness (corpus `m16.1`, unchanged; schema `sudoku.benchmark/1`, unchanged), Release build
(`-DCMAKE_BUILD_TYPE=Release`, g++ 13.3.0, Linux 6.17, AMD Ryzen 5 4500), `--tier standard --repetitions 5 --warmup 1`, the
baseline and the optimized binary each run three times alternately on the same machine; profiling with a gprof build. Timings on this
machine vary by up to about 25 % between identical runs of small cases, so only large, consistent differences are claimed.
For every comparison the deterministic part of every benchmark record (status, counters, detail) was identical before and after,
and a differential run of 755 CLI requests (all nine operations, seeds up to 2^64-1, bounded and unbounded searches, traces)
produced byte-identical stdout, stderr and exit codes.

**Where the time went** (targeted generation, gprof, share of run time): classifying an empty board to validate the profile ladder
(`ladder_valid`), repeated for every candidate: about 40 %; `CandidateGrid::find_contradictions` (run after every logical step): about
28 % inclusive; the backtracking search (uniqueness checks inside clue removal): about 31 %, 47 % once the first two were fixed;
the real classification of candidates: the rest. In the Python path, process start (about 1.6 ms) dominates a 1.7-2.2 ms call; JSON
handling and SVG rendering (0.12 ms) are negligible.

**What changed** (all exact: same results, same counters):

* `generate_targeted`/`check_candidate` validated the profile list by classifying an empty board (up to five full logical solves, about
  2.3 ms) for every attempt. They now call the new `is_valid_ladder`, the very validation `classify_difficulty` performs first (a test
  checks the two agree on valid and invalid ladders).
* `find_contradictions` scanned each unit once per digit (81 reads per unit and digit pass); it now makes one pass per unit and builds a
  digit mask. Output order is unchanged; a randomized test compares it with the original definition.
* The search reads row/column/box indices from a compile-time table and scans only the cells that were empty at the start. Visiting
  order, tie-breaking, node/assignment/backtrack counts and the RNG sequence are unchanged.

| workload (Release, median of 5 reps; median over 3 runs) | before | after | change |
| --- | --- | --- | --- |
| standard tier, sum of case medians | 440 ms (434-453) | 243 ms (242-256) | -42 % |
| `targeted.expert.seed13` (9 attempts) | 82.1 ms | 41.7 ms | -49 % |
| `targeted.master.seed8` (15 attempts) | 140 ms | 76.3 ms | -45 % |
| `targeted.medium.seed1` (5 attempts) | 52.7 ms | 30.0 ms | -43 % |
| `classify.master` | 4.74 ms | 2.12 ms | -55 % |
| `logical.hard` | 2.65 ms | 1.34 ms | -49 % |
| extended tier (3 reps), sum of medians | 32.3 s | 16.7 s | -48 % |
| standard tier, Debug build (3 reps) | 2.45 s | 1.02 s | -58 % |

Cases that do not run the changed code (`grid.*`, `puzzle.*`) show only noise. Reproduce with the commands in "Benchmark tool",
building the baseline from an older checkout in a separate directory.

**Left unchanged on purpose.** The search algorithm itself (counts such as `nodes` are part of the protocol and benchmark output); each
profile rung of `classify_difficulty` re-solves from scratch (resuming a stalled run would have to preserve the exact trace and metrics);
the independent re-count in `check_candidate` (required verification); JSON parsing, the CLI, the Python client and SVG rendering (not
material); the one-process-per-request design (process start is the dominant per-call cost; a persistent engine is out of scope).
The client's response-size limit is still checked after the process ends; no evidence showed it matters (responses are a few kilobytes).
Candidates for later: incremental candidate maintenance in the logical solver and rung reuse in classification.

## Batch operations and settings

Gaps this closes: every workflow used to be a hand-written loop over single calls, with no uniform way to see which items
failed, no place for defaults (profile, attempt budget, theme, output directory), and no safe way to write many SVG files.
Batches are plain Python over the existing client: **one ordinary request (one process) per item**, in input order, no retries,
no Sudoku logic, no persistent process; the one-request-per-process protocol is unchanged.

```python run
import tempfile
from sudoku_app import SudokuEngine, classify_puzzles, generate_puzzles, generate_targeted_puzzles

engine = SudokuEngine(timeout=60)
with tempfile.TemporaryDirectory() as out:                       # use a real, existing directory in practice
    batch = generate_puzzles(engine, seed=100, count=3, target_clues=32, export="solution", output_dir=out)
    assert batch.all_succeeded and [i.id for i in batch] == ["puzzle-0000", "puzzle-0001", "puzzle-0002"]
    assert batch[1].result == engine.generate_puzzle(101, target_clues=32)     # item i uses seed 100 + i
    print(batch.summary(), batch[0].output_path.name)

hard = generate_targeted_puzzles(engine, seed=1, count=2, target="Master", max_attempts=2)
for item in hard:                                                # exhaustion is an outcome, never a puzzle
    print(item.id, item.outcome, item.status)                    # unsuccessful exhausted
checked = classify_puzzles(engine, ["." * 81, "not a board"])    # a bad board fails only itself
print([i.outcome for i in checked], checked[1].error.category)   # ['unsuccessful', 'failed'] invalid_input
```

Functions (all take the engine first and return a `BatchResult`): `generate_puzzles(engine, seed, count, *, target_clues, node_budget, ...)`,
`generate_targeted_puzzles(engine, seed, count, *, target, max_attempts, target_clues, node_budget, ...)`, `classify_puzzles(engine, boards)`,
`solve_puzzles(engine, boards, *, node_budget)` (search; no uniqueness claim), `verify_solutions(engine, pairs)`. Common keywords:
`ids`, `fail_fast`, `timeout`, `settings`; the generating ones add `export` (`"puzzle"` or `"solution"`), `output_dir`, `overwrite`, `theme`,
`cell_size`, `margin`.

**Results.** `BatchResult` holds one `BatchItem` per input in input order (`index`, `id`, `outcome`, `seed`, `result`, `error`, `output_path`,
`status`); `summary()` returns a `BatchSummary` (total, succeeded, unsuccessful, failed, skipped) computed from the items, `all_succeeded`, `stopped_early`,
`successes()` and `to_dict()` (a JSON report) are provided. Outcomes:

| outcome | meaning |
| --- | --- |
| `success` | the engine reported success |
| `unsuccessful` | a valid engine outcome that is not a success (generation exhausted, unsolvable, stalled, invalid board, ...): `result` holds it, no puzzle is fabricated |
| `failed` | the item could not be carried out; `error` has `category` (`invalid_input`, `rejected`, `timeout`, `process`, `response_format`, `engine_internal`, `output`, `verification`), the exception type and message. Only `SudokuError`/`OSError` are caught; any other exception is a defect and propagates. If only the file export failed, `result` is kept |
| `skipped` | not attempted because `fail_fast` stopped the batch |

`fail_fast=True` stops after the current item when it is not a success (failed or unsuccessful); the rest are reported as `skipped`. The default
continues. An empty batch is valid: no request, no items, `all_succeeded` is vacuously true. Problems of the whole call (bad `count`, `ids`,
`output_dir`, `export`) raise `InvalidArgumentError` before any request or file; a bad board only fails its own item.

**Seeds.** Item `i` uses `item_seed(seed, i) = (seed + i) mod 2**64`, independent of batch size and execution order, so any item can be
regenerated alone (`engine.generate_puzzle(seed + i)`). Targeted batches pass the same seed per item to `generate_targeted`, which derives its
attempt seeds as before.

**Files.** `export` writes `<id>.svg` into an existing `output_dir` (never created implicitly). Ids default to `puzzle-0000`/`targeted-0000`/...;
custom ids must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}` and be unique (duplicates are rejected), so a name cannot leave the directory. An existing
file fails that item (`output`) unless `overwrite=True` (then it is replaced through a temporary file); a failing write leaves no partial file;
solution exports are verified by the engine before anything is written. An exported solution image says nothing about uniqueness.

### Settings

```python
from sudoku_app import Settings, load_settings
settings = load_settings("sudoku.json")        # or Settings(profile="Hard", max_attempts=200, theme="dark")
engine = settings.engine()
batch = generate_targeted_puzzles(engine, 1, 5, settings=settings)       # profile/attempts/theme/... from the file
```

```json settings
{"version": 1, "timeout": 120, "profile": "Hard", "max_attempts": 200, "theme": "dark", "output_dir": "puzzles"}
```

Fields (all optional except `version`): `executable`, `timeout` (seconds, default 60), `profile` (no default; targeted batches need one from the
settings or the call), `max_attempts` (1..1000000; absent means the engine's own default), `theme` (`default`, `light`, `dark`), `cell_size` (16-200,
default 50), `margin` (4-100, default 10), `output_dir`, `fail_fast` (default false). Precedence, highest first: an explicit argument of the call, the
settings object/file, then (executable only) `SUDOKU_CLI`, the standard build location, then the built-in defaults. Unknown keys, duplicate keys, wrong
types, out-of-range values, a missing or unreadable file, malformed JSON and unsupported versions raise `ConfigurationError` (an `InvalidArgumentError`)
that names the file. Relative `executable` and `output_dir` paths are relative to the file. A profile the engine does not know fails per item with
`rejected`; it is never replaced, and no budget is loosened. Settings are immutable; `settings.with_overrides(...)` returns a validated copy.

Tests: `.venv/bin/python -m pytest tests/python/test_batch.py tests/python/test_config.py -q`.

## Known limitations and release status

**Accepted limitations**
* One platform and one compiler have been verified (Ubuntu 24.04 x86_64, g++ 13.3.0). clang++ and non-Linux systems are untested. Windows/MSVC flags exist in `CMakeLists.txt` but have never been exercised.
* Warnings are enabled per target (`-Wall -Wextra -Wpedantic`, `/W4` on MSVC) and are not errors.
* C++ tests are plain executables evaluated by CTest (no framework); the JSON reader/writer are in-house, strict, and cross-checked against Python's `json`.
* Seed reproducibility is guaranteed only for one implementation build (see "Correctness contracts"); results may change in a future version.
* Expert and Master targets are rare with the current clue-removal policy and need large `max_attempts` (see the generation notes above).
* Timings are environment-specific; benchmarks make no performance guarantee.
* The Python client reads the whole process output into memory and checks its size (32 MiB) afterwards; engine responses are a few kilobytes.
* Batches run sequentially and are not resumable; settings cannot define custom themes.
* No PNG/PDF export or GUI; the Python command line offers generate, solve, verify and classify only (batches and settings files are library features).

**Release status.** Not released, tagged or published.
* **Licence:** MIT, copyright (c) 2026 Chovaneček Štěpán Adam (see `LICENSE`). The repository contains no third-party code; all components, including the JSON reader and writer, are original to the project.
* `CLAUDE.md` (the developer instructions) still says that no JSON library or C++ test framework is chosen yet; the README above describes the actual state (in-house JSON, plain CTest executables).
* No continuous-integration configuration, packaging metadata beyond `pyproject.toml`, or version tag exists. `pyproject.toml` and the engine both report version 0.1.0.
