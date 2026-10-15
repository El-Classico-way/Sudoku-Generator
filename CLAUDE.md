# Claude Code Instructions

## 1. Project Overview

This project is a Sudoku puzzle generator focused on correctness,
reproducibility, maintainability, and high-quality visual output.

The target is standard 9x9 Sudoku.

The intended application combines:
- A C++20 Sudoku engine.
- A complete search solver and solution counter.
- A logical solver that explains deductions without guessing.
- A puzzle generator that guarantees unique solutions.
- Configurable difficulty profiles.
- A Python command-line interface and orchestration layer.
- Customizable SVG rendering.
- Automated tests and reproducible benchmarks.

The project is developed incrementally. Implement only the milestone
currently requested.

Do not assume that features described in the roadmap already exist.
Inspect the repository to determine the actual implementation state.

## 2. Engineering Priorities

Prioritize these properties in order:

1. Correctness.
2. Testability and reproducibility.
3. Clear interfaces and maintainable architecture.
4. Reliable error handling.
5. Performance supported by measurements.
6. Visual quality and usability.

Prefer straightforward, well-tested implementations over clever
algorithms or unnecessary abstractions.

Do not optimize before establishing correctness and measuring a
meaningful baseline.

## 3. Technology

Use the following technologies unless the existing environment or
repository provides a justified alternative:

- C++20 for the computational core.
- CMake for the C++ build system.
- CTest for test discovery and execution.
- Python 3 for orchestration, CLI, configuration, and rendering.
- pytest for Python tests.
- JSON for communication between C++ and Python.
- SVG as the primary visual export format.
- Git for version control.

Prefer a standalone C++ executable communicating with Python through
a documented JSON protocol.

Do not introduce pybind11 unless a demonstrated benefit justifies
the additional complexity.

Keep runtime dependencies minimal. Investigate availability and
compatibility before adding dependencies.

Do not assume internet access, package installation permissions,
a particular operating system, or a specific compiler.

Inspect the actual environment before making environment-dependent
decisions.

## 4. Architecture

Maintain clear boundaries between:

1. Board representation and validation.
2. Search solving and solution counting.
3. Complete-grid generation.
4. Clue removal and uniqueness verification.
5. Logical solving techniques.
6. Deduction traces.
7. Difficulty evaluation.
8. Difficulty-targeted generation.
9. C++ command-line interface.
10. Python orchestration and configuration.
11. SVG rendering and export.
12. Tests and benchmarks.

Keep the Sudoku engine independent of Python rendering code.

Keep rendering independent of solver internals.

Do not create separate libraries, executables, or modules without
a clear responsibility and practical benefit.

Avoid global mutable state.

Use RAII, const-correctness, explicit interfaces, and appropriate
standard-library facilities.

Prefer fixed-size data structures where they naturally represent
the standard 9x9 board.

## 5. Correctness Invariants

These rules are mandatory.

### Search solver

The search solver must eventually support:

- Zero solutions.
- Exactly one solution.
- At least two solutions.
- Invalid input.
- Interrupted or incomplete search.

Solution counting should stop after finding two solutions when only
uniqueness needs to be established.

Finding one solution does not establish uniqueness.

If the search is interrupted before uniqueness is established,
never report the puzzle as uniquely solvable.

### Logical solver

The logical solver must use explicit deduction techniques.

It must never use backtracking, trial placements, speculative
assumptions, or guessing to advance a logical solution.

Every deduction must record the technique that justifies it.

Failure to make progress means STUCK under the enabled techniques.
It does not prove that the puzzle is unsolvable.

Uniqueness and logical solvability are separate properties.

### Puzzle generation

Every successfully generated puzzle must have exactly one solution.

Verify uniqueness using the search solver and appropriate independent
validation.

Preserve the original solution when removing clues.

Respect configured generation limits.

If generation fails, return an explicit failure result and diagnostic.
Never silently return a puzzle that does not meet the requested
requirements.

### Difficulty

Difficulty must be based on documented solving behavior, not only
on clue count.

A difficulty label is meaningful only relative to a documented
technique set and evaluation model.

Never silently downgrade a requested difficulty.

Do not claim support for techniques that have not been implemented
and tested.

### Rendering

The renderer must not silently repair invalid puzzle data.

Changing a theme must not change puzzle values or solver behavior.

Visual polish must not compromise readability or print quality.

## 6. Development Workflow

Before changing files:

1. Inspect the working directory.
2. Read relevant project instructions and documentation.
3. Inspect Git status.
4. Examine relevant source files and existing tests.
5. Identify related interfaces and dependencies.
6. Determine the scope of the requested milestone.

Preserve unrelated user changes.

Do not overwrite existing work without inspecting it.

Implement only the requested milestone.

Avoid speculative future features and unrelated refactoring.

Prefer small, focused changes that can be reviewed independently.

When modifying an existing interface, inspect all relevant callers
and tests before making changes.

Update documentation when externally visible behavior or interfaces
change.

## 7. Testing

Testing is part of implementation, not an optional final step.

Add focused tests for new behavior and important failure cases.

Use existing test conventions when appropriate.

Do not remove, weaken, skip, or rewrite tests merely to make a build
pass.

Do not change expected results simply to accommodate an unexplained
implementation failure.

Prefer independently verified fixtures and reference solutions
where practical.

Combine unit tests, integration tests, regression tests, and
randomized testing as the project matures.

For logical techniques, test both valid deductions and situations
where no deduction should occur.

Verify that candidate eliminations are justified by the technique.

After implementation:

1. Build affected targets.
2. Run relevant tests.
3. Run broader regression tests when practical.
4. Inspect failures and diagnose their causes.
5. Review the final diff.

If a command cannot run, report the reason.

Never claim that tests passed unless they actually executed
successfully.

Distinguish PASS, FAIL, BLOCKED, and NOT RUN.

Compilation alone does not establish algorithmic correctness.

## 8. Performance

Establish correctness before optimization.

Measure relevant performance before changing algorithms for speed.

Where appropriate, measure:

- Search nodes.
- Generation attempts.
- Generation success rate.
- Elapsed time.
- Median generation time.
- 95th-percentile generation time.

Use reproducible inputs and seeds.

Distinguish measured results from estimates.

Consider bitmasks, minimum-remaining-values selection, and efficient
candidate calculations when justified.

Do not introduce complex optimizations without evidence that their
benefits justify their maintenance costs.

Run correctness tests after every optimization.

## 9. Git Safety

Before editing, inspect Git status and the current branch.

Preserve all unrelated modifications and untracked files.

Never execute destructive Git operations without explicit
authorization.

Do not run commands that discard local changes, reset the working
tree, clean untracked files, or rewrite history.

Do not commit, push, merge, or delete branches unless explicitly
authorized.

At the end of each milestone:

1. Inspect Git status.
2. Review the complete final diff.
3. Inspect relevant untracked files separately.
4. Identify unintended changes.
5. Report modified and untracked files.

Do not stage or commit changes automatically.

## 10. Session Continuity

Do not assume conversational memory persists between sessions.

Before resuming work after a context reset:

1. Read this file.
2. Read README.md and relevant architecture documentation.
3. Inspect Git status and recent history.
4. Inspect the actual implementation.
5. Review relevant tests and available test results.
6. Determine which milestone is genuinely complete.
7. Identify unresolved issues before proceeding.

Do not infer completion from previous conversational claims alone.

## 11. Completion Report

At the end of each milestone, report:

### Changes
- Files created or modified.
- Significant implementation decisions.

### Verification
- Exact commands executed.
- Actual results.
- Failures or blocked checks.

### Acceptance criteria
- Which criteria passed.
- Which criteria failed or remain blocked.

### Git state
- Modified files.
- Untracked files.
- Confirmation that the final diff was reviewed.
- Confirmation that no unauthorized Git operations occurred.

### Remaining issues
- Known defects.
- Environment limitations.
- Outstanding decisions.
- Recommended next milestone.

Be concise, factual, and explicit about uncertainty.

Never claim that unexecuted checks passed.

## 12. Build and Test Commands

From the repository root (Linux/bash; other platforms unverified):

```sh
cmake --preset dev && cmake --build --preset dev
ctest --preset dev
.venv/bin/python -m pytest
```

Build output lives in `build/` (ignored). Python tests find the package
through `pythonpath` in `pyproject.toml`; pytest is installed in the
project-local `.venv` only. No C++ test framework or JSON library is
chosen yet; see README "Decisions and known limitations".
