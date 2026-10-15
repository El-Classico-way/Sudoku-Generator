// Application protocol: one JSON request in, one JSON response out.
//
// Request  (schema "sudoku.request/1"):
//   { "schema": "sudoku.request/1", "op": "<operation>",
//     "id": "<optional string, 1..128 chars, echoed>", "params": { ... } }
// Response (schema "sudoku.response/1"):
//   { "schema": "sudoku.response/1", "op": "...", "id": "...",
//     "ok": true,  "success": <bool>, "status": "<op-specific>", "result": { ... } }
//   { "schema": "sudoku.response/1", ["op": "...",] ["id": "...",]
//     "ok": false, "error": { "code": "...", "message": "...", ["field": "..."], ["position": N] } }
//
// `ok` is true iff the request was valid and executed. A valid request can still
// report a non-success engine outcome (`success` false, e.g. status "exhausted",
// "stalled", "unsolvable", "budget_exhausted"). Protocol errors and internal
// errors have `ok` false and an `error` object and never a `status`/`result`.
//
// Process exit codes (see ExitCode): 0 ok and success; 1 ok but not success;
// 2 protocol error (the request was not valid); 3 internal/operational failure.
//
// This protocol is distinct from the benchmark/diagnostics schemas of
// diagnostics.hpp: 64-bit seeds are decimal STRINGS here (exact for every
// consumer); the diagnostics write them as numbers.
//
// The full field reference, validation rules and examples are in the README
// ("JSON protocol and CLI").
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sudoku {

inline constexpr std::string_view kRequestSchema = "sudoku.request/1";
inline constexpr std::string_view kResponseSchema = "sudoku.response/1";
// Requests larger than this are rejected (error code request_too_large)
// without being parsed.
inline constexpr std::size_t kMaxRequestBytes = 1024 * 1024;

enum ExitCode : int {
  kExitSuccess = 0,        // valid request, engine goal achieved
  kExitNonSuccess = 1,     // valid request, engine reported a non-success outcome
  kExitProtocolError = 2,  // malformed/invalid request (or bad command line)
  kExitInternalError = 3,  // internal or operational failure
};

struct ProtocolOutcome {
  std::string response_json;     // exactly one JSON object, no trailing newline
  int exit_code = kExitInternalError;
  std::string internal_detail;   // non-empty only for internal errors; for stderr, never in the response
};

// Handles one request document. Never throws: every failure becomes a response.
ProtocolOutcome handle_request(std::string_view request_text);

// The response for input that exceeded kMaxRequestBytes (used by the CLI, which
// stops reading at the limit).
ProtocolOutcome request_too_large_outcome();

// A response for an operational failure that happens outside request handling
// (for example the input file cannot be read): an `internal_error` response with
// exit code kExitInternalError. `detail` is for stderr only.
ProtocolOutcome internal_error_outcome(std::string detail);

}  // namespace sudoku
