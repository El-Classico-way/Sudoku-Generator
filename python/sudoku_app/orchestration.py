"""Thin orchestration over the typed client.

Only compositions of existing engine operations live here; no Sudoku algorithm is
reimplemented. Facts about a puzzle (uniqueness, difficulty, verification) are
taken from the engine's own typed answers and exposed next to them, so nothing is
claimed that the engine did not establish.
"""
from __future__ import annotations

from dataclasses import dataclass

from .client import SudokuEngine
from .models import ClassifyResult, CountResult, PuzzleResult, SolutionClass, VerifyResult


@dataclass(frozen=True)
class PuzzleReport:
    """A puzzle together with the engine's answers about it.

    ``count`` is an independent exhaustive solution count (limit 2), ``verification``
    checks the generator's solution against the puzzle, and ``classification`` is the
    engine's difficulty profile. The properties below only restate those answers.
    """
    generation: PuzzleResult
    count: CountResult
    verification: VerifyResult
    classification: ClassifyResult

    @property
    def puzzle(self) -> str:
        return self.generation.puzzle

    @property
    def solution(self) -> str:
        return self.generation.solution

    @property
    def is_unique(self) -> bool:
        """True only if the engine's exhaustive count found exactly one solution."""
        return self.count.exact and self.count.count == 1 and self.count.classification is SolutionClass.UNIQUE

    @property
    def solution_verified(self) -> bool:
        return self.verification.success

    @property
    def profile(self) -> str | None:
        """The engine's difficulty profile, or None if the engine did not classify the puzzle."""
        return self.classification.profile


def describe_puzzle(engine: SudokuEngine, generation: PuzzleResult, *, timeout: float | None = None) -> PuzzleReport:
    """Asks the engine to count, verify and classify an already generated puzzle."""
    return PuzzleReport(
        generation=generation,
        count=engine.count_solutions(generation.puzzle, limit=2, timeout=timeout),
        verification=engine.verify(generation.puzzle, generation.solution, timeout=timeout),
        classification=engine.classify(generation.puzzle, timeout=timeout),
    )


def generate_puzzle_report(engine: SudokuEngine, seed: int, *, target_clues: int | None = None,
                           node_budget: int | None = None, timeout: float | None = None) -> PuzzleReport:
    """Generates a puzzle (same seed and options, same puzzle) and attaches the engine's
    uniqueness, verification and classification answers."""
    generation = engine.generate_puzzle(seed, target_clues=target_clues, node_budget=node_budget, timeout=timeout)
    return describe_puzzle(engine, generation, timeout=timeout)
