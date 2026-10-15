// Small, strict JSON reader for the application protocol (requests are
// untrusted input). It accepts exactly RFC 8259 JSON and is stricter in the
// ways that matter for a protocol:
//   * exactly one value; anything but whitespace after it is an error;
//   * duplicate object keys are an error (no "last one wins" ambiguity);
//   * nesting is limited (JsonLimits::max_depth), so input cannot exhaust the stack;
//   * the text must be valid UTF-8 (overlong forms, surrogates and values above
//     U+10FFFF are rejected) and raw control characters are rejected in strings;
//   * \uXXXX escapes are decoded to UTF-8, surrogate pairs combined, lone
//     surrogates rejected;
//   * numbers follow the JSON grammar (no leading zeros, no "+", no NaN or
//     Infinity). They are kept as their source text, so integers are never
//     rounded through a double; use as_int64() to read an exact integer.
// Errors report a byte offset into the input. Never throws for bad input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sudoku {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  std::string text;                                    // String contents or Number source text
  std::vector<JsonValue> items;                        // Array
  std::vector<std::pair<std::string, JsonValue>> members;  // Object, in document order

  const JsonValue* find(std::string_view key) const;
  bool is_string() const noexcept { return type == Type::String; }
  bool is_object() const noexcept { return type == Type::Object; }
  bool is_array() const noexcept { return type == Type::Array; }
  bool is_bool() const noexcept { return type == Type::Bool; }
  // A Number written without fraction or exponent (e.g. 17, -3; not 17.0 or 1e2).
  bool is_integer() const noexcept;
  // The exact value of an integer that fits int64; nullopt otherwise (including
  // non-integers and out-of-range values).
  std::optional<std::int64_t> as_int64() const;
};

struct JsonParseError {
  std::string message;
  std::size_t offset = 0;
};

struct JsonLimits {
  std::size_t max_depth = 32;
};

// Parses `text` as exactly one JSON value. On failure returns nullopt and fills
// `error`.
std::optional<JsonValue> parse_json(std::string_view text, JsonParseError& error, JsonLimits limits = {});

}  // namespace sudoku
