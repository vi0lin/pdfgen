#include "markup.h"
#include <algorithm>   // std::count/min/max (MSVC does not pull this in transitively)
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX            // keep windows.h from macro-izing min/max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#endif
#include <sstream>

// ---- release consistency check (see header comment) ----
#ifndef PDFGEN_MARKUP_API
#error "stale markup.h: it lacks PDFGEN_MARKUP_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_MARKUP_API != 17
#error "version mismatch in markup.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_FLOWABLES_API
#error "stale flowables.h: it lacks PDFGEN_FLOWABLES_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_FLOWABLES_API != 13
#error "version mismatch in flowables.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_PDFWRITER_API
#error "stale pdfwriter.h: it lacks PDFGEN_PDFWRITER_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_PDFWRITER_API != 7
#error "version mismatch in pdfwriter.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif


namespace markup {
namespace {

std::string toLower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool hasImageExtension(const std::string& path) {
  static const char* exts[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"};
  std::string low = toLower(path);
  for (const char* e : exts) {
    size_t n = std::strlen(e);
    if (low.size() > n && low.compare(low.size() - n, n, e) == 0) return true;
  }
  return false;
}

// Parses "[pic.png, width=3*cm, height=2*cm]" starting at s[pos] == '['.
// On success fills tok and sets `end` to the index just past ']'.
bool tryParseImageTag(const std::string& s, size_t pos, Token& tok, size_t& end) {
  size_t close = s.find(']', pos);
  if (close == std::string::npos) return false;
  std::string inner = s.substr(pos + 1, close - pos - 1);
  if (inner.find('[') != std::string::npos) return false;

  // split on commas
  std::vector<std::string> parts;
  std::stringstream ss(inner);
  std::string part;
  while (std::getline(ss, part, ',')) parts.push_back(trim(part));
  if (parts.empty() || !hasImageExtension(parts[0])) return false;

  tok = Token{};
  tok.kind = Token::Kind::Image;
  tok.text = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) {
    size_t eq = parts[i].find('=');
    if (eq == std::string::npos) return false;
    std::string key = toLower(trim(parts[i].substr(0, eq)));
    std::string val = trim(parts[i].substr(eq + 1));
    if (key == "align" || key == "ausrichtung") {
      std::string a = toLower(val);
      if      (a == "left"   || a == "links")  tok.align = 0;
      else if (a == "center" || a == "mitte")  tok.align = 1;
      else if (a == "right"  || a == "rechts") tok.align = 2;
      else return false;
      continue;
    }
    if (key == "float") {
      std::string side = toLower(val);
      if      (side == "left")  tok.floatSide = 1;
      else if (side == "right") tok.floatSide = 2;
      else return false;
      continue;
    }
    if (key == "layer" || key == "ebene") {
      std::string l = toLower(val);
      if      (l == "back" || l == "hinten")  tok.layerBack = true;
      else if (l == "front" || l == "vorne") tok.layerBack = false;
      else return false;
      continue;
    }
    if (key == "dpi" || key == "aufloesung") {
      char* endp = nullptr;
      double v = std::strtod(val.c_str(), &endp);
      if (endp == val.c_str() || *endp != '\0' || v <= 0) return false;
      tok.dpi = v;
      continue;
    }
    double dim;
    if (!evalDimension(val, dim)) return false;
    if      (key == "width")  tok.widthPt  = dim;
    else if (key == "height") tok.heightPt = dim;
    else if (key == "x")      { tok.absX = dim; tok.hasAbs = true; }
    else if (key == "y")      { tok.absY = dim; tok.hasAbs = true; }
    else if (key == "dx")     tok.dx = dim;
    else if (key == "dy")     tok.dy = dim;
    else return false;
  }
  end = close + 1;
  return true;
}

// "| a | b |" -> cells {"a","b"} with spans {1,1}.
// A ZERO-WIDTH segment between pipes ("||") is a span marker extending the
// previous cell by one column; "| |" (whitespace only) stays an empty cell.
void splitTableRow(const std::string& line,
                   std::vector<std::string>& cells, std::vector<int>& spans) {
  cells.clear(); spans.clear();
  std::string t = trim(line);
  size_t i = 1;                                 // skip leading '|'
  std::string raw;
  auto push = [&](const std::string& seg) {
    if (seg.empty() && !cells.empty()) { spans.back()++; return; }  // "||"
    cells.push_back(trim(seg));
    spans.push_back(1);
  };
  for (; i < t.size(); ++i) {
    if (t[i] == '|') { push(raw); raw.clear(); }
    else raw += t[i];
  }
  if (!trim(raw).empty()) push(raw);            // row without trailing '|'
}

// ":---" 0, ":--:" 1, "---:" 2; returns false if any cell isn't a separator.
bool parseSeparatorRow(const std::vector<std::string>& cells, std::vector<int>& aligns) {
  if (cells.empty()) return false;
  aligns.clear();
  for (const auto& c : cells) {
    size_t a = 0, b = c.size();
    bool l = a < b && c[a] == ':', r = b > a && c[b-1] == ':';
    if (l) ++a;
    if (r) --b;
    if (b - a < 1) return false;
    for (size_t i = a; i < b; ++i) if (c[i] != '-') return false;
    aligns.push_back(l && r ? 1 : (r ? 2 : 0));
  }
  return true;
}

int headingLevel(const std::string& line, std::string& title) {
  size_t i = 0;
  while (i < line.size() && i < 5 && line[i] == '#') ++i;
  if (i == 0 || (i < line.size() && line[i] == '#')) return 0;  // none or ######
  title = trim(line.substr(i));
  return (int)i;
}

} // namespace

// ---- dimension expressions -----------------------------------------------------
bool evalDimension(const std::string& expr, double& outPt) {
  // grammar: factor ('*' factor)* ; factor = number | unit
  static const struct { const char* name; double pts; } units[] = {
    {"cm", pdf::CM}, {"mm", pdf::MM}, {"inch", pdf::INCH},
    {"in", pdf::INCH}, {"pica", pdf::PICA}, {"pt", pdf::PT},
  };
  double result = 1.0;
  bool any = false;
  std::stringstream ss(expr);
  std::string factor;
  while (std::getline(ss, factor, '*')) {
    factor = toLower(trim(factor));
    if (factor.empty()) return false;
    bool matched = false;
    for (auto& u : units)
      if (factor == u.name) { result *= u.pts; matched = true; break; }
    if (!matched) {
      char* endp = nullptr;
      double v = std::strtod(factor.c_str(), &endp);
      if (endp == factor.c_str()) return false;
      if (*endp != '\0') {
        // "<number><unit>" glued together, e.g. "0mm", "2mm", "0.5cm"
        std::string suffix = endp;
        bool unitOk = false;
        for (auto& u : units)
          if (suffix == u.name) { v *= u.pts; unitOk = true; break; }
        if (!unitOk) return false;
      }
      result *= v;
    }
    any = true;
  }
  if (!any) return false;
  outPt = result;
  return true;
}

static void localTime(std::time_t t, std::tm& tm) {
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
}

std::string todayString() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localTime(t, tm);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%02d.%02d.%04d",
                tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
  return buf;
}

// ---- stage -1: comments + conditional rendering ----------------------------------
namespace {

// Evaluates "embedded" / "!embedded"; unknown names warn and fail.
bool evalCondition(const std::string& expr, bool embedded,
                   std::vector<std::string>& warnings) {
  std::string e = trim(expr);
  bool neg = !e.empty() && e[0] == '!';
  if (neg) e = trim(e.substr(1));
  bool value;
  if (toLower(e) == "embedded") value = embedded;
  else {
    warnings.push_back("unknown condition: " + expr);
    value = false;
  }
  return neg ? !value : value;
}

// For one "[...]" tag text: returns 0 = no condition, 1 = passes (tag
// rewritten without the option), -1 = fails.
int handleTagCondition(std::string& tagText, bool embedded,
                       std::vector<std::string>& warnings) {
  size_t open = tagText.find('['), close = tagText.rfind(']');
  if (open == std::string::npos || close == std::string::npos || close < open)
    return 0;
  std::string inner = tagText.substr(open + 1, close - open - 1);
  std::vector<std::string> parts;
  std::string cur;
  bool inQ = false;
  for (char c : inner) {
    if (c == '"') inQ = !inQ;
    if (c == ',' && !inQ) { parts.push_back(cur); cur.clear(); }
    else cur += c;
  }
  parts.push_back(cur);
  int found = 0;
  std::string rebuilt;
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string t = trim(parts[i]);
    std::string low = toLower(t);
    if (low.rfind("condition", 0) == 0 &&
        t.find('=') != std::string::npos &&
        toLower(trim(t.substr(0, t.find('=')))) == "condition") {
      std::string expr = trim(t.substr(t.find('=') + 1));
      found = evalCondition(expr, embedded, warnings) ? 1 : -1;
      continue;                                  // drop the option
    }
    if (!rebuilt.empty()) rebuilt += ", ";
    rebuilt += t;
  }
  if (found == 1)
    tagText = tagText.substr(0, open) + "[" + rebuilt + "]" +
              tagText.substr(close + 1);
  return found;
}

} // namespace

// Translates the backslash line-tag syntax into the internal bracket form.
// \t/\\t, @, \s and \c are handled natively later and pass through.
std::string translateBackslashTags(const std::string& text,
                                   std::vector<std::string>& warnings) {
  std::stringstream ss(text);
  std::string line, out;
  bool first = true;
  auto emit = [&](const std::string& l) {
    if (!first) out += "\n";
    out += l;
    first = false;
  };
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = trim(line);
    if (t.size() < 2 || t[0] != '\\') { emit(line); continue; }

    bool closing = t[1] == '\\';
    size_t ws = closing ? 2 : 1;
    std::string word;
    while (ws < t.size() && (std::isalpha((unsigned char)t[ws]) || t[ws] == '_'))
      word += (char)std::tolower((unsigned char)t[ws++]);
    std::string rest = trim(t.substr(ws));
    if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));

    // native pass-through: block tables, spacing, conditional blocks
    if (word == "t" || word == "table" || word == "tabelle" ||
        word == "s" || word == "c" || word == "loop" || word == "file")
      { emit(line); continue; }

    static const struct { const char* w; const char* name; bool headVal; bool block; }
    map[] = {
      {"g", "group", false, true},  {"group", "group", false, true},
      {"gruppe", "group", false, true},
      {"i", "", true, false},       {"image", "", true, false},
      {"bild", "", true, false},
      {"r", "row", false, false},   {"row", "row", false, false},
      {"n", "newpage", false, false}, {"newpage", "newpage", false, false},
      {"neueseite", "newpage", false, false},
      {"m", "margins", false, false}, {"margins", "margins", false, false},
      {"h", "header", false, true}, {"header", "header", false, true},
      {"b", "bottom", false, true}, {"bottom", "bottom", false, true},
      {"e", "embed", true, false},  {"embed", "embed", true, false},
      {"p", "pdf", true, false},    {"pdf", "pdf", true, false},
      {"pdfgen", "pdfgen", true, false},
      {"mail", "mail", true, false},
      {"attach", "attach", false, false},
    };
    int hitIdx = -1;
    for (int mi = 0; mi < (int)(sizeof(map) / sizeof(map[0])); ++mi)
      if (word == map[mi].w) { hitIdx = mi; break; }
    if (hitIdx < 0) { emit(line); continue; }    // unknown: literal text
    const auto& hit = map[hitIdx];

    if (closing) {
      if (hit.block) emit("[/" + std::string(hit.name) + "]");
      else warnings.push_back("\\\\" + word + ": this tag has no closing form");
      continue;
    }
    std::string head = hit.name;
    if (hit.headVal && !rest.empty()) {
      // first comma part without '=' becomes the tag's value
      size_t comma = rest.find(',');
      std::string firstPart = trim(rest.substr(0, comma));
      if (firstPart.find('=') == std::string::npos && !firstPart.empty()) {
        head = head.empty() ? firstPart : head + "=" + firstPart;
        rest = comma == std::string::npos ? "" : trim(rest.substr(comma + 1));
      }
    }
    if (head.empty()) {                          // \i without a path
      warnings.push_back("\\" + word + ": missing value");
      continue;
    }
    emit(rest.empty() ? "[" + head + "]" : "[" + head + ", " + rest + "]");
  }
  if (!text.empty() && text.back() == '\n') out += "\n";
  return out;
}

// \[ and \] produce literal brackets: swapped to sentinel bytes here so no
// pipeline stage (includes, dates, tokenizer, cells) can mistake them for a
// tag, and swapped back just before the text reaches a paragraph.
// ---- \on / \off: verbatim mode ---------------------------------------------------
// Runs before every other stage. Literal lines get a \x03 prefix plus
// [ ] -> \x01/\x02 so no later stage interprets them; the tokenizer strips
// the prefix and treats such lines as plain text.
namespace {

std::string unescapeDelim(const std::string& v);   // defined with \loop below

struct OnOffScope {
  enum Kind { Table, Loop, File, Delim } kind;
  bool saved;                 // state to restore when the scope closes
  std::string delim;          // Delim: raw delimiter string
  int blankRun = 0;           // Delim with \n-only delimiters
};

std::string literalLine(const std::string& line) {
  std::string out;
  out += '\x03';
  for (char c : line) {
    if (c == '[') out += '\x01';
    else if (c == ']') out += '\x02';
    else out += c;
  }
  return out;
}

// which block does this (trimmed, lowercased) line start / end?
int blockStart(const std::string& low) {          // 1 table, 2 loop, 3 file
  auto word = [&](const char* w) {
    size_t n = std::strlen(w);
    return low.rfind(w, 0) == 0 &&
           (low.size() == n || low[n] == ' ' || low[n] == ',' || low[n] == '\t');
  };
  if (word("\\t") || word("\\table") || word("\\tabelle")) return 1;
  if (word("\\loop")) return 2;
  if (word("\\file")) return 3;
  return 0;
}
int blockEnd(const std::string& low) {
  if (low == "\\\\t" || low == "\\\\table" || low == "\\\\tabelle") return 1;
  if (low == "\\\\loop") return 2;
  if (low == "\\\\file") return 3;
  return 0;
}

std::string processOnOff(const std::string& text,
                         std::vector<std::string>& warnings) {
  if (text.find("\\on") == std::string::npos &&
      text.find("\\off") == std::string::npos)
    return text;                                   // fast path

  std::vector<std::string> lines;
  {
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  bool state = true;                               // parsing on
  std::vector<OnOffScope> scopes;
  std::string out;
  auto emit = [&](const std::string& l) { out += l; out += '\n'; };

  for (auto& raw : lines) {
    std::string t = trim(raw);
    std::string low = toLower(t);

    // delimiter scopes close on matching SOURCE lines (before anything else)
    if (!scopes.empty() && scopes.back().kind == OnOffScope::Delim) {
      auto& d = scopes.back();
      bool hit = false;
      bool nlOnly = !d.delim.empty() &&
                    d.delim.find_first_not_of('\n') == std::string::npos;
      if (nlOnly) {
        if (t.empty()) {
          if (++d.blankRun >= (int)d.delim.size() - 1) hit = true;
        } else d.blankRun = 0;
      } else if (t == d.delim) hit = true;
      if (hit) { state = d.saved; scopes.pop_back(); }
    }

    // leading \on / \off token?
    bool tok = false, tokState = true, force = false;
    std::string rest;
    if (low.rfind("\\on", 0) == 0 || low.rfind("\\off", 0) == 0) {
      size_t n = low.rfind("\\off", 0) == 0 ? 4 : 3;
      tokState = n == 3;
      if (n <= t.size() && t.size() > n && t[n] == '!') { force = true; ++n; }
      if (t.size() == n || t[n] == ' ' || t[n] == '\t') {
        tok = true;
        rest = trim(t.substr(n));
      }
    }

    if (tok) {
      // "D=..." option on a standalone switch: delimiter-bounded scope
      std::string lrest = toLower(rest);
      if (!rest.empty() &&
          (lrest.rfind("d=", 0) == 0 || lrest.rfind("delim=", 0) == 0 ||
           lrest.rfind("trenner=", 0) == 0)) {
        OnOffScope sc{OnOffScope::Delim, state, {}, 0};
        sc.delim = unescapeDelim(trim(rest.substr(rest.find('=') + 1)));
        scopes.push_back(sc);
        state = tokState;
        continue;
      }
      if (force) {
        for (auto& sc : scopes) sc.saved = tokState;   // through all levels
        state = tokState;
        if (rest.empty()) continue;
      }
      if (rest.empty()) { state = tokState; continue; }

      // prefix form: applies to this line -- or to the whole block it opens
      std::string rlow = toLower(rest);
      if (int b = blockStart(rlow)) {
        scopes.push_back({(OnOffScope::Kind)(b - 1), state, {}, 0});
        state = tokState;
        if (state) emit(rest);
        else emit(literalLine(rest));
        continue;
      }
      if (tokState) emit(rest);                     // one line on
      else emit(literalLine(rest));                 // one line off
      continue;
    }

    // block structure (tracked in BOTH states, so inner switches can scope)
    if (int b = blockStart(low)) {
      scopes.push_back({(OnOffScope::Kind)(b - 1), state, {}, 0});
      if (state) emit(raw); else emit(literalLine(raw));
      continue;
    }
    if (int b = blockEnd(low)) {
      if (state) emit(raw); else emit(literalLine(raw));
      for (size_t i = scopes.size(); i-- > 0;) {
        if ((int)scopes[i].kind == b - 1) {
          state = scopes[i].saved;
          scopes.resize(i);
          break;
        }
      }
      continue;
    }

    if (state || t.empty()) emit(raw);
    else emit(literalLine(raw));
  }
  if (!text.empty() && text.back() != '\n' && !out.empty()) out.pop_back();
  return out;
}

} // namespace

// ---- GitHub-flavored markdown -----------------------------------------------------
namespace {

// inline transforms on one line: code spans, emphasis, strike, links,
// autolinks, images, escapes. Order matters: escapes & code spans first.
std::string mdInline(const std::string& line) {
  std::string out;
  size_t i = 0, n = line.size();
  bool bold = false, ital = false, strike = false;
  auto starts = [&](const char* t) {
    size_t l = std::strlen(t);
    return i + l <= n && line.compare(i, l, t) == 0;
  };
  while (i < n) {
    char c = line[i];
    // backslash escapes for md metacharacters
    if (c == '\\' && i + 1 < n) {
      char e = line[i + 1];
      if (e == '*' || e == '_' || e == '`' || e == '~' || e == '|' ||
          e == '#' || e == '!' || e == '(' || e == ')') {
        out += e;
        i += 2;
        continue;
      }
    }
    if (c == '`') {                                // inline code span
      size_t close = line.find('`', i + 1);
      if (close != std::string::npos) {
        out += "<code>";
        for (size_t k = i + 1; k < close; ++k) {
          char cc = line[k];
          if (cc == '<') out += '\x06';           // keep <b> etc. literal
          else if (cc == '[') out += '\x01';
          else if (cc == ']') out += '\x02';
          else out += cc;
        }
        out += "</code>";
        i = close + 1;
        continue;
      }
    }
    if (starts("~~")) {
      out += strike ? "</s>" : "<s>";
      strike = !strike;
      i += 2;
      continue;
    }
    if (starts("***")) {                           // bold+italic together
      out += (bold || ital) ? "</i></b>" : "<b><i>";
      bold = ital = !bold;
      i += 3;
      continue;
    }
    if (starts("**") || starts("__")) {
      out += bold ? "</b>" : "<b>";
      bold = !bold;
      i += 2;
      continue;
    }
    auto wordy = [&](size_t k) {
      return k < n && (std::isalnum((unsigned char)line[k]) || line[k] == '.');
    };
    if ((c == '*' || c == '_') &&
        // intraword stars/underscores stay literal: 3.3*cm, file_name_x
        !(i > 0 && wordy(i - 1) && wordy(i + 1))) {
      // emphasis only when it can open before/close after non-space
      bool canOpen  = i + 1 < n && !std::isspace((unsigned char)line[i + 1]);
      bool canClose = ital && i > 0 && !std::isspace((unsigned char)line[i - 1]);
      if (canClose) { out += "</i>"; ital = false; ++i; continue; }
      if (canOpen && !ital &&
          line.find(c, i + 1) != std::string::npos) {
        out += "<i>";
        ital = true;
        ++i;
        continue;
      }
    }
    if (starts("![")) {                            // image -> pdfgen tag
      size_t rb = line.find("](", i + 2);
      size_t end = rb == std::string::npos ? std::string::npos
                                           : line.find(')', rb + 2);
      if (end != std::string::npos) {
        out += "[" + trim(line.substr(rb + 2, end - rb - 2)) + "]";
        i = end + 1;
        continue;
      }
    }
    if (c == '[') {                                // [text](url)
      size_t rb = line.find("](", i + 1);
      size_t end = rb == std::string::npos ? std::string::npos
                                           : line.find(')', rb + 2);
      if (rb != std::string::npos && end != std::string::npos) {
        std::string txt = line.substr(i + 1, rb - i - 1);
        std::string url = trim(line.substr(rb + 2, end - rb - 2));
        out += "<a=" + url + ">" + txt + "</a>";
        i = end + 1;
        continue;
      }
    }
    if (c == '<' && (starts("<http://") || starts("<https://"))) {
      size_t end = line.find('>', i);
      if (end != std::string::npos) {
        std::string url = line.substr(i + 1, end - i - 1);
        out += "<a=" + url + ">" + url + "</a>";
        i = end + 1;
        continue;
      }
    }
    out += c;
    ++i;
  }
  if (bold)   out += "</b>";
  if (ital)   out += "</i>";
  if (strike) out += "</s>";
  return out;
}

bool isHRuleLine(const std::string& t) {
  if (t.size() < 3) return false;
  char c = t[0];
  if (c != '-' && c != '*' && c != '_') return false;
  for (char x : t)
    if (x != c && x != ' ') return false;
  return true;
}

// list bullet? returns marker length and fills bullet text
int listMarker(const std::string& t, std::string& bullet, bool& ordered) {
  ordered = false;
  if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') &&
      t[1] == ' ') {
    // task list?
    if (t.size() >= 6 && t.compare(1, 4, " [ ]") == 0 &&
        (t.size() == 6 || t[6] != '\0')) { bullet = "\xE2\x98\x90"; return 6; }
    if (t.size() >= 6 && (t.compare(1, 4, " [x]") == 0 ||
                          t.compare(1, 4, " [X]") == 0)) {
      bullet = "\xE2\x98\x91";
      return 6;
    }
    bullet = "\xE2\x80\xA2";                       // bullet dot
    return 2;
  }
  size_t d = 0;
  while (d < t.size() && std::isdigit((unsigned char)t[d])) ++d;
  if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') &&
      t[d + 1] == ' ') {
    bullet = t.substr(0, d + 1);
    ordered = true;
    return (int)d + 2;
  }
  return 0;
}

} // namespace

// GitHub-flavored markdown: fenced code -> verbatim code lines (\x04),
// inline markup -> pdfgen tags, lists/quotes/rules -> styled lines. With
// mdLineSemantics, single newlines inside a paragraph become soft.
std::string translateMarkdown(const std::string& text, bool mdLineSemantics,
                              std::vector<std::string>& warnings) {
  (void)warnings;
  std::vector<std::string> lines;
  {
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  std::string out;
  auto emit = [&](const std::string& l) { out += l; out += '\n'; };
  bool inFence = false;
  int tableDepth = 0;                              // \t ... \\t nesting
  bool prevSoftText = false;                       // md line joining

  for (size_t li = 0; li < lines.size(); ++li) {
    const std::string& raw = lines[li];
    std::string t = trim(raw);

    if (!t.empty() && t[0] == '\x03') {            // \off verbatim: untouched
      emit(raw);
      prevSoftText = false;
      continue;
    }
    // fences
    if (t.rfind("```", 0) == 0) {
      inFence = !inFence;
      prevSoftText = false;
      continue;                                    // fence line disappears
    }
    if (inFence) {
      std::string code = "\x04";
      for (char c : raw) {                         // raw: keep indentation
        if (c == '[') code += '\x01';
        else if (c == ']') code += '\x02';
        else if (c == '<') code += '\x06';
        else code += c;
      }
      emit(code);
      prevSoftText = false;
      continue;
    }
    std::string low = toLower(t);
    {
      auto tword = [&](const char* w) {
        size_t wl = std::strlen(w);
        return low.rfind(w, 0) == 0 &&
               (low.size() == wl || low[wl] == ' ' || low[wl] == ',' ||
                low[wl] == '\t');
      };
      if (low == "\\\\t" || low == "\\\\table" || low == "\\\\tabelle") {
        if (tableDepth) --tableDepth;
      } else if (tword("\\t") || tword("\\table") || tword("\\tabelle")) {
        ++tableDepth;
      }
    }
    bool inTable = tableDepth > 0 || (!t.empty() && t[0] == '|');

    // structural markdown (outside tables and pdfgen tag lines)
    bool tagLine = !t.empty() && (t[0] == '[' || t[0] == '\\' || t[0] == '@' ||
                                  t[0] == '#');
    if (!inTable && !tagLine) {
      if (isHRuleLine(t)) {
        emit("\x08rule");                          // token for the tokenizer
        prevSoftText = false;
        continue;
      }
      // blockquote
      if (!t.empty() && t[0] == '>') {
        std::string body = trim(t.substr(1));
        emit("\x08q " + mdInline(body));
        prevSoftText = false;
        continue;
      }
      // lists with 2-space nesting
      size_t ind = 0;
      while (ind < raw.size() && raw[ind] == ' ') ++ind;
      std::string bullet;
      bool ordered = false;
      int ml = listMarker(t, bullet, ordered);
      if (ml > 0) {
        int level = (int)(ind / 2);
        emit("\x08l" + std::to_string(level) + " " + bullet + "\x07" +
             mdInline(trim(t.substr(ml))));
        prevSoftText = false;
        continue;
      }
    }

    // plain line: inline markdown; whole-line tags stay untouched so no
    // parameter (width=3.3*cm, urls, formats) can ever be mangled
    bool protectedTag =
        !t.empty() &&
        (t[0] == '\\' ||
         (t[0] == '[' && t.back() == ']'));   // whole-line bracket tag;
                                             // "[Link](url) ..." stays markdown
    std::string conv = protectedTag ? raw : mdInline(raw);
    if (mdLineSemantics && !inTable && !tagLine && !t.empty()) {
      bool hardBreak = raw.size() >= 2 &&
                       raw.compare(raw.size() - 2, 2, "  ") == 0;
      if (!raw.empty() && raw.back() == '\\') {
        hardBreak = true;
        conv = mdInline(raw.substr(0, raw.size() - 1));
      }
      if (prevSoftText && !out.empty() && out.back() == '\n') {
        out.pop_back();                            // join with a space
        out += ' ';
      }
      emit(trim(conv));
      prevSoftText = !hardBreak;
      continue;
    }
    prevSoftText = false;
    emit(conv);
  }
  if (!text.empty() && text.back() != '\n' && !out.empty()) out.pop_back();
  return out;
}

static std::string escapeLiteralBrackets(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() &&
        (text[i + 1] == '[' || text[i + 1] == ']')) {
      out += text[i + 1] == '[' ? '\x01' : '\x02';
      ++i;
    } else out += text[i];
  }
  return out;
}

std::string unescapeLiteralBrackets(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\x01') out += '[';
    else if (c == '\x02') out += ']';
    else if (c == '\x03') continue;               // verbatim-line marker
    else out += c;
  }
  return out;
}

// ---- styles / mementos / forward -------------------------------------------------
namespace {

using ParamList = std::vector<std::pair<std::string, std::string>>;

// comma split honoring double quotes
std::vector<std::string> splitParams(const std::string& inner) {
  std::vector<std::string> parts;
  std::string cur;
  bool inQ = false;
  for (char c : inner) {
    if (c == '"') inQ = !inQ;
    if (c == ',' && !inQ) { parts.push_back(trim(cur)); cur.clear(); }
    else cur += c;
  }
  parts.push_back(trim(cur));
  return parts;
}

void applyParam(ParamList& eff, const std::string& key, const std::string& val) {
  for (auto& kv : eff)
    if (kv.first == key) { kv.second = val; return; }
  eff.push_back({key, val});
}

struct StyleState {
  std::map<std::string, ParamList> mementos;
  std::map<std::string, ParamList> forwardCache;   // per element kind
};

// Applies forward/style/name/clean to ONE tag's parameter list.
ParamList resolveStyledParams(const std::string& kind,
                              const std::vector<std::string>& rawParams,
                              StyleState& st,
                              std::vector<std::string>& warnings) {
  bool hasClean = false, hasForward = false;
  std::string mementoName;
  // pre-scan for clean (it decides the base)
  for (const auto& p : rawParams)
    if (toLower(p) == "clean") hasClean = true;

  ParamList eff;
  if (!hasClean) {
    auto it = st.forwardCache.find(kind);
    if (it != st.forwardCache.end()) eff = it->second;
  }
  for (const auto& p : rawParams) {
    if (p.empty()) continue;
    size_t eq = p.find('=');
    std::string key = toLower(trim(eq == std::string::npos ? p : p.substr(0, eq)));
    std::string val = eq == std::string::npos ? "" : trim(p.substr(eq + 1));
    if (key == "clean")   continue;
    if (key == "forward") { hasForward = true; continue; }
    if (key == "name")    { mementoName = val; continue; }
    if (key == "style" || key == "stil") {
      auto it = st.mementos.find(val);
      if (it == st.mementos.end())
        warnings.push_back("style '" + val + "' is not defined (yet)");
      else
        for (const auto& kv : it->second) applyParam(eff, kv.first, kv.second);
      continue;
    }
    applyParam(eff, key, val);
  }
  if (hasClean)   st.forwardCache[kind].clear();
  if (hasForward) st.forwardCache[kind] = eff;      // accumulative by design
  if (!mementoName.empty()) st.mementos[mementoName] = eff;
  return eff;
}

std::string joinStyled(const std::string& head, const ParamList& eff) {
  std::string out = head;
  for (const auto& kv : eff) {
    out += out.empty() ? "" : ", ";
    out += kv.second.empty() ? kv.first : kv.first + "=" + kv.second;
  }
  return out;
}

bool looksLikeImageHead(const std::string& head) {
  std::string low = toLower(head);
  for (const char* ext : {".png", ".jpg", ".jpeg", ".webp", ".bmp"}) {
    size_t n = std::strlen(ext);
    if (low.size() > n && low.compare(low.size() - n, n, ext) == 0) return true;
  }
  return false;
}

} // namespace

// Rewrites every whole-line tag applying forward/style=/name=/clean.
std::string applyStyles(const std::string& text,
                        std::vector<std::string>& warnings) {
  StyleState st;
  std::vector<std::string> lines;
  {
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  std::string out;
  auto emit = [&](const std::string& l) { out += l; out += '\n'; };

  for (size_t li = 0; li < lines.size(); ++li) {
    std::string t = trim(lines[li]);

    // native \t table start (single backslash!)
    if (t.size() >= 2 && t[0] == '\\' && t[1] != '\\') {
      std::string low = toLower(t);
      bool isT = low == "\\t" || low.rfind("\\t ", 0) == 0 ||
                 low.rfind("\\t,", 0) == 0 ||
                 low.rfind("\\table", 0) == 0 || low.rfind("\\tabelle", 0) == 0;
      bool isLoop = low.rfind("\\loop", 0) == 0 || low.rfind("\\file", 0) == 0;
      if (isT || isLoop) {
        size_t sp = t.find_first_of(" ,");
        std::string word = sp == std::string::npos ? t : t.substr(0, sp);
        std::string rest = sp == std::string::npos ? "" : trim(t.substr(sp + 1));
        if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));
        auto parts = splitParams(rest);
        std::string head;
        std::string kind = isT ? "table"
                         : (low.rfind("\\file", 0) == 0 ? "file" : "loop");
        if (isLoop && !parts.empty() &&
            parts[0].find('=') == std::string::npos && !parts[0].empty()) {
          head = parts[0];                          // loop data file
          parts.erase(parts.begin());
        }
        ParamList eff = resolveStyledParams(kind, parts, st, warnings);
        std::string joined = joinStyled(head, eff);
        emit(joined.empty() ? word : word + " " + joined);
        continue;
      }
      emit(lines[li]);
      continue;
    }

    // whole-line bracket tag (may span lines until its ']')
    if (!t.empty() && t[0] == '[' && t.rfind("[/", 0) != 0) {
      std::string tagText = t;
      size_t consumedTo = li;
      while (tagText.find(']') == std::string::npos && consumedTo + 1 < lines.size()) {
        ++consumedTo;
        tagText += " " + trim(lines[consumedTo]);
      }
      size_t close = tagText.find(']');
      if (close != std::string::npos && close == tagText.size() - 1) {
        std::string inner = tagText.substr(1, tagText.size() - 2);
        auto parts = splitParams(inner);
        std::string head = parts.empty() ? "" : parts[0];
        if (!parts.empty()) parts.erase(parts.begin());
        std::string kind;
        {
          size_t eq = head.find('=');
          kind = toLower(trim(eq == std::string::npos ? head : head.substr(0, eq)));
          if (looksLikeImageHead(eq == std::string::npos ? head : head.substr(0, eq)) ||
              looksLikeImageHead(head))
            kind = "image";
        }
        ParamList eff = resolveStyledParams(kind, parts, st, warnings);
        emit("[" + joinStyled(head, eff) + "]");
        li = consumedTo;
        continue;
      }
    }
    emit(lines[li]);
  }
  if (!text.empty() && text.back() != '\n' && !out.empty()) out.pop_back();
  return out;
}

std::string preprocessSource(const std::string& text, bool embedded,
                             std::vector<std::string>& warnings,
                             bool mdLineSemantics) {
  std::stringstream ss(applyStyles(
      escapeLiteralBrackets(translateBackslashTags(
          translateMarkdown(processOnOff(text, warnings), mdLineSemantics,
                            warnings), warnings)), warnings));
  std::string line, out;
  bool skipTableRows = false;
  std::vector<bool> condStack;                   // \c blocks
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = trim(line);

    // \c <expr> ... \\c : conditional block
    if (t.rfind("\\\\c", 0) == 0 && trim(t.substr(3)).empty()) {
      if (condStack.empty())
        warnings.push_back("\\\\c without \\c");
      else
        condStack.pop_back();
      continue;
    }
    if (t.rfind("\\c", 0) == 0 && (t.size() == 2 || t[2] == ' ' || t[2] == '\t')) {
      std::string expr = trim(t.substr(2));
      if (expr.empty()) {
        warnings.push_back("\\c needs a condition (embedded / !embedded)");
        condStack.push_back(true);
      } else {
        condStack.push_back(evalCondition(expr, embedded, warnings));
      }
      continue;
    }
    bool suppressed = false;
    for (bool ok : condStack)
      if (!ok) { suppressed = true; break; }
    if (suppressed) continue;

    if (skipTableRows) {                         // rows of a hidden table
      if (!t.empty() && (t[0] == '|' || toLower(t).rfind("[row", 0) == 0))
        continue;
      skipTableRows = false;
    }
    if (t.rfind("//", 0) == 0) continue;         // whole-line comment

    // conditions on every [...] occurrence in the line
    std::string rebuilt;
    size_t pos = 0;
    bool dropLine = false;
    while (pos < line.size()) {
      size_t br = line.find('[', pos);
      if (br == std::string::npos) { rebuilt.append(line, pos, std::string::npos); break; }
      size_t close = line.find(']', br);
      if (close == std::string::npos) { rebuilt.append(line, pos, std::string::npos); break; }
      rebuilt.append(line, pos, br - pos);
      std::string tag = line.substr(br, close - br + 1);
      // [pdfgen] (without include) / [mail] / [attach] are project control
      // tags whose conditions must skip whole SECTIONS — the project parser
      // handles those itself, so leave them untouched here
      {
        std::string lowT = toLower(tag);
        bool ctrl = (lowT.rfind("[pdfgen", 0) == 0 &&
                     lowT.find("include") == std::string::npos) ||
                    lowT.rfind("[mail", 0) == 0 ||
                    lowT.rfind("[attach", 0) == 0;
        if (ctrl) { rebuilt += tag; pos = close + 1; continue; }
      }
      int r = handleTagCondition(tag, embedded, warnings);
      if (r >= 0) rebuilt += tag;
      else {
        // failing tag disappears; a failing whole-line [table...] tag also
        // hides the table that follows it
        std::string lowTag = toLower(tag);
        if (trim(line) == line.substr(br, close - br + 1) ||
            (br == line.find_first_not_of(" \t") && close == line.find_last_not_of(" \t"))) {
          if (lowTag.rfind("[table", 0) == 0 || lowTag.rfind("[tabelle", 0) == 0)
            skipTableRows = true;
          dropLine = true;
        }
      }
      pos = close + 1;
    }
    if (dropLine && trim(rebuilt).empty()) continue;
    out += rebuilt + "\n";
  }
  return out;
}

// ---- stage 0a: \loop over data files ----------------------------------------------
namespace {

std::string unescapeDelim(const std::string& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char c = v[++i];
      if      (c == 'n')  out += '\n';
      else if (c == 't')  out += '\t';
      else if (c == '\\') out += '\\';
      else { out += '\\'; out += c; }
    } else out += v[i];
  }
  return out;
}

// Placeholder engine shared by \loop and \file.
//
//   :START(:END)?        START: absolute number | +N / -N relative to the
//                        cursor | '.' (line 0) | $var | empty (= cursor)
//                        END:   absolute number | +N (START+N, inclusive)
//                        | $var | '$' (definitely file/record end) | empty
//                        (to the end, or to the next delimiter when D= is
//                        set on the tag)
//   $name='<  $name='>   store the FIRST / LAST delivered line number of
//                        the most recent placeholder into a variable
//
// The cursor starts at 0 and moves to (last delivered line + 1) after every
// placeholder that delivered something.
struct FieldCtx {
  const std::vector<std::string>& fields;
  // last line of the delimiter segment containing line i (= fields.size()-1
  // when no delimiter is set)
  const std::vector<long>& segEnd;
  long cursor = 0;
  long lastStart = -1, lastEnd = -1;
  std::map<std::string, long> vars;
  bool warned = false;
};

bool parseRangeValue(const std::string& line, size_t& j, const FieldCtx& ctx,
                     bool isEnd, long start, long& out, bool& toEof) {
  toEof = false;
  if (j >= line.size()) return false;
  char c = line[j];
  auto readNum = [&](long& v) {
    size_t k = j;
    v = 0;
    while (k < line.size() && std::isdigit((unsigned char)line[k]))
      v = v * 10 + (line[k++] - '0');
    bool any = k > j;
    j = k;
    return any;
  };
  if (c == '+' || c == '-') {
    ++j;
    long n;
    if (!readNum(n)) return false;
    out = isEnd ? start + (c == '+' ? n : -n)
                : ctx.cursor + (c == '+' ? n : -n);
    return true;
  }
  if (c == '.' && !isEnd) { ++j; out = 0; return true; }
  if (c == '$') {
    size_t k = j + 1;
    std::string name;
    while (k < line.size() &&
           (std::isalnum((unsigned char)line[k]) || line[k] == '_'))
      name += line[k++];
    if (name.empty()) {                            // bare '$' = end of file
      if (!isEnd) return false;
      j = k;
      out = (long)ctx.fields.size() - 1;
      toEof = true;
      return true;
    }
    auto it = ctx.vars.find(name);
    if (it == ctx.vars.end()) return false;        // unknown var: literal
    j = k;
    out = it->second;
    return true;
  }
  if (std::isdigit((unsigned char)c)) return readNum(out);
  return false;
}

std::string substFields(const std::string& line, FieldCtx& ctx,
                        const char* tagName,
                        std::vector<std::string>& warnings) {
  if (!line.empty() && line[0] == '\x03') return line;   // verbatim line
  std::string out;
  size_t i = 0;
  long n = (long)ctx.fields.size();
  while (i < line.size()) {
    char prev = out.empty() ? ' ' : out.back();
    bool boundary = prev == ' ' || prev == '\t' || prev == '|';

    // variable assignment  $name='<  /  $name='>
    if (line[i] == '$' && boundary) {
      size_t j = i + 1;
      std::string name;
      while (j < line.size() &&
             (std::isalnum((unsigned char)line[j]) || line[j] == '_'))
        name += line[j++];
      if (!name.empty() && j + 2 < line.size() && line[j] == '=' &&
          line[j + 1] == '\'' && (line[j + 2] == '<' || line[j + 2] == '>')) {
        if (ctx.lastStart < 0)
          warnings.push_back(std::string(tagName) +
                             ": $" + name + "='" + line[j + 2] +
                             " before any placeholder");
        else
          ctx.vars[name] = line[j + 2] == '<' ? ctx.lastStart : ctx.lastEnd;
        i = j + 3;
        if (i < line.size() && line[i] == ' ') ++i;   // swallow one space
        continue;
      }
    }

    if (line[i] == ':' && boundary && i + 1 <= line.size()) {
      size_t j = i + 1;
      long a = 0;
      bool aOk = false, dummyEof = false;
      if (j < line.size() && line[j] == ':') {       // "::..." empty start
        a = ctx.cursor;
        aOk = true;
      } else {
        aOk = parseRangeValue(line, j, ctx, false, 0, a, dummyEof);
      }
      if (aOk) {
        long b;
        bool haveEnd = false, toEof = false, openEnd = false;
        if (j < line.size() && line[j] == ':') {
          ++j;
          size_t save = j;
          if (parseRangeValue(line, j, ctx, true, a, b, toEof)) haveEnd = true;
          else { j = save; openEnd = true; }         // ":A:" open end
        } else {
          b = a;                                     // ":A" single line
          haveEnd = true;
        }
        if (openEnd) {
          b = (a >= 0 && a < n) ? ctx.segEnd[(size_t)a] : n - 1;
        }
        // clamp + deliver
        long a2 = a, b2 = b;
        if (a2 < 0) a2 = 0;
        if (b2 >= n) b2 = n - 1;
        if (a >= n || b2 < a2) {
          if (!ctx.warned) {
            warnings.push_back(std::string(tagName) + ": range :" +
                               std::to_string(a) + ":" + std::to_string(b) +
                               " is empty (record has " + std::to_string(n) +
                               " lines)");
            ctx.warned = true;
          }
        } else {
          for (long k = a2; k <= b2; ++k) {
            if (k > a2) out += '\n';
            out += ctx.fields[(size_t)k];
          }
          ctx.lastStart = a2;
          ctx.lastEnd = b2;
          ctx.cursor = b2 + 1;
        }
        (void)haveEnd;
        i = j;
        continue;
      }
    }
    out += line[i++];
  }
  return out;
}

// segment-end table for open-ended ranges stopping at a delimiter
std::vector<long> buildSegEnds(const std::string& norm,
                               const std::vector<std::string>& fields,
                               const std::string& delim, bool useDelim) {
  long n = (long)fields.size();
  std::vector<long> segEnd((size_t)std::max<long>(n, 1), n - 1);
  if (!useDelim || delim.empty() || n == 0) return segEnd;
  // Every delimiter occurrence closes the segment of the line it starts on.
  // When the delimiter itself starts with a newline, that newline is the
  // terminator of the PREVIOUS line, which therefore still belongs to the
  // segment; otherwise the hit line is cut short and the segment ends one
  // line earlier.
  std::vector<long> ends;                            // segment end lines
  size_t pos = 0;
  while ((pos = norm.find(delim, pos)) != std::string::npos) {
    long lineOfHit = (long)std::count(norm.begin(), norm.begin() + (long)pos, '\n');
    ends.push_back(delim[0] == '\n' ? lineOfHit : lineOfHit - 1);
    pos += delim.size();
  }
  size_t e = 0;
  for (long i = 0; i < n; ++i) {
    while (e < ends.size() && ends[e] < i) ++e;
    segEnd[(size_t)i] = e < ends.size() ? std::max(ends[e], i) : n - 1;
  }
  return segEnd;
}

} // namespace

std::string expandLoops(const std::string& text, const std::string& baseDir,
                        std::vector<std::string>& warnings) {
  std::vector<std::string> lines;
  {
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  std::string out;
  auto emit = [&](const std::string& l) { out += l; out += '\n'; };

  for (size_t li = 0; li < lines.size(); ++li) {
    std::string t = trim(lines[li]);
    std::string low = toLower(t);
    bool isLoop = low.rfind("\\loop", 0) == 0 &&
                  (t.size() == 5 || t[5] == ' ' || t[5] == '\t' || t[5] == ',');
    bool isFile = low.rfind("\\file", 0) == 0 &&
                  (t.size() == 5 || t[5] == ' ' || t[5] == '\t' || t[5] == ',');
    if (!isLoop && !isFile) { emit(lines[li]); continue; }
    const char* tagName = isFile ? "file" : "loop";
    const std::string closeTag = isFile ? "\\\\file" : "\\\\loop";

    // parse "\loop file[, D=...]"  /  "\file file[, D=...]"
    std::string rest = trim(t.substr(5));
    if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));
    std::string file, delim = "\n\n";
    {
      std::stringstream ps(rest);
      std::string part;
      bool first = true;
      while (std::getline(ps, part, ',')) {
        part = trim(part);
        size_t eq = part.find('=');
        std::string key = eq == std::string::npos ? "" :
            toLower(trim(part.substr(0, eq)));
        if (first && eq == std::string::npos) file = part;
        else if (key == "d" || key == "delim" || key == "trenner")
          delim = unescapeDelim(trim(part.substr(eq + 1)));
        else if (!part.empty())
          warnings.push_back(std::string("\\") + tagName +
                             ": unknown option: " + part);
        first = false;
      }
    }
    // collect the body up to the closing tag
    size_t bodyStart = li + 1, bodyEnd = bodyStart;
    bool closed = false;
    for (size_t j = bodyStart; j < lines.size(); ++j) {
      std::string jt = toLower(trim(lines[j]));
      if (jt == closeTag) { bodyEnd = j; closed = true; break; }
      if ((jt.rfind("\\loop", 0) == 0 || jt.rfind("\\file", 0) == 0) &&
          (trim(lines[j]).size() == 5 || jt[5] == ' ' || jt[5] == ',')) {
        warnings.push_back("nested \\loop/\\file is not supported yet");
      }
    }
    if (!closed) {
      warnings.push_back(std::string("\\") + tagName + " without \\\\" + tagName);
      emit(lines[li]);
      continue;
    }
    if (file.empty()) {
      warnings.push_back(std::string("\\") + tagName +
                         " needs a data file: \\" + tagName + " werte.txt");
      li = bodyEnd;
      continue;
    }
    std::string path = baseDir.empty() ? file : baseDir + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      warnings.push_back(std::string("\\") + tagName + ": cannot read " + path);
      li = bodyEnd;
      continue;
    }
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // normalize line endings
    std::string norm;
    for (size_t k = 0; k < data.size(); ++k)
      if (data[k] != '\r') norm += data[k];

    if (isFile) {
      // \file: ONE pass over the whole file; absolute numbers are file
      // lines. Open-ended ranges stop at the next delimiter when D= is set.
      bool delimSet = !delim.empty() && delim != "\n\n" ? true : false;
      // D=\n\n explicitly given also counts as set; detect via raw rest
      if (rest.find("D=") != std::string::npos ||
          rest.find("d=") != std::string::npos ||
          toLower(rest).find("delim=") != std::string::npos ||
          toLower(rest).find("trenner=") != std::string::npos)
        delimSet = true;
      std::vector<std::string> fields;
      {
        std::stringstream rs(norm);
        std::string fl;
        while (std::getline(rs, fl)) fields.push_back(fl);
      }
      auto segEnd = buildSegEnds(norm, fields, delim, delimSet);
      FieldCtx ctx{fields, segEnd};
      for (size_t j = bodyStart; j < bodyEnd; ++j)
        emit(substFields(lines[j], ctx, tagName, warnings));
      li = bodyEnd;
      continue;
    }

    // \loop: split into records
    std::vector<std::string> records;
    if (delim.empty()) delim = "\n\n";
    size_t pos = 0;
    while (pos <= norm.size()) {
      size_t hit = norm.find(delim, pos);
      std::string rec = hit == std::string::npos ?
          norm.substr(pos) : norm.substr(pos, hit - pos);
      // trim outer newlines of the record
      size_t a = rec.find_first_not_of('\n');
      size_t b = rec.find_last_not_of('\n');
      if (a != std::string::npos) records.push_back(rec.substr(a, b - a + 1));
      if (hit == std::string::npos) break;
      pos = hit + delim.size();
    }
    if (records.empty())
      warnings.push_back("\\loop: " + file + " has no records");

    // expand the body once per record (cursor and variables reset each time)
    for (const auto& rec : records) {
      std::vector<std::string> fields;
      std::stringstream rs(rec);
      std::string fl;
      while (std::getline(rs, fl)) fields.push_back(fl);
      std::vector<long> segEnd((size_t)std::max<size_t>(fields.size(), 1),
                               (long)fields.size() - 1);
      FieldCtx ctx{fields, segEnd};
      for (size_t j = bodyStart; j < bodyEnd; ++j)
        emit(substFields(lines[j], ctx, tagName, warnings));
    }
    li = bodyEnd;                                    // skip past the closer
  }
  if (!text.empty() && text.back() != '\n' && !out.empty()) out.pop_back();
  return out;
}

// ---- stage 0: file includes ------------------------------------------------------
namespace {
bool isTxtIncludeTag(const std::string& s, size_t pos, std::string& name, size_t& end) {
  if (s[pos] != '[') return false;
  size_t close = s.find(']', pos);
  if (close == std::string::npos) return false;
  std::string inner = trim(s.substr(pos + 1, close - pos - 1));
  if (inner.find('\n') != std::string::npos ||
      inner.find(',')  != std::string::npos ||
      inner.find('|')  != std::string::npos) return false;
  std::string low = toLower(inner);
  bool txt = low.size() >= 5 && low.compare(low.size() - 4, 4, ".txt") == 0;
  bool md  = low.size() >= 4 && low.compare(low.size() - 3, 3, ".md") == 0;
  if (!txt && !md) return false;
  name = inner;
  end = close + 1;
  return true;
}

std::string expandIncludesDepth(const std::string& text, const std::string& baseDir,
                                std::vector<std::string>& warnings, int depth,
                                bool embedded) {
  if (depth > 8) {
    warnings.push_back("include depth limit reached (circular include?)");
    return text;
  }
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t br = text.find('[', pos);
    if (br == std::string::npos) { out.append(text, pos, std::string::npos); break; }
    out.append(text, pos, br - pos);

    std::string name; size_t end;
    if (!isTxtIncludeTag(text, br, name, end)) { out += '['; pos = br + 1; continue; }

    std::string path = baseDir.empty() ? name : baseDir + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      warnings.push_back("include not found: " + path);
      pos = end;
      continue;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
      content.pop_back();
    bool incMd = name.size() > 3 &&
                 toLower(name).compare(name.size() - 3, 3, ".md") == 0;
    content = preprocessSource(content, embedded, warnings, incMd);
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
      content.pop_back();
    content = expandIncludesDepth(content, baseDir, warnings, depth + 1, embedded);

    // whole-line tag (only whitespace before/after on its line)? -> verbatim.
    size_t ls = text.rfind('\n', br);
    ls = ls == std::string::npos ? 0 : ls + 1;
    size_t le = text.find('\n', end);
    le = le == std::string::npos ? text.size() : le;
    bool wholeLine = trim(text.substr(ls, br - ls)).empty() &&
                     trim(text.substr(end, le - end)).empty();
    if (!wholeLine) {                            // inline: newlines -> <br/>
      std::string flat;
      for (char c : content) {
        if (c == '\r') continue;
        if (c == '\n') flat += "<br/>";
        else flat += c;
      }
      content = flat;
    }
    out += content;
    pos = end;
  }
  return out;
}
} // namespace

std::string expandIncludes(const std::string& text, const std::string& baseDir,
                           std::vector<std::string>& warnings, bool embedded) {
  return expandIncludesDepth(text, baseDir, warnings, 0, embedded);
}

// ---- stage 0b: date tags ---------------------------------------------------------
namespace {

struct DateNames {
  const char* months[12];      // in date form (genitive for ru/pl)
  const char* monthsShort[12];
  const char* weekdays[7];     // Monday..Sunday
  const char* weekdaysShort[7];
};

const DateNames* namesFor(const std::string& lang) {
  static const DateNames en = {
    {"January","February","March","April","May","June","July","August",
     "September","October","November","December"},
    {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"},
    {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"},
    {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"}};
  static const DateNames de = {
    {"Januar","Februar","März","April","Mai","Juni","Juli","August",
     "September","Oktober","November","Dezember"},
    {"Jan","Feb","Mär","Apr","Mai","Jun","Jul","Aug","Sep","Okt","Nov","Dez"},
    {"Montag","Dienstag","Mittwoch","Donnerstag","Freitag","Samstag","Sonntag"},
    {"Mo","Di","Mi","Do","Fr","Sa","So"}};
  static const DateNames es = {
    {"enero","febrero","marzo","abril","mayo","junio","julio","agosto",
     "septiembre","octubre","noviembre","diciembre"},
    {"ene","feb","mar","abr","may","jun","jul","ago","sep","oct","nov","dic"},
    {"lunes","martes","miércoles","jueves","viernes","sábado","domingo"},
    {"lun","mar","mié","jue","vie","sáb","dom"}};
  static const DateNames pl = {   // months in the genitive, as used in dates
    {"stycznia","lutego","marca","kwietnia","maja","czerwca","lipca",
     "sierpnia","września","października","listopada","grudnia"},
    {"sty","lut","mar","kwi","maj","cze","lip","sie","wrz","paź","lis","gru"},
    {"poniedziałek","wtorek","środa","czwartek","piątek","sobota","niedziela"},
    {"pon","wt","śr","czw","pt","sob","niedz"}};
  static const DateNames ru = {   // months in the genitive, as used in dates
    {"января","февраля","марта","апреля","мая","июня","июля","августа",
     "сентября","октября","ноября","декабря"},
    {"янв","фев","мар","апр","мая","июн","июл","авг","сен","окт","ноя","дек"},
    {"понедельник","вторник","среда","четверг","пятница","суббота","воскресенье"},
    {"пн","вт","ср","чт","пт","сб","вс"}};
  std::string l = toLower(lang);
  if (l == "de") return &de;
  if (l == "es") return &es;
  if (l == "pl") return &pl;
  if (l == "ru") return &ru;
  return &en;
}

// Best-effort file creation time; falls back to the modification time on
// filesystems / platforms without a birth time. Returns false if the file
// doesn't exist.
bool fileCreationTime(const std::string& path, std::time_t& out) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return false;
  ULARGE_INTEGER u;
  u.LowPart = fad.ftCreationTime.dwLowDateTime;
  u.HighPart = fad.ftCreationTime.dwHighDateTime;
  out = (std::time_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
  return true;
#else
#ifdef __linux__
  struct statx stx;
  if (statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME | STATX_MTIME, &stx) == 0) {
    if (stx.stx_mask & STATX_BTIME) { out = stx.stx_btime.tv_sec; return true; }
    if (stx.stx_mask & STATX_MTIME) { out = stx.stx_mtime.tv_sec; return true; }
  }
#endif
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  out = st.st_mtime;
  return true;
#endif
}

std::string formatDate(const std::tm& tm, const std::string& fmt, const DateNames& n) {
  auto num = [](int v, int pad) {
    char b[16];
    std::snprintf(b, sizeof b, pad ? "%02d" : "%d", v);
    return std::string(b);
  };
  int wd = (tm.tm_wday + 6) % 7;                        // 0 = Monday
  std::string out;
  size_t i = 0;
  auto at = [&](const char* tok) {
    size_t L = std::strlen(tok);
    return fmt.compare(i, L, tok) == 0 ? L : 0;
  };
  while (i < fmt.size()) {
    size_t L;
    if      ((L = at("WEEKDAY"))) { out += n.weekdays[wd];            i += L; }
    else if ((L = at("MONTH")))   { out += n.months[tm.tm_mon];       i += L; }
    else if ((L = at("YYYY")))    { out += num(tm.tm_year + 1900, 0); i += L; }
    else if ((L = at("MON")))     { out += n.monthsShort[tm.tm_mon];  i += L; }
    else if ((L = at("WD")))      { out += n.weekdaysShort[wd];       i += L; }
    else if ((L = at("YY")))      { out += num((tm.tm_year + 1900) % 100, 1); i += L; }
    else if ((L = at("MM")))      { out += num(tm.tm_mon + 1, 1);     i += L; }
    else if ((L = at("DD")))      { out += num(tm.tm_mday, 1);        i += L; }
    else if ((L = at("M")))       { out += num(tm.tm_mon + 1, 0);     i += L; }
    else if ((L = at("D")))       { out += num(tm.tm_mday, 0);        i += L; }
    else out += fmt[i++];
  }
  return out;
}

// Parses the inside of a [date, ...] tag, honoring "quoted" values.
// Returns false if this isn't a date tag.
bool tryParseDateTag(const std::string& s, size_t pos, size_t& end,
                     std::string& fmt, std::string& lang, bool& modified) {
  if (s[pos] != '[') return false;
  // find the closing ] outside of quotes
  size_t close = std::string::npos;
  bool inQ = false;
  for (size_t i = pos + 1; i < s.size(); ++i) {
    if (s[i] == '"') inQ = !inQ;
    else if (s[i] == ']' && !inQ) { close = i; break; }
    else if (s[i] == '\n') break;
  }
  if (close == std::string::npos) return false;
  std::string inner = s.substr(pos + 1, close - pos - 1);

  // split on commas outside quotes
  std::vector<std::string> parts;
  std::string cur;
  inQ = false;
  for (char c : inner) {
    if (c == '"') { inQ = !inQ; continue; }
    if (c == ',' && !inQ) { parts.push_back(trim(cur)); cur.clear(); }
    else cur += c;
  }
  parts.push_back(trim(cur));
  if (parts.empty()) return false;
  std::string head = toLower(parts[0]);
  if (head != "date" && head != "datum") return false;

  fmt = "DD.MM.YYYY"; lang = "en"; modified = false;
  for (size_t i = 1; i < parts.size(); ++i) {
    size_t eq = parts[i].find('=');
    if (eq == std::string::npos) return false;
    std::string key = toLower(trim(parts[i].substr(0, eq)));
    std::string val = trim(parts[i].substr(eq + 1));
    if      (key == "format")                     fmt = val;
    else if (key == "lang" || key == "sprache")   lang = val;
    else if (key == "modified" || key == "fest")
      modified = (val == "1" || toLower(val) == "true" || toLower(val) == "on");
    else return false;
  }
  end = close + 1;
  return true;
}

} // namespace

std::string expandDates(const std::string& text, const Options& opt,
                        std::vector<std::string>& warnings) {
  std::string out;
  size_t pos = 0;
  bool warnedMissingPath = false;
  while (pos < text.size()) {
    size_t br = text.find('[', pos);
    if (br == std::string::npos) { out.append(text, pos, std::string::npos); break; }
    out.append(text, pos, br - pos);

    std::string fmt, lang; bool modified; size_t end;
    if (!tryParseDateTag(text, br, end, fmt, lang, modified)) {
      out += '[';
      pos = br + 1;
      continue;
    }
    std::time_t t = std::time(nullptr);
    if (modified) {
      if (opt.pdfPath.empty()) {
        if (!warnedMissingPath) {
          warnings.push_back("[date, modified=1]: no PDF path known, using today");
          warnedMissingPath = true;
        }
      } else {
        std::time_t ct;
        if (fileCreationTime(opt.pdfPath, ct)) t = ct;   // else: first run, today
      }
    }
    std::tm tm{};
    localTime(t, tm);
    out += formatDate(tm, fmt, *namesFor(lang));
    pos = end;
  }
  return out;
}

// ---- stage 1: tokenize -----------------------------------------------------------
namespace {
// "[table, widths=4*cm:2:1, padding=2*mm:1*mm, spacing=0.5*cm, indent=1*cm,
//  width=12*cm, grid=off]" -> TableOpts. Returns false if the line is no
// table-options tag at all.
bool tryParseTableOpts(const std::string& line, pdf::TableOpts& opts,
                       std::vector<std::string>& warnings) {
  std::string t = trim(line);
  if (t.size() < 7 || t.front() != '[' || t.back() != ']') return false;
  std::string inner = t.substr(1, t.size() - 2);
  std::vector<std::string> parts;
  std::stringstream ss(inner);
  std::string part;
  while (std::getline(ss, part, ',')) parts.push_back(trim(part));
  if (parts.empty()) return false;
  std::string head = toLower(parts[0]);
  if (head != "table" && head != "tabelle") return false;

  opts = pdf::TableOpts{};
  auto hasUnit = [](const std::string& v) {
    for (char c : v) if (std::isalpha((unsigned char)c)) return true;
    return false;
  };
  for (size_t i = 1; i < parts.size(); ++i) {
    size_t eq = parts[i].find('=');
    if (eq == std::string::npos) {
      warnings.push_back("table option ignored: " + parts[i]);
      continue;
    }
    std::string key = toLower(trim(parts[i].substr(0, eq)));
    std::string val = trim(parts[i].substr(eq + 1));
    double d;
    auto dimOr = [&](double& slot) {
      if (evalDimension(val, d)) slot = d;
      else warnings.push_back("bad " + key + " value: " + val);
    };
    if (key == "widths" || key == "breiten") {
      std::stringstream vs(val);
      std::string col;
      while (std::getline(vs, col, ':')) {
        col = trim(col);
        if (evalDimension(col, d))
          opts.widths.push_back({d, hasUnit(col)});   // unit = fixed, bare = weight
        else
          warnings.push_back("bad column width: " + col);
      }
    } else if (key == "padding" || key == "innenabstand") {
      size_t colon = val.find(':');
      std::string hv = trim(colon == std::string::npos ? val : val.substr(0, colon));
      std::string vv = trim(colon == std::string::npos ? val : val.substr(colon + 1));
      if (evalDimension(hv, d)) opts.padH = d;
      else warnings.push_back("bad padding value: " + hv);
      if (evalDimension(vv, d)) opts.padV = d;
      else warnings.push_back("bad padding value: " + vv);
    } else if (key == "spacing" || key == "abstand") {
      dimOr(opts.spacing);
    } else if (key == "rowspacing" || key == "zeilenabstand") {
      dimOr(opts.rowGap);
    } else if (key == "indent" || key == "einzug") {
      dimOr(opts.indent);
    } else if (key == "width" || key == "breite") {
      dimOr(opts.totalWidth);
    } else if (key == "grid" || key == "gitter") {
      std::string v = toLower(val);
      opts.grid = (v == "on" || v == "yes" || v == "1" || v == "true" || v == "an" || v == "ja");
    } else if (key == "frame" || key == "rahmen") {
      std::string v = toLower(val);
      opts.frame = (v == "on" || v == "yes" || v == "1" || v == "true" || v == "an" || v == "ja");
    } else if (key == "linewidth" || key == "linienbreite") {
      if (evalDimension(val, d)) opts.lineWidth = d;
    } else if (key == "linecolor" || key == "linienfarbe") {
      opts.lineColor = val;
    } else {
      warnings.push_back("unknown table option: " + key);
    }
  }
  return true;
}
} // namespace

std::vector<Token> tokenize(const std::string& rawText, const Options& opt,
                            std::vector<std::string>& warnings) {
  std::vector<Token> tokens;
  std::string pendingText;                 // paragraph text accumulated so far
  Token pendingTable;                      // table rows accumulated so far
  bool inTable = false;
  pdf::TableOpts pendingOpts;              // from a preceding [table, ...] line
  bool haveOpts = false;

  auto flushText = [&]() {
    std::string t = trim(pendingText);
    pendingText.clear();
    if (!t.empty()) {
      Token tk; tk.kind = Token::Kind::Text; tk.text = t;
      tokens.push_back(std::move(tk));
    }
  };
  bool blockTable = false;             // inside \t ... \\t
  bool groupOpen = false;              // @/[group] between table rows
  bool flowGroupOpen = false;          // @/[group] outside tables
  std::string pipedRow;                // multi-line |-row being collected
  std::string fullWidthRow;            // multi-line no-pipe row (block table)

  auto emitGroupMark = [&](bool start) {
    Token tk; tk.kind = Token::Kind::GroupMark;
    tk.blankLines = start ? 1 : 0;
    tokens.push_back(std::move(tk));
  };
  auto closeFlowGroup = [&]() {
    if (flowGroupOpen) { emitGroupMark(false); flowGroupOpen = false; }
  };

  auto pushTableRow = [&](const std::string& raw, bool fullWidth) {
    std::vector<std::string> cells;
    std::vector<int> cellSpans;
    if (fullWidth) {
      std::string cell = raw;
      // physical line breaks inside become real breaks
      std::string joined;
      for (char c : cell) joined += (c == '\n') ? std::string("<br/>") : std::string(1, c);
      cells.push_back(trim(joined));
      cellSpans.push_back(-1);                       // spans ALL columns
    } else {
      std::string joined;
      for (char c : raw) joined += (c == '\n') ? std::string("<br/>") : std::string(1, c);
      splitTableRow(joined, cells, cellSpans);
      std::vector<int> aligns;
      if (parseSeparatorRow(cells, aligns)) {        // separator also in \t mode
        if (pendingTable.rows.size() > (size_t)pendingTable.headerRows &&
            pendingTable.headerRows == 0)
          pendingTable.headerRows = (int)pendingTable.rows.size();
        if (pendingTable.aligns.empty()) pendingTable.aligns = aligns;
        else pendingTable.topts.rulesAfterRow.push_back(
            (int)pendingTable.rows.size() - 1);
        return;
      }
    }
    pendingTable.rows.push_back(cells);
    pendingTable.spans.push_back(cellSpans);
  };
  auto flushPartialRows = [&]() {
    if (!pipedRow.empty())      { pushTableRow(pipedRow, false); pipedRow.clear(); }
    if (!fullWidthRow.empty())  { pushTableRow(fullWidthRow, true); fullWidthRow.clear(); }
  };

  auto flushTable = [&]() {
    if (inTable && !pendingTable.rows.empty())
      tokens.push_back(std::move(pendingTable));
    pendingTable = Token{};
    inTable = false;
  };
  // Consecutive blank lines are counted: 1 blank line = the normal
  // paragraph gap, each additional one adds another gap unit. A run at the
  // very top of the file contributes (n-1) units, so a single leading blank
  // line keeps the old behavior while stacked ones push the first content
  // (e.g. a heading) down.
  int blankRun = 0;
  bool anyContent = false;
  std::string pendingCode;               // consecutive \x04 lines
  auto flushCode = [&]() {
    if (pendingCode.empty()) return;
    if (!pendingCode.empty() && pendingCode.back() == '\n')
      pendingCode.pop_back();
    Token tk; tk.kind = Token::Kind::CodeBlock; tk.text = pendingCode;
    tokens.push_back(std::move(tk));
    pendingCode.clear();
  };
  auto flushBreak = [&]() {
    if (blankRun == 0) return;
    int count = anyContent ? blankRun : blankRun - 1;
    blankRun = 0;
    if (count <= 0) return;
    Token tk; tk.kind = Token::Kind::ParagraphEnd; tk.blankLines = count;
    tokens.push_back(std::move(tk));
  };

  (void)opt;
  // walk line by line
  std::stringstream ss(rawText);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (!line.empty() && line[0] == '\x04' && !blockTable) {   // fenced code
      flushTable(); flushText();
      pendingCode += line.substr(1);
      pendingCode += '\n';
      anyContent = true;
      blankRun = 0;
      continue;
    }
    if (!pendingCode.empty()) flushCode();

    if (!line.empty() && line[0] == '\x08' && !blockTable) {    // md structure
      std::string m = line.substr(1);
      flushTable(); flushText();
      flushBreak();
      anyContent = true;
      if (m == "rule") {
        Token tk; tk.kind = Token::Kind::HRule;
        tokens.push_back(std::move(tk));
      } else if (m.rfind("q ", 0) == 0) {
        Token tk; tk.kind = Token::Kind::Quote; tk.text = trim(m.substr(2));
        tokens.push_back(std::move(tk));
      } else if (m[0] == 'l') {
        size_t sp = m.find(' ');
        Token tk; tk.kind = Token::Kind::ListItem;
        tk.level = std::atoi(m.c_str() + 1);
        tk.text = sp == std::string::npos ? "" : m.substr(sp + 1);
        tokens.push_back(std::move(tk));
      }
      continue;
    }

    if (blockTable) {                               // inside \t ... \\t
      if (!line.empty() && line[0] == '\x03') {    // verbatim: full-width text
        std::string lit = trim(line.substr(1));
        if (lit.empty()) { flushPartialRows(); continue; }
        fullWidthRow += fullWidthRow.empty() ? ('\x03' + lit)
                                             : "\n\x03" + lit;
        continue;
      }
      std::string bt = trim(line);
      std::string btl = toLower(bt);
      if (bt.empty()) { flushPartialRows(); continue; }
      if (btl == "\\\\t" || btl == "\\\\table" || btl == "\\\\tabelle") {
        flushPartialRows();
        if (groupOpen) {                            // auto-close open group
          auto& g = pendingTable.topts.keepGroups.back();
          if (g.second < 0) g.second = (int)pendingTable.rows.size() - 1;
          groupOpen = false;
        }
        blockTable = false;
        flushTable();
        continue;
      }
      if (bt == "@") {                              // group separator
        flushPartialRows();
        if (groupOpen) {
          auto& g = pendingTable.topts.keepGroups.back();
          if (g.second < 0) g.second = (int)pendingTable.rows.size() - 1;
        }
        pendingTable.topts.keepGroups.push_back(
            {(int)pendingTable.rows.size(), -1});
        groupOpen = true;
        continue;
      }
      if (btl == "[group]" || btl == "[gruppe]" ||
          btl == "[/group]" || btl == "[/gruppe]") {
        flushPartialRows();
        // fall through to the shared [group] handlers below
      } else if (btl.rfind("[row", 0) == 0 && bt.back() == ']') {
        flushPartialRows();
        // fall through to the shared [row, ...] handler below
      } else if (!pipedRow.empty()) {               // continuing a |-row
        pipedRow += "\n" + bt;
        if (bt.back() == '|') { pushTableRow(pipedRow, false); pipedRow.clear(); }
        continue;
      } else if (bt[0] == '|') {                    // new |-row
        if (!fullWidthRow.empty()) { pushTableRow(fullWidthRow, true); fullWidthRow.clear(); }
        if (bt.back() == '|' && bt.size() > 1) pushTableRow(bt, false);
        else pipedRow = bt;
        continue;
      } else {                                      // full-width text row
        fullWidthRow += fullWidthRow.empty() ? bt : "\n" + bt;
        continue;
      }
    }

    if (trim(line).empty()) {                       // blank line: count the run
      if (blankRun == 0) { flushTable(); flushText(); }
      blankRun++;
      continue;
    }
    flushBreak();                                   // emit accumulated gap
    anyContent = true;

    {   // verbatim text line (\on/\off pass)
      std::string vt = trim(line);
      if (!vt.empty() && vt[0] == '\x03') {
        flushTable();
        pendingText += vt.substr(1);               // sentinel stripped
        pendingText += '\n';                       // hard break
        continue;
      }
    }

    {   // \t options — block-style table until \\t
      std::string bt = trim(line);
      std::string btl = toLower(bt);
      if (btl == "\\t" || btl.rfind("\\t ", 0) == 0 || btl.rfind("\\t,", 0) == 0 ||
          btl == "\\table" || btl.rfind("\\table ", 0) == 0 ||
          btl == "\\tabelle" || btl.rfind("\\tabelle ", 0) == 0) {
        flushTable(); flushText();
        size_t sp = bt.find_first_of(" ,");
        std::string rest = sp == std::string::npos ? "" : trim(bt.substr(sp + 1));
        pdf::TableOpts to;
        std::string optLine = rest.empty() ? "[table]" : "[table, " + rest + "]";
        closeFlowGroup();
        pendingTable = Token{};
        pendingTable.kind = Token::Kind::Table;
        if (tryParseTableOpts(optLine, to, warnings))
          pendingTable.topts = to;
        inTable = true;
        blockTable = true;
        groupOpen = false;
        continue;
      }
      // \s <n|dim> — vertical space
      if (btl.rfind("\\s", 0) == 0 &&
          (bt.size() == 2 || bt[2] == ' ' || bt[2] == '\t')) {
        std::string v = trim(bt.substr(2));
        flushTable(); flushText();
        Token tk; tk.kind = Token::Kind::VSpace;
        bool hasUnit = false;
        for (char c : v)
          if (std::isalpha((unsigned char)c)) { hasUnit = true; break; }
        double d;
        if (v.empty()) {
          warnings.push_back("\\s needs a value: \\s 3  or  \\s 5*mm");
          tk.vspacePts = 0;
        } else if (hasUnit) {
          if (evalDimension(v, d)) tk.vspacePts = d;
          else { warnings.push_back("bad \\s value: " + v); tk.vspacePts = 0; }
        } else {
          char* endp = nullptr;
          double n = std::strtod(v.c_str(), &endp);
          if (endp == v.c_str() || *endp != '\0') {
            warnings.push_back("bad \\s value: " + v);
            n = 0;
          }
          tk.vspacePts = n * pdf::styles::body().leading;   // n text lines
        }
        tokens.push_back(std::move(tk));
        continue;
      }
      // @ outside tables: flowable group separator (auto-closing)
      if (bt == "@") {
        flushTable(); flushText();
        closeFlowGroup();
        emitGroupMark(true);
        flowGroupOpen = true;
        continue;
      }
    }

    {   // [newpage] / [neueseite], optionally with no_blank
      std::string tl = toLower(trim(line));
      if (tl.rfind("[newpage", 0) == 0 || tl.rfind("[neueseite", 0) == 0) {
        std::string t = trim(line);
        if (!t.empty() && t.back() == ']') {
          std::string inner = toLower(t.substr(1, t.size() - 2));
          Token tk; tk.kind = Token::Kind::NewPage;
          bool valid = (inner == "newpage" || inner == "neueseite");
          if (!valid) {
            size_t comma = inner.find(',');
            std::string head = trim(inner.substr(0, comma));
            if (head == "newpage" || head == "neueseite") {
              valid = true;
              std::string rest = comma == std::string::npos ? "" : inner.substr(comma + 1);
              std::stringstream ps(rest);
              std::string part;
              while (std::getline(ps, part, ',')) {
                part = trim(part);
                if (part == "no_blank" || part == "noblank" ||
                    part == "keine_leerseite")
                  tk.noBlank = true;
                else if (!part.empty())
                  warnings.push_back("unknown [newpage] option: " + part);
              }
            }
          }
          if (valid) {
            flushTable(); flushText();
            closeFlowGroup();
            tokens.push_back(std::move(tk));
            continue;
          }
        }
      }
      // [group] / [/group]: inside a table the enclosed rows stay together,
      // outside it the enclosed flowables do (kept on one page)
      if (!inTable && (tl == "[group]" || tl == "[gruppe]")) {
        flushText();
        closeFlowGroup();
        emitGroupMark(true);
        flowGroupOpen = true;
        continue;
      }
      if (!inTable && (tl == "[/group]" || tl == "[/gruppe]")) {
        flushText();
        if (!flowGroupOpen) warnings.push_back("[/group] without [group]");
        closeFlowGroup();
        continue;
      }
      if (inTable && (tl == "[group]" || tl == "[gruppe]")) {
        pendingTable.topts.keepGroups.push_back(
            {(int)pendingTable.rows.size(), -1});          // open group
        groupOpen = true;
        continue;
      }
      if (inTable && (tl == "[/group]" || tl == "[/gruppe]")) {
        groupOpen = false;
        bool closed = false;
        for (auto it = pendingTable.topts.keepGroups.rbegin();
             it != pendingTable.topts.keepGroups.rend(); ++it)
          if (it->second < 0) {
            it->second = (int)pendingTable.rows.size() - 1;
            closed = true;
            break;
          }
        if (!closed) warnings.push_back("[/group] without [group]");
        continue;
      }
      // [margins, ...] anywhere in the content = margin change from here on
      if (tl.rfind("[margins", 0) == 0 && !trim(line).empty() &&
          trim(line).back() == ']') {
        std::string t = trim(line);
        std::string inner = t.substr(1, t.size() - 2);
        Token tk; tk.kind = Token::Kind::MarginsChange;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : inner) {
          if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
          else cur += c;
        }
        parts.push_back(trim(cur));
        auto setDim = [&](const std::string& v, int slot) {
          double d;
          if (evalDimension(v, d)) tk.margins[slot] = d;
          else warnings.push_back("bad margin value: " + v);
        };
        // head "margins=l:r:t:b"
        {
          size_t eq = parts[0].find('=');
          if (eq != std::string::npos) {
            std::stringstream vs(trim(parts[0].substr(eq + 1)));
            std::string seg;
            int i = 0;
            while (std::getline(vs, seg, ':') && i < 4) setDim(trim(seg), i++);
          }
        }
        for (size_t i = 1; i < parts.size(); ++i) {
          size_t eq = parts[i].find('=');
          if (eq == std::string::npos) continue;
          std::string k = toLower(trim(parts[i].substr(0, eq)));
          std::string v = trim(parts[i].substr(eq + 1));
          if (k == "left" || k == "links")   setDim(v, 0);
          if (k == "right" || k == "rechts") setDim(v, 1);
          if (k == "top" || k == "oben")     setDim(v, 2);
          if (k == "bottom" || k == "unten") setDim(v, 3);
        }
        flushTable(); flushText();
        closeFlowGroup();
        tokens.push_back(std::move(tk));
        continue;
      }
      // [row, spacing=...] between two table rows: one-off row gap
      if (inTable && tl.rfind("[row", 0) == 0 && trim(line).back() == ']') {
        std::string t = trim(line);
        std::string inner = t.substr(1, t.size() - 2);
        std::stringstream ps(inner);
        std::string part;
        double gap = -1;
        bool pageBreak = false;
        while (std::getline(ps, part, ',')) {
          part = trim(part);
          std::string low = toLower(part);
          if (low == "pagebreak" || low == "break" || low == "seitenumbruch") {
            pageBreak = true;
            continue;
          }
          size_t eq = part.find('=');
          if (eq == std::string::npos) continue;
          std::string k = toLower(trim(part.substr(0, eq)));
          if (k == "spacing" || k == "abstand") {
            double d;
            if (evalDimension(trim(part.substr(eq + 1)), d)) gap = d;
          }
        }
        if (pendingTable.rows.empty()) {
          warnings.push_back("[row, ...] before the first table row is ignored");
        } else {
          int after = (int)pendingTable.rows.size() - 1;
          if (gap >= 0)
            pendingTable.topts.rowGapOverrides.push_back({after, gap});
          if (pageBreak)
            pendingTable.topts.breakAfterRow.push_back(after);
          if (gap < 0 && !pageBreak)
            warnings.push_back("[row] needs spacing=<dim> and/or pagebreak");
        }
        continue;
      }
    }

    {   // [pdf=Datei.pdf(, newpage_no_blank)] — merge an existing PDF here
      std::string t = trim(line);
      std::string low = toLower(t);
      if (low.rfind("[pdf=", 0) == 0 && t.back() == ']') {
        std::string inner = t.substr(1, t.size() - 2);
        std::vector<std::string> parts;
        std::string cur;
        for (char c : inner) {
          if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
          else cur += c;
        }
        parts.push_back(trim(cur));
        std::string path = trim(parts[0].substr(parts[0].find('=') + 1));
        Token tk; tk.kind = Token::Kind::ExternalPdf; tk.text = path;
        for (size_t k = 1; k < parts.size(); ++k) {
          std::string o = toLower(parts[k]);
          if (o == "newpage_no_blank" || o == "newpage_noblank" ||
              o == "neueseite_keine_leerseite")
            tk.noBlank = true;
          else if (!o.empty())
            warnings.push_back("unknown [pdf] option: " + parts[k]);
        }
        if (!path.empty()) {
          flushTable(); flushText();
          tokens.push_back(std::move(tk));
          continue;
        }
      }
      // [pdfgen=Quelle.txt, include] — render + merge (a [pdfgen...] tag
      // WITHOUT `include` is a document separator handled by the project
      // layer and never reaches this tokenizer)
      if (low.rfind("[pdfgen", 0) == 0 && t.back() == ']' &&
          low.find("include") != std::string::npos) {
        std::string inner = t.substr(1, t.size() - 2);
        std::string src;
        bool nb = false;
        std::stringstream ps(inner);
        std::string part;
        while (std::getline(ps, part, ',')) {
          part = trim(part);
          std::string lp = toLower(part);
          if (lp.rfind("pdfgen=", 0) == 0) src = trim(part.substr(7));
          if (lp == "newpage_no_blank" || lp == "newpage_noblank" ||
              lp == "neueseite_keine_leerseite")
            nb = true;
        }
        flushTable(); flushText();
        closeFlowGroup();
        if (src.empty()) {
          warnings.push_back("[pdfgen, include] needs a source file: [pdfgen=Quelle.txt, include]");
        } else {
          Token tk; tk.kind = Token::Kind::IncludeSource; tk.text = src;
          tk.noBlank = nb;
          tokens.push_back(std::move(tk));
        }
        continue;
      }
    }

    {                                                // [table, ...] options line
      pdf::TableOpts to;
      if (tryParseTableOpts(line, to, warnings)) {
        flushTable();
        flushText();
        pendingOpts = to;
        haveOpts = true;
        continue;
      }
    }

    if (!trim(line).empty() && trim(line)[0] == '|') {   // table row line
      flushText();
      if (!inTable) {
        closeFlowGroup();
        pendingTable = Token{};
        pendingTable.kind = Token::Kind::Table;
        if (haveOpts) {
          auto rules = pendingTable.topts.rulesAfterRow;   // keep separator rules
          pendingTable.topts = pendingOpts;
          for (int r : rules) pendingTable.topts.rulesAfterRow.push_back(r);
          haveOpts = false;
        }
        inTable = true;
      }
      std::vector<std::string> cells;
      std::vector<int> cellSpans;
      splitTableRow(line, cells, cellSpans);
      std::vector<int> aligns;
      if (parseSeparatorRow(cells, aligns)) {
        if (!pendingTable.rows.empty()) {
          int after = (int)pendingTable.rows.size() - 1;
          pendingTable.topts.rulesAfterRow.push_back(after);
          if (pendingTable.headerRows == 0 && pendingTable.aligns.empty()) {
            // first separator: rows above become the (bold) header
            pendingTable.aligns = aligns;
            pendingTable.headerRows = (int)pendingTable.rows.size();
          }
        } else if (pendingTable.aligns.empty()) {
          pendingTable.aligns = aligns;     // separator before any row: only aligns
        }
      } else {
        pendingTable.rows.push_back(cells);
        pendingTable.spans.push_back(cellSpans);
      }
      continue;
    }
    flushTable();                                   // non-| line ends a table

    std::string title;
    if (int lvl = headingLevel(line, title)) {      // heading line
      flushText();
      closeFlowGroup();
      Token tk; tk.kind = Token::Kind::Heading; tk.text = title; tk.level = lvl;
      tokens.push_back(std::move(tk));
      continue;
    }

    // scan the line for [image...] tags; the rest is plain text
    size_t pos = 0;
    while (pos < line.size()) {
      size_t br = line.find('[', pos);
      Token img; size_t end;
      if (br != std::string::npos && tryParseImageTag(line, br, img, end)) {
        pendingText += line.substr(pos, br - pos);
        flushText();
        tokens.push_back(img);
        pos = end;
      } else {
        pendingText += line.substr(pos);
        break;
      }
    }
    pendingText += '\n';                            // hard break between lines
  }
  flushCode();
  if (blockTable) {
    flushPartialRows();
    if (groupOpen) {
      auto& g = pendingTable.topts.keepGroups.back();
      if (g.second < 0) g.second = (int)pendingTable.rows.size() - 1;
    }
    warnings.push_back("\\t table not closed with \\\\t before end of input");
  }
  closeFlowGroup();
  // EOF: finish pending content; trailing blank lines are capped at one
  // normal gap (a large trailing spacer could push out an empty page)
  flushTable();
  flushText();
  if (!tokens.empty() && tokens.back().kind != Token::Kind::ParagraphEnd) {
    Token tk; tk.kind = Token::Kind::ParagraphEnd;
    tokens.push_back(std::move(tk));
  }
  return tokens;
}

// ---- stage 2: build flowables ----------------------------------------------------
static pdf::FlowList buildFlowablesInner(const std::vector<Token>& tokens,
                                         const std::string& baseDir,
                                         std::vector<std::string>& warnings,
                                         const BuildContext* ctx);

pdf::FlowList buildFlowables(const std::vector<Token>& tokens,
                             const std::string& baseDir,
                             std::vector<std::string>& warnings,
                             const BuildContext* ctx) {
  using namespace pdf;
  FlowList flat = buildFlowablesInner(tokens, baseDir, warnings, ctx);
  // group marks: wrap the flowables between a start and its end into one
  // StackFlow so they can never be split across pages
  FlowList out;
  FlowList groupKids;
  bool inGroup = false;
  size_t flatIdx = 0;
  // rebuild the mark sequence alongside the flowables
  std::vector<int> marks;                 // 1 start, 0 end, -1 flowable
  for (const auto& t : tokens) {
    if (t.kind == Token::Kind::GroupMark) marks.push_back(t.blankLines);
  }
  // simpler: walk tokens and flowables in lockstep is fragile; instead the
  // inner builder emits sentinel spacers? -- no: use the marker flow below.
  (void)flatIdx; (void)marks;
  for (auto& f : flat) {
    if (auto* gm = dynamic_cast<GroupMarkFlow*>(f.get())) {
      if (gm->start()) {
        if (inGroup && !groupKids.empty())
          out.push_back(std::make_unique<StackFlow>(std::move(groupKids), 0.0));
        groupKids.clear();
        inGroup = true;
      } else {
        if (inGroup && !groupKids.empty())
          out.push_back(std::make_unique<StackFlow>(std::move(groupKids), 0.0));
        groupKids.clear();
        inGroup = false;
      }
      continue;
    }
    if (inGroup) groupKids.push_back(std::move(f));
    else out.push_back(std::move(f));
  }
  if (inGroup && !groupKids.empty())
    out.push_back(std::make_unique<StackFlow>(std::move(groupKids), 0.0));
  return out;
}

static pdf::FlowList buildFlowablesInner(const std::vector<Token>& tokens,
                                         const std::string& baseDir,
                                         std::vector<std::string>& warnings,
                                         const BuildContext* ctx) {
  using namespace pdf;
  FlowList out;
  const double gap = 0.3 * CM;                      // paragraph / image spacing

  auto makeImage = [&](const Token& t) -> std::unique_ptr<ImageFlow> {
    std::string path = baseDir.empty() ? t.text : baseDir + "/" + t.text;
    if (t.dpi > 0 && t.widthPt <= 0 && t.heightPt <= 0)
      warnings.push_back(t.text + ": dpi= needs width= or height= to know the "
                         "display size; embedding at full resolution");
    auto img = std::make_unique<ImageFlow>(path, t.widthPt, t.heightPt, t.dpi);
    if (!img->ok()) { warnings.push_back(img->error()); return nullptr; }
    if (t.hasAbs) img->setAbsolute(t.absX, t.absY);
    img->setOffset(t.dx, t.dy);
    img->setBackLayer(t.layerBack);
    if (t.align == 1) img->setAlign(pdf::Align::Center);
    if (t.align == 2) img->setAlign(pdf::Align::Right);
    return img;
  };

  // Builds one table cell from its markup: plain text becomes a Paragraph,
  // [image] tags become ImageFlows; an image with float= followed by text in
  // the same cell becomes a FloatFlow; an image alone with float=right is
  // right-aligned. Several items stack vertically.
  auto buildCell = [&](const std::string& markup, const Style& st) -> FlowPtr {
    // scan into segments
    struct Seg { bool isImage; std::string text; Token img; };
    std::vector<Seg> segs;
    std::string pend;
    size_t pos = 0;
    auto flushT = [&]() {
      std::string t = trim(pend);
      pend.clear();
      if (!t.empty()) segs.push_back({false, t, {}});
    };
    while (pos < markup.size()) {
      size_t br = markup.find('[', pos);
      Token img; size_t end;
      if (br != std::string::npos && tryParseImageTag(markup, br, img, end)) {
        pend += markup.substr(pos, br - pos);
        flushT();
        segs.push_back({true, "", img});
        pos = end;
      } else if (br != std::string::npos) {
        pend += markup.substr(pos, br - pos + 1);
        pos = br + 1;
      } else {
        pend += markup.substr(pos);
        break;
      }
    }
    flushT();

    // assemble segments into flowables
    FlowList items;
    for (size_t k = 0; k < segs.size(); ++k) {
      if (!segs[k].isImage) {
        // "# Titel" inside a cell becomes a real heading (the line-based
        // detection in tokenize() never sees cell content); the cell's
        // column alignment is kept
        std::string title;
        bool verbatimSeg = segs[k].text.find('\x03') != std::string::npos;
        segs[k].text = unescapeLiteralBrackets(segs[k].text);
        if (int lvl = !verbatimSeg ? headingLevel(segs[k].text, title) : 0;
            lvl && true) {
          Style hs = styles::heading(lvl);
          hs.align = st.align;
          hs.spaceBefore = hs.spaceAfter = 0;   // rows size by content only
          items.push_back(std::make_unique<Paragraph>(title, hs));
        } else {
          items.push_back(std::make_unique<Paragraph>(segs[k].text, st));
        }
        continue;
      }
      auto img = makeImage(segs[k].img);
      if (!img) continue;
      int fs = segs[k].img.floatSide;
      if (fs != 0 && k + 1 < segs.size() && !segs[k + 1].isImage) {
        auto para = std::make_unique<Paragraph>(segs[k + 1].text, st);
        items.push_back(std::make_unique<FloatFlow>(std::move(img), std::move(para),
                                                    fs == 2));
        ++k;
      } else {
        if (fs == 2) img->setAlign(Align::Right);
        else if (fs == 0 && st.align == Align::Right)  img->setAlign(Align::Right);
        else if (fs == 0 && st.align == Align::Center) img->setAlign(Align::Center);
        items.push_back(std::move(img));
      }
    }
    if (items.empty()) return std::make_unique<Paragraph>("", st);
    if (items.size() == 1) return std::move(items[0]);
    return std::make_unique<StackFlow>(std::move(items));
  };

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token& t = tokens[i];
    switch (t.kind) {
      case Token::Kind::Text: {
        out.push_back(std::make_unique<Paragraph>(
            unescapeLiteralBrackets(t.text), styles::body()));
        // a directly following table with its own spacing= governs the gap
        bool nextTableWithSpacing =
            i + 1 < tokens.size() && tokens[i + 1].kind == Token::Kind::Table &&
            tokens[i + 1].topts.spacing >= 0;
        if (!nextTableWithSpacing)
          out.push_back(std::make_unique<Spacer>(gap));
        break;
      }

      case Token::Kind::Heading:
        out.push_back(std::make_unique<Paragraph>(
            unescapeLiteralBrackets(t.text), styles::heading(t.level)));
        break;

      case Token::Kind::Image: {
        auto img = makeImage(t);
        if (!img) break;
        if (t.hasAbs) {                        // floats above the flow
          out.push_back(std::move(img));       // no spacer: takes no space
          break;
        }
        // float=left/right: bind the image to the next Text token so its
        // paragraph wraps around the image
        if (t.floatSide != 0 && i + 1 < tokens.size() &&
            tokens[i + 1].kind == Token::Kind::Text) {
          auto para = std::make_unique<Paragraph>(tokens[i + 1].text, styles::body());
          out.push_back(std::make_unique<FloatFlow>(
              std::move(img), std::move(para), t.floatSide == 2));
          out.push_back(std::make_unique<Spacer>(gap));
          ++i;                                       // consume the Text token
          break;
        }
        if (t.floatSide == 2) img->setAlign(Align::Right);
        out.push_back(std::move(img));
        out.push_back(std::make_unique<Spacer>(gap));
        break;
      }

      case Token::Kind::Table: {
#ifdef PDFGEN_DEBUG_SPANS
        fprintf(stderr, "TABLE rows=%zu spans=%zu\n", t.rows.size(), t.spans.size());
        for (size_t r = 0; r < t.rows.size(); ++r) {
          fprintf(stderr, "  row %zu cells=%zu spans:", r, t.rows[r].size());
          if (r < t.spans.size())
            for (int sp : t.spans[r]) fprintf(stderr, " %d", sp);
          else fprintf(stderr, " <missing>");
          fprintf(stderr, "  [%.30s]\n", t.rows[r].empty() ? "" : t.rows[r][0].c_str());
        }
#endif
        // full-width cells (span -1 from block tables) span ALL columns
        size_t nCols = 1;
        for (size_t r = 0; r < t.rows.size(); ++r) {
          size_t sum = 0;
          for (size_t c = 0; c < t.rows[r].size(); ++c) {
            int sp = (r < t.spans.size() && c < t.spans[r].size())
                         ? t.spans[r][c] : 1;
            sum += sp > 0 ? (size_t)sp : 1;
          }
          nCols = std::max(nCols, sum);
        }
        std::vector<TableFlow::Row> rows;
        for (size_t r = 0; r < t.rows.size(); ++r) {
          TableFlow::Row row;
          size_t col = 0;                         // column the cell starts in
          for (size_t c = 0; c < t.rows[r].size(); ++c) {
            int span = (r < t.spans.size() && c < t.spans[r].size())
                           ? t.spans[r][c] : 1;
            if (span < 0) span = (int)nCols;         // full-width cell
            // cells use the body text metrics (size + leading) so that with
            // padding/spacing set to 0 a table row equals exactly one body
            // text line; only the justification is replaced by Left
            Style st = pdf::styles::body();
            st.align = Align::Left;
            if ((int)r < t.headerRows) st.font = Font::HelveticaBold;
            if (col < t.aligns.size()) {          // alignment of first column
              if (t.aligns[col] == 1) st.align = Align::Center;
              if (t.aligns[col] == 2) st.align = Align::Right;
            }
            TableFlow::Cell cell;
            cell.flow = buildCell(t.rows[r][c], st);
            cell.span = span;
            row.push_back(std::move(cell));
            col += (size_t)std::max(1, span);
          }
          rows.push_back(std::move(row));
        }
        out.push_back(std::make_unique<TableFlow>(std::move(rows), t.headerRows,
                                                  t.topts));
        // when the tag defines its own outer spacing, don't add the default gap
        if (t.topts.spacing < 0)
          out.push_back(std::make_unique<Spacer>(gap));
        break;
      }

      case Token::Kind::ExternalPdf: {
        std::string path = baseDir.empty() ? t.text : baseDir + "/" + t.text;
        std::ifstream in(path, std::ios::binary);
        if (!in) { warnings.push_back("[pdf=...]: cannot open " + path); break; }
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        if (bytes.compare(0, 5, "%PDF-") != 0) {
          warnings.push_back("[pdf=...]: not a PDF file: " + path);
          break;
        }
        auto epf = std::make_unique<ExternalPdfFlow>(std::move(bytes));
        epf->setNoBlankBefore(t.noBlank);
        out.push_back(std::move(epf));
        break;
      }

      case Token::Kind::IncludeSource: {
        if (!ctx || !ctx->renderInclude) {
          warnings.push_back("[pdfgen=..., include] is not available in this context");
          break;
        }
        std::string bytes, err;
        if (!ctx->renderInclude(t.text, bytes, err)) {
          warnings.push_back("include of " + t.text + " failed: " + err);
          break;
        }
        auto epf = std::make_unique<ExternalPdfFlow>(std::move(bytes));
        epf->setNoBlankBefore(t.noBlank);
        out.push_back(std::move(epf));
        break;
      }

      case Token::Kind::CodeBlock: {
        // monospace block, indentation preserved via NBSP
        std::string txt;
        for (char c : t.text) {
          if (c == ' ') txt += "\xC2\xA0";          // NBSP survives wrapping
          else if (c == '\x06') txt += '\x06';      // '<' restored in Paragraph
          else txt += c;
        }
        Style cs = styles::code();
        cs.spaceBefore = 4;
        cs.spaceAfter = 6;
        out.push_back(std::make_unique<Paragraph>(
            unescapeLiteralBrackets(txt), cs));
        break;
      }

      case Token::Kind::HRule:
        out.push_back(std::make_unique<HRuleFlow>());
        break;

      case Token::Kind::Quote: {
        Style qs = styles::body();
        qs.leftIndent = 18;
        out.push_back(std::make_unique<Paragraph>(
            unescapeLiteralBrackets(t.text), qs));
        break;
      }

      case Token::Kind::ListItem: {
        Style ls = styles::body();
        ls.leftIndent = 16.0 + 14.0 * t.level;
        ls.hangOutdent = 16.0;
        std::string txt = t.text;
        size_t sep = txt.find('\x07');
        if (sep != std::string::npos)
          txt = txt.substr(0, sep) + "\xC2\xA0" + txt.substr(sep + 1);
        out.push_back(std::make_unique<Paragraph>(
            unescapeLiteralBrackets(txt), ls));
        break;
      }

      case Token::Kind::VSpace:
        out.push_back(std::make_unique<Spacer>(t.vspacePts));
        break;

      case Token::Kind::GroupMark:
        out.push_back(std::make_unique<GroupMarkFlow>(t.blankLines == 1));
        break;

      case Token::Kind::NewPage:
        out.push_back(std::make_unique<PageBreakFlow>(t.noBlank));
        break;

      case Token::Kind::MarginsChange:
        out.push_back(std::make_unique<MarginsFlow>(
            t.margins[0], t.margins[1], t.margins[2], t.margins[3]));
        break;

      case Token::Kind::ParagraphEnd:
        out.push_back(std::make_unique<Spacer>(gap * t.blankLines));
        break;
    }
  }
  return out;
}

} // namespace markup
