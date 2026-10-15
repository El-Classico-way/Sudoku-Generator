// Logical solver: repeatedly applies the implemented deduction techniques to a
// puzzle, with no guessing, backtracking or search of any kind.
//
// Techniques and priority (default order, also the order of enum Technique):
//   NakedSingle, HiddenSingle, Pointing, Claiming, NakedPair, HiddenPair,
//   NakedTriple, HiddenTriple, XWing.
//
// Loop (one deduction per iteration, always from a freshly detected snapshot):
//   1. candidate state contradictory?            -> Contradiction
//   2. board complete?  verify_solution(...)     -> Solved (or Error if the
//      independent check fails)
//   3. for each enabled technique in priority order, run its detector; the
//      first technique that proposes anything wins and its FIRST deduction (in
//      the detector's documented order) is chosen
//   4. none proposed                             -> Stalled
//   5. apply() it (which re-validates the evidence against the current grid):
//        Applied         -> record it and continue
//        WouldContradict -> Contradiction (a sound deduction cannot be
//                           carried out without emptying a cell, so the
//                           puzzle has no solution)
//        NoChange/Rejected/Unsupported -> Error (a deduction detected from the
//                           current state must be applicable)
// Only one deduction is applied per detection, so a stale list is never
// reused. The candidate grid is maintained incrementally (CandidateGrid::
// place / eliminate): sound eliminations are never undone by a rebuild.
//
// Progress guarantee: every Applied deduction removes at least one candidate,
// and candidates are never added, so the total candidate count strictly
// decreases; the loop therefore ends after at most 729 applications. The
// solver checks the decrease and returns Error (NoProgress) rather than loop if
// it ever fails to hold.
//
// Results (LogicalStatus):
//   Solved        board complete AND verify_solution() passes against the
//                 givens (the filled cells of the input).
//   Stalled       consistent state, no enabled technique proposes anything.
//                 Says nothing about solvability; the puzzle may well be
//                 solvable with stronger techniques or by search.
//   Contradiction the candidate state (or a required application) shows the
//                 puzzle cannot be completed.
//   InvalidInput  the Board repeats a digit in a unit, or the options are
//                 invalid (empty-vs-default is fine; duplicate technique
//                 entries or values outside the enum are not).
//   Error         an internal inconsistency (verification failed, a detected
//                 deduction was refused, no progress); `reason` and the
//                 recorded fields say what.
// The input Board is never modified. Givens are the nonzero cells of the Board
// passed in.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"
#include "sudoku/verify.hpp"

namespace sudoku {

inline constexpr int kTechniqueCount = 9;

// The default priority order (all techniques).
std::vector<Technique> default_technique_order();

struct LogicalOptions {
  // Techniques to use, highest priority first. Empty means the default order.
  // Duplicates or values outside the enum make the call InvalidInput.
  std::vector<Technique> techniques;
};

enum class LogicalStatus { Solved, Stalled, Contradiction, InvalidInput, Error };

enum class LogicalReason {
  PuzzleSolved,                 // Solved
  NoApplicableDeduction,        // Stalled
  ContradictoryInitialState,    // Contradiction: the starting candidates
  ContradictionAfterDeduction,  // Contradiction: after >= 1 applied deduction
  DeductionWouldContradict,     // Contradiction: `refused` holds the deduction
  InconsistentBoard,            // InvalidInput: `conflicts` lists them
  InvalidConfiguration,         // InvalidInput: bad `techniques`
  VerificationFailed,           // Error: `verdict` says why
  ApplicationRefused,           // Error: `refused` holds the failed application
  DetectorFailure,              // Error: a detector reported a non-Ok status
  NoProgress,                   // Error: Applied without shrinking candidates
};

struct LogicalStats {
  int deductions_applied = 0;
  int placements = 0;           // applied placement deductions
  int eliminated_candidates = 0;  // candidates removed by elimination deductions
  std::array<int, kTechniqueCount> by_technique{};  // applied deductions per Technique
  int count(Technique t) const { return by_technique[static_cast<std::size_t>(t)]; }
};

struct LogicalResult {
  LogicalStatus status = LogicalStatus::InvalidInput;
  LogicalReason reason = LogicalReason::InvalidConfiguration;
  // State at termination (absent for InvalidInput). For Solved the board is the
  // verified solution; for other statuses it is the board as far as it got and
  // must not be treated as a solution.
  std::optional<Board> board;
  std::optional<CandidateGrid> candidates;
  std::vector<AppliedDeduction> steps;       // every applied deduction, in order
  LogicalStats stats;
  std::vector<Conflict> conflicts;           // InconsistentBoard
  std::vector<Contradiction> contradictions; // Contradiction (state-based)
  std::optional<AppliedDeduction> refused;   // DeductionWouldContradict / ApplicationRefused
  SolutionVerdict verdict;                   // VerificationFailed
};

LogicalResult solve_logically(const Board& puzzle, const LogicalOptions& options = {});

}  // namespace sudoku
