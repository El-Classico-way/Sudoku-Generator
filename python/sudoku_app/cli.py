"""Command-line interface: ``python -m sudoku_app`` (normally started through ``./run.sh``).

A thin presentation layer over the existing client and export APIs. It contains no Sudoku logic: every
fact (solution, uniqueness, difficulty, validity) comes from the C++ engine through :class:`SudokuEngine`.

Exit codes
----------
0  success
1  the operation ran but the answer is negative: generation exhausted, no solution, logical stall, invalid solution, ...
2  invalid command line or invalid input (a malformed board, a bad option value)
3  the engine could not be used: executable missing, timeout, crash, unusable response, rejected request
4  an output file could not be written

Expected application errors are reported as one ``error:`` line on standard error; unexpected exceptions
(programming defects) are not caught and show a traceback.
"""
from __future__ import annotations

import argparse
import secrets
import sys
from pathlib import Path
from typing import Sequence

from . import errors
from .batch import _write_svg
from .client import SudokuEngine
from .export import export_solution_svg
from .models import LogicalStatus, SearchStatus, SolutionClass, VerifyStatus
from .protocol import MAX_ATTEMPTS, MAX_CLUES, MAX_SEED, MIN_CLUES, check_board
from .svg import THEME_NAMES, render_board_svg

EXIT_OK, EXIT_NEGATIVE, EXIT_USAGE, EXIT_ENGINE, EXIT_OUTPUT = 0, 1, 2, 3, 4
PROFILES = ("Easy", "Medium", "Hard", "Expert", "Master")
DEFAULT_TIMEOUT = 300.0

EXAMPLES = """\
examples:
  ./run.sh generate                                   a new puzzle (random seed, printed)
  ./run.sh generate --difficulty easy --seed 7        an Easy puzzle, reproducible
  ./run.sh generate --difficulty hard --max-attempts 500 --output puzzle.svg
  ./run.sh solve --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"
  ./run.sh solve --method logical --board "..."       deductions only (may stall)
  ./run.sh verify --puzzle "..." --solution "..."
  ./run.sh classify --board "..."

boards are 81 characters, row by row: digits 1-9 and . (or 0) for an empty cell.
exit codes: 0 success, 1 negative answer (exhausted, no solution, stall, invalid solution),
2 invalid input or usage, 3 engine problem, 4 output file problem.
"""


# ------------------------------------------------------------------ output helpers

def format_board(board: str) -> str:
    """A conventional 9x9 picture; empty cells are dots."""
    cells = board.replace("0", ".")
    lines = []
    for r in range(9):
        row = cells[r * 9:(r + 1) * 9]
        lines.append(" | ".join(" ".join(row[c:c + 3]) for c in (0, 3, 6)))
        if r in (2, 5):
            lines.append("------+-------+------")
    return "\n".join(lines)


def fail(message: str, code: int) -> int:
    print(f"error: {message}", file=sys.stderr)
    return code


def _cell_name(index: int | None) -> str:
    return "an unknown cell" if index is None else f"row {index // 9 + 1}, column {index % 9 + 1}"


# --------------------------------------------------------------------- argument types

def _board(text: str) -> str:
    try:
        return check_board(text.strip(), "board")
    except errors.InvalidArgumentError as exc:
        raise argparse.ArgumentTypeError(f"{exc}. A board is 81 characters: digits 1-9 and . (or 0) for empty cells") from None


def _int_in(low: int, high: int, name: str):
    def parse(text: str) -> int:
        try:
            value = int(text)
        except ValueError:
            raise argparse.ArgumentTypeError(f"{name} must be a whole number, got {text!r}") from None
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"{name} must be between {low} and {high}, got {value}")
        return value
    return parse


def _difficulty(text: str) -> str:
    for name in PROFILES:
        if text.lower() == name.lower():
            return name
    raise argparse.ArgumentTypeError(f"unknown difficulty {text!r}; choose from {', '.join(p.lower() for p in PROFILES)}")


def _timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"timeout must be a number of seconds, got {text!r}") from None
    if not 0 < value < float("inf"):
        raise argparse.ArgumentTypeError("timeout must be a positive number of seconds")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="run.sh", description="Sudoku Studio: generate, solve, verify and classify Sudoku puzzles with the C++ engine.",
        epilog=EXAMPLES, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", metavar="PATH", help="engine executable (default: $SUDOKU_CLI, then build/dev/sudoku_cli)")
    parser.add_argument("--timeout", type=_timeout, default=DEFAULT_TIMEOUT, metavar="SECONDS",
                        help=f"time limit for each engine call (default {DEFAULT_TIMEOUT:g})")
    sub = parser.add_subparsers(dest="command", metavar="COMMAND", required=True)

    gen = sub.add_parser("generate", help="generate a puzzle", formatter_class=argparse.RawDescriptionHelpFormatter,
                         description="Generate a puzzle with exactly one solution. Without --difficulty the engine removes clues "
                                     "and the puzzle is classified afterwards; with --difficulty it searches for a puzzle of exactly "
                                     "that level (at most --max-attempts attempts) and fails if none is found.",
                         epilog="examples:\n  ./run.sh generate --difficulty medium --seed 7 --output puzzle.svg\n")
    gen.add_argument("--difficulty", type=_difficulty, metavar="LEVEL", help=f"one of {', '.join(p.lower() for p in PROFILES)}")
    gen.add_argument("--seed", type=_int_in(0, MAX_SEED, "seed"),
                     help="same seed, same puzzle (for this build); default: a random seed, which is printed")
    gen.add_argument("--target-clues", type=_int_in(MIN_CLUES, MAX_CLUES, "target-clues"), metavar="N",
                     help=f"clue count to aim for ({MIN_CLUES}-{MAX_CLUES}); a goal, not a promise")
    gen.add_argument("--max-attempts", type=_int_in(1, MAX_ATTEMPTS, "max-attempts"), metavar="N",
                     help="attempt limit with --difficulty (engine default 100; Expert and Master usually need many more)")
    gen.add_argument("--output", metavar="FILE", help="write the puzzle as an SVG image (relative paths: current directory)")
    gen.add_argument("--solution-output", metavar="FILE", help="write the engine-verified solution as an SVG image")
    gen.add_argument("--theme", choices=THEME_NAMES, default="default", help="SVG theme (default: default)")
    gen.add_argument("--overwrite", action="store_true", help="replace existing output files (default: refuse)")
    gen.add_argument("--show-solution", action="store_true", help="also print the solution (hidden by default)")

    solve = sub.add_parser("solve", help="solve a board", formatter_class=argparse.RawDescriptionHelpFormatter,
                           epilog='example:\n  ./run.sh solve --board "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"\n')
    solve.add_argument("--board", type=_board, required=True, help="81 characters, digits 1-9 and . or 0 for empty")
    solve.add_argument("--method", choices=("search", "logical"), default="search",
                       help="search: backtracking, finds a solution if one exists; logical: human-style deductions only, never guesses and may stall (default: search)")
    solve.add_argument("--unique", action="store_true", help="with the search method, also count solutions to check uniqueness")

    verify = sub.add_parser("verify", help="check a solution against its puzzle", formatter_class=argparse.RawDescriptionHelpFormatter,
                            epilog='example:\n  ./run.sh verify --puzzle "53..7..." --solution "534678912..."\n')
    verify.add_argument("--puzzle", "--board", dest="puzzle", type=_board, required=True, help="the puzzle (81 characters)")
    verify.add_argument("--solution", type=_board, required=True, help="the proposed complete solution (81 characters)")

    cls = sub.add_parser("classify", help="difficulty of a board by logical techniques", formatter_class=argparse.RawDescriptionHelpFormatter,
                         epilog='example:\n  ./run.sh classify --board "53..7..."\n')
    cls.add_argument("--board", type=_board, required=True, help="81 characters")
    return parser


# ---------------------------------------------------------------------------- commands

def _claim_uniqueness(engine: SudokuEngine, puzzle: str) -> str | int:
    """'yes' only after an exact engine count of one; an inconsistency is reported as an engine problem."""
    count = engine.count_solutions(puzzle)
    if count.exact and count.count == 1 and count.classification is SolutionClass.UNIQUE:
        return "yes (exhaustive search found exactly one solution)"
    return fail(f"the engine returned a puzzle that is not provably unique (count {count.count}, exact={count.exact})", EXIT_ENGINE)


def _check_output_path(path: str | None, overwrite: bool) -> Path | None:
    if path is None:
        return None
    target = Path(path)
    if not target.parent.is_dir():
        raise errors.InvalidArgumentError(f"the directory for output file {path!r} does not exist")
    if target.is_dir():
        raise errors.InvalidArgumentError(f"output path {path!r} is a directory")
    if target.exists() and not overwrite:
        raise errors.InvalidArgumentError(f"output file {path!r} already exists (use --overwrite to replace it)")
    return target


def cmd_generate(engine: SudokuEngine, args: argparse.Namespace) -> int:
    puzzle_file = _check_output_path(args.output, args.overwrite)
    solution_file = _check_output_path(args.solution_output, args.overwrite)
    if puzzle_file is not None and puzzle_file == solution_file:
        raise errors.InvalidArgumentError("--output and --solution-output name the same file")
    if args.max_attempts is not None and args.difficulty is None:
        raise errors.InvalidArgumentError("--max-attempts only applies together with --difficulty")
    seed_given = args.seed is not None
    seed = args.seed if seed_given else secrets.randbits(63)

    if args.difficulty is None:
        generated = engine.generate_puzzle(seed, target_clues=args.target_clues)
        puzzle, solution, clues = generated.puzzle, generated.solution, generated.clue_count
        note = "" if generated.target_reached else f" (the target of {generated.requested_clues} was not reached; the puzzle is still unique)"
        classified = engine.classify(puzzle)
        level = classified.profile
    else:
        result = engine.generate_targeted(seed, args.difficulty, max_attempts=args.max_attempts, target_clues=args.target_clues)
        if not result.success:
            return fail(f"no {args.difficulty} puzzle found in {result.attempts} attempt(s) with seed {seed}. "
                        "That does not mean none exists: try a larger --max-attempts or another --seed", EXIT_NEGATIVE)
        puzzle, solution, clues, note = result.puzzle, result.solution, result.clue_count, ""
        level = result.classification.profile

    unique = _claim_uniqueness(engine, puzzle)
    if isinstance(unique, int):
        return unique
    puzzle_svg = solution_svg = None
    if puzzle_file is not None:
        puzzle_svg = render_board_svg(puzzle, original=puzzle, theme=args.theme)
    if solution_file is not None:
        solution_svg = export_solution_svg(engine, puzzle, solution, None, theme=args.theme)   # verified by the engine first

    print(format_board(puzzle))
    print()
    print(f"seed:       {seed}" + ("" if seed_given else "  (random; use --seed to reproduce)"))
    if level is None:
        print("difficulty: unclassified (the implemented logical techniques cannot solve it alone; not a claim about solvability)")
    else:
        print(f"difficulty: {level}" + (" (requested and verified)" if args.difficulty else " (classified by the engine)"))
    print(f"clues:      {clues}{note}")
    print(f"unique:     {unique}")
    if args.show_solution:
        print("\nsolution:")
        print(format_board(solution))
    else:
        print("solution:   hidden (add --show-solution to print it)")
    try:
        for file, text, label in ((puzzle_file, puzzle_svg, "puzzle"), (solution_file, solution_svg, "solution")):
            if file is not None:
                _write_svg(file, text, args.overwrite)
                print(f"wrote {label} image: {file}")
    except OSError as exc:
        return fail(f"could not write {exc.filename or 'the output file'}: {exc.strerror or exc}", EXIT_OUTPUT)
    return EXIT_OK


def cmd_solve(engine: SudokuEngine, args: argparse.Namespace) -> int:
    board = args.board
    if args.method == "search":
        result = engine.solve_search(board)
        if result.status is SearchStatus.SOLVED:
            print(format_board(result.solution))
            print("\nsolved (search)")
            if args.unique:
                count = engine.count_solutions(board)
                if count.exact and count.count == 1:
                    print("unique: yes (exhaustive search found exactly one solution)")
                elif count.classification is SolutionClass.MULTIPLE:
                    print("unique: no (at least two solutions exist; only one is shown)")
                else:
                    print("unique: not established (the count stopped early)")
            else:
                print("note: one solution was found; use --unique to check that it is the only one")
            return EXIT_OK
        if result.status is SearchStatus.INVALID_INPUT:
            return fail("invalid board: a digit repeats in a row, column or box", EXIT_USAGE)
        if result.status is SearchStatus.UNSOLVABLE:
            return fail("this board has no solution (the search was exhaustive)", EXIT_NEGATIVE)
        return fail(f"the search stopped early ({result.status.value}); no answer", EXIT_NEGATIVE)

    result = engine.solve_logical(board)
    if result.status is LogicalStatus.SOLVED:
        print(format_board(result.board))
        print(f"\nsolved (logical deductions only: {result.deductions} steps, no guessing)")
        return EXIT_OK
    if result.status is LogicalStatus.INVALID_INPUT:
        return fail("invalid board: a digit repeats in a row, column or box", EXIT_USAGE)
    if result.status is LogicalStatus.CONTRADICTION:
        return fail("the deductions led to a contradiction: this board cannot be completed", EXIT_NEGATIVE)
    if result.board is not None:
        print(format_board(result.board))
        print()
    print("stalled: the implemented logical techniques cannot go further from here. "
          "This does NOT mean the puzzle is unsolvable; try --method search.", file=sys.stderr)
    return EXIT_NEGATIVE


def cmd_verify(engine: SudokuEngine, args: argparse.Namespace) -> int:
    result = engine.verify(args.puzzle, args.solution)
    if result.status is VerifyStatus.VALID:
        print("valid: the solution is complete, follows the Sudoku rules and keeps every given of the puzzle")
        return EXIT_OK
    n = None if result.index is None else result.index + 1
    reasons = {
        VerifyStatus.EMPTY_CELL: f"incomplete: {_cell_name(result.index)} is empty",
        VerifyStatus.GIVEN_CHANGED: f"a given of the puzzle was changed at {_cell_name(result.index)}",
        VerifyStatus.ROW_VIOLATION: f"row {n} does not contain each digit exactly once",
        VerifyStatus.COLUMN_VIOLATION: f"column {n} does not contain each digit exactly once",
        VerifyStatus.BOX_VIOLATION: f"box {n} (numbered row by row) does not contain each digit exactly once",
    }
    print(f"invalid: {reasons[result.status]}")
    return EXIT_NEGATIVE


def cmd_classify(engine: SudokuEngine, args: argparse.Namespace) -> int:
    result = engine.classify(args.board)
    if result.profile is not None:
        m = result.metrics
        print(f"difficulty: {result.profile}  (by the implemented logical techniques; not a universal rating)")
        print(f"steps: {m.deductions}   placements: {m.placements}   eliminations: {m.eliminated_candidates}")
        if m.hardest_technique:
            print(f"hardest technique used: {m.hardest_technique}")
        return EXIT_OK
    if result.status.value == "invalid_input":
        return fail("invalid board: a digit repeats in a row, column or box", EXIT_USAGE)
    if result.status.value == "contradiction":
        return fail("the deductions led to a contradiction: this board cannot be completed", EXIT_NEGATIVE)
    return fail("not classified: the logical techniques stall on this board (that does not mean it is unsolvable)", EXIT_NEGATIVE)


COMMANDS = {"generate": cmd_generate, "solve": cmd_solve, "verify": cmd_verify, "classify": cmd_classify}


def main(argv: Sequence[str] | None = None) -> int:
    args_list = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    if not args_list:
        parser.print_help()
        return EXIT_OK
    args = parser.parse_args(args_list)          # usage errors exit with status 2 and a one-line message
    try:
        engine = SudokuEngine(args.engine, timeout=args.timeout)
        return COMMANDS[args.command](engine, args)
    except errors.InvalidArgumentError as exc:
        return fail(str(exc), EXIT_USAGE)
    except errors.ExecutableNotFoundError as exc:
        return fail(f"{exc}\nBuild it with ./run.sh (or: cmake --preset dev && cmake --build --preset dev --target sudoku_cli).", EXIT_ENGINE)
    except errors.EngineTimeoutError as exc:
        return fail(f"the engine did not answer within {exc.timeout:g} seconds (raise --timeout)", EXIT_ENGINE)
    except errors.OperationNotSuccessfulError as exc:
        return fail(str(exc), EXIT_NEGATIVE)
    except errors.SudokuError as exc:            # launch failures, crashes, rejected requests, unusable responses
        return fail(f"engine problem: {exc}", EXIT_ENGINE)
