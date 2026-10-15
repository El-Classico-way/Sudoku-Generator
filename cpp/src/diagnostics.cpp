#include "sudoku/diagnostics.hpp"

namespace sudoku {

std::string_view to_string(TargetedStatus s) {
  switch (s) {
    case TargetedStatus::Generated: return "generated";
    case TargetedStatus::Exhausted: return "exhausted";
    case TargetedStatus::InvalidRequest: return "invalid_request";
    case TargetedStatus::Contradiction: return "contradiction";
    case TargetedStatus::Error: return "error";
  }
  return "unknown";
}

std::string_view to_string(RequestFault f) {
  switch (f) {
    case RequestFault::None: return "none";
    case RequestFault::EmptyTarget: return "empty_target";
    case RequestFault::UnknownTarget: return "unknown_target";
    case RequestFault::InvalidProfiles: return "invalid_profiles";
    case RequestFault::ZeroAttempts: return "zero_attempts";
    case RequestFault::InvalidClueTarget: return "invalid_clue_target";
    case RequestFault::ZeroNodeBudget: return "zero_node_budget";
  }
  return "unknown";
}

std::string_view to_string(CandidateVerdict v) {
  switch (v) {
    case CandidateVerdict::Matches: return "matches";
    case CandidateVerdict::WrongDifficulty: return "wrong_difficulty";
    case CandidateVerdict::Unsolved: return "unsolved";
    case CandidateVerdict::NotUnique: return "not_unique";
    case CandidateVerdict::Inconclusive: return "inconclusive";
    case CandidateVerdict::Degenerate: return "degenerate";
    case CandidateVerdict::Invalid: return "invalid";
    case CandidateVerdict::Contradiction: return "contradiction";
    case CandidateVerdict::Error: return "error";
  }
  return "unknown";
}

std::string_view to_string(DifficultyStatus s) {
  switch (s) {
    case DifficultyStatus::Classified: return "classified";
    case DifficultyStatus::Unsolved: return "unsolved";
    case DifficultyStatus::Contradiction: return "contradiction";
    case DifficultyStatus::InvalidInput: return "invalid_input";
    case DifficultyStatus::Error: return "error";
  }
  return "unknown";
}

std::string_view to_string(LogicalStatus s) {
  switch (s) {
    case LogicalStatus::Solved: return "solved";
    case LogicalStatus::Stalled: return "stalled";
    case LogicalStatus::Contradiction: return "contradiction";
    case LogicalStatus::InvalidInput: return "invalid_input";
    case LogicalStatus::Error: return "error";
  }
  return "unknown";
}

std::string_view to_string(LogicalReason r) {
  switch (r) {
    case LogicalReason::PuzzleSolved: return "puzzle_solved";
    case LogicalReason::NoApplicableDeduction: return "no_applicable_deduction";
    case LogicalReason::ContradictoryInitialState: return "contradictory_initial_state";
    case LogicalReason::ContradictionAfterDeduction: return "contradiction_after_deduction";
    case LogicalReason::DeductionWouldContradict: return "deduction_would_contradict";
    case LogicalReason::InconsistentBoard: return "inconsistent_board";
    case LogicalReason::InvalidConfiguration: return "invalid_configuration";
    case LogicalReason::VerificationFailed: return "verification_failed";
    case LogicalReason::ApplicationRefused: return "application_refused";
    case LogicalReason::DetectorFailure: return "detector_failure";
    case LogicalReason::NoProgress: return "no_progress";
  }
  return "unknown";
}

std::string_view to_string(PuzzleStatus s) {
  switch (s) {
    case PuzzleStatus::TargetReached: return "target_reached";
    case PuzzleStatus::TargetNotReached: return "target_not_reached";
    case PuzzleStatus::InvalidSolution: return "invalid_solution";
    case PuzzleStatus::InvalidTarget: return "invalid_target";
  }
  return "unknown";
}

std::string_view technique_id(Technique t) {
  switch (t) {
    case Technique::NakedSingle: return "naked_single";
    case Technique::HiddenSingle: return "hidden_single";
    case Technique::Pointing: return "pointing";
    case Technique::Claiming: return "claiming";
    case Technique::NakedPair: return "naked_pair";
    case Technique::HiddenPair: return "hidden_pair";
    case Technique::NakedTriple: return "naked_triple";
    case Technique::HiddenTriple: return "hidden_triple";
    case Technique::XWing: return "x_wing";
  }
  return "unknown";
}

std::string board_string(const Board& board) {
  std::string s;
  s.reserve(kCellCount);
  for (int i = 0; i < kCellCount; ++i) {
    const int v = board.get(i);
    s += v == kEmptyCell ? '.' : static_cast<char>('0' + v);
  }
  return s;
}

void write_metrics(JsonWriter& w, const SolveMetrics& m) {
  w.begin_object();
  w.field("deductions", m.deductions);
  w.field("placements", m.placements);
  w.field("eliminated_candidates", m.eliminated_candidates);
  w.field("hardest_level", m.hardest_level);
  if (m.hardest_technique) w.field("hardest_technique", technique_id(*m.hardest_technique));
  w.key("by_technique").begin_object();
  for (Technique t : default_technique_order()) w.field(technique_id(t), m.count(t));
  w.end_object();
  w.end_object();
}

void write_targeted_diagnostics(JsonWriter& w, const TargetedResult& result, const TargetedOptions& options,
                                std::optional<std::int64_t> elapsed_ns) {
  w.begin_object();
  w.field("schema", kTargetedDiagnosticsSchema);
  w.field("status", to_string(result.status));
  if (result.status == TargetedStatus::InvalidRequest) w.field("fault", to_string(result.fault));
  w.field("target", result.target);
  w.field("seed", result.seed);
  w.field("max_attempts", options.max_attempts);
  w.field("attempts", result.attempts);
  if (result.attempts > 0) w.field("attempt_seed", result.attempt_seed);
  w.key("rejections").begin_object();
  w.field("easier_than_target", result.rejections.easier_than_target);
  w.field("harder_than_target", result.rejections.harder_than_target);
  w.field("unsolved", result.rejections.unsolved);
  w.field("degenerate", result.rejections.degenerate);
  w.field("inconclusive", result.rejections.inconclusive);
  w.end_object();
  if (result.last_rejection) w.field("last_rejection", to_string(*result.last_rejection));
  if (!result.detail.empty()) w.field("detail", result.detail);
  if (result.status == TargetedStatus::Generated && result.puzzle && result.solution) {
    w.key("result").begin_object();
    w.field("clue_count", result.clue_count);
    w.field("puzzle", board_string(*result.puzzle));
    w.field("solution", board_string(*result.solution));
    w.key("classification").begin_object();
    w.field("profile", result.classification.profile);
    w.field("profile_index", result.classification.profile_index);
    w.key("metrics");
    write_metrics(w, result.classification.metrics);
    w.end_object();
    w.end_object();
  }
  if (elapsed_ns) w.field("elapsed_ns", *elapsed_ns);
  w.end_object();
}

std::string targeted_diagnostics_json(const TargetedResult& result, const TargetedOptions& options,
                                      std::optional<std::int64_t> elapsed_ns) {
  JsonWriter w;
  write_targeted_diagnostics(w, result, options, elapsed_ns);
  return w.str();
}

}  // namespace sudoku
