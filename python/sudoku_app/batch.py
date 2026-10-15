"""Batch workflows over the existing client (M23).

Every function runs ONE ordinary engine request per item (one process each, exactly as the single-item
methods do), in input order, and returns a :class:`BatchResult` with one :class:`BatchItem` per input.
Nothing is retried, no Sudoku logic lives here, and no item is ever dropped.

Item outcomes
-------------
``success``       the engine reported success (a verified-unique puzzle, a solved board, a classified puzzle, ...)
``unsuccessful``  a valid engine outcome that is not a success (generation exhausted, search unsolvable, a
                  stalled/contradictory/invalid board, ...): ``result`` holds it, ``error`` is ``None``
``failed``        the item could not be carried out: ``error`` says why (see :class:`ItemError`); ``result`` is ``None``,
                  except when the engine part succeeded and only the file export failed (``result`` is kept, the
                  error category is ``output`` or ``verification``)
``skipped``       not attempted because ``fail_fast`` stopped the batch

Only :class:`SudokuError` (and, for file output, :class:`OSError`) are turned into ``failed`` items; any other
exception is a defect and propagates. Argument problems of the whole call (bad count, ids, directory) raise
:class:`InvalidArgumentError` before any engine request or file is touched.

``fail_fast=True`` stops after the current item when it is not a success (``failed`` or ``unsuccessful``); the
remaining items are reported as ``skipped``. The default continues with every item.

Seeds: item ``i`` of a generating batch uses ``(seed + i) mod 2**64`` (``item_seed``), so any item can be
reproduced alone with the single-item method, independent of batch size and order of execution.

An empty batch is valid: no request is made, the result has no items, and ``all_succeeded`` is vacuously true
(check ``len(result)`` if you need at least one item).

Exported SVG files (``export=``) go to an existing ``output_dir`` as ``<id>.svg``. Ids must match
``[A-Za-z0-9][A-Za-z0-9._-]{0,63}`` and be unique within the batch, so a name can never leave the directory. An existing
file makes that item fail (category ``output``) unless ``overwrite=True``; a failed write leaves no partial file; invalid
data never creates a file. An exported image of a solution says nothing about uniqueness.
"""
from __future__ import annotations

import os
import re
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

from .client import SudokuEngine
from .config import Settings
from .errors import (
    EngineInternalError, EngineLaunchError, EngineProcessError, EngineTimeoutError, ExecutableNotFoundError,
    InvalidArgumentError, OperationNotSuccessfulError, RequestRejectedError, ResponseFormatError, SudokuError,
)
from .export import export_solution_svg
from .protocol import MAX_SEED, check_board, check_int
from .svg import render_board_svg

__all__ = ["BatchItem", "BatchResult", "BatchSummary", "ItemError", "item_seed", "generate_puzzles",
           "generate_targeted_puzzles", "classify_puzzles", "solve_puzzles", "verify_solutions"]

_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
MAX_BATCH_SIZE = 100000


def item_seed(seed: int, index: int) -> int:
    """The seed of item ``index`` of a generating batch: ``(seed + index) mod 2**64``."""
    check_int(seed, "seed", 0, MAX_SEED)
    check_int(index, "index", 0, MAX_BATCH_SIZE)
    return (seed + index) % (MAX_SEED + 1)


# ------------------------------------------------------------------ models

@dataclass(frozen=True)
class ItemError:
    """Why an item failed. ``category``: ``invalid_input`` (rejected in Python, nothing was sent), ``rejected`` (the
    engine refused the request), ``timeout``, ``process`` (executable missing, cannot start, unexpected exit),
    ``response_format`` (unusable or inconsistent response), ``engine_internal``, ``output`` (file system),
    ``verification`` (export refused: the engine did not confirm the solution)."""
    category: str
    type: str
    message: str


@dataclass(frozen=True)
class BatchItem:
    index: int
    id: str
    outcome: str                         # success | unsuccessful | failed | skipped
    seed: int | None = None
    result: Any = None                   # the typed result (success / unsuccessful)
    error: ItemError | None = None
    output_path: Path | None = None      # exported SVG, only for exported successes

    @property
    def status(self) -> str | None:
        return getattr(getattr(self.result, "status", None), "value", None)


@dataclass(frozen=True)
class BatchSummary:
    total: int
    succeeded: int
    unsuccessful: int
    failed: int
    skipped: int


@dataclass(frozen=True)
class BatchResult:
    """Items in input order. The summary is computed from them, so it cannot disagree."""
    items: tuple[BatchItem, ...] = field(default_factory=tuple)

    def __len__(self) -> int:
        return len(self.items)

    def __iter__(self):
        return iter(self.items)

    def __getitem__(self, index: int) -> BatchItem:
        return self.items[index]

    def summary(self) -> BatchSummary:
        count = lambda name: sum(i.outcome == name for i in self.items)
        return BatchSummary(len(self.items), count("success"), count("unsuccessful"), count("failed"), count("skipped"))

    @property
    def all_succeeded(self) -> bool:
        """True iff every item succeeded (vacuously true for an empty batch)."""
        return all(i.outcome == "success" for i in self.items)

    @property
    def stopped_early(self) -> bool:
        return any(i.outcome == "skipped" for i in self.items)

    def successes(self) -> tuple[BatchItem, ...]:
        return tuple(i for i in self.items if i.outcome == "success")

    def to_dict(self) -> dict[str, Any]:
        """A JSON-serializable report (seeds as decimal strings, like the protocol)."""
        s = self.summary()
        return {
            "summary": {"total": s.total, "succeeded": s.succeeded, "unsuccessful": s.unsuccessful, "failed": s.failed,
                        "skipped": s.skipped, "all_succeeded": self.all_succeeded},
            "items": [{"index": i.index, "id": i.id, "outcome": i.outcome, "status": i.status,
                       "seed": None if i.seed is None else str(i.seed),
                       "error": None if i.error is None else {"category": i.error.category, "type": i.error.type,
                                                              "message": i.error.message},
                       "output_path": None if i.output_path is None else str(i.output_path)} for i in self.items],
        }


# ---------------------------------------------------------------- helpers

def _categorize(exc: Exception) -> ItemError:
    if isinstance(exc, InvalidArgumentError):
        category = "invalid_input"
    elif isinstance(exc, RequestRejectedError):
        category = "rejected"
    elif isinstance(exc, EngineTimeoutError):
        category = "timeout"
    elif isinstance(exc, (EngineProcessError, EngineLaunchError, ExecutableNotFoundError)):
        category = "process"
    elif isinstance(exc, ResponseFormatError):
        category = "response_format"
    elif isinstance(exc, EngineInternalError):
        category = "engine_internal"
    elif isinstance(exc, OSError):
        category = "output"
    elif isinstance(exc, OperationNotSuccessfulError):
        category = "verification"   # the export's engine verification did not confirm the solution
    else:
        category = "other"
    return ItemError(category, type(exc).__name__, str(exc))


def _ids(count: int, ids: Sequence[str] | None, prefix: str) -> list[str]:
    if ids is None:
        return [f"{prefix}-{i:04d}" for i in range(count)]
    if isinstance(ids, (str, bytes)) or len(ids) != count:
        raise InvalidArgumentError(f"ids must be a sequence with one id per item ({count})")
    out = list(ids)
    for value in out:
        if not isinstance(value, str) or not _ID.fullmatch(value):
            raise InvalidArgumentError(f"invalid item id {value!r}: use 1-64 characters from A-Z a-z 0-9 . _ - starting with a letter or digit")
    if len(set(out)) != len(out):
        dup = next(v for v in out if out.count(v) > 1)
        raise InvalidArgumentError(f"duplicate item id {dup!r}: ids must be unique within a batch")
    return out


def _output_dir(output_dir: Any, export: Any) -> Path | None:
    if export not in (None, "puzzle", "solution"):
        raise InvalidArgumentError("export must be None, 'puzzle' or 'solution'")
    if export is None:
        return None
    if output_dir is None:
        raise InvalidArgumentError("export needs an output_dir (argument or settings); there is no default location")
    path = Path(output_dir)
    if not path.is_dir():
        raise InvalidArgumentError(f"output_dir does not exist or is not a directory: {path}")
    return path


def _write_svg(path: Path, text: str, overwrite: bool) -> None:
    """Exclusive create (or temp file + atomic replace when overwriting); a failed write leaves no partial file."""
    data = text.encode("utf-8")
    if overwrite:
        fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(data)
            os.replace(tmp, path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)   # FileExistsError if it exists
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
    except BaseException:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise


def _run(count: int, ids: list[str], seeds: Sequence[int | None], fail_fast: bool,
         work: Callable[[int], tuple[Any, Callable[[], Path | None] | None]]) -> BatchResult:
    """``work(i)`` performs the engine request and returns ``(result, export)``; ``export`` (or ``None``) writes the file."""
    items: list[BatchItem] = []
    stop = False
    for i in range(count):
        if stop:
            items.append(BatchItem(i, ids[i], "skipped", seeds[i]))
            continue
        try:
            result, export = work(i)
        except (SudokuError, OSError) as exc:
            items.append(BatchItem(i, ids[i], "failed", seeds[i], None, _categorize(exc)))
        else:
            try:
                path = export() if export is not None and result.success else None
            except (SudokuError, OSError) as exc:     # the engine part is kept; only the file failed
                items.append(BatchItem(i, ids[i], "failed", seeds[i], result, _categorize(exc)))
            else:
                items.append(BatchItem(i, ids[i], "success" if result.success else "unsuccessful", seeds[i], result, None, path))
        stop = fail_fast and items[-1].outcome != "success"
    return BatchResult(tuple(items))


def _common(settings: Settings | None, fail_fast, output_dir, theme, cell_size, margin):
    st = settings or Settings()
    return (st.fail_fast if fail_fast is None else fail_fast, st.output_dir if output_dir is None else output_dir,
            st.theme if theme is None else theme, st.cell_size if cell_size is None else cell_size,
            st.margin if margin is None else margin)


def _exporter(engine: SudokuEngine, kind: str | None, directory: Path | None, item_id: str, puzzle: str | None,
              solution: str | None, theme: str, cell_size: int, margin: int, overwrite: bool, timeout: float | None):
    if kind is None or puzzle is None:
        return None

    def export() -> Path:
        target = directory / f"{item_id}.svg"          # item_id is validated: no separators, no leading dot
        if kind == "puzzle":
            text = render_board_svg(puzzle, original=puzzle, theme=theme, cell_size=cell_size, margin=margin)
        else:                                          # engine-verified; nothing is written before it succeeded
            text = export_solution_svg(engine, puzzle, solution, None, theme=theme, cell_size=cell_size, margin=margin,
                                       timeout=timeout)
        _write_svg(target, text, overwrite)
        return target
    return export


def _count(count: Any) -> int:
    return check_int(count, "count", 0, MAX_BATCH_SIZE)


# ------------------------------------------------------------- generation

def generate_puzzles(engine: SudokuEngine, seed: int, count: int, *, target_clues: int | None = None,
                     node_budget: int | None = None, ids: Sequence[str] | None = None, fail_fast: bool | None = None,
                     export: str | None = None, output_dir: str | os.PathLike[str] | None = None, overwrite: bool = False,
                     theme: str | None = None, cell_size: int | None = None, margin: int | None = None,
                     timeout: float | None = None, settings: Settings | None = None) -> BatchResult:
    """``count`` puzzles with seeds ``item_seed(seed, i)``; each is a ``generate_puzzle`` request.
    ``export`` is ``None``, ``"puzzle"`` (the clues) or ``"solution"`` (the engine-verified solution, one extra
    ``verify`` request per item)."""
    count = _count(count)
    check_int(seed, "seed", 0, MAX_SEED)
    fail_fast, output_dir, theme, cell_size, margin = _common(settings, fail_fast, output_dir, theme, cell_size, margin)
    directory = _output_dir(output_dir, export)
    names = _ids(count, ids, "puzzle")
    seeds = [item_seed(seed, i) for i in range(count)]

    def work(i: int):
        r = engine.generate_puzzle(seeds[i], target_clues=target_clues, node_budget=node_budget, timeout=timeout)
        return r, _exporter(engine, export, directory, names[i], r.puzzle, r.solution, theme, cell_size, margin, overwrite, timeout)
    return _run(count, names, seeds, fail_fast, work)


def generate_targeted_puzzles(engine: SudokuEngine, seed: int, count: int, *, target: str | None = None,
                              max_attempts: int | None = None, target_clues: int | None = None,
                              node_budget: int | None = None, ids: Sequence[str] | None = None,
                              fail_fast: bool | None = None, export: str | None = None,
                              output_dir: str | os.PathLike[str] | None = None, overwrite: bool = False,
                              theme: str | None = None, cell_size: int | None = None, margin: int | None = None,
                              timeout: float | None = None, settings: Settings | None = None) -> BatchResult:
    """``count`` difficulty-targeted puzzles (seed ``item_seed(seed, i)`` each). ``target`` falls back to
    ``settings.profile``; there is no default profile. An exhausted item is ``unsuccessful`` with no puzzle."""
    count = _count(count)
    check_int(seed, "seed", 0, MAX_SEED)
    st = settings or Settings()
    target = st.profile if target is None else target
    if target is None:
        raise InvalidArgumentError("a target profile is required (argument or settings.profile); none is assumed")
    max_attempts = st.max_attempts if max_attempts is None else max_attempts
    fail_fast, output_dir, theme, cell_size, margin = _common(settings, fail_fast, output_dir, theme, cell_size, margin)
    directory = _output_dir(output_dir, export)
    names = _ids(count, ids, "targeted")
    seeds = [item_seed(seed, i) for i in range(count)]

    def work(i: int):
        r = engine.generate_targeted(seeds[i], target, max_attempts=max_attempts, target_clues=target_clues,
                                     node_budget=node_budget, timeout=timeout)
        return r, _exporter(engine, export, directory, names[i], r.puzzle, r.solution, theme, cell_size, margin, overwrite, timeout)
    return _run(count, names, seeds, fail_fast, work)


# ---------------------------------------------------- supplied boards

def _boards(boards: Iterable[Any]) -> list[Any]:
    if isinstance(boards, (str, bytes)):
        raise InvalidArgumentError("boards must be a collection of board strings, not a single string")
    try:
        items = list(boards)
    except TypeError:
        raise InvalidArgumentError("boards must be an iterable of board strings") from None
    if len(items) > MAX_BATCH_SIZE:
        raise InvalidArgumentError(f"a batch holds at most {MAX_BATCH_SIZE} items")
    return items          # individual items are validated per item, so one bad board fails only itself


def classify_puzzles(engine: SudokuEngine, boards: Iterable[Any], *, ids: Sequence[str] | None = None,
                     fail_fast: bool | None = None, timeout: float | None = None,
                     settings: Settings | None = None) -> BatchResult:
    """``classify`` every board. Unsolved/contradictory/invalid boards are ``unsuccessful`` and carry no label."""
    items = _boards(boards)
    fail_fast = (settings or Settings()).fail_fast if fail_fast is None else fail_fast
    names = _ids(len(items), ids, "board")
    return _run(len(items), names, [None] * len(items), fail_fast,
                lambda i: (engine.classify(items[i], timeout=timeout), None))


def solve_puzzles(engine: SudokuEngine, boards: Iterable[Any], *, node_budget: int | None = None,
                  ids: Sequence[str] | None = None, fail_fast: bool | None = None, timeout: float | None = None,
                  settings: Settings | None = None) -> BatchResult:
    """``solve_search`` every board (one solution each; says nothing about uniqueness)."""
    items = _boards(boards)
    fail_fast = (settings or Settings()).fail_fast if fail_fast is None else fail_fast
    names = _ids(len(items), ids, "board")
    return _run(len(items), names, [None] * len(items), fail_fast,
                lambda i: (engine.solve_search(items[i], node_budget=node_budget, timeout=timeout), None))


def verify_solutions(engine: SudokuEngine, pairs: Iterable[Any], *, ids: Sequence[str] | None = None,
                     fail_fast: bool | None = None, timeout: float | None = None,
                     settings: Settings | None = None) -> BatchResult:
    """``verify`` every ``(puzzle, solution)`` pair with the engine."""
    items = _boards(pairs)
    fail_fast = (settings or Settings()).fail_fast if fail_fast is None else fail_fast
    names = _ids(len(items), ids, "pair")

    def work(i: int):
        pair = items[i]
        if not isinstance(pair, (tuple, list)) or len(pair) != 2:
            raise InvalidArgumentError("each item must be a (puzzle, solution) pair")
        return engine.verify(pair[0], pair[1], timeout=timeout), None
    return _run(len(items), names, [None] * len(items), fail_fast, work)
