#include "sudoku/protocol.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>

#include "sudoku/diagnostics.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/json_reader.hpp"
#include "sudoku/json_writer.hpp"
#include "sudoku/logical.hpp"
#include "sudoku/puzzle.hpp"
#include "sudoku/solver.hpp"
#include "sudoku/targeted.hpp"
#include "sudoku/verify.hpp"

#ifndef SUDOKU_ENGINE_VERSION
#define SUDOKU_ENGINE_VERSION "unknown"
#endif

namespace sudoku {
namespace {

// Limits that protect the process (documented in the README).
constexpr std::int64_t kMaxJsonSafeInteger = 9007199254740991;  // 2^53 - 1
constexpr std::int64_t kMaxCountLimit = 100000;
constexpr std::int64_t kMaxAttempts = 1000000;
constexpr std::size_t kMaxIdLength = 128;

// A request the caller got wrong: becomes ok=false with a stable code.
struct ProtocolError {
  std::string code;
  std::string message;
  std::string field;                    // empty if not about one field
  std::optional<std::size_t> position;  // byte offset for parse errors
};

// The engine contradicted itself: becomes internal_error, details only on stderr.
struct InternalFault : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string truncated(const std::string& s) {
  constexpr std::size_t kMax = 64;
  return s.size() <= kMax ? s : s.substr(0, kMax) + "...";
}

// ------------------------------------------------------------ field reading

class Params {
 public:
  // Rejects any member not in `known` immediately, so typos are reported first.
  Params(const JsonValue* object, std::initializer_list<std::string_view> known) : object_(object) {
    if (object_ == nullptr) return;
    for (const auto& [key, value] : object_->members) {
      (void)value;
      if (std::find(known.begin(), known.end(), key) == known.end()) {
        throw ProtocolError{"unknown_field", "unknown field", "params." + truncated(key), std::nullopt};
      }
    }
  }

  const JsonValue* get(std::string_view name) const { return object_ ? object_->find(name) : nullptr; }

  const JsonValue& require(std::string_view name) const {
    const JsonValue* v = get(name);
    if (v == nullptr) throw ProtocolError{"missing_field", "required field is missing", path(name), std::nullopt};
    return *v;
  }

  std::string require_string(std::string_view name) const {
    const JsonValue& v = require(name);
    if (!v.is_string()) throw bad(name, "must be a string");
    return v.text;
  }

  Board require_board(std::string_view name) const {
    const JsonValue& v = require(name);
    if (!v.is_string()) throw bad(name, "must be a string of 81 characters");
    return parse_board(name, v.text);
  }

  // Decimal string 0..18446744073709551615 without sign or leading zeros.
  std::uint64_t require_seed(std::string_view name) const {
    const JsonValue& v = require(name);
    if (!v.is_string()) throw bad(name, "must be a decimal string (a JSON number is not accepted)");
    const std::string& s = v.text;
    const bool shape = !s.empty() && s.size() <= 20 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                       (s.size() == 1 || s[0] != '0');
    if (!shape) throw bad(name, "must be a decimal string of digits without sign or leading zeros");
    std::uint64_t value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) throw bad(name, "is outside 0..18446744073709551615");
    return value;
  }

  std::int64_t optional_int(std::string_view name, std::int64_t lo, std::int64_t hi, std::int64_t fallback) const {
    const JsonValue* v = get(name);
    return v == nullptr ? fallback : read_int(name, *v, lo, hi);
  }
  std::optional<std::int64_t> optional_int(std::string_view name, std::int64_t lo, std::int64_t hi) const {
    const JsonValue* v = get(name);
    if (v == nullptr) return std::nullopt;
    return read_int(name, *v, lo, hi);
  }
  bool optional_bool(std::string_view name, bool fallback) const {
    const JsonValue* v = get(name);
    if (v == nullptr) return fallback;
    if (!v->is_bool()) throw bad(name, "must be true or false");
    return v->boolean;
  }

 private:
  std::string path(std::string_view name) const { return "params." + std::string(name); }
  ProtocolError bad(std::string_view name, const std::string& why) const {
    return {"invalid_field", why, path(name), std::nullopt};
  }
  std::int64_t read_int(std::string_view name, const JsonValue& v, std::int64_t lo, std::int64_t hi) const {
    if (!v.is_integer()) throw bad(name, "must be an integer (not a string, fraction or exponent form)");
    const auto value = v.as_int64();
    if (!value || *value < lo || *value > hi) {
      throw bad(name, "must be in " + std::to_string(lo) + ".." + std::to_string(hi));
    }
    return *value;
  }
  Board parse_board(std::string_view name, const std::string& s) const {
    if (s.size() != kCellCount) throw bad(name, "must be exactly 81 characters, got " + std::to_string(s.size()));
    std::array<int, kCellCount> cells{};
    for (std::size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c == '.' || c == '0') cells[i] = 0;
      else if (c >= '1' && c <= '9') cells[i] = c - '0';
      else throw bad(name, "invalid character at index " + std::to_string(i) + " (allowed: 1-9, '.', '0')");
    }
    return Board(cells);
  }

  const JsonValue* object_;
};

// --------------------------------------------------------------- operations

struct OpResult {
  bool success = false;
  std::string status;
  std::string result_json = "{}";
};

std::string decimal(std::uint64_t v) { return std::to_string(v); }

void write_stats(JsonWriter& w, const SearchStats& s) {
  w.key("stats").begin_object();
  w.field("nodes", s.nodes);
  w.field("assignments", s.assignments);
  w.field("backtracks", s.backtracks);
  w.end_object();
}

SearchOptions search_options(const Params& p) {
  SearchOptions o;
  if (const auto budget = p.optional_int("node_budget", 1, kMaxJsonSafeInteger)) {
    o.node_budget = static_cast<std::uint64_t>(*budget);
  }
  return o;
}

OpResult op_capabilities(const JsonValue* params) {
  Params p(params, {});
  JsonWriter w;
  w.begin_object();
  w.field("engine_version", SUDOKU_ENGINE_VERSION);
  w.key("request_schemas").begin_array().value(kRequestSchema).end_array();
  w.field("response_schema", kResponseSchema);
  w.key("operations").begin_array();
  for (const char* op : {"capabilities", "generate_grid", "generate_puzzle", "solve_search", "count_solutions",
                         "solve_logical", "verify", "classify", "generate_targeted"}) {
    w.value(op);
  }
  w.end_array();
  w.key("techniques").begin_array();
  for (Technique t : default_technique_order()) w.value(technique_id(t));
  w.end_array();
  w.key("profiles").begin_array();
  for (const DifficultyProfile& profile : standard_profiles()) {
    w.begin_object().field("name", profile.name);
    w.key("techniques").begin_array();
    for (Technique t : profile.techniques) w.value(technique_id(t));
    w.end_array().end_object();
  }
  w.end_array();
  w.key("board").begin_object();
  w.field("length", kCellCount);
  w.field("input_characters", "123456789.0");
  w.field("output_empty", ".");
  w.end_object();
  w.key("limits").begin_object();
  w.field("max_request_bytes", static_cast<std::uint64_t>(kMaxRequestBytes));
  w.field("max_json_depth", static_cast<std::uint64_t>(JsonLimits{}.max_depth));
  w.field("max_count_limit", kMaxCountLimit);
  w.field("max_attempts", kMaxAttempts);
  w.field("max_node_budget", kMaxJsonSafeInteger);
  w.field("max_id_length", static_cast<std::uint64_t>(kMaxIdLength));
  w.end_object();
  w.end_object();
  return {true, "ok", w.str()};
}

OpResult op_generate_grid(const JsonValue* params) {
  Params p(params, {"seed"});
  const std::uint64_t seed = p.require_seed("seed");
  const GenerateResult r = generate_complete_grid(seed);
  if (r.status != GenerateStatus::Generated || !r.grid || !r.grid->is_valid_solution()) {
    throw InternalFault("generate_complete_grid did not return a valid grid");
  }
  JsonWriter w;
  w.begin_object();
  w.field("seed", decimal(seed));
  w.field("grid", board_string(*r.grid));
  write_stats(w, r.stats);
  w.end_object();
  return {true, "generated", w.str()};
}

OpResult op_generate_puzzle(const JsonValue* params) {
  Params p(params, {"seed", "target_clues", "node_budget"});
  const std::uint64_t seed = p.require_seed("seed");
  PuzzleOptions options;
  options.target_clues = static_cast<int>(p.optional_int("target_clues", kMinPuzzleClues, kCellCount, kMinPuzzleClues));
  if (const auto budget = p.optional_int("node_budget", 1, kMaxJsonSafeInteger)) {
    options.node_budget = static_cast<std::uint64_t>(*budget);
  }
  const PuzzleResult r = generate_puzzle(seed, options);
  if ((r.status != PuzzleStatus::TargetReached && r.status != PuzzleStatus::TargetNotReached) || !r.puzzle || !r.solution) {
    throw InternalFault("generate_puzzle returned no puzzle for a validated request");
  }
  // Re-establish the guarantees independently before reporting success.
  const CountResult unique = count_solutions(*r.puzzle, 2);
  if (!(unique.status == SearchStatus::Exhausted && unique.count == 1 && unique.first_solution &&
        *unique.first_solution == *r.solution) ||
      !verify_solution(*r.solution, *r.puzzle).valid()) {
    throw InternalFault("generated puzzle failed independent uniqueness/verification");
  }
  JsonWriter w;
  w.begin_object();
  w.field("seed", decimal(seed));
  w.field("requested_clues", options.target_clues);
  w.field("target_reached", r.status == PuzzleStatus::TargetReached);
  w.field("puzzle", board_string(*r.puzzle));
  w.field("solution", board_string(*r.solution));
  w.field("clue_count", r.clue_count);
  w.key("removal").begin_object();
  w.field("cells_considered", r.stats.cells_considered);
  w.field("removed", r.stats.removed);
  w.field("rejected_not_unique", r.stats.rejected_not_unique);
  w.field("rejected_inconclusive", r.stats.rejected_inconclusive);
  w.end_object();
  w.end_object();
  return {true, std::string(to_string(r.status)), w.str()};
}

OpResult op_solve_search(const JsonValue* params) {
  Params p(params, {"puzzle", "node_budget"});
  const Board puzzle = p.require_board("puzzle");
  const SolveResult r = find_solution(puzzle, search_options(p));
  const char* status = "";
  switch (r.status) {
    case SolveStatus::Solved: status = "solved"; break;
    case SolveStatus::Unsolvable: status = "unsolvable"; break;
    case SolveStatus::InvalidInput: status = "invalid_input"; break;
    case SolveStatus::BudgetExhausted: status = "budget_exhausted"; break;
  }
  JsonWriter w;
  w.begin_object();
  if (r.status == SolveStatus::Solved) {
    if (!r.solution || !verify_solution(*r.solution, puzzle).valid()) {
      throw InternalFault("search solver returned an invalid solution");
    }
    w.field("solution", board_string(*r.solution));
  }
  write_stats(w, r.stats);
  w.end_object();
  return {r.status == SolveStatus::Solved, status, w.str()};
}

std::string_view solution_class_id(SolutionClass c) {
  switch (c) {
    case SolutionClass::InvalidInput: return "invalid_input";
    case SolutionClass::NoSolution: return "no_solution";
    case SolutionClass::Unique: return "unique";
    case SolutionClass::Multiple: return "multiple";
    case SolutionClass::Unknown: return "unknown";
  }
  return "unknown";
}

OpResult op_count_solutions(const JsonValue* params) {
  Params p(params, {"puzzle", "limit", "node_budget"});
  const Board puzzle = p.require_board("puzzle");
  const std::int64_t limit = p.optional_int("limit", 1, kMaxCountLimit, 2);
  const CountResult r = count_solutions(puzzle, static_cast<std::uint64_t>(limit), search_options(p));
  const char* status = "";
  switch (r.status) {
    case SearchStatus::InvalidInput: status = "invalid_input"; break;
    case SearchStatus::Exhausted: status = "exhausted"; break;
    case SearchStatus::SolutionLimitReached: status = "limit_reached"; break;
    case SearchStatus::BudgetExhausted: status = "budget_exhausted"; break;
  }
  JsonWriter w;
  w.begin_object();
  w.field("limit", limit);
  w.field("count", r.count);
  w.field("exact", r.is_exact());   // true only after an exhaustive search
  w.field("classification", solution_class_id(classify(r)));
  if (r.first_solution) {
    if (!verify_solution(*r.first_solution, puzzle).valid()) throw InternalFault("counter returned an invalid solution");
    w.field("first_solution", board_string(*r.first_solution));
  }
  write_stats(w, r.stats);
  w.end_object();
  const bool answered = r.status == SearchStatus::Exhausted || r.status == SearchStatus::SolutionLimitReached;
  return {answered, status, w.str()};
}

std::optional<Technique> technique_from_id(std::string_view id) {
  for (Technique t : default_technique_order()) {
    if (technique_id(t) == id) return t;
  }
  return std::nullopt;
}

OpResult op_solve_logical(const JsonValue* params) {
  Params p(params, {"puzzle", "techniques", "include_trace"});
  const Board puzzle = p.require_board("puzzle");
  LogicalOptions options;
  if (const JsonValue* list = p.get("techniques")) {
    if (!list->is_array() || list->items.empty()) {
      throw ProtocolError{"invalid_field", "must be a non-empty array of technique ids", "params.techniques", std::nullopt};
    }
    for (const JsonValue& item : list->items) {
      const auto t = item.is_string() ? technique_from_id(item.text) : std::nullopt;
      if (!t || std::find(options.techniques.begin(), options.techniques.end(), *t) != options.techniques.end()) {
        throw ProtocolError{"invalid_field", "unknown or repeated technique id", "params.techniques", std::nullopt};
      }
      options.techniques.push_back(*t);
    }
  }
  const bool trace = p.optional_bool("include_trace", false);
  const LogicalResult r = solve_logically(puzzle, options);
  if (r.status == LogicalStatus::Error) {
    throw InternalFault(std::string("logical solver error: ") + std::string(to_string(r.reason)));
  }
  JsonWriter w;
  w.begin_object();
  w.field("reason", to_string(r.reason));
  if (r.board) {
    if (r.status == LogicalStatus::Solved && !verify_solution(*r.board, puzzle).valid()) {
      throw InternalFault("logical solver reported an unverifiable solution");
    }
    w.field("board", board_string(*r.board));   // a solution only when status is "solved"
  }
  w.key("stats").begin_object();
  w.field("deductions", r.stats.deductions_applied);
  w.field("placements", r.stats.placements);
  w.field("eliminated_candidates", r.stats.eliminated_candidates);
  w.key("by_technique").begin_object();
  for (Technique t : default_technique_order()) w.field(technique_id(t), r.stats.count(t));
  w.end_object();
  w.end_object();
  if (trace) {
    w.key("steps").begin_array();
    for (const AppliedDeduction& step : r.steps) {
      w.begin_object();
      w.field("technique", technique_id(step.deduction.technique()));
      w.field("action", step.deduction.action() == DeductionAction::PlaceDigit ? "place" : "eliminate");
      w.key("targets").begin_array();
      for (const Elimination& e : step.deduction.targets()) {
        w.begin_object().field("cell", e.cell).field("digit", e.digit).end_object();
      }
      w.end_array().end_object();
    }
    w.end_array();
  }
  w.end_object();
  return {r.status == LogicalStatus::Solved, std::string(to_string(r.status)), w.str()};
}

std::string_view fault_id(SolutionFault f) {
  switch (f) {
    case SolutionFault::None: return "valid";
    case SolutionFault::EmptyCell: return "empty_cell";
    case SolutionFault::GivenChanged: return "given_changed";
    case SolutionFault::RowViolation: return "row_violation";
    case SolutionFault::ColumnViolation: return "column_violation";
    case SolutionFault::BoxViolation: return "box_violation";
  }
  return "unknown";
}

OpResult op_verify(const JsonValue* params) {
  Params p(params, {"puzzle", "solution"});
  const Board puzzle = p.require_board("puzzle");
  const Board solution = p.require_board("solution");
  const SolutionVerdict v = verify_solution(solution, puzzle);
  JsonWriter w;
  w.begin_object();
  w.field("valid", v.valid());
  if (!v.valid()) {
    w.field("fault", fault_id(v.fault));
    w.field("index", v.index);
    if (v.digit != 0) w.field("digit", v.digit);
  }
  w.end_object();
  return {v.valid(), std::string(fault_id(v.fault)), w.str()};
}

OpResult op_classify(const JsonValue* params) {
  Params p(params, {"puzzle"});
  const Board puzzle = p.require_board("puzzle");
  const DifficultyResult r = classify_difficulty(puzzle);
  if (r.status == DifficultyStatus::Error) throw InternalFault("classifier error");
  JsonWriter w;
  w.begin_object();
  w.field("solver_status", to_string(r.solver_status));
  w.field("solver_reason", to_string(r.solver_reason));
  if (r.status == DifficultyStatus::Classified) {
    w.field("profile", r.profile);
    w.field("profile_index", r.profile_index);
    w.key("metrics");
    write_metrics(w, r.metrics);
  }
  w.end_object();
  return {r.status == DifficultyStatus::Classified, std::string(to_string(r.status)), w.str()};
}

OpResult op_generate_targeted(const JsonValue* params) {
  Params p(params, {"seed", "target", "max_attempts", "target_clues", "node_budget"});
  const std::uint64_t seed = p.require_seed("seed");
  TargetedOptions options;
  options.target = p.require_string("target");
  const auto profiles = standard_profiles();
  const bool known = std::any_of(profiles.begin(), profiles.end(),
                                 [&](const DifficultyProfile& d) { return d.name == options.target; });
  if (!known) {
    throw ProtocolError{"invalid_field", "unknown profile (see the capabilities operation for the names)",
                        "params.target", std::nullopt};
  }
  options.max_attempts = static_cast<std::uint64_t>(p.optional_int("max_attempts", 1, kMaxAttempts, 100));
  options.puzzle.target_clues = static_cast<int>(p.optional_int("target_clues", kMinPuzzleClues, kCellCount, kMinPuzzleClues));
  if (const auto budget = p.optional_int("node_budget", 1, kMaxJsonSafeInteger)) {
    options.puzzle.node_budget = static_cast<std::uint64_t>(*budget);
  }
  const TargetedResult r = generate_targeted(seed, options);
  if (r.status != TargetedStatus::Generated && r.status != TargetedStatus::Exhausted) {
    // Validated request, so these mean a defect; the detail stays out of the response.
    throw InternalFault("targeted generation reported " + std::string(to_string(r.status)) + ": " + r.detail);
  }
  if (r.status == TargetedStatus::Generated) {
    const CandidateReport check = check_candidate(*r.puzzle, *r.solution, options.target);
    if (check.verdict != CandidateVerdict::Matches) {
      throw InternalFault("targeted result failed independent re-validation");
    }
  }
  JsonWriter w;
  w.begin_object();
  w.field("target", r.target);
  w.field("seed", decimal(seed));
  w.field("attempts", r.attempts);
  w.field("max_attempts", options.max_attempts);
  if (r.attempts > 0) w.field("attempt_seed", decimal(r.attempt_seed));
  w.key("rejections").begin_object();
  w.field("easier_than_target", r.rejections.easier_than_target);
  w.field("harder_than_target", r.rejections.harder_than_target);
  w.field("unsolved", r.rejections.unsolved);
  w.field("degenerate", r.rejections.degenerate);
  w.field("inconclusive", r.rejections.inconclusive);
  w.end_object();
  if (r.last_rejection) w.field("last_rejection", to_string(*r.last_rejection));
  if (r.status == TargetedStatus::Generated) {
    w.field("puzzle", board_string(*r.puzzle));
    w.field("solution", board_string(*r.solution));
    w.field("clue_count", r.clue_count);
    w.key("classification").begin_object();
    w.field("profile", r.classification.profile);
    w.field("profile_index", r.classification.profile_index);
    w.key("metrics");
    write_metrics(w, r.classification.metrics);
    w.end_object();
  }
  w.end_object();
  return {r.status == TargetedStatus::Generated, std::string(to_string(r.status)), w.str()};
}

// --------------------------------------------------------------- envelope

struct Operation {
  std::string_view name;
  OpResult (*run)(const JsonValue* params);
};
constexpr Operation kOperations[] = {
    {"capabilities", op_capabilities},   {"generate_grid", op_generate_grid},
    {"generate_puzzle", op_generate_puzzle}, {"solve_search", op_solve_search},
    {"count_solutions", op_count_solutions}, {"solve_logical", op_solve_logical},
    {"verify", op_verify},               {"classify", op_classify},
    {"generate_targeted", op_generate_targeted},
};

void begin_envelope(JsonWriter& w, const std::optional<std::string>& op, const std::optional<std::string>& id) {
  w.begin_object();
  w.field("schema", kResponseSchema);
  if (op) w.field("op", *op);
  if (id) w.field("id", *id);
}

ProtocolOutcome error_outcome(const ProtocolError& e, const std::optional<std::string>& op,
                              const std::optional<std::string>& id) {
  JsonWriter w;
  begin_envelope(w, op, id);
  w.field("ok", false);
  w.key("error").begin_object();
  w.field("code", e.code);
  w.field("message", e.message);
  if (!e.field.empty()) w.field("field", e.field);
  if (e.position) w.field("position", static_cast<std::uint64_t>(*e.position));
  w.end_object();
  w.end_object();
  return {w.str(), kExitProtocolError, {}};
}

ProtocolOutcome internal_outcome(const std::string& detail, const std::optional<std::string>& op,
                                 const std::optional<std::string>& id) {
  JsonWriter w;
  begin_envelope(w, op, id);
  w.field("ok", false);
  w.key("error").begin_object();
  w.field("code", "internal_error");
  w.field("message", "internal error; no result is available");
  w.end_object();
  w.end_object();
  return {w.str(), kExitInternalError, detail};
}

bool all_whitespace(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

ProtocolOutcome process(std::string_view text, std::optional<std::string>& op, std::optional<std::string>& id) {
  if (text.size() > kMaxRequestBytes) return request_too_large_outcome();
  if (all_whitespace(text)) {
    return error_outcome({"empty_request", "the request is empty", "", std::nullopt}, op, id);
  }
  JsonParseError parse_error;
  const std::optional<JsonValue> doc = parse_json(text, parse_error);
  if (!doc) {
    return error_outcome({"parse_error", parse_error.message, "", parse_error.offset}, op, id);
  }
  if (!doc->is_object()) {
    return error_outcome({"invalid_request", "the request must be a JSON object", "", std::nullopt}, op, id);
  }

  // Envelope fields. `id` is validated first so later errors can echo it.
  if (const JsonValue* v = doc->find("id")) {
    if (!v->is_string() || v->text.empty() || v->text.size() > kMaxIdLength) {
      return error_outcome({"invalid_field", "must be a string of 1..128 characters", "id", std::nullopt}, op, id);
    }
    id = v->text;
  }
  const JsonValue* schema = doc->find("schema");
  if (schema == nullptr) return error_outcome({"missing_field", "required field is missing", "schema", std::nullopt}, op, id);
  if (!schema->is_string()) return error_outcome({"invalid_field", "must be a string", "schema", std::nullopt}, op, id);
  if (schema->text != kRequestSchema) {
    return error_outcome({"unsupported_schema",
                          "unsupported request schema; this engine understands " + std::string(kRequestSchema),
                          "schema", std::nullopt}, op, id);
  }
  const JsonValue* opv = doc->find("op");
  if (opv == nullptr) return error_outcome({"missing_field", "required field is missing", "op", std::nullopt}, op, id);
  if (!opv->is_string()) return error_outcome({"invalid_field", "must be a string", "op", std::nullopt}, op, id);
  const Operation* operation = nullptr;
  for (const Operation& candidate : kOperations) {
    if (candidate.name == opv->text) operation = &candidate;
  }
  if (operation == nullptr) {
    return error_outcome({"unknown_operation", "unknown operation", "op", std::nullopt}, op, id);
  }
  op = std::string(operation->name);
  for (const auto& [key, value] : doc->members) {
    (void)value;
    if (key != "schema" && key != "op" && key != "id" && key != "params") {
      return error_outcome({"unknown_field", "unknown field", truncated(key), std::nullopt}, op, id);
    }
  }
  const JsonValue* params = doc->find("params");
  if (params != nullptr && !params->is_object()) {
    return error_outcome({"invalid_field", "must be an object", "params", std::nullopt}, op, id);
  }

  OpResult result;
  try {
    result = operation->run(params);
  } catch (const ProtocolError& e) {
    return error_outcome(e, op, id);
  }

  JsonWriter w;
  begin_envelope(w, op, id);
  w.field("ok", true);
  w.field("success", result.success);
  w.field("status", result.status);
  w.key("result").raw(result.result_json);
  w.end_object();
  return {w.str(), result.success ? kExitSuccess : kExitNonSuccess, {}};
}

}  // namespace

ProtocolOutcome request_too_large_outcome() {
  return error_outcome({"request_too_large",
                        "the request exceeds " + std::to_string(kMaxRequestBytes) + " bytes", "", std::nullopt},
                       std::nullopt, std::nullopt);
}

ProtocolOutcome internal_error_outcome(std::string detail) {
  return internal_outcome(detail, std::nullopt, std::nullopt);
}

ProtocolOutcome handle_request(std::string_view request_text) {
  std::optional<std::string> op;
  std::optional<std::string> id;
  try {
    return process(request_text, op, id);
  } catch (const std::exception& e) {
    return internal_outcome(e.what(), op, id);
  } catch (...) {
    return internal_outcome("unknown exception", op, id);
  }
}

}  // namespace sudoku
