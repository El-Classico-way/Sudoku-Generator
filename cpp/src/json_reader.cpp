#include "sudoku/json_reader.hpp"

#include <charconv>

namespace sudoku {

const JsonValue* JsonValue::find(std::string_view key) const {
  for (const auto& [k, v] : members) {
    if (k == key) return &v;
  }
  return nullptr;
}

bool JsonValue::is_integer() const noexcept {
  return type == Type::Number && text.find_first_of(".eE") == std::string::npos;
}

std::optional<std::int64_t> JsonValue::as_int64() const {
  if (!is_integer()) return std::nullopt;
  std::int64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) return std::nullopt;
  return value;
}

namespace {

struct Failure {
  std::string message;
  std::size_t offset;
};

class Reader {
 public:
  Reader(std::string_view s, JsonLimits limits) : s_(s), limits_(limits) {}

  JsonValue parse_document() {
    skip_ws();
    if (pos_ >= s_.size()) fail("empty input", pos_);
    JsonValue v = value(0);
    skip_ws();
    if (pos_ != s_.size()) fail("unexpected trailing input", pos_);
    return v;
  }

 private:
  [[noreturn]] static void fail(std::string message, std::size_t offset) { throw Failure{std::move(message), offset}; }

  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool at_end() const { return pos_ >= s_.size(); }
  char peek() const { return at_end() ? '\0' : s_[pos_]; }
  void expect(char c) {
    if (at_end() || s_[pos_] != c) fail(std::string("expected '") + c + "'", pos_);
    ++pos_;
  }
  bool literal(std::string_view word) {
    if (s_.compare(pos_, word.size(), word) != 0) return false;
    pos_ += word.size();
    return true;
  }

  JsonValue value(std::size_t depth) {
    skip_ws();
    if (at_end()) fail("unexpected end of input", pos_);
    const char c = s_[pos_];
    JsonValue v;
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') { v.type = JsonValue::Type::String; v.text = string(); return v; }
    if (c == 't') { if (!literal("true")) fail("invalid literal", pos_); v.type = JsonValue::Type::Bool; v.boolean = true; return v; }
    if (c == 'f') { if (!literal("false")) fail("invalid literal", pos_); v.type = JsonValue::Type::Bool; return v; }
    if (c == 'n') { if (!literal("null")) fail("invalid literal", pos_); return v; }
    if (c == '-' || (c >= '0' && c <= '9')) { v.type = JsonValue::Type::Number; v.text = number(); return v; }
    fail("unexpected character", pos_);
  }

  JsonValue object(std::size_t depth) {
    if (depth + 1 > limits_.max_depth) fail("nesting too deep", pos_);
    ++pos_;  // '{'
    JsonValue v;
    v.type = JsonValue::Type::Object;
    skip_ws();
    if (peek() == '}') { ++pos_; return v; }
    for (;;) {
      skip_ws();
      if (peek() != '"') fail("expected object key", pos_);
      const std::size_t key_pos = pos_;
      std::string key = string();
      if (v.find(key) != nullptr) fail("duplicate object key", key_pos);
      skip_ws();
      expect(':');
      JsonValue member = value(depth + 1);
      v.members.emplace_back(std::move(key), std::move(member));
      skip_ws();
      if (peek() == ',') { ++pos_; continue; }
      expect('}');
      return v;
    }
  }

  JsonValue array(std::size_t depth) {
    if (depth + 1 > limits_.max_depth) fail("nesting too deep", pos_);
    ++pos_;  // '['
    JsonValue v;
    v.type = JsonValue::Type::Array;
    skip_ws();
    if (peek() == ']') { ++pos_; return v; }
    for (;;) {
      v.items.push_back(value(depth + 1));
      skip_ws();
      if (peek() == ',') { ++pos_; continue; }
      expect(']');
      return v;
    }
  }

  static bool digit(char c) { return c >= '0' && c <= '9'; }

  std::string number() {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (peek() == '0') {
      ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (digit(peek())) ++pos_;
    } else {
      fail("invalid number", pos_);
    }
    if (peek() == '.') {
      ++pos_;
      if (!digit(peek())) fail("invalid number", pos_);
      while (digit(peek())) ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!digit(peek())) fail("invalid number", pos_);
      while (digit(peek())) ++pos_;
    }
    return std::string(s_.substr(start, pos_ - start));
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::uint32_t hex4() {
    if (pos_ + 4 > s_.size()) fail("truncated \\u escape", pos_);
    std::uint32_t code = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s_[pos_++];
      code <<= 4;
      if (h >= '0' && h <= '9') code |= static_cast<std::uint32_t>(h - '0');
      else if (h >= 'a' && h <= 'f') code |= static_cast<std::uint32_t>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') code |= static_cast<std::uint32_t>(h - 'A' + 10);
      else fail("invalid \\u escape", pos_ - 1);
    }
    return code;
  }

  // Validates one UTF-8 sequence starting at pos_ (first byte >= 0x80) and copies it.
  void copy_utf8(std::string& out) {
    const auto b0 = static_cast<unsigned char>(s_[pos_]);
    std::size_t len;
    std::uint32_t cp;
    if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; cp = b0 & 0x1F; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; cp = b0 & 0x0F; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; cp = b0 & 0x07; }
    else fail("invalid UTF-8", pos_);
    if (pos_ + len > s_.size()) fail("invalid UTF-8", pos_);
    for (std::size_t i = 1; i < len; ++i) {
      const auto b = static_cast<unsigned char>(s_[pos_ + i]);
      if ((b & 0xC0) != 0x80) fail("invalid UTF-8", pos_);
      cp = (cp << 6) | (b & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      fail("invalid UTF-8", pos_);
    }
    out.append(s_.substr(pos_, len));
    pos_ += len;
  }

  std::string string() {
    expect('"');
    std::string out;
    for (;;) {
      if (at_end()) fail("unterminated string", pos_);
      const auto c = static_cast<unsigned char>(s_[pos_]);
      if (c == '"') { ++pos_; return out; }
      if (c < 0x20) fail("control character in string", pos_);
      if (c >= 0x80) { copy_utf8(out); continue; }
      if (c != '\\') { out += static_cast<char>(c); ++pos_; continue; }
      ++pos_;
      if (at_end()) fail("unterminated string", pos_);
      const char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::uint32_t cp = hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF) {   // high surrogate: needs a low one
            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') fail("lone surrogate", pos_);
            pos_ += 2;
            const std::uint32_t low = hex4();
            if (low < 0xDC00 || low > 0xDFFF) fail("lone surrogate", pos_ - 6);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail("lone surrogate", pos_ - 6);
          }
          append_utf8(out, cp);
          break;
        }
        default: fail("invalid escape", pos_ - 1);
      }
    }
  }

  std::string_view s_;
  JsonLimits limits_;
  std::size_t pos_ = 0;
};

}  // namespace

std::optional<JsonValue> parse_json(std::string_view text, JsonParseError& error, JsonLimits limits) {
  try {
    return Reader(text, limits).parse_document();
  } catch (const Failure& f) {
    error.message = f.message;
    error.offset = f.offset;
    return std::nullopt;
  }
}

}  // namespace sudoku
