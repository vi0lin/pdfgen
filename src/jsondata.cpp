#include "jsondata.h"
#if PDFGEN_JSONDATA_API != 1
#error "version mismatch in jsondata.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jsondata {

const Value& Value::nullValue() { static Value v; return v; }

Value Value::object() { Value v; v.kind_ = Kind::Object; return v; }
Value Value::array()  { Value v; v.kind_ = Kind::Array;  return v; }
Value::Value(bool b) : kind_(Kind::Bool), b_(b) {}
Value::Value(double n) : kind_(Kind::Number), num_(n) {}
Value::Value(int n) : kind_(Kind::Number), num_(n) {}
Value::Value(const char* s) : kind_(Kind::String), str_(s ? s : "") {}
Value::Value(const std::string& s) : kind_(Kind::String), str_(s) {}

Value& Value::set(const std::string& key, Value v) {
  if (kind_ == Kind::Null) kind_ = Kind::Object;
  for (auto& kv : obj_)
    if (kv.first == key) { kv.second = std::move(v); return *this; }
  obj_.emplace_back(key, std::move(v));
  return *this;
}
Value& Value::push(Value v) {
  if (kind_ == Kind::Null) kind_ = Kind::Array;
  arr_.push_back(std::move(v));
  return *this;
}

size_t Value::size() const {
  return kind_ == Kind::Array ? arr_.size()
       : kind_ == Kind::Object ? obj_.size() : 0;
}
const Value& Value::index(size_t i) const {
  return kind_ == Kind::Array && i < arr_.size() ? arr_[i] : nullValue();
}
std::vector<std::string> Value::keys() const {
  std::vector<std::string> k;
  for (const auto& kv : obj_) k.push_back(kv.first);
  return k;
}

const Value& Value::at(const std::string& path) const {
  const Value* cur = this;
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '.') { ++i; continue; }
    if (path[i] == '[') {                           // [N]
      size_t e = path.find(']', i);
      if (e == std::string::npos) return nullValue();
      long n = std::atol(path.substr(i + 1, e - i - 1).c_str());
      cur = &cur->index((size_t)n);
      i = e + 1;
      continue;
    }
    size_t e = i;
    while (e < path.size() && path[e] != '.' && path[e] != '[') ++e;
    std::string key = path.substr(i, e - i);
    i = e;
    if (cur->kind_ == Kind::Array) {                // bare numeric step
      char* endp = nullptr;
      long n = std::strtol(key.c_str(), &endp, 10);
      if (endp && *endp == 0) { cur = &cur->index((size_t)n); continue; }
      return nullValue();
    }
    if (cur->kind_ != Kind::Object) return nullValue();
    const Value* next = nullptr;
    for (const auto& kv : cur->obj_)
      if (kv.first == key) { next = &kv.second; break; }
    if (!next) return nullValue();
    cur = next;
  }
  return *cur;
}

bool Value::truthy() const {
  switch (kind_) {
    case Kind::Null:   return false;
    case Kind::Bool:   return b_;
    case Kind::Number: return num_ != 0;
    case Kind::String: return !str_.empty();
    case Kind::Array:  return !arr_.empty();
    case Kind::Object: return !obj_.empty();
  }
  return false;
}

static void appendEscaped(std::string& o, const std::string& s) {
  o += '"';
  for (char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else o += c;
    }
  }
  o += '"';
}

static std::string numText(double n) {
  char b[32];
  std::snprintf(b, sizeof b, "%.10g", n);
  return b;
}

std::string Value::toText() const {
  switch (kind_) {
    case Kind::Null:   return "";
    case Kind::Bool:   return b_ ? "true" : "false";
    case Kind::Number: return numText(num_);
    case Kind::String: return str_;
    default:           return toJson();
  }
}

std::string Value::toJson() const {
  std::string o;
  switch (kind_) {
    case Kind::Null:   return "null";
    case Kind::Bool:   return b_ ? "true" : "false";
    case Kind::Number: return numText(num_);
    case Kind::String: appendEscaped(o, str_); return o;
    case Kind::Array:
      o += '[';
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) o += ',';
        o += arr_[i].toJson();
      }
      o += ']';
      return o;
    case Kind::Object:
      o += '{';
      for (size_t i = 0; i < obj_.size(); ++i) {
        if (i) o += ',';
        appendEscaped(o, obj_[i].first);
        o += ':';
        o += obj_[i].second.toJson();
      }
      o += '}';
      return o;
  }
  return o;
}

// ---- parser -----------------------------------------------------------------
namespace {
struct Parser {
  const std::string& s;
  size_t i = 0;
  std::string err;

  void skip() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {   // // comment
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      break;
    }
  }
  void fail(const char* msg) {
    if (!err.empty()) return;
    long line = 1;
    for (size_t k = 0; k < i && k < s.size(); ++k)
      if (s[k] == '\n') ++line;
    err = std::string(msg) + " (Zeile " + std::to_string(line) + ")";
  }
  bool lit(const char* w) {
    size_t n = std::strlen(w);
    if (s.compare(i, n, w) == 0) { i += n; return true; }
    return false;
  }
  Value value() {
    skip();
    if (i >= s.size()) { fail("unerwartetes Ende"); return {}; }
    char c = s[i];
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') return Value(string());
    if (lit("true"))  return Value(true);
    if (lit("false")) return Value(false);
    if (lit("null"))  return Value();
    if (c == '-' || std::isdigit((unsigned char)c)) {
      char* endp = nullptr;
      double n = std::strtod(s.c_str() + i, &endp);
      if (endp == s.c_str() + i) { fail("ungueltige Zahl"); return {}; }
      i = (size_t)(endp - s.c_str());
      return Value(n);
    }
    fail("unerwartetes Zeichen");
    return {};
  }
  std::string string() {
    std::string o;
    ++i;                                           // opening quote
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') { o += c; continue; }
      if (i >= s.size()) break;
      char e = s[i++];
      switch (e) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': o += '\r'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
          if (i + 4 <= s.size()) {
            unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(),
                                                 nullptr, 16);
            i += 4;
            if (cp < 0x80) o += (char)cp;          // UTF-8 encode (BMP only)
            else if (cp < 0x800) {
              o += (char)(0xC0 | (cp >> 6));
              o += (char)(0x80 | (cp & 0x3F));
            } else {
              o += (char)(0xE0 | (cp >> 12));
              o += (char)(0x80 | ((cp >> 6) & 0x3F));
              o += (char)(0x80 | (cp & 0x3F));
            }
          }
          break;
        }
        default: o += e;                           // \" \\ \/ and friends
      }
    }
    if (i < s.size()) ++i;                         // closing quote
    else fail("String nicht geschlossen");
    return o;
  }
  Value object() {
    Value v = Value::object();
    ++i;
    skip();
    if (i < s.size() && s[i] == '}') { ++i; return v; }
    while (i < s.size()) {
      skip();
      if (i >= s.size() || s[i] != '"') { fail("Objektschluessel erwartet"); return v; }
      std::string k = string();
      skip();
      if (i >= s.size() || s[i] != ':') { fail("':' erwartet"); return v; }
      ++i;
      v.set(k, value());
      skip();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; return v; }
      fail("',' oder '}' erwartet");
      return v;
    }
    fail("Objekt nicht geschlossen");
    return v;
  }
  Value array() {
    Value v = Value::array();
    ++i;
    skip();
    if (i < s.size() && s[i] == ']') { ++i; return v; }
    while (i < s.size()) {
      v.push(value());
      skip();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; return v; }
      fail("',' oder ']' erwartet");
      return v;
    }
    fail("Array nicht geschlossen");
    return v;
  }
};
}  // namespace

Value Value::parse(const std::string& text, std::string& err) {
  Parser p{text};
  Value v = p.value();
  p.skip();
  if (p.err.empty() && p.i < text.size()) p.fail("Daten nach dem Ende");
  err = p.err;
  return p.err.empty() ? v : Value();
}

}  // namespace jsondata
