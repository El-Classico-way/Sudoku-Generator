#include "sudoku/logical.hpp"

#include <algorithm>

#include "sudoku/locked.hpp"
#include "sudoku/pairs.hpp"
#include "sudoku/singles.hpp"
#include "sudoku/triples.hpp"
#include "sudoku/xwing.hpp"

namespace sudoku {
namespace {

constexpr Technique kAllTechniques[kTechniqueCount] = {
    Technique::NakedSingle, Technique::HiddenSingle,  Technique::Pointing,
    Technique::Claiming,    Technique::NakedPair,     Technique::HiddenPair,
    Technique::NakedTriple, Technique::HiddenTriple,  Technique::XWing};

DetectResult detect(Technique t, const CandidateGrid& g) {
  switch (t) {
    case Technique::NakedSingle: return find_naked_singles(g);
    case Technique::HiddenSingle: return find_hidden_singles(g);
    case Technique::Pointing: return find_pointing(g);
    case Technique::Claiming: return find_claiming(g);
    case Technique::NakedPair: return find_naked_pairs(g);
    case Technique::HiddenPair: return find_hidden_pairs(g);
    case Technique::NakedTriple: return find_naked_triples(g);
    case Technique::HiddenTriple: return find_hidden_triples(g);
    case Technique::XWing: return find_x_wings(g);
  }
  DetectResult unknown;
  unknown.status = DetectStatus::InvalidBoard;  // unreachable for validated options
  return unknown;
}

int total_candidates(const CandidateGrid& g) {
  int n = 0;
  for (int c = 0; c < kCellCount; ++c) n += g.candidate_count(c);
  return n;
}

bool valid_technique(Technique t) {
  return std::find(std::begin(kAllTechniques), std::end(kAllTechniques), t) !=
         std::end(kAllTechniques);
}

LogicalResult finish(LogicalResult r, LogicalStatus status, LogicalReason reason,
                     const CandidateGrid& g) {
  r.status = status;
  r.reason = reason;
  r.board = g.board();
  r.candidates = g;
  return r;
}

}  // namespace

std::vector<Technique> default_technique_order() {
  return std::vector<Technique>(std::begin(kAllTechniques), std::end(kAllTechniques));
}

LogicalResult solve_logically(const Board& puzzle, const LogicalOptions& options) {
  LogicalResult result;

  // Configuration.
  std::vector<Technique> order = options.techniques.empty() ? default_technique_order()
                                                            : options.techniques;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (!valid_technique(order[i]) ||
        std::find(order.begin() + static_cast<std::ptrdiff_t>(i) + 1, order.end(), order[i]) != order.end()) {
      result.status = LogicalStatus::InvalidInput;
      result.reason = LogicalReason::InvalidConfiguration;
      return result;
    }
  }

  // Board.
  CandidateInitResult init = CandidateGrid::from_board(puzzle);
  if (init.status != InitStatus::Ok) {
    result.status = LogicalStatus::InvalidInput;
    result.reason = LogicalReason::InconsistentBoard;
    result.conflicts = std::move(init.conflicts);
    return result;
  }
  CandidateGrid grid = *init.grid;
  const Board givens = puzzle;

  for (;;) {
    // 1. Contradictory candidate state?
    std::vector<Contradiction> found = grid.find_contradictions();
    if (!found.empty()) {
      result.contradictions = std::move(found);
      const LogicalReason reason = result.steps.empty()
                                       ? LogicalReason::ContradictoryInitialState
                                       : LogicalReason::ContradictionAfterDeduction;
      return finish(std::move(result), LogicalStatus::Contradiction, reason, grid);
    }

    // 2. Complete? Verify independently; never trust the candidate state.
    if (grid.board().is_complete()) {
      const SolutionVerdict verdict = verify_solution(grid.board(), givens);
      if (!verdict.valid()) {
        result.verdict = verdict;
        return finish(std::move(result), LogicalStatus::Error, LogicalReason::VerificationFailed, grid);
      }
      return finish(std::move(result), LogicalStatus::Solved, LogicalReason::PuzzleSolved, grid);
    }

    // 3. First enabled technique that proposes a deduction.
    const Deduction* chosen = nullptr;
    DetectResult detected;
    for (Technique t : order) {
      detected = detect(t, grid);
      if (detected.status != DetectStatus::Ok) {
        return finish(std::move(result), LogicalStatus::Error, LogicalReason::DetectorFailure, grid);
      }
      if (!detected.deductions.empty()) {
        chosen = &detected.deductions.front();
        break;
      }
    }
    if (chosen == nullptr) {
      return finish(std::move(result), LogicalStatus::Stalled, LogicalReason::NoApplicableDeduction, grid);
    }

    // 5. Apply through the evidence-checking path.
    const int before = total_candidates(grid);
    AppliedDeduction applied = apply(grid, *chosen);
    switch (applied.status) {
      case ApplyStatus::Applied: break;
      case ApplyStatus::WouldContradict:
        result.refused = std::move(applied);
        return finish(std::move(result), LogicalStatus::Contradiction,
                      LogicalReason::DeductionWouldContradict, grid);
      case ApplyStatus::NoChange:
      case ApplyStatus::Rejected:
      case ApplyStatus::Unsupported:
        result.refused = std::move(applied);
        return finish(std::move(result), LogicalStatus::Error, LogicalReason::ApplicationRefused, grid);
    }
    if (total_candidates(grid) >= before || applied.effective.empty()) {
      result.refused = std::move(applied);
      return finish(std::move(result), LogicalStatus::Error, LogicalReason::NoProgress, grid);
    }

    // Record.
    ++result.stats.deductions_applied;
    ++result.stats.by_technique[static_cast<std::size_t>(chosen->technique())];
    if (chosen->action() == DeductionAction::PlaceDigit) {
      ++result.stats.placements;
    } else {
      result.stats.eliminated_candidates += static_cast<int>(applied.effective.size());
    }
    result.steps.push_back(std::move(applied));
  }
}

}  // namespace sudoku
