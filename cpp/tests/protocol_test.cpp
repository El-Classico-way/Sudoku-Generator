#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "check.hpp"
#include "json_parse.hpp"
#include "reference.hpp"
#include "sudoku/diagnostics.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/puzzle.hpp"
#include "sudoku/targeted.hpp"
#include "sudoku/json_reader.hpp"
#include "sudoku/protocol.hpp"
#include "sudoku/solver.hpp"
#include "sudoku/verify.hpp"

using namespace sudoku;
using jsontest::Value;

namespace {

// ---- fixtures (each puzzle has exactly one solution unless noted, confirmed independently) ----
const char* kEasy = "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79";
const char* kEasySolved = "534678912672195348198342567859761423426853791713924856961537284287419635345286179";
const char* kMedium = "5...2.8671...........45..1......7....6......443....2..81.6........179.5..79..5..1";
const char* kStalled = ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4.";
const char* kTwoSolutions = "534008912672195348198342567859001423426853791713924856961537284287419635345286179";
std::string pad(std::string prefix) { return prefix + std::string(81 - prefix.size(), '.'); }
// Row 0 holds 1..7 and (1,7) is 9: cells (0,7) and (0,8) can only be 8, so no completion exists.
const std::string kUnsolvable = pad("123456700.......9.");
const char* kContradiction = "..............2..1.....12.....1....2..1.2.....2.....1....21.....1.....2...2...1..";
const std::string kDuplicate = pad("55");   // two 5s in one row

std::string req(const std::string& op, const std::string& params = "", const std::string& id = "") {
  std::string s = "{\"schema\":\"sudoku.request/1\",\"op\":\"" + op + "\"";
  if (!id.empty()) s += ",\"id\":\"" + id + "\"";
  if (!params.empty()) s += ",\"params\":" + params;
  return s + "}";
}
std::string q(const std::string& s) { return "\"" + s + "\""; }

struct Reply {
  ProtocolOutcome outcome;
  Value json;
  bool parsed = false;
};

Reply call(const std::string& text) {
  Reply r;
  r.outcome = handle_request(text);
  const auto v = jsontest::parse(r.outcome.response_json);
  CHECK_CTX(v.has_value(), r.outcome.response_json.substr(0, 200));   // always exactly one valid JSON document
  if (v) { r.json = *v; r.parsed = true; }
  return r;
}

// Checks of every response, success or not.
void check_envelope(const Reply& r, const std::string& ctx) {
  if (!r.parsed) return;
  CHECK_CTX(r.json.is_object() && r.json.at("schema").text == "sudoku.response/1", ctx);
  CHECK_CTX(r.json.at("ok").type == Value::Type::Bool, ctx);
  CHECK_CTX(r.outcome.response_json.find('\n') == std::string::npos, ctx);
  if (r.json.at("ok").boolean) {
    CHECK_CTX(r.json.has("success") && r.json.has("status") && r.json.has("result") && !r.json.has("error"), ctx);
    CHECK_CTX(r.outcome.exit_code == (r.json.at("success").boolean ? kExitSuccess : kExitNonSuccess), ctx);
    CHECK_CTX(r.json.at("result").is_object() && r.outcome.internal_detail.empty(), ctx);
  } else {
    CHECK_CTX(r.json.has("error") && !r.json.has("status") && !r.json.has("result") && !r.json.has("success"), ctx);
    CHECK_CTX(r.json.at("error").has("code") && r.json.at("error").has("message"), ctx);
    CHECK_CTX(r.outcome.exit_code == kExitProtocolError || r.outcome.exit_code == kExitInternalError, ctx);
  }
}

void expect_error(const std::string& text, const std::string& code, const std::string& field, const std::string& ctx) {
  const Reply r = call(text);
  check_envelope(r, ctx);
  if (!r.parsed) return;
  CHECK_CTX(!r.json.at("ok").boolean && r.outcome.exit_code == kExitProtocolError, ctx);
  CHECK_CTX(r.json.at("error").at("code").text == code, ctx + " got " + r.json.at("error").at("code").text);
  if (!field.empty()) CHECK_CTX(r.json.at("error").has("field") && r.json.at("error").at("field").text == field, ctx);
}

// Parses the reply's result board and returns it as a Board.
Board board_of(const std::string& s) {
  std::array<int, kCellCount> v{};
  CHECK(s.size() == 81);
  for (std::size_t i = 0; i < s.size() && i < 81; ++i) v[i] = s[i] == '.' ? 0 : s[i] - '0';
  return Board(v);
}

// ---------- the JSON reader ----------
void test_reader() {
  JsonParseError err;
  auto ok = [&](const std::string& s) { return parse_json(s, err).has_value(); };
  for (const char* good : {"{}", "[]", "null", "true", "0", "-0", "-1.5e+3", "\"x\"", " [1, 2 , {\"a\": [true]}] \n",
                           "\"\\u0041\\n\\\"\\\\\\/\"", "{\"k\":\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"}"}) {
    CHECK_CTX(ok(good), good);
  }
  for (const char* bad : {"", " ", "{", "}", "[1,]", "{\"a\":1,}", "{,}", "[1 2]", "01", "1.", ".5", "+1", "-", "1e", "NaN", "Infinity",
                          "-Infinity", "tru", "nul", "'a'", "{a:1}", "{\"a\" 1}", "\"abc", "\"a\nb\"", "\"\\q\"", "\"\\u12\"",
                          "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\\ud83dxxDE00\"", "\"\\ud83d\\\\uDE00\"", "[1] x", "{} {}", "\"\xff\"", "\"\xc0\x80\"",
                          "\"\xed\xa0\x80\"", "\"\xe2\x82\"", "\"\xf4\x90\x80\x80\"", "{\"a\":1,\"a\":2}", "[\"\x01\"]"}) {
    CHECK_CTX(!ok(bad), std::string("accepted: ") + bad);
  }
  // Failure reports where.
  CHECK(!parse_json("[1, x]", err) && err.offset == 4 && !err.message.empty());
  CHECK(!parse_json("{\"a\":1,\"a\":2}", err) && err.message == "duplicate object key" && err.offset == 7);
  // Unicode escapes decode to UTF-8; a pair combines.
  auto v = parse_json("\"\\u00e9\\ud83d\\ude00\"", err);
  CHECK(v && v->text == "\xc3\xa9\xf0\x9f\x98\x80");
  // Nesting limit: 32 levels are fine, 33 are not (no stack exhaustion even for huge nesting).
  CHECK(ok(std::string(32, '[') + std::string(32, ']')));
  CHECK(!ok(std::string(33, '[') + std::string(33, ']')) && err.message == "nesting too deep");
  CHECK(!ok(std::string(100000, '[')));
  CHECK(!ok(std::string(100000, '{') ));
  // Integers keep their exact value.
  auto n = parse_json("9223372036854775807", err);
  CHECK(n && n->is_integer() && n->as_int64() == std::numeric_limits<std::int64_t>::max());
  CHECK(!parse_json("9223372036854775808", err)->as_int64().has_value());
  CHECK(parse_json("-9223372036854775808", err)->as_int64() == std::numeric_limits<std::int64_t>::min());
  CHECK(!parse_json("17.0", err)->is_integer() && !parse_json("1e2", err)->is_integer());
  CHECK(!parse_json("\"5\"", err)->is_integer());
}

// ---------- envelope and field validation ----------
void test_envelope_errors() {
  expect_error("", "empty_request", "", "empty");
  expect_error("  \n\t ", "empty_request", "", "whitespace only");
  expect_error("{", "parse_error", "", "truncated");
  expect_error("not json", "parse_error", "", "garbage");
  expect_error(req("capabilities") + " x", "parse_error", "", "trailing input");
  expect_error(req("capabilities") + req("capabilities"), "parse_error", "", "two requests");
  expect_error("{\"schema\":\"sudoku.request/1\",\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\"}", "parse_error", "", "duplicate key");
  for (const char* top : {"[]", "\"x\"", "5", "null", "true"}) expect_error(top, "invalid_request", "", top);
  expect_error("{\"op\":\"capabilities\"}", "missing_field", "schema", "missing schema");
  expect_error("{\"schema\":5,\"op\":\"capabilities\"}", "invalid_field", "schema", "schema type");
  expect_error("{\"schema\":\"sudoku.request/2\",\"op\":\"capabilities\"}", "unsupported_schema", "schema", "future schema");
  expect_error("{\"schema\":\"sudoku.request/0\",\"op\":\"capabilities\"}", "unsupported_schema", "schema", "old schema");
  expect_error("{\"schema\":\"sudoku.request/1\"}", "missing_field", "op", "missing op");
  expect_error("{\"schema\":\"sudoku.request/1\",\"op\":7}", "invalid_field", "op", "op type");
  expect_error(req("explode"), "unknown_operation", "op", "unknown op");
  expect_error(req("Generate_Grid"), "unknown_operation", "op", "operation names are case-sensitive");
  expect_error("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"extra\":1}", "unknown_field", "extra", "top-level unknown");
  expect_error("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"params\":[]}", "invalid_field", "params", "params type");
  expect_error(req("capabilities", "{\"x\":1}"), "unknown_field", "params.x", "params unknown");
  // id: echoed when valid, rejected (and not echoed) when not.
  const Reply good = call(req("explode", "", "abc-1"));
  CHECK(good.json.at("id").text == "abc-1" && !good.json.has("op"));   // unknown op is not echoed
  expect_error(req("capabilities", "", std::string(129, 'x')), "invalid_field", "id", "id too long");
  expect_error("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"id\":\"\"}", "invalid_field", "id", "empty id");
  expect_error("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"id\":5}", "invalid_field", "id", "id type");
  CHECK(!call("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"id\":\"\"}").json.has("id"));
  CHECK(call(req("capabilities", "", std::string(128, 'x'))).json.at("id").text.size() == 128);
  // Error responses for a known op echo it.
  const Reply known = call(req("verify", "{}", "r7"));
  CHECK(known.json.at("op").text == "verify" && known.json.at("id").text == "r7");
  // Oversized requests are refused without being parsed.
  const std::string huge(kMaxRequestBytes + 1, ' ');
  expect_error(huge, "request_too_large", "", "oversized");
  const Reply at_limit = call(req("capabilities") + std::string(kMaxRequestBytes - req("capabilities").size(), ' '));
  CHECK(at_limit.json.at("ok").boolean);
  CHECK(request_too_large_outcome().exit_code == kExitProtocolError);
}

void test_field_validation() {
  const std::string p = q(kEasy);
  // seeds
  for (const char* seed : {"\"0\"", "\"1\"", "\"18446744073709551615\"", "\"9007199254740993\""}) {
    const Reply r = call(req("generate_grid", std::string("{\"seed\":") + seed + "}"));
    check_envelope(r, seed);
    CHECK_CTX(r.json.at("ok").boolean && r.json.at("result").at("seed").text == std::string(seed).substr(1, std::strlen(seed) - 2), seed);
  }
  for (const char* bad : {"\"18446744073709551616\"", "\"99999999999999999999\"", "\"184467440737095516150\"", "\"-1\"", "\"+1\"", "\"01\"",
                          "\"00\"", "\"\"", "\"1.0\"", "\" 1\"", "\"1 \"", "\"0x10\"", "\"abc\"", "\"\xef\xbc\x91\"", "5", "1.5", "null", "true", "[]", "{}"}) {
    expect_error(req("generate_grid", std::string("{\"seed\":") + bad + "}"), "invalid_field", "params.seed", std::string("seed ") + bad);
  }
  expect_error(req("generate_grid", "{}"), "missing_field", "params.seed", "missing seed");
  expect_error(req("generate_grid"), "missing_field", "params.seed", "no params");
  expect_error(req("generate_grid", "{\"seed\":\"1\",\"sede\":\"1\"}"), "unknown_field", "params.sede", "typo");
  // boards
  for (const std::string& bad : {std::string(80, '.'), std::string(82, '.'), std::string(""), std::string(81, 'x'),
                                 std::string(40, '.') + "a" + std::string(40, '.'), std::string(81, ' '),
                                 std::string(80, '.') + "\n", std::string(80, '.') + "\xc3\xa9"}) {
    std::string esc;
    for (char c : bad) esc += c == '\n' ? std::string("\\n") : std::string(1, c);
    expect_error(req("classify", "{\"puzzle\":" + q(esc) + "}"), "invalid_field", "params.puzzle", "board len " + std::to_string(bad.size()));
  }
  expect_error(req("classify", "{\"puzzle\":5}"), "invalid_field", "params.puzzle", "board type");
  expect_error(req("classify", "{\"puzzle\":null}"), "invalid_field", "params.puzzle", "board null");
  expect_error(req("classify", "{}"), "missing_field", "params.puzzle", "board missing");
  CHECK(call(req("classify", "{\"puzzle\":" + q(std::string(81, '0')) + "}")).json.at("ok").boolean);   // '0' and '.' both mean empty
  CHECK(call(req("classify", "{\"puzzle\":" + q(std::string(81, '.')) + "}")).json.at("ok").boolean);
  // integer options
  for (const char* bad : {"16", "82", "-1", "0", "\"17\"", "17.0", "1e1", "null", "true", "[]"}) {
    expect_error(req("generate_puzzle", std::string("{\"seed\":\"1\",\"target_clues\":") + bad + "}"), "invalid_field", "params.target_clues", bad);
  }
  for (const char* bad : {"0", "-1", "9007199254740992", "1.0", "\"5\"", "null"}) {
    expect_error(req("solve_search", "{\"puzzle\":" + p + ",\"node_budget\":" + bad + "}"), "invalid_field", "params.node_budget", bad);
  }
  CHECK(call(req("solve_search", "{\"puzzle\":" + p + ",\"node_budget\":9007199254740991}")).json.at("ok").boolean);
  for (const char* bad : {"0", "100001", "-5", "2.5", "\"2\""}) {
    expect_error(req("count_solutions", "{\"puzzle\":" + p + ",\"limit\":" + bad + "}"), "invalid_field", "params.limit", bad);
  }
  for (const char* bad : {"0", "1000001", "-1", "\"1\""}) {
    expect_error(req("generate_targeted", std::string("{\"seed\":\"1\",\"target\":\"Easy\",\"max_attempts\":") + bad + "}"),
                 "invalid_field", "params.max_attempts", bad);
  }
  expect_error(req("generate_targeted", "{\"seed\":\"1\"}"), "missing_field", "params.target", "target missing");
  expect_error(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"easy\"}"), "invalid_field", "params.target", "target is case-sensitive");
  expect_error(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Legendary\"}"), "invalid_field", "params.target", "unknown profile");
  expect_error(req("generate_targeted", "{\"seed\":\"1\",\"target\":5}"), "invalid_field", "params.target", "target type");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"techniques\":[]}"), "invalid_field", "params.techniques", "empty list");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"techniques\":[\"naked_single\",\"naked_single\"]}"), "invalid_field", "params.techniques", "repeat");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"techniques\":[\"teleport\"]}"), "invalid_field", "params.techniques", "unknown technique");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"techniques\":\"naked_single\"}"), "invalid_field", "params.techniques", "not an array");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"techniques\":[5]}"), "invalid_field", "params.techniques", "element type");
  expect_error(req("solve_logical", "{\"puzzle\":" + p + ",\"include_trace\":1}"), "invalid_field", "params.include_trace", "trace type");
  expect_error(req("verify", "{\"puzzle\":" + p + "}"), "missing_field", "params.solution", "solution missing");
  // Validation happens before engine work: nothing expensive runs for a bad request
  // (this would take forever if it reached the solver).
  expect_error(req("count_solutions", "{\"puzzle\":" + q(std::string(81, '.')) + ",\"limit\":100001}"), "invalid_field", "params.limit", "limit before search");
}

// ---------- operations ----------
const Value& result_of(const Reply& r) { return r.json.at("result"); }

void test_capabilities() {
  const Reply r = call(req("capabilities"));
  check_envelope(r, "capabilities");
  CHECK(r.json.at("ok").boolean && r.json.at("status").text == "ok" && r.outcome.exit_code == kExitSuccess);
  const Value& c = result_of(r);
  CHECK(c.at("operations").items.size() == 9);
  CHECK(c.at("request_schemas").items.size() == 1 && c.at("request_schemas").items[0].text == "sudoku.request/1");
  CHECK(c.at("response_schema").text == "sudoku.response/1");
  CHECK(c.at("profiles").items.size() == 5 && c.at("profiles").items[0].at("name").text == "Easy" &&
        c.at("profiles").items[4].at("name").text == "Master");
  CHECK(c.at("techniques").items.size() == 9 && c.at("techniques").items[8].text == "x_wing");
  CHECK(c.at("limits").at("max_request_bytes").integer() == static_cast<long long>(kMaxRequestBytes));
  CHECK(c.at("board").at("length").integer() == 81);
  for (const Value& op : c.at("operations").items) {   // every advertised operation exists
    const Reply probe = call(req(op.text));
    CHECK_CTX(!(probe.json.has("error") && probe.json.at("error").at("code").text == "unknown_operation"), op.text);
  }
}

void test_generate_grid() {
  const Reply a = call(req("generate_grid", "{\"seed\":\"42\"}", "g"));
  check_envelope(a, "grid");
  CHECK(a.outcome.exit_code == kExitSuccess && a.json.at("status").text == "generated" && a.json.at("id").text == "g");
  const Board grid = board_of(result_of(a).at("grid").text);
  CHECK(grid.is_valid_solution() && verify_solution(grid, Board{}).valid());
  CHECK(count_solutions(grid, 2).count == 1);
  CHECK(result_of(a).at("stats").at("nodes").integer() >= 82);
  // Deterministic, and the seed matters.
  CHECK(call(req("generate_grid", "{\"seed\":\"42\"}", "g")).outcome.response_json == a.outcome.response_json);
  CHECK(call(req("generate_grid", "{\"seed\":\"43\"}")).json.at("result").at("grid").text != result_of(a).at("grid").text);
  // Same grid as the engine API.
  CHECK(result_of(a).at("grid").text == board_string(*generate_complete_grid(42).grid));
}

void test_generate_puzzle() {
  const Reply a = call(req("generate_puzzle", "{\"seed\":\"7\",\"target_clues\":30}"));
  check_envelope(a, "puzzle");
  CHECK(a.outcome.exit_code == kExitSuccess && a.json.at("status").text == "target_reached" && result_of(a).at("target_reached").boolean);
  const Value& r = result_of(a);
  const Board puzzle = board_of(r.at("puzzle").text), solution = board_of(r.at("solution").text);
  CHECK(r.at("clue_count").integer() == 30 && r.at("requested_clues").integer() == 30 && r.at("seed").text == "7");
  const CountResult count = count_solutions(puzzle, 2);   // exactly one solution, exhaustively
  CHECK(count.status == SearchStatus::Exhausted && count.count == 1 && *count.first_solution == solution);
  CHECK(verify_solution(solution, puzzle).valid());
  CHECK(call(req("generate_puzzle", "{\"seed\":\"7\",\"target_clues\":30}")).outcome.response_json == a.outcome.response_json);
  // Default clue target: a valid unique puzzle, honestly reported as not reaching 17.
  const Reply d = call(req("generate_puzzle", "{\"seed\":\"7\"}"));
  CHECK(d.json.at("ok").boolean && d.json.at("success").boolean && d.json.at("status").text == "target_not_reached" &&
        !result_of(d).at("target_reached").boolean && result_of(d).at("clue_count").integer() > 17);
  CHECK(count_solutions(board_of(result_of(d).at("puzzle").text), 2).count == 1);
  // A starved uniqueness budget keeps every clue: still unique, never a half-checked puzzle.
  const Reply starved = call(req("generate_puzzle", "{\"seed\":\"7\",\"node_budget\":1}"));
  CHECK(starved.json.at("ok").boolean && result_of(starved).at("clue_count").integer() == 81 &&
        result_of(starved).at("removal").at("removed").integer() == 0);
  // Matches the engine.
  PuzzleOptions o;
  o.target_clues = 30;
  CHECK(r.at("puzzle").text == board_string(*generate_puzzle(7, o).puzzle));
}

void test_solve_search() {
  auto solve = [](const std::string& puzzle, const std::string& extra = "") {
    return call(req("solve_search", "{\"puzzle\":" + q(puzzle) + extra + "}"));
  };
  Reply r = solve(kEasy);
  check_envelope(r, "solve");
  CHECK(r.outcome.exit_code == kExitSuccess && r.json.at("status").text == "solved" && result_of(r).at("solution").text == kEasySolved);
  CHECK(verify_solution(board_of(result_of(r).at("solution").text), board_of(kEasy)).valid());   // givens preserved
  r = solve(kTwoSolutions);   // several solutions: still solved (uniqueness is not claimed)
  CHECK(r.json.at("status").text == "solved" && !result_of(r).has("unique"));
  r = solve(kUnsolvable);
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("ok").boolean && !r.json.at("success").boolean &&
        r.json.at("status").text == "unsolvable" && !result_of(r).has("solution"));
  r = solve(kDuplicate);
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("status").text == "invalid_input" && !result_of(r).has("solution"));
  r = solve(kEasy, ",\"node_budget\":1");
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("status").text == "budget_exhausted" && !result_of(r).has("solution"));
  CHECK(solve(kEasy).outcome.response_json == call(req("solve_search", "{\"puzzle\":" + q(kEasy) + "}")).outcome.response_json);
}

void test_count_solutions() {
  auto count = [](const std::string& puzzle, const std::string& extra = "") {
    return call(req("count_solutions", "{\"puzzle\":" + q(puzzle) + extra + "}"));
  };
  Reply r = count(kEasy);   // default limit 2
  check_envelope(r, "count");
  CHECK(r.json.at("status").text == "exhausted" && result_of(r).at("exact").boolean && result_of(r).at("count").integer() == 1 &&
        result_of(r).at("classification").text == "unique" && result_of(r).at("limit").integer() == 2);
  CHECK(result_of(r).at("first_solution").text == kEasySolved && r.outcome.exit_code == kExitSuccess);
  r = count(kTwoSolutions);   // reaching the limit is a lower bound, not an exact count
  CHECK(r.json.at("status").text == "limit_reached" && !result_of(r).at("exact").boolean && result_of(r).at("count").integer() == 2 &&
        result_of(r).at("classification").text == "multiple" && r.outcome.exit_code == kExitSuccess);
  r = count(kTwoSolutions, ",\"limit\":10");   // a bigger limit proves the exact total
  CHECK(r.json.at("status").text == "exhausted" && result_of(r).at("exact").boolean && result_of(r).at("count").integer() == 2);
  r = count(kEasy, ",\"limit\":1");   // one solution found: uniqueness is NOT established
  CHECK(r.json.at("status").text == "limit_reached" && !result_of(r).at("exact").boolean && result_of(r).at("classification").text == "unknown");
  r = count(kMedium, ",\"node_budget\":1");   // interrupted: inconclusive, non-success exit
  CHECK(r.json.at("status").text == "budget_exhausted" && !r.json.at("success").boolean && r.outcome.exit_code == kExitNonSuccess &&
        !result_of(r).at("exact").boolean && result_of(r).at("classification").text == "unknown");
  r = count(kUnsolvable);
  CHECK(r.json.at("status").text == "exhausted" && result_of(r).at("count").integer() == 0 && result_of(r).at("exact").boolean &&
        result_of(r).at("classification").text == "no_solution" && !result_of(r).has("first_solution"));
  r = count(kDuplicate);
  CHECK(r.json.at("status").text == "invalid_input" && r.outcome.exit_code == kExitNonSuccess && result_of(r).at("count").integer() == 0 &&
        result_of(r).at("classification").text == "invalid_input");
  r = call(req("count_solutions", "{\"puzzle\":" + q(std::string(81, '.')) + ",\"limit\":5}"));   // empty board: bounded, not enumerated
  CHECK(r.json.at("status").text == "limit_reached" && result_of(r).at("count").integer() == 5 && !result_of(r).at("exact").boolean);
  const Reply two = count(kTwoSolutions);
  CHECK(verify_solution(board_of(result_of(two).at("first_solution").text), board_of(kTwoSolutions)).valid());
}

void test_solve_logical() {
  auto logical = [](const std::string& puzzle, const std::string& extra = "") {
    return call(req("solve_logical", "{\"puzzle\":" + q(puzzle) + extra + "}"));
  };
  Reply r = logical(kEasy);
  check_envelope(r, "logical");
  CHECK(r.outcome.exit_code == kExitSuccess && r.json.at("status").text == "solved" && result_of(r).at("board").text == kEasySolved);
  CHECK(result_of(r).at("reason").text == "puzzle_solved" && !result_of(r).has("steps"));
  const Value& stats = result_of(r).at("stats");
  CHECK(stats.at("placements").integer() == 51 && stats.at("deductions").integer() == 51 && stats.at("by_technique").members.size() == 9);
  const LogicalResult direct = solve_logically(board_of(kEasy));
  CHECK(stats.at("deductions").integer() == direct.stats.deductions_applied);
  r = logical(kStalled);   // stalled is not solved, and the board is not presented as a solution
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("ok").boolean && r.json.at("status").text == "stalled" &&
        result_of(r).at("reason").text == "no_applicable_deduction" && result_of(r).at("board").text.find('.') != std::string::npos);
  r = logical(kContradiction);
  CHECK(r.json.at("status").text == "contradiction" && r.outcome.exit_code == kExitNonSuccess);
  r = logical(kDuplicate);
  CHECK(r.json.at("status").text == "invalid_input" && !result_of(r).has("board"));
  r = logical(kMedium, ",\"techniques\":[\"naked_single\",\"hidden_single\"]");   // a subset can stall where the full set solves
  CHECK(r.json.at("status").text == "stalled");
  CHECK(logical(kMedium).json.at("status").text == "solved");
  r = logical(kEasy, ",\"include_trace\":true");
  const Value& steps = result_of(r).at("steps");
  CHECK(steps.items.size() == 51 && steps.items[0].at("action").text == "place");
  CHECK(steps.items[0].at("targets").items.size() == 1 && steps.items[0].at("targets").items[0].has("cell") && steps.items[0].at("technique").type == Value::Type::String);
  r = logical(kMedium, ",\"include_trace\":true");
  bool saw_elimination = false;
  for (const Value& s : result_of(r).at("steps").items) saw_elimination = saw_elimination || s.at("action").text == "eliminate";
  CHECK(saw_elimination);
  CHECK(logical(kEasy).outcome.response_json == call(req("solve_logical", "{\"puzzle\":" + q(kEasy) + "}")).outcome.response_json);
}

void test_verify() {
  auto verify = [](const std::string& puzzle, const std::string& solution) {
    return call(req("verify", "{\"puzzle\":" + q(puzzle) + ",\"solution\":" + q(solution) + "}"));
  };
  Reply r = verify(kEasy, kEasySolved);
  check_envelope(r, "verify");
  CHECK(r.outcome.exit_code == kExitSuccess && r.json.at("status").text == "valid" && result_of(r).at("valid").boolean && !result_of(r).has("fault"));
  std::string hole = kEasySolved;
  hole[40] = '.';
  r = verify(kEasy, hole);
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("status").text == "empty_cell" && !result_of(r).at("valid").boolean &&
        result_of(r).at("fault").text == "empty_cell" && result_of(r).at("index").integer() == 40);
  std::string row = kEasySolved;
  row[0] = row[1];
  r = verify(kEasy, row);
  CHECK(r.json.at("status").text == "given_changed" || r.json.at("status").text == "row_violation");
  r = verify(".........", std::string(81, '1'));
  CHECK(r.json.at("ok").boolean == false);   // 9-character board is a protocol error
  r = verify(std::string(81, '.'), std::string(81, '1'));
  CHECK(r.json.at("status").text == "row_violation" && result_of(r).at("index").integer() == 0 && result_of(r).at("digit").integer() == 1);
  r = verify(kEasy, kEasySolved);
  std::string other = kEasySolved;   // a different but valid grid is a different failure: changes a given
  r = verify(kEasySolved, "534768912672195348198342567859671423426853791713924856961537284287419635345286179");
  CHECK(r.json.at("status").text == "given_changed" && result_of(r).at("index").integer() == 3 && result_of(r).at("digit").integer() == 6);
  r = verify(std::string(81, '0'), kEasySolved);   // '0' is an empty given
  CHECK(r.json.at("status").text == "valid");
}

void test_classify() {
  auto classify_req = [](const std::string& puzzle) { return call(req("classify", "{\"puzzle\":" + q(puzzle) + "}")); };
  Reply r = classify_req(kEasy);
  check_envelope(r, "classify");
  CHECK(r.outcome.exit_code == kExitSuccess && r.json.at("status").text == "classified" && result_of(r).at("profile").text == "Easy" &&
        result_of(r).at("profile_index").integer() == 0);
  CHECK(result_of(r).at("metrics").at("deductions").integer() == 51 && result_of(r).at("metrics").at("by_technique").members.size() == 9);
  CHECK(classify_req(kMedium).json.at("result").at("profile").text == "Medium");
  r = classify_req(kStalled);   // never labelled
  CHECK(r.outcome.exit_code == kExitNonSuccess && r.json.at("status").text == "unsolved" && !result_of(r).has("profile") && !result_of(r).has("metrics"));
  r = classify_req(kDuplicate);
  CHECK(r.json.at("status").text == "invalid_input" && !result_of(r).has("profile") && result_of(r).at("solver_reason").text == "inconsistent_board");
  r = classify_req(kContradiction);
  CHECK(r.json.at("status").text == "contradiction" && !result_of(r).has("profile"));
}

void test_generate_targeted() {
  Reply r = call(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Easy\",\"max_attempts\":60}"));
  check_envelope(r, "targeted");
  CHECK(r.outcome.exit_code == kExitSuccess && r.json.at("status").text == "generated");
  const Value& t = result_of(r);
  const Board puzzle = board_of(t.at("puzzle").text), solution = board_of(t.at("solution").text);
  CHECK(count_solutions(puzzle, 2).count == 1 && count_solutions(puzzle, 2).status == SearchStatus::Exhausted);
  CHECK(verify_solution(solution, puzzle).valid());
  CHECK(classify_difficulty(puzzle).profile == "Easy" && t.at("classification").at("profile").text == "Easy");   // exact match, independently
  CHECK(t.at("attempts").integer() >= 1 && t.at("attempts").integer() <= 60 && t.at("max_attempts").integer() == 60 && t.at("seed").text == "1");
  CHECK(t.at("attempt_seed").type == Value::Type::String);
  std::uint64_t rejected = 0;
  for (const auto& [k, v] : t.at("rejections").members) rejected += v.unsigned_integer();
  CHECK(rejected + 1 == t.at("attempts").unsigned_integer());
  CHECK(t.at("attempt_seed").text == std::to_string(attempt_seed(1, t.at("attempts").unsigned_integer() - 1)));
  CHECK(call(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Easy\",\"max_attempts\":60}")).outcome.response_json == r.outcome.response_json);
  // Exhaustion: valid response, non-success exit, and never a fallback puzzle.
  r = call(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Master\",\"max_attempts\":3}"));
  check_envelope(r, "exhausted");
  CHECK(r.json.at("ok").boolean && !r.json.at("success").boolean && r.json.at("status").text == "exhausted" && r.outcome.exit_code == kExitNonSuccess);
  CHECK(!result_of(r).has("puzzle") && !result_of(r).has("solution") && !result_of(r).has("classification") && !result_of(r).has("clue_count"));
  CHECK(result_of(r).at("attempts").integer() == 3 && result_of(r).at("max_attempts").integer() == 3);
  std::uint64_t sum = 0;
  for (const auto& [k, v] : result_of(r).at("rejections").members) sum += v.unsigned_integer();
  CHECK(sum == 3 && result_of(r).has("last_rejection"));
  CHECK(call(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Master\",\"max_attempts\":3}")).outcome.response_json == r.outcome.response_json);
  // Full-range seed preserved in the response.
  r = call(req("generate_targeted", "{\"seed\":\"18446744073709551615\",\"target\":\"Easy\",\"max_attempts\":2}"));
  CHECK(r.json.at("ok").boolean && result_of(r).at("seed").text == "18446744073709551615");
}

// ---------- compatibility, internal errors, robustness ----------
void test_other_contracts() {
  // The application protocol is not the diagnostics schema.
  const std::string response = call(req("generate_targeted", "{\"seed\":\"1\",\"target\":\"Master\",\"max_attempts\":2}")).outcome.response_json;
  CHECK(response.find("sudoku.targeted-diagnostics") == std::string::npos);
  // The M16 diagnostics output is unchanged (golden string for a fixed result).
  TargetedResult fixed;
  fixed.status = TargetedStatus::Exhausted;
  fixed.target = "Master";
  fixed.seed = 18446744073709551615ull;
  fixed.attempts = 2;
  fixed.attempt_seed = 99;
  fixed.rejections.easier_than_target = 1;
  fixed.rejections.unsolved = 1;
  fixed.last_rejection = CandidateVerdict::Unsolved;
  TargetedOptions options;
  options.target = "Master";
  options.max_attempts = 2;
  CHECK(targeted_diagnostics_json(fixed, options) ==
        "{\"schema\":\"sudoku.targeted-diagnostics/1\",\"status\":\"exhausted\",\"target\":\"Master\","
        "\"seed\":18446744073709551615,\"max_attempts\":2,\"attempts\":2,\"attempt_seed\":99,"
        "\"rejections\":{\"easier_than_target\":1,\"harder_than_target\":0,\"unsolved\":1,\"degenerate\":0,\"inconclusive\":0},"
        "\"last_rejection\":\"unsolved\"}");
  // Internal errors: generic message, details only in internal_detail, exit 3.
  const ProtocolOutcome internal = internal_error_outcome("secret detail \"x\"\n");
  const auto doc = jsontest::parse(internal.response_json);
  CHECK(doc && !doc->at("ok").boolean && doc->at("error").at("code").text == "internal_error" && internal.exit_code == kExitInternalError);
  CHECK(internal.response_json.find("secret") == std::string::npos && internal.internal_detail == "secret detail \"x\"\n");
  // Hostile text in the request never breaks the response JSON.
  const Reply hostile = call("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"id\":\"a\\\"b\\\\c\\u0001\\n\",\"params\":{}}");
  CHECK(hostile.json.at("ok").boolean && hostile.json.at("id").text == "a\"b\\c\x01\n");
  const Reply key = call("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"params\":{\"q\\\"\\n\":1}}");
  CHECK(key.json.at("error").at("code").text == "unknown_field");
  const Reply longkey = call("{\"schema\":\"sudoku.request/1\",\"op\":\"capabilities\",\"" + std::string(10000, 'k') + "\":1}");
  CHECK(longkey.json.at("error").at("field").text.size() < 100);   // echoed field names are truncated
}

// Deterministic mutation fuzz: the handler never throws and always returns exactly
// one strictly valid JSON object with a consistent exit code.
void test_mutation_fuzz() {
  const std::vector<std::string> seeds = {
      req("capabilities"), req("generate_grid", "{\"seed\":\"5\"}", "id1"),
      req("solve_search", "{\"puzzle\":" + q(kEasy) + "}"), req("verify", "{\"puzzle\":" + q(kEasy) + ",\"solution\":" + q(kEasySolved) + "}"),
      req("count_solutions", "{\"puzzle\":" + q(kTwoSolutions) + ",\"limit\":3}"),
      req("generate_targeted", "{\"seed\":\"3\",\"target\":\"Easy\",\"max_attempts\":2}")};
  ref::Rng rng(31337);
  const std::string noise = "{}[]\",:\\ \n0123456789.-eEtrufalsn\x01\xff\xc3";
  int errors = 0, oks = 0;
  for (int trial = 0; trial < 1500; ++trial) {
    std::string text = seeds[static_cast<std::size_t>(rng.below(static_cast<int>(seeds.size())))];
    const int edits = 1 + rng.below(3);
    for (int e = 0; e < edits && !text.empty(); ++e) {
      const std::size_t pos = static_cast<std::size_t>(rng.below(static_cast<int>(text.size())));
      switch (rng.below(5)) {
        case 4: {   // change a letter/digit to another letter/digit: usually still valid JSON
          const char c = text[pos];
          if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) text[pos] = static_cast<char>(c >= '0' && c <= '9' ? '0' + rng.below(10) : 'a' + rng.below(26));
          break;
        }
        case 0: text.erase(pos, 1); break;
        case 1: text.insert(pos, 1, noise[static_cast<std::size_t>(rng.below(static_cast<int>(noise.size())))]); break;
        case 2: text[pos] = noise[static_cast<std::size_t>(rng.below(static_cast<int>(noise.size())))]; break;
        default: text.resize(pos); break;
      }
    }
    ProtocolOutcome out;
    bool threw = false;
    try { out = handle_request(text); } catch (...) { threw = true; }
    CHECK_CTX(!threw, "threw on: " + text.substr(0, 120));
    if (threw) continue;
    const auto doc = jsontest::parse(out.response_json);
    CHECK_CTX(doc.has_value(), "invalid response for: " + text.substr(0, 120));
    if (!doc) continue;
    const bool ok = doc->at("ok").boolean;
    (ok ? oks : errors)++;
    CHECK_CTX(out.exit_code >= 0 && out.exit_code <= 3, text.substr(0, 120));
    CHECK_CTX(ok == (out.exit_code == kExitSuccess || out.exit_code == kExitNonSuccess), text.substr(0, 120));
    CHECK_CTX(ok || doc->at("error").at("code").type == Value::Type::String, text.substr(0, 120));
    CHECK_CTX(out.exit_code != kExitInternalError, "internal error on: " + text.substr(0, 120));   // garbage must not be an internal defect
  }
  CHECK(errors > 300 && oks > 20);
  std::printf("fuzz: %d ok responses, %d error responses\n", oks, errors);
}

}  // namespace

int main() {
  test_reader();
  test_envelope_errors();
  test_field_validation();
  test_capabilities();
  test_generate_grid();
  test_generate_puzzle();
  test_solve_search();
  test_count_solutions();
  test_solve_logical();
  test_verify();
  test_classify();
  test_generate_targeted();
  test_other_contracts();
  test_mutation_fuzz();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
