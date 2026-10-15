// Structured diagnostics for targeted generation.
//
// to_json() renders what generate_targeted() actually reported, together with
// the request that produced it, as JSON. It adds nothing that was not measured
// or reported: absent information is OMITTED (never replaced by a default that
// would look like a value), and no timing is taken inside the engine.
//
// Schema "sudoku.targeted-diagnostics/1" (all fields below are JSON numbers,
// strings, booleans or objects; 64-bit seeds are written as exact integers, so
// consumers that parse numbers as IEEE doubles may lose precision above 2^53):
//   schema          always the string above
//   status          "generated" | "exhausted" | "invalid_request" |
//                   "contradiction" | "error"
//   fault           only when status is invalid_request: why it was rejected
//                   (empty_target, unknown_target, invalid_profiles,
//                   zero_attempts, invalid_clue_target, zero_node_budget)
//   target          the requested profile name
//   seed            the request seed
//   max_attempts    the configured attempt limit (from the options)
//   attempts        attempts made (0 for an invalid request); never above
//                   max_attempts. Includes the successful or fatal attempt.
//   attempt_seed    seed of the last attempt made (the match when generated);
//                   omitted when attempts is 0. Attempt i uses
//                   attempt_seed(seed, i), see targeted.hpp.
//   rejections      counts of candidates rejected: easier_than_target,
//                   harder_than_target, unsolved, degenerate, inconclusive.
//                   Fatal outcomes are NOT counted here: for generated they sum
//                   to attempts - 1, for exhausted to attempts, for
//                   contradiction/error to attempts - 1 (the fatal attempt is
//                   reported through status/detail instead).
//   last_rejection  verdict of the last rejected candidate; omitted if none
//   detail          what failed; present only for error/contradiction
//   result          only when status is generated:
//     clue_count, puzzle, solution (81 characters, row-major, '.' = empty
//     in the puzzle),
//     classification: profile, profile_index, metrics {deductions, placements,
//       eliminated_candidates, hardest_level, hardest_technique (omitted when
//       there is none), by_technique {technique_id: count, ...}}
//   elapsed_ns      only when the caller supplied a wall-clock measurement
//
// The classification of a non-generated result is never emitted: the engine
// leaves it unassigned or describes a defective candidate, so it is not a
// measurement of the request.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sudoku/json_writer.hpp"
#include "sudoku/targeted.hpp"

namespace sudoku {

inline constexpr std::string_view kTargetedDiagnosticsSchema = "sudoku.targeted-diagnostics/1";

// Stable snake_case identifiers used in the JSON output.
std::string_view to_string(TargetedStatus s);
std::string_view to_string(RequestFault f);
std::string_view to_string(CandidateVerdict v);
std::string_view to_string(DifficultyStatus s);
std::string_view to_string(LogicalStatus s);
std::string_view to_string(LogicalReason r);
std::string_view to_string(PuzzleStatus s);
std::string_view technique_id(Technique t);   // e.g. "naked_single", "x_wing"

// Board as an 81-character string, '.' for empty cells.
std::string board_string(const Board& board);

void write_metrics(JsonWriter& w, const SolveMetrics& m);
void write_targeted_diagnostics(JsonWriter& w, const TargetedResult& result, const TargetedOptions& options,
                                std::optional<std::int64_t> elapsed_ns = std::nullopt);
std::string targeted_diagnostics_json(const TargetedResult& result, const TargetedOptions& options,
                                      std::optional<std::int64_t> elapsed_ns = std::nullopt);

}  // namespace sudoku
