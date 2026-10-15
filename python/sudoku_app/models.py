"""Typed results of the engine operations.

Every operation returns a frozen dataclass. ``success`` is the engine's own
verdict (``ok`` responses only; protocol and internal errors are exceptions, see
:mod:`sudoku_app.errors`). A non-success outcome such as an exhausted targeted
generation is a normal result with ``success == False`` and the optional fields
(for example ``puzzle``) left as ``None``; call :meth:`require_success` if you
prefer an exception.

Responses are validated strictly against protocol ``sudoku.response/1``: missing
or unexpected fields, wrong types, unknown statuses and inconsistencies (for
example ``success`` disagreeing with ``status``) raise
:class:`~sudoku_app.errors.ResponseFormatError`. A result field is never filled
with a default that the engine did not report.

Boards are 81-character ``str`` values, row-major, ``'.'`` for empty cells.
Seeds are Python ``int`` values in 0..2**64-1.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Any, Mapping

from .errors import OperationNotSuccessfulError, ResponseFormatError
from .protocol import Envelope, check_response_board, decode_seed

__all__ = [
    "GridStatus", "PuzzleStatus", "SearchStatus", "CountStatus", "SolutionClass", "LogicalStatus", "VerifyStatus",
    "ClassifyStatus", "TargetedStatus", "SearchStats", "Metrics", "Capabilities", "ProfileInfo", "GridResult",
    "PuzzleResult", "SearchSolveResult", "CountResult", "LogicalStep", "LogicalResult", "VerifyResult",
    "ClassifyResult", "TargetedResult", "Rejections", "Classification",
]


class GridStatus(StrEnum):
    GENERATED = "generated"


class PuzzleStatus(StrEnum):
    TARGET_REACHED = "target_reached"
    TARGET_NOT_REACHED = "target_not_reached"   # still a verified-unique puzzle, with more clues than requested


class SearchStatus(StrEnum):
    SOLVED = "solved"
    UNSOLVABLE = "unsolvable"
    INVALID_INPUT = "invalid_input"
    BUDGET_EXHAUSTED = "budget_exhausted"


class CountStatus(StrEnum):
    EXHAUSTED = "exhausted"          # exact count
    LIMIT_REACHED = "limit_reached"  # lower bound
    BUDGET_EXHAUSTED = "budget_exhausted"
    INVALID_INPUT = "invalid_input"


class SolutionClass(StrEnum):
    NO_SOLUTION = "no_solution"
    UNIQUE = "unique"
    MULTIPLE = "multiple"
    UNKNOWN = "unknown"
    INVALID_INPUT = "invalid_input"


class LogicalStatus(StrEnum):
    SOLVED = "solved"
    STALLED = "stalled"
    CONTRADICTION = "contradiction"
    INVALID_INPUT = "invalid_input"


class VerifyStatus(StrEnum):
    VALID = "valid"
    EMPTY_CELL = "empty_cell"
    GIVEN_CHANGED = "given_changed"
    ROW_VIOLATION = "row_violation"
    COLUMN_VIOLATION = "column_violation"
    BOX_VIOLATION = "box_violation"


class ClassifyStatus(StrEnum):
    CLASSIFIED = "classified"
    UNSOLVED = "unsolved"
    CONTRADICTION = "contradiction"
    INVALID_INPUT = "invalid_input"


class TargetedStatus(StrEnum):
    GENERATED = "generated"
    EXHAUSTED = "exhausted"   # a valid outcome: the bounded search found no exact match


# ----------------------------------------------------------- strict reading

class _Obj:
    """Reads one JSON object strictly: typed getters, then `done()` rejects leftovers."""

    def __init__(self, data: Any, where: str) -> None:
        if not isinstance(data, dict):
            raise ResponseFormatError(f"{where} must be an object")
        self._data, self._where, self._seen = data, where, set()

    def _fail(self, key: str, why: str) -> ResponseFormatError:
        return ResponseFormatError(f"{self._where}.{key} {why}")

    def _get(self, key: str, optional: bool) -> Any:
        self._seen.add(key)
        if key not in self._data:
            if optional:
                return None
            raise self._fail(key, "is missing")
        return self._data[key]

    def str(self, key: str, *, optional: bool = False) -> str | None:
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        if not isinstance(v, str):
            raise self._fail(key, "must be a string")
        return v

    def int(self, key: str, *, optional: bool = False, minimum: int | None = 0) -> int | None:
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        if isinstance(v, bool) or not isinstance(v, int):
            raise self._fail(key, "must be an integer")
        if minimum is not None and v < minimum:
            raise self._fail(key, f"must be >= {minimum}")
        return v

    def bool(self, key: str) -> bool:
        v = self._get(key, False)
        if not isinstance(v, bool):
            raise self._fail(key, "must be a boolean")
        return v

    def obj(self, key: str, *, optional: bool = False) -> "_Obj | None":
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        return _Obj(v, f"{self._where}.{key}")

    def list(self, key: str, *, optional: bool = False) -> list[Any] | None:
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        if not isinstance(v, list):
            raise self._fail(key, "must be an array")
        return v

    def seed(self, key: str, *, optional: bool = False) -> int | None:
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        return decode_seed(v, f"{self._where}.{key}")

    def board(self, key: str, *, complete: bool, optional: bool = False) -> str | None:
        v = self._get(key, optional)
        if v is None and optional and key not in self._data:
            return None
        return check_response_board(v, f"{self._where}.{key}", complete=complete)

    def enum(self, key: str, enum_type: type[StrEnum], *, optional: bool = False) -> Any:
        v = self.str(key, optional=optional)
        if v is None:
            return None
        try:
            return enum_type(v)
        except ValueError:
            raise self._fail(key, f"has unknown value {v!r}") from None

    def has(self, key: str) -> bool:
        return key in self._data

    def done(self) -> None:
        extra = sorted(set(self._data) - self._seen)
        if extra:
            raise ResponseFormatError(f"{self._where} has unexpected field(s): {', '.join(map(str, extra))}")


def _status(env: Envelope, enum_type: type[StrEnum], success_values: set[StrEnum]) -> Any:
    try:
        status = enum_type(env.status)
    except ValueError:
        raise ResponseFormatError(f"{env.op}: unknown status {env.status!r}") from None
    if env.success != (status in success_values):
        raise ResponseFormatError(f"{env.op}: 'success' ({env.success}) disagrees with status {env.status!r}")
    return status


class _Outcome:
    """Mixin: explicit opt-in to exceptions for non-success outcomes."""

    success: bool

    def require_success(self):
        """Returns self if the engine reported success, else raises OperationNotSuccessfulError."""
        if not self.success:
            raise OperationNotSuccessfulError(self)
        return self


# ------------------------------------------------------------ shared pieces

@dataclass(frozen=True)
class SearchStats:
    """Search work counters of the engine's backtracking search."""
    nodes: int
    assignments: int
    backtracks: int

    @staticmethod
    def read(obj: _Obj | None) -> "SearchStats":
        assert obj is not None
        s = SearchStats(obj.int("nodes"), obj.int("assignments"), obj.int("backtracks"))
        obj.done()
        return s


@dataclass(frozen=True)
class Metrics:
    """Difficulty metrics of a logical solving trace (engine definitions)."""
    deductions: int
    placements: int
    eliminated_candidates: int
    hardest_level: int
    hardest_technique: str | None
    by_technique: Mapping[str, int]

    @staticmethod
    def read(obj: _Obj | None) -> "Metrics":
        assert obj is not None
        by = obj.obj("by_technique")
        counts = {}
        for key in by._data:
            counts[key] = by.int(key)
        by.done()
        m = Metrics(obj.int("deductions"), obj.int("placements"), obj.int("eliminated_candidates"),
                    obj.int("hardest_level"), obj.str("hardest_technique", optional=True), counts)
        obj.done()
        return m


# ------------------------------------------------------------- operations

@dataclass(frozen=True)
class ProfileInfo:
    name: str
    techniques: tuple[str, ...]


@dataclass(frozen=True)
class Capabilities:
    engine_version: str
    request_schemas: tuple[str, ...]
    response_schema: str
    operations: tuple[str, ...]
    techniques: tuple[str, ...]
    profiles: tuple[ProfileInfo, ...]
    board_length: int
    board_input_characters: str
    board_output_empty: str
    limits: Mapping[str, int]

    @staticmethod
    def from_envelope(env: Envelope) -> "Capabilities":
        if env.status != "ok" or not env.success:
            raise ResponseFormatError("capabilities: expected status 'ok' with success true")
        r = _Obj(env.result, "result")

        def strings(key: str) -> tuple[str, ...]:
            items = r.list(key)
            if not all(isinstance(i, str) for i in items):
                raise ResponseFormatError(f"result.{key} must contain strings")
            return tuple(items)

        profiles = []
        for i, p in enumerate(r.list("profiles")):
            po = _Obj(p, f"result.profiles[{i}]")
            techniques = po.list("techniques")
            if not all(isinstance(t, str) for t in techniques):
                raise ResponseFormatError("result.profiles techniques must be strings")
            profiles.append(ProfileInfo(po.str("name"), tuple(techniques)))
            po.done()
        board = r.obj("board")
        limits_obj = r.obj("limits")
        limits = {k: limits_obj.int(k) for k in list(limits_obj._data)}
        limits_obj.done()
        caps = Capabilities(
            r.str("engine_version"), strings("request_schemas"), r.str("response_schema"), strings("operations"),
            strings("techniques"), tuple(profiles), board.int("length"), board.str("input_characters"),
            board.str("output_empty"), limits)
        board.done()
        r.done()
        return caps


@dataclass(frozen=True)
class GridResult(_Outcome):
    status: GridStatus
    success: bool
    seed: int
    grid: str
    stats: SearchStats

    @staticmethod
    def from_envelope(env: Envelope) -> "GridResult":
        status = _status(env, GridStatus, {GridStatus.GENERATED})
        r = _Obj(env.result, "result")
        out = GridResult(status, env.success, r.seed("seed"), r.board("grid", complete=True), SearchStats.read(r.obj("stats")))
        r.done()
        return out


@dataclass(frozen=True)
class PuzzleResult(_Outcome):
    """A generated puzzle. The engine only reports it after establishing a unique solution."""
    status: PuzzleStatus
    success: bool
    seed: int
    requested_clues: int
    target_reached: bool
    puzzle: str
    solution: str
    clue_count: int
    cells_considered: int
    removed: int
    rejected_not_unique: int
    rejected_inconclusive: int

    @staticmethod
    def from_envelope(env: Envelope) -> "PuzzleResult":
        status = _status(env, PuzzleStatus, set(PuzzleStatus))
        r = _Obj(env.result, "result")
        seed, requested, reached = r.seed("seed"), r.int("requested_clues"), r.bool("target_reached")
        puzzle, solution = r.board("puzzle", complete=False), r.board("solution", complete=True)
        clues = r.int("clue_count")
        rm = r.obj("removal")
        removal = (rm.int("cells_considered"), rm.int("removed"), rm.int("rejected_not_unique"), rm.int("rejected_inconclusive"))
        rm.done()
        r.done()
        if reached != (status is PuzzleStatus.TARGET_REACHED):
            raise ResponseFormatError("generate_puzzle: target_reached disagrees with status")
        if (clues == requested) != reached or clues < requested:
            raise ResponseFormatError("generate_puzzle: 'target reached' means exactly the requested clue count; "
                                      "otherwise the puzzle keeps more clues than requested")
        if clues != sum(c != "." for c in puzzle):
            raise ResponseFormatError("generate_puzzle: clue_count disagrees with the puzzle")
        if any(p != "." and p != s for p, s in zip(puzzle, solution)):
            raise ResponseFormatError("generate_puzzle: the puzzle contradicts its solution")
        return PuzzleResult(status, env.success, seed, requested, reached, puzzle, solution, clues, *removal)


@dataclass(frozen=True)
class SearchSolveResult(_Outcome):
    status: SearchStatus
    success: bool
    solution: str | None
    stats: SearchStats

    @staticmethod
    def from_envelope(env: Envelope) -> "SearchSolveResult":
        status = _status(env, SearchStatus, {SearchStatus.SOLVED})
        r = _Obj(env.result, "result")
        solution = r.board("solution", complete=True, optional=True)
        stats = SearchStats.read(r.obj("stats"))
        r.done()
        if (solution is not None) != (status is SearchStatus.SOLVED):
            raise ResponseFormatError("solve_search: a solution must be present exactly when status is 'solved'")
        return SearchSolveResult(status, env.success, solution, stats)


@dataclass(frozen=True)
class CountResult(_Outcome):
    """A solution count. ``exact`` is True only after an exhaustive search; a count
    that stopped at the limit is a lower bound, and ``classification`` is
    ``UNIQUE`` only when uniqueness is established."""
    status: CountStatus
    success: bool
    limit: int
    count: int
    exact: bool
    classification: SolutionClass
    first_solution: str | None
    stats: SearchStats

    @staticmethod
    def from_envelope(env: Envelope) -> "CountResult":
        status = _status(env, CountStatus, {CountStatus.EXHAUSTED, CountStatus.LIMIT_REACHED})
        r = _Obj(env.result, "result")
        limit, count, exact = r.int("limit", minimum=1), r.int("count"), r.bool("exact")
        classification = r.enum("classification", SolutionClass)
        first = r.board("first_solution", complete=True, optional=True)
        stats = SearchStats.read(r.obj("stats"))
        r.done()
        if exact != (status is CountStatus.EXHAUSTED):
            raise ResponseFormatError("count_solutions: 'exact' disagrees with status")
        if count > limit:
            raise ResponseFormatError("count_solutions: count exceeds the limit")
        if classification is SolutionClass.UNIQUE and not (exact and count == 1):
            raise ResponseFormatError("count_solutions: 'unique' without an exact count of one")
        # The engine's own definition (classify in solver.cpp): invalid input; two or more solutions are 'multiple'
        # whatever stopped the search; an exhausted search with 0 or 1 solutions is exact; anything else is unknown.
        if status is CountStatus.INVALID_INPUT:
            expected = SolutionClass.INVALID_INPUT
        elif count >= 2:
            expected = SolutionClass.MULTIPLE
        elif exact:
            expected = SolutionClass.NO_SOLUTION if count == 0 else SolutionClass.UNIQUE
        else:
            expected = SolutionClass.UNKNOWN
        if classification is not expected:
            raise ResponseFormatError(f"count_solutions: classification {classification.value!r} is impossible for status "
                                      f"{status.value!r} with count {count} (expected {expected.value!r})")
        if (count == limit) != (status is CountStatus.LIMIT_REACHED):
            raise ResponseFormatError("count_solutions: 'limit_reached' exactly when the count equals the limit")
        if (first is not None) != (count >= 1):
            raise ResponseFormatError("count_solutions: first_solution must be present exactly when the count is at least 1")
        return CountResult(status, env.success, limit, count, exact, classification, first, stats)


@dataclass(frozen=True)
class LogicalStep:
    technique: str
    action: str                       # "place" or "eliminate"
    targets: tuple[tuple[int, int], ...]   # (cell 0..80 row-major, digit)


@dataclass(frozen=True)
class LogicalResult(_Outcome):
    """Result of the no-guessing logical solver. ``board`` is a solution only when
    ``status`` is ``SOLVED``; otherwise it is how far the solver got."""
    status: LogicalStatus
    success: bool
    reason: str
    board: str | None
    deductions: int
    placements: int
    eliminated_candidates: int
    by_technique: Mapping[str, int]
    steps: tuple[LogicalStep, ...] | None   # only when include_trace was requested

    @staticmethod
    def from_envelope(env: Envelope) -> "LogicalResult":
        status = _status(env, LogicalStatus, {LogicalStatus.SOLVED})
        r = _Obj(env.result, "result")
        reason = r.str("reason")
        board = r.board("board", complete=status is LogicalStatus.SOLVED, optional=True)
        st = r.obj("stats")
        by = st.obj("by_technique")
        counts = {k: by.int(k) for k in list(by._data)}
        by.done()
        deductions, placements, eliminated = st.int("deductions"), st.int("placements"), st.int("eliminated_candidates")
        st.done()
        raw_steps = r.list("steps", optional=True)
        steps = None
        if raw_steps is not None:
            parsed = []
            for i, item in enumerate(raw_steps):
                so = _Obj(item, f"result.steps[{i}]")
                technique, action = so.str("technique"), so.str("action")
                if action not in ("place", "eliminate"):
                    raise ResponseFormatError(f"result.steps[{i}].action has unknown value {action!r}")
                targets = []
                for j, t in enumerate(so.list("targets")):
                    to = _Obj(t, f"result.steps[{i}].targets[{j}]")
                    cell, digit = to.int("cell"), to.int("digit")
                    to.done()
                    if cell > 80 or not 1 <= digit <= 9:
                        raise ResponseFormatError(f"result.steps[{i}].targets[{j}] is out of range")
                    targets.append((cell, digit))
                so.done()
                parsed.append(LogicalStep(technique, action, tuple(targets)))
            steps = tuple(parsed)
        r.done()
        if status is LogicalStatus.SOLVED and board is None:
            raise ResponseFormatError("solve_logical: 'solved' without a board")
        if steps is not None and len(steps) != deductions:
            raise ResponseFormatError("solve_logical: trace length disagrees with the deduction count")
        if sum(counts.values()) != deductions:
            raise ResponseFormatError("solve_logical: technique counts disagree with the deduction count")
        return LogicalResult(status, env.success, reason, board, deductions, placements, eliminated, counts, steps)


@dataclass(frozen=True)
class VerifyResult(_Outcome):
    status: VerifyStatus
    success: bool
    valid: bool
    index: int | None     # offending cell / unit index for a fault
    digit: int | None

    @staticmethod
    def from_envelope(env: Envelope) -> "VerifyResult":
        status = _status(env, VerifyStatus, {VerifyStatus.VALID})
        r = _Obj(env.result, "result")
        valid = r.bool("valid")
        fault = r.enum("fault", VerifyStatus, optional=True)
        index, digit = r.int("index", optional=True), r.int("digit", optional=True)
        r.done()
        if valid != (status is VerifyStatus.VALID):
            raise ResponseFormatError("verify: 'valid' disagrees with status")
        if valid and (fault is not None or index is not None or digit is not None):
            raise ResponseFormatError("verify: a valid result must not report a fault")
        if not valid and (fault is not status or index is None):
            raise ResponseFormatError("verify: a fault result must report the matching fault and index")
        return VerifyResult(status, env.success, valid, index, digit)


@dataclass(frozen=True)
class ClassifyResult(_Outcome):
    """Difficulty classification. ``profile`` and ``metrics`` exist only when the
    puzzle was classified; an unsolved, contradictory or invalid puzzle is never labelled."""
    status: ClassifyStatus
    success: bool
    solver_status: str
    solver_reason: str
    profile: str | None
    profile_index: int | None
    metrics: Metrics | None

    @staticmethod
    def from_envelope(env: Envelope) -> "ClassifyResult":
        status = _status(env, ClassifyStatus, {ClassifyStatus.CLASSIFIED})
        r = _Obj(env.result, "result")
        solver_status, solver_reason = r.str("solver_status"), r.str("solver_reason")
        profile = r.str("profile", optional=True)
        index = r.int("profile_index", optional=True)
        metrics_obj = r.obj("metrics", optional=True)
        metrics = Metrics.read(metrics_obj) if metrics_obj is not None else None
        r.done()
        classified = status is ClassifyStatus.CLASSIFIED
        if classified != (profile is not None and index is not None and metrics is not None):
            raise ResponseFormatError("classify: profile, profile_index and metrics must be present exactly when classified")
        return ClassifyResult(status, env.success, solver_status, solver_reason, profile, index, metrics)


@dataclass(frozen=True)
class Rejections:
    """Candidates the targeted search rejected, by reason."""
    easier_than_target: int
    harder_than_target: int
    unsolved: int
    degenerate: int
    inconclusive: int

    def total(self) -> int:
        return (self.easier_than_target + self.harder_than_target + self.unsolved + self.degenerate + self.inconclusive)


@dataclass(frozen=True)
class Classification:
    profile: str
    profile_index: int
    metrics: Metrics


@dataclass(frozen=True)
class TargetedResult(_Outcome):
    """Difficulty-targeted generation. When ``status`` is ``EXHAUSTED`` the bounded
    search found no exact match: ``puzzle``/``solution``/``classification`` are
    ``None`` and nothing of another difficulty is substituted. Exhaustion does not
    mean the difficulty is impossible."""
    status: TargetedStatus
    success: bool
    target: str
    seed: int
    attempts: int
    max_attempts: int
    attempt_seed: int | None
    rejections: Rejections
    last_rejection: str | None
    puzzle: str | None
    solution: str | None
    clue_count: int | None
    classification: Classification | None

    @staticmethod
    def from_envelope(env: Envelope) -> "TargetedResult":
        status = _status(env, TargetedStatus, {TargetedStatus.GENERATED})
        r = _Obj(env.result, "result")
        target, seed = r.str("target"), r.seed("seed")
        attempts, max_attempts = r.int("attempts"), r.int("max_attempts", minimum=1)
        attempt_seed = r.seed("attempt_seed", optional=True)
        rj = r.obj("rejections")
        rejections = Rejections(rj.int("easier_than_target"), rj.int("harder_than_target"), rj.int("unsolved"),
                                rj.int("degenerate"), rj.int("inconclusive"))
        rj.done()
        last = r.str("last_rejection", optional=True)
        puzzle, solution = r.board("puzzle", complete=False, optional=True), r.board("solution", complete=True, optional=True)
        clues = r.int("clue_count", optional=True)
        co = r.obj("classification", optional=True)
        classification = None
        if co is not None:
            classification = Classification(co.str("profile"), co.int("profile_index"), Metrics.read(co.obj("metrics")))
            co.done()
        r.done()
        generated = status is TargetedStatus.GENERATED
        present = [puzzle is not None, solution is not None, clues is not None, classification is not None]
        if generated != all(present) or (not generated and any(present)):
            raise ResponseFormatError("generate_targeted: puzzle, solution, clue_count and classification must be present exactly when generated")
        if generated:
            if clues != sum(c != "." for c in puzzle):
                raise ResponseFormatError("generate_targeted: clue_count disagrees with the puzzle")
            if any(p != "." and p != s for p, s in zip(puzzle, solution)):
                raise ResponseFormatError("generate_targeted: the puzzle contradicts its solution")
        if attempts > max_attempts:
            raise ResponseFormatError("generate_targeted: attempts exceed max_attempts")
        if (attempts > 0) != (attempt_seed is not None):
            raise ResponseFormatError("generate_targeted: attempt_seed must be present exactly when attempts were made")
        if generated and classification.profile != target:
            raise ResponseFormatError("generate_targeted: the classification does not match the requested target")
        if rejections.total() + (1 if generated else 0) != attempts:
            raise ResponseFormatError("generate_targeted: rejection counts disagree with the attempt count")
        return TargetedResult(status, env.success, target, seed, attempts, max_attempts, attempt_seed, rejections,
                              last, puzzle, solution, clues, classification)
