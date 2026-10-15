// Small strict JSON parser for tests, independent of JsonWriter: it exists so the
// writer's output can be checked by something that did not produce it. Numbers
// are kept as their source text. It rejects trailing commas, leading zeros,
// control characters in strings, bad escapes and trailing garbage.
#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jsontest {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool boolean = false;
  std::string text;                                  // number source text or string contents
  std::vector<Value> items;                          // array
  std::vector<std::pair<std::string, Value>> members;  // object, in document order

  bool is_object() const { return type == Type::Object; }
  const Value* find(const std::string& key) const {
    for (const auto& [k, v] : members) if (k == key) return &v;
    return nullptr;
  }
  const Value& at(const std::string& key) const {
    const Value* v = find(key);
    if (!v) throw std::runtime_error("missing key: " + key);
    return *v;
  }
  bool has(const std::string& key) const { return find(key) != nullptr; }
  double number() const { return std::strtod(text.c_str(), nullptr); }
  long long integer() const { return std::strtoll(text.c_str(), nullptr, 10); }
  unsigned long long unsigned_integer() const { return std::strtoull(text.c_str(), nullptr, 10); }
  bool is_integer_text() const { return type == Type::Number && text.find_first_of(".eE") == std::string::npos; }
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}
  // nullopt if the text is not exactly one valid JSON value.
  std::optional<Value> parse() {
    try {
      Value v = value();
      skip();
      if (pos_ != s_.size()) return std::nullopt;
      return v;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

 private:
  [[noreturn]] void fail() const { throw std::runtime_error("json"); }
  void skip() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' || s_[pos_] == '\r')) ++pos_; }
  char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
  void expect(char c) { if (peek() != c) fail(); ++pos_; }
  bool literal(const char* lit) {
    std::size_t n = 0;
    while (lit[n]) ++n;
    if (s_.compare(pos_, n, lit) != 0) return false;
    pos_ += n;
    return true;
  }
  Value value() {
    skip();
    Value v;
    const char c = peek();
    if (c == '{') {
      ++pos_; v.type = Value::Type::Object; skip();
      if (peek() == '}') { ++pos_; return v; }
      for (;;) {
        skip();
        if (peek() != '"') fail();
        std::string key = string();
        skip(); expect(':');
        v.members.push_back({std::move(key), value()});
        skip();
        if (peek() == ',') { ++pos_; continue; }
        expect('}');
        return v;
      }
    }
    if (c == '[') {
      ++pos_; v.type = Value::Type::Array; skip();
      if (peek() == ']') { ++pos_; return v; }
      for (;;) {
        v.items.push_back(value());
        skip();
        if (peek() == ',') { ++pos_; continue; }
        expect(']');
        return v;
      }
    }
    if (c == '"') { v.type = Value::Type::String; v.text = string(); return v; }
    if (literal("true")) { v.type = Value::Type::Bool; v.boolean = true; return v; }
    if (literal("false")) { v.type = Value::Type::Bool; return v; }
    if (literal("null")) return v;
    if (c == '-' || (c >= '0' && c <= '9')) { v.type = Value::Type::Number; v.text = number(); return v; }
    fail();
  }
  std::string number() {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (peek() == '0') { ++pos_; }
    else if (peek() >= '1' && peek() <= '9') { while (peek() >= '0' && peek() <= '9') ++pos_; }
    else fail();
    if (peek() == '.') { ++pos_; if (!(peek() >= '0' && peek() <= '9')) fail(); while (peek() >= '0' && peek() <= '9') ++pos_; }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) fail();
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    return s_.substr(start, pos_ - start);
  }
  std::string string() {
    expect('"');
    std::string out;
    for (;;) {
      if (pos_ >= s_.size()) fail();
      const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
      if (c == '"') return out;
      if (c < 0x20) fail();
      if (c != '\\') { out += static_cast<char>(c); continue; }
      if (pos_ >= s_.size()) fail();
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
          if (pos_ + 4 > s_.size()) fail();
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else fail();
          }
          if (code > 0x7F) fail();  // the writer only emits \u00XX for control characters
          out += static_cast<char>(code);
          break;
        }
        default: fail();
      }
    }
  }
  const std::string& s_;
  std::size_t pos_ = 0;
};

inline std::optional<Value> parse(const std::string& s) { return Parser(s).parse(); }

}  // namespace jsontest
