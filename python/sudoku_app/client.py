"""The typed Python client of the C++ engine.

``SudokuEngine`` sends one request per engine process and returns typed results.
It implements no Sudoku logic: every answer (solutions, uniqueness, difficulty,
generated puzzles) comes from the C++ engine, and the client only validates the
traffic. Errors are described in :mod:`sudoku_app.errors`: protocol and process
failures are exceptions; legitimate non-success outcomes (an exhausted targeted
generation, a stalled logical solve, an inconclusive count) are returned as typed
results with ``success == False``.

Options are passed exactly as given and only when given, so the engine's
documented defaults apply to omitted ones. Seeds are Python ints in 0..2**64-1
and are sent as decimal strings (never through floating point). Every method
accepts an optional ``request_id`` (echoed and verified) and a per-call
``timeout`` in seconds.
"""
from __future__ import annotations

import os
from typing import Any, Iterable, Mapping, Sequence

from . import models, protocol
from .errors import InvalidArgumentError, ResponseFormatError
from .runner import DEFAULT_TIMEOUT_SECONDS, EngineRunner


def _opt_int(params: dict[str, Any], name: str, value: Any, lo: int, hi: int) -> None:
    if value is not None:
        params[name] = protocol.check_int(value, name, lo, hi)


def _echo(actual: Any, expected: Any, what: str, op: str) -> None:
    """The response must repeat what the request said; a different value means the answer is for another request."""
    if actual != expected:
        raise ResponseFormatError(f"{op}: the response reports {what} {actual!r} but the request said {expected!r}")


def _keeps_givens(puzzle: str, board: str | None, what: str, op: str) -> None:
    """A returned board must keep every digit of the submitted puzzle ('0' and '.' both mean empty)."""
    if board is None:
        return
    for i, (p, b) in enumerate(zip(puzzle, board)):
        if p not in ".0" and p != b:
            raise ResponseFormatError(f"{op}: {what} has {b!r} at index {i} but the puzzle given there is {p!r}")


class SudokuEngine:
    """Client for one engine executable.

    ``executable`` is a path (or argv prefix); see :mod:`sudoku_app.runner` for the
    discovery rules when it is omitted. ``timeout`` (seconds) bounds every call.
    """

    def __init__(self, executable: str | os.PathLike[str] | Sequence[str] | None = None, *,
                 timeout: float = DEFAULT_TIMEOUT_SECONDS) -> None:
        self._runner = EngineRunner(executable, timeout=timeout)

    @property
    def argv(self) -> tuple[str, ...]:
        """The command that is started for every request."""
        return self._runner.argv

    # ---------------------------------------------------------- raw layer
    def request(self, op: str, params: Mapping[str, Any] | None = None, *, request_id: str | None = None,
                timeout: float | None = None) -> protocol.Envelope:
        """Sends one request and returns the validated response envelope (any ``ok``
        response, including non-success outcomes). Protocol and process failures raise.
        Params are sent as given; use the typed methods for validated arguments."""
        message = protocol.build_request(op, params, request_id)
        outcome = self._runner.run(protocol.serialize_request(message), timeout=timeout)
        return protocol.parse_process_output(exit_code=outcome.exit_code, stdout=outcome.stdout,
                                             stderr=outcome.stderr, op=op, request_id=request_id)

    # ------------------------------------------------------ typed operations
    def capabilities(self, *, request_id: str | None = None, timeout: float | None = None) -> models.Capabilities:
        """What the engine supports (operations, profiles, techniques, limits)."""
        return models.Capabilities.from_envelope(self.request("capabilities", None, request_id=request_id, timeout=timeout))

    def generate_grid(self, seed: int, *, request_id: str | None = None, timeout: float | None = None) -> models.GridResult:
        """A complete valid grid; the same seed always gives the same grid."""
        params = {"seed": protocol.encode_seed(seed)}
        result = models.GridResult.from_envelope(self.request("generate_grid", params, request_id=request_id, timeout=timeout))
        _echo(result.seed, seed, "seed", "generate_grid")
        return result

    def generate_puzzle(self, seed: int, *, target_clues: int | None = None, node_budget: int | None = None,
                        request_id: str | None = None, timeout: float | None = None) -> models.PuzzleResult:
        """A puzzle with exactly one solution, made by uniqueness-preserving clue removal.
        ``target_clues`` (17..81) is a goal: check ``target_reached`` on the result."""
        params: dict[str, Any] = {"seed": protocol.encode_seed(seed)}
        _opt_int(params, "target_clues", target_clues, protocol.MIN_CLUES, protocol.MAX_CLUES)
        _opt_int(params, "node_budget", node_budget, 1, protocol.MAX_NODE_BUDGET)
        result = models.PuzzleResult.from_envelope(self.request("generate_puzzle", params, request_id=request_id, timeout=timeout))
        _echo(result.seed, seed, "seed", "generate_puzzle")
        if target_clues is not None:
            _echo(result.requested_clues, target_clues, "requested_clues", "generate_puzzle")
        return result

    def solve_search(self, puzzle: str, *, node_budget: int | None = None, request_id: str | None = None,
                     timeout: float | None = None) -> models.SearchSolveResult:
        """Finds one solution by backtracking search. Says nothing about uniqueness."""
        params: dict[str, Any] = {"puzzle": protocol.check_board(puzzle, "puzzle")}
        _opt_int(params, "node_budget", node_budget, 1, protocol.MAX_NODE_BUDGET)
        result = models.SearchSolveResult.from_envelope(self.request("solve_search", params, request_id=request_id, timeout=timeout))
        _keeps_givens(puzzle, result.solution, "solution", "solve_search")
        return result

    def count_solutions(self, puzzle: str, *, limit: int | None = None, node_budget: int | None = None,
                        request_id: str | None = None, timeout: float | None = None) -> models.CountResult:
        """Counts solutions up to ``limit`` (engine default 2). Reaching the limit is a lower
        bound (``exact`` False); only an exact count of 1 establishes uniqueness."""
        params: dict[str, Any] = {"puzzle": protocol.check_board(puzzle, "puzzle")}
        _opt_int(params, "limit", limit, 1, protocol.MAX_COUNT_LIMIT)
        _opt_int(params, "node_budget", node_budget, 1, protocol.MAX_NODE_BUDGET)
        result = models.CountResult.from_envelope(self.request("count_solutions", params, request_id=request_id, timeout=timeout))
        if limit is not None:
            _echo(result.limit, limit, "limit", "count_solutions")
        _keeps_givens(puzzle, result.first_solution, "first_solution", "count_solutions")
        return result

    def solve_logical(self, puzzle: str, *, techniques: Iterable[str] | None = None, include_trace: bool | None = None,
                      request_id: str | None = None, timeout: float | None = None) -> models.LogicalResult:
        """Solves with human-style deductions only (no guessing, no search fallback)."""
        params: dict[str, Any] = {"puzzle": protocol.check_board(puzzle, "puzzle")}
        if techniques is not None:
            if isinstance(techniques, str):
                raise InvalidArgumentError("techniques must be an iterable of technique ids, not a single string")
            listed = list(techniques)
            if not listed or not all(isinstance(t, str) and t for t in listed):
                raise InvalidArgumentError("techniques must be a non-empty collection of technique id strings")
            params["techniques"] = listed
        if include_trace is not None:
            if not isinstance(include_trace, bool):
                raise InvalidArgumentError("include_trace must be a bool")
            params["include_trace"] = include_trace
        result = models.LogicalResult.from_envelope(self.request("solve_logical", params, request_id=request_id, timeout=timeout))
        _keeps_givens(puzzle, result.board, "board", "solve_logical")
        if result.board is not None:   # every placement fills exactly one empty cell (no search, no overwriting)
            gained = sum(c != "." for c in result.board) - sum(c not in ".0" for c in puzzle)
            if gained != result.placements:
                raise ResponseFormatError(f"solve_logical: the board gained {gained} digits but {result.placements} placements are reported")
        return result

    def verify(self, puzzle: str, solution: str, *, request_id: str | None = None,
               timeout: float | None = None) -> models.VerifyResult:
        """Independently checks that ``solution`` is a complete valid solution that keeps the givens of ``puzzle``."""
        params = {"puzzle": protocol.check_board(puzzle, "puzzle"), "solution": protocol.check_board(solution, "solution")}
        return models.VerifyResult.from_envelope(self.request("verify", params, request_id=request_id, timeout=timeout))

    def classify(self, puzzle: str, *, request_id: str | None = None, timeout: float | None = None) -> models.ClassifyResult:
        """Difficulty profile of a puzzle (Easy..Master). Unsolved, contradictory or invalid puzzles get no label."""
        params = {"puzzle": protocol.check_board(puzzle, "puzzle")}
        return models.ClassifyResult.from_envelope(self.request("classify", params, request_id=request_id, timeout=timeout))

    def generate_targeted(self, seed: int, target: str, *, max_attempts: int | None = None, target_clues: int | None = None,
                          node_budget: int | None = None, request_id: str | None = None,
                          timeout: float | None = None) -> models.TargetedResult:
        """A puzzle of exactly the ``target`` profile, within ``max_attempts`` attempts (engine default 100).

        If the bounded search finds no exact match the result has ``status == EXHAUSTED`` and
        ``success == False`` (no puzzle, no substitute); this is a normal outcome, not an exception.
        Exhaustion does not mean the difficulty is impossible. Unknown profile names are rejected by
        the engine (RequestRejectedError); see ``capabilities().profiles``."""
        if not isinstance(target, str) or not target:
            raise InvalidArgumentError("target must be a non-empty profile name")
        params: dict[str, Any] = {"seed": protocol.encode_seed(seed), "target": target}
        _opt_int(params, "max_attempts", max_attempts, 1, protocol.MAX_ATTEMPTS)
        _opt_int(params, "target_clues", target_clues, protocol.MIN_CLUES, protocol.MAX_CLUES)
        _opt_int(params, "node_budget", node_budget, 1, protocol.MAX_NODE_BUDGET)
        result = models.TargetedResult.from_envelope(self.request("generate_targeted", params, request_id=request_id, timeout=timeout))
        _echo(result.seed, seed, "seed", "generate_targeted")
        _echo(result.target, target, "target", "generate_targeted")
        if max_attempts is not None:
            _echo(result.max_attempts, max_attempts, "max_attempts", "generate_targeted")
        return result
