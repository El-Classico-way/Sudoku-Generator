// Minimal JSON writer used for diagnostics and benchmark output. It only
// WRITES JSON (there is no parser here); it is deliberately small and is not the
// application protocol planned for a later milestone.
//
// Guarantees:
//   * output is syntactically valid JSON (commas and nesting handled for you);
//   * strings are escaped: " \ and control characters (< 0x20) as \b \f \n \r
//     \t or \u00XX. Bytes >= 0x80 are copied unchanged, so text must already be
//     UTF-8 (everything this project emits is ASCII);
//   * integers are written as JSON numbers; doubles use the shortest
//     round-trip form and are locale independent; a NaN or infinite double has no
//     JSON representation and is written as null (never as 0);
//   * misuse (value without a key inside an object, key outside an object,
//     unbalanced begin/end, reading an unfinished document) throws
//     std::logic_error instead of producing broken output.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku {

class JsonWriter {
 public:
  JsonWriter& begin_object() { before_value(); out_ += '{'; stack_.push_back({true, true, false}); return *this; }
  JsonWriter& end_object() { close('}', true); return *this; }
  JsonWriter& begin_array() { before_value(); out_ += '['; stack_.push_back({false, true, false}); return *this; }
  JsonWriter& end_array() { close(']', false); return *this; }

  JsonWriter& key(std::string_view name) {
    if (stack_.empty() || !stack_.back().object || stack_.back().key_pending) {
      throw std::logic_error("JsonWriter: key() only valid inside an object, before a value");
    }
    if (!stack_.back().first) out_ += ',';
    stack_.back().first = false;
    append_string(name);
    out_ += ':';
    stack_.back().key_pending = true;
    return *this;
  }

  JsonWriter& value(std::string_view s) { before_value(); append_string(s); return *this; }
  JsonWriter& value(const char* s) { return value(std::string_view(s)); }
  JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
  JsonWriter& value(bool b) { before_value(); out_ += b ? "true" : "false"; return *this; }
  JsonWriter& value(int v) { return value(static_cast<std::int64_t>(v)); }
  JsonWriter& value(std::int64_t v) { before_value(); out_ += std::to_string(v); return *this; }
  JsonWriter& value(std::uint64_t v) { before_value(); out_ += std::to_string(v); return *this; }
  JsonWriter& value(double v) {
    before_value();
    if (!std::isfinite(v)) { out_ += "null"; return *this; }
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
    return *this;
  }
  JsonWriter& null() { before_value(); out_ += "null"; return *this; }
  // Embeds text that is ALREADY valid JSON (for example another JsonWriter's
  // str()). The caller guarantees validity; it is not checked.
  JsonWriter& raw(std::string_view json) { before_value(); out_ += json; return *this; }

  // Convenience: key + value in one call.
  template <typename T>
  JsonWriter& field(std::string_view name, const T& v) { key(name); return value(v); }

  // The finished document. Throws if objects/arrays are still open or nothing was written.
  const std::string& str() const {
    if (!stack_.empty() || out_.empty()) throw std::logic_error("JsonWriter: document is not complete");
    return out_;
  }

 private:
  struct Frame { bool object; bool first; bool key_pending; };
  void before_value() {
    if (stack_.empty()) {
      if (!out_.empty()) throw std::logic_error("JsonWriter: only one top-level value is allowed");
      return;
    }
    Frame& f = stack_.back();
    if (f.object) {
      if (!f.key_pending) throw std::logic_error("JsonWriter: object value needs key() first");
      f.key_pending = false;
    } else {
      if (!f.first) out_ += ',';
      f.first = false;
    }
  }
  void close(char c, bool object) {
    if (stack_.empty() || stack_.back().object != object || stack_.back().key_pending) {
      throw std::logic_error("JsonWriter: mismatched end");
    }
    stack_.pop_back();
    out_ += c;
  }
  void append_string(std::string_view s) {
    out_ += '"';
    for (const char ch : s) {
      const auto c = static_cast<unsigned char>(ch);
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\b': out_ += "\\b"; break;
        case '\f': out_ += "\\f"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            static const char* hex = "0123456789abcdef";
            out_ += "\\u00";
            out_ += hex[c >> 4];
            out_ += hex[c & 0xF];
          } else {
            out_ += ch;
          }
      }
    }
    out_ += '"';
  }

  std::string out_;
  std::vector<Frame> stack_;
};

}  // namespace sudoku
