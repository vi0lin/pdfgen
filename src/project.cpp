#include "project.h"
#include "htmlwriter.h"
#include <cstring>
#include "jsondata.h"
#include <set>
#include <algorithm>   // std::count (MSVC/MinGW pull this in transitively, GCC-libstdc++ on Linux does -- never rely on it)

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

#include "flowables.h"
#include "markup.h"
#include "pdfwriter.h"

#include "pdfgen_host.h"   // readFile, Log

// ---- release consistency check ----
#ifndef PDFGEN_MARKUP_API
#error "stale markup.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_MARKUP_API != 20
#error "version mismatch in markup.h: replace ALL pdfgen source files from the same release."
#endif
#ifndef PDFGEN_FLOWABLES_API
#error "stale flowables.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_FLOWABLES_API != 13
#error "version mismatch in flowables.h: replace ALL pdfgen source files from the same release."
#endif

namespace project {
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
// Whitespace split with double-quote support: attachment="Mit Leerzeichen.pdf"
// bleibt EIN Wert (wichtig, sobald $heading & Co. in Dateinamen stecken).
std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::string w;
  bool q = false;
  for (char c : s) {
    if (c == '"') { q = !q; continue; }
    if (!q && (c == ' ' || c == '\t' || c == '\n')) {
      if (!w.empty()) { out.push_back(w); w.clear(); }
    } else w += c;
  }
  if (!w.empty()) out.push_back(w);
  return out;
}
std::string readFile(const std::string& path) {
  std::string x;
  pdfgen::readFile(path, x);
  return x;
}

// A control tag: whole (possibly multi-line) "[...]" starting a line with
// pdfgen/mail/attach as first word. Returns kind ("" = not a control tag)
// and the comma-separated parts.
struct Tag { std::string kind, headValue; std::vector<std::pair<std::string,std::string>> opts; };

bool parseTag(const std::string& tagText, Tag& out) {
  std::string t = trim(tagText);
  if (t.size() < 3 || t.front() != '[' || t.back() != ']') return false;
  std::string inner = t.substr(1, t.size() - 2);
  std::vector<std::string> parts;
  {
    std::string cur;
    for (char c : inner) {
      if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
      else cur += c;
    }
    parts.push_back(trim(cur));
  }
  if (parts.empty()) return false;
  std::string head = parts[0];
  size_t eq = head.find('=');
  out.kind = toLower(trim(eq == std::string::npos ? head : head.substr(0, eq)));
  out.headValue = eq == std::string::npos ? "" : trim(head.substr(eq + 1));
  if (out.kind != "pdfgen" && out.kind != "mail" && out.kind != "attach" &&
      out.kind != "doc" && out.kind != "html") return false;
  out.opts.clear();
  for (size_t i = 1; i < parts.size(); ++i) {
    size_t e = parts[i].find('=');
    if (e == std::string::npos) out.opts.push_back({toLower(parts[i]), ""});
    else out.opts.push_back({toLower(trim(parts[i].substr(0, e))), trim(parts[i].substr(e + 1))});
  }
  return true;
}

// margin keys on a tag / a [margins...] line -> layout; unset keys keep
// their previous (default) values
void applyMarginOpts(const std::vector<std::pair<std::string,std::string>>& opts,
                     const std::string& headValue, PageLayout& lay,
                     std::vector<std::string>& warnings) {
  auto dim = [&](const std::string& v, double& slot) {
    double d;
    if (markup::evalDimension(v, d)) slot = d;
    else warnings.push_back("bad margin value: " + v);
  };
  if (!headValue.empty()) {                       // [margins=l:r:t:b]
    std::stringstream ss(headValue);
    std::string part;
    double* slots[4] = {&lay.left, &lay.right, &lay.top, &lay.bottom};
    int i = 0;
    while (std::getline(ss, part, ':') && i < 4) dim(trim(part), *slots[i++]);
  }
  for (auto& [k, v] : opts) {
    if (k == "margins" || k == "raender" || k == "rand") {   // margins=l:r:t:b
      std::stringstream ss(v);
      std::string part;
      double* slots[4] = {&lay.left, &lay.right, &lay.top, &lay.bottom};
      int i = 0;
      while (std::getline(ss, part, ':') && i < 4) dim(trim(part), *slots[i++]);
    }
    else if (k == "left"   || k == "links")  dim(v, lay.left);
    else if (k == "right"  || k == "rechts") dim(v, lay.right);
    else if (k == "top"    || k == "oben")   dim(v, lay.top);
    else if (k == "bottom" || k == "unten")  dim(v, lay.bottom);
  }
}

// Finds a LEADING whole-line [margins...] tag (only blank lines before it)
// and applies it as the document's base layout, removing the line. A
// [margins] tag after any content stays in the flow and changes the margins
// from that point on instead.
void extractMarginsTag(std::string& text, PageLayout& lay,
                       std::vector<std::string>& warnings) {
  std::stringstream ss(text);
  std::string line, out;
  bool done = false;
  bool contentSeen = false;
  while (std::getline(ss, line)) {
    std::string t = trim(line);
    std::string low = toLower(t);
    if (!done && !contentSeen && low.rfind("[margins", 0) == 0 &&
        !t.empty() && t.back() == ']') {
      std::string inner = t.substr(1, t.size() - 2);
      std::vector<std::string> parts;
      std::string cur;
      for (char c : inner) {
        if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
        else cur += c;
      }
      parts.push_back(trim(cur));
      std::string head = parts.empty() ? "" : parts[0];
      size_t eq = head.find('=');
      std::string headValue = eq == std::string::npos ? "" : trim(head.substr(eq + 1));
      std::vector<std::pair<std::string,std::string>> opts;
      for (size_t i = 1; i < parts.size(); ++i) {
        size_t e = parts[i].find('=');
        if (e == std::string::npos) opts.push_back({toLower(parts[i]), ""});
        else opts.push_back({toLower(trim(parts[i].substr(0, e))), trim(parts[i].substr(e + 1))});
      }
      applyMarginOpts(opts, headValue, lay, warnings);
      done = true;
      continue;                                   // strip the line
    }
    if (!t.empty()) contentSeen = true;
    out += line + "\n";
  }
  text = out;
}

bool controlConditionPasses(const Tag& t, std::vector<std::string>& warnings) {
  for (auto& kv : t.opts) {
    if (kv.first != "condition") continue;
    std::string e = trim(kv.second);
    bool neg = !e.empty() && e[0] == '!';
    if (neg) e = trim(e.substr(1));
    bool value = false;                 // top-level sources are never embedded
    if (toLower(e) == "embedded") value = false;
    else warnings.push_back("unknown condition: " + kv.second);
    return neg ? !value : value;
  }
  return true;
}

// Extracts a [header]...[/header] or [bottom]...[/bottom] block (whole
// lines) and removes it from the text. Returns the block's inner markup.
std::string extractBlock(std::string& text, const std::string& open,
                         const std::string& close) {
  std::stringstream ss(text);
  std::string line, out, block;
  bool in = false, taken = false;
  while (std::getline(ss, line)) {
    std::string low = toLower(trim(line));
    if (!taken && !in && low == open) { in = true; continue; }
    if (in && low == close) { in = false; taken = true; continue; }
    if (in) block += line + "\n";
    else out += line + "\n";
  }
  if (in) block.clear();                // unclosed block: leave text as-is
  else text = out;
  return taken ? block : std::string();
}

// Expands whole-line [embed=Quelle.txt(, newpage)(, keepmargins)] tags.
// The embedded content runs with the `embedded` condition active; without
// keepmargins its [margins] tags apply and the parent layout is restored
// after the block.
std::string expandEmbeds(const std::string& text, const std::string& baseDir,
                         const PageLayout& parentLayout,
                         std::vector<std::string>& warnings, int depth) {
  if (depth > 6) {
    warnings.push_back("embed depth limit reached (circular [embed]?)");
    return text;
  }
  std::stringstream ss(text);
  std::string line, out;
  while (std::getline(ss, line)) {
    std::string t = trim(line);
    std::string low = toLower(t);
    if (low.rfind("[embed", 0) != 0 || t.empty() || t.back() != ']') {
      out += line + "\n";
      continue;
    }
    std::string inner = t.substr(1, t.size() - 2);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : inner) {
      if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
      else cur += c;
    }
    parts.push_back(trim(cur));
    std::string src;
    {
      size_t eq = parts[0].find('=');
      if (eq != std::string::npos) src = trim(parts[0].substr(eq + 1));
    }
    bool newpage = false, newpageNoBlank = false, keepmargins = false;
    for (size_t i = 1; i < parts.size(); ++i) {
      std::string k = toLower(parts[i]);
      if (k == "newpage" || k == "neueseite") newpage = true;
      if (k == "newpage_no_blank" || k == "newpage_noblank" ||
          k == "neueseite_keine_leerseite")
        newpageNoBlank = true;
      if (k == "keepmargins" || k == "raenderbehalten") keepmargins = true;
    }
    if (src.empty()) {
      warnings.push_back("[embed] needs a source: [embed=Quelle.txt]");
      continue;
    }
    std::string child = markup::readDataFile(baseDir, src);
    if (child.empty()) {
      warnings.push_back("[embed]: cannot read " + src);
      continue;
    }
    bool childMd = src.size() > 3 &&
                   toLower(src).compare(src.size() - 3, 3, ".md") == 0;
    child = markup::preprocessSource(child, /*embedded=*/true, warnings, childMd);
    // an embedded file cannot redefine the parent's header/footer
    extractBlock(child, "[header]", "[/header]");
    extractBlock(child, "[bottom]", "[/bottom]");
    if (keepmargins) {
      // drop the child's [margins] lines so the parent margins stay
      std::stringstream cs(child);
      std::string cl, rebuilt;
      while (std::getline(cs, cl))
        if (toLower(trim(cl)).rfind("[margins", 0) != 0) rebuilt += cl + "\n";
      child = rebuilt;
    }
    child = expandEmbeds(child, baseDir, parentLayout, warnings, depth + 1);

    if (newpageNoBlank) out += "[newpage, no_blank]\n";
    else if (newpage)    out += "[newpage]\n";
    out += child;
    if (!out.empty() && out.back() != '\n') out += "\n";
    if (!keepmargins) {                 // restore the parent margins
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "[margins, left=%.3f*pt, right=%.3f*pt, top=%.3f*pt, bottom=%.3f*pt]\n",
                    parentLayout.left, parentLayout.right,
                    parentLayout.top, parentLayout.bottom);
      out += buf;
    }
  }
  return out;
}

const std::string* opt(const Tag& t, const std::string& key) {
  for (auto& kv : t.opts)
    if (kv.first == key) return &kv.second;
  return nullptr;
}
bool hasFlag(const Tag& t, const std::string& key) {
  for (auto& kv : t.opts)
    if (kv.first == key && kv.second.empty()) return true;
  return false;
}

} // namespace

// ---- section parser --------------------------------------------------------

// ==== mail-merge: \jloop / \jload / $placeholders / $-Bedingungen ===========
// Laeuft auf der ROHEN Hauptquelle, bevor Dokumente/Mails gesplittet werden,
// damit ein Schleifenkoerper \pdfgen/\mail/\attach vervielfachen kann.
namespace {

using jsondata::Value;

struct MergeFrame {
  std::string name;        // "" = aktueller Datensatz ($_, nackte Keys)
  const Value* value;
  long index1 = 0;         // 1-basiert fuer $#
};

struct MergeCtx {
  std::vector<MergeFrame> frames;
  std::map<std::string, Value> files;
  std::map<std::string, std::string> vars;   // $name=wert assignments
  std::map<std::string, std::vector<std::string>> segments;  // \segment
  std::set<std::string> segExpanding;        // Zyklus-Schutz
  std::map<std::string, long> varRecPtr;     // $name:#:: Paket-Zeiger
  int embedSeq = 0;                          // \e-Datenparams: Zaehler

  const Value* lookup(const std::string& path, long* idx = nullptr) const {
    for (size_t i = frames.size(); i-- > 0;) {
      const MergeFrame& f = frames[i];
      if (path == "_" || path.rfind("_.", 0) == 0 || path.rfind("_[", 0) == 0) {
        if (!f.name.empty()) continue;
        if (idx) *idx = f.index1;
        if (path == "_") return f.value;
        size_t off = path[1] == '.' ? 2 : 1;
        return &f.value->at(path.substr(off));
      }
      if (!f.name.empty()) {
        if (path == f.name) { if (idx) *idx = f.index1; return f.value; }
        if (path.rfind(f.name + ".", 0) == 0 ||
            path.rfind(f.name + "[", 0) == 0) {
          size_t off = f.name.size() + (path[f.name.size()] == '.' ? 1 : 0);
          return &f.value->at(path.substr(off));
        }
      } else {
        const Value& v = f.value->at(path);
        if (!v.isNull()) { if (idx) *idx = f.index1; return &v; }
        if (f.value->kind() == Value::Kind::Object)
          for (const auto& k : f.value->keys())
            if (k == path) return &f.value->at(path);   // existiert, ist null
      }
    }
    return nullptr;
  }
  long currentIndex() const {
    for (size_t i = frames.size(); i-- > 0;)
      if (frames[i].name.empty()) return frames[i].index1;
    return 0;
  }
};

bool mergePathChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '.' ||
         c == '[' || c == ']';
}

// ---- Bereichs-Logik auf Variablenwerten: $name::0, $name::0:, $name:#0:0:
// Der Wert wird an D= (Standard \n\n) in Pakete geteilt; die Range-Formen
// entsprechen der Loop-Grammatik. Rueckgabe: true wenn ab `pos` eine Range
// stand (dann ist `out` die Lieferung und `pos` hinter dem Konstrukt).
std::string miniUnescape(const std::string& v) {
  std::string o;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char e = v[++i];
      if (e == 'n') o += '\n';
      else if (e == 't') o += '\t';
      else o += e;
    } else o += v[i];
  }
  return o;
}

bool applyVarRange(const std::string& line, size_t& pos,
                   const std::string& name, const std::string& value,
                   MergeCtx& ctx, std::string& out) {
  size_t i = pos;
  if (i >= line.size() || line[i] != ':') return false;
  ++i;
  if (i >= line.size() || (line[i] != ':' && line[i] != '#')) return false;
  bool recMode = line[i] == '#';
  long recNum = -1;
  if (recMode) {
    ++i;
    bool any = false;
    long v = 0;
    while (i < line.size() && std::isdigit((unsigned char)line[i]))
      { v = v * 10 + (line[i] - '0'); ++i; any = true; }
    if (any) recNum = v;
    if (i >= line.size() || line[i] != ':') return false;
  }
  // range token  :a | :a: | :a:b | ::
  ++i;                                             // first range ':'
  long a = -1, b = -1;
  bool aAny = false, openEnd = false, haveB = false;
  while (i < line.size() && std::isdigit((unsigned char)line[i]))
    { a = (aAny ? a : 0) * 10 + (line[i] - '0'); ++i; aAny = true; }
  if (i < line.size() && line[i] == ':') {
    ++i;
    bool bAny = false;
    while (i < line.size() && std::isdigit((unsigned char)line[i]))
      { b = (bAny ? b : 0) * 10 + (line[i] - '0'); ++i; bAny = true; }
    if (!bAny) openEnd = true;
    else haveB = true;
  }
  if (!aAny && !openEnd && !haveB && !recMode) return false;   // nur ":" ohne alles

  // optionales ", D=..." direkt dahinter
  std::string delim = "\n\n";
  {
    size_t k = i;
    if (k < line.size() && line[k] == ',') {
      ++k;
      while (k < line.size() && line[k] == ' ') ++k;
      if (k + 1 < line.size() && (line[k] == 'D' || line[k] == 'd') &&
          line[k + 1] == '=') {
        k += 2;
        std::string raw;
        while (k < line.size() && line[k] != ' ' && line[k] != ',' &&
               line[k] != '\t')
          raw += line[k++];
        if (!raw.empty()) { delim = miniUnescape(raw); i = k; }
      }
    }
  }

  // Wert in Zeilen + Pakete teilen
  std::vector<std::string> fl;
  {
    std::string norm;
    for (char c : value) if (c != '\r') norm += c;
    std::stringstream ss(norm);
    std::string l;
    while (std::getline(ss, l)) fl.push_back(l);
    // Pakete
    std::vector<std::pair<long, long>> recs;
    {
      long start = 0;
      size_t p2 = 0;
      while ((p2 = norm.find(delim, p2)) != std::string::npos) {
        long lineOfHit = (long)std::count(norm.begin(),
                                          norm.begin() + (long)p2, '\n');
        long end = delim[0] == '\n' ? lineOfHit : lineOfHit - 1;
        if (end >= start) recs.push_back({start, end});
        long dl = (long)std::count(delim.begin(), delim.end(), '\n');
        start = lineOfHit + (delim[0] == '\n' ? dl : dl + 1);
        p2 += delim.size();
      }
      if (start <= (long)fl.size() - 1) recs.push_back({start, (long)fl.size() - 1});
      if (recs.empty() && !fl.empty()) recs.push_back({0, (long)fl.size() - 1});
    }
    long lo = 0, hi = (long)fl.size() - 1;         // Lieferbereich global
    if (recMode) {
      long r = recNum >= 0 ? recNum : ctx.varRecPtr[name];
      if (r < 0 || r >= (long)recs.size()) { pos = i; out.clear(); 
        ctx.varRecPtr[name] = r + 1; return true; }
      lo = recs[(size_t)r].first;
      hi = recs[(size_t)r].second;
      ctx.varRecPtr[name] = r + 1;                 // Zeiger ruecken
      if (aAny) {
        long s2 = lo + a;
        long e2 = haveB ? lo + b : (openEnd ? hi : s2);
        lo = s2; hi = std::min(e2, hi);
      } else if (!openEnd && haveB) { /* unreachable */ }
      // "::" ohne a: ganzes Paket
    } else {
      if (!aAny) { pos = i; out.clear(); return true; }   // "$x:::" Unsinn
      long s2 = a;
      long e2;
      if (haveB) e2 = b;
      else if (openEnd) {                          // bis Delimiter-Segment-Ende
        e2 = (long)fl.size() - 1;
        for (auto& rc : recs)
          if (s2 >= rc.first && s2 <= rc.second) { e2 = rc.second; break; }
      } else e2 = s2;                              // einzelne Zeile
      lo = s2; hi = std::min(e2, (long)fl.size() - 1);
    }
    out.clear();
    if (lo <= hi && lo < (long)fl.size())
      for (long k2 = std::max(lo, 0L); k2 <= hi; ++k2) {
        if (k2 > std::max(lo, 0L)) out += '\n';
        out += fl[(size_t)k2];
      }
  }
  pos = i;
  return true;
}

// $pfad / ${pfad} / $# ersetzen; unbekannte Namen bleiben woertlich
// (so ueberleben \loop-Mementos wie $a und gewoehnliche $-Zeichen).
std::string mergeSubst(const std::string& line, MergeCtx& ctx) {
  std::string out;
  size_t i = 0, n = line.size();
  while (i < n) {
    char c = line[i];
    if (c != '$') { out += c; ++i; continue; }
    if (i + 1 < n && line[i + 1] == '#') {
      // "$#:" / "$##" / "$#0:" belong to the \loop/\file record engine
      if (i + 2 < n && (line[i + 2] == ':' || line[i + 2] == '#' ||
                        std::isdigit((unsigned char)line[i + 2]))) {
        out += c; ++i; continue;
      }
      long ix = ctx.currentIndex();
      if (ix > 0) { out += std::to_string(ix); i += 2; continue; }
      out += c; ++i; continue;
    }
    bool braced = i + 1 < n && line[i + 1] == '{';
    size_t start = i + (braced ? 2 : 1), e = start;
    if (braced) {
      e = line.find('}', start);
      if (e == std::string::npos) { out += c; ++i; continue; }
    } else {
      while (e < n && mergePathChar(line[e])) ++e;
      while (e > start && line[e - 1] == '.') --e;   // Satzpunkt nach $name.
    }
    std::string path = line.substr(start, e - start);
    if (path.empty()) { out += c; ++i; continue; }
    const Value* v = ctx.lookup(path);
    std::string val;
    bool hit = false;
    if (v) { val = v->toText(); hit = true; }
    else {
      auto vi = ctx.vars.find(path);               // plain $name variables
      if (vi != ctx.vars.end()) { val = vi->second; hit = true; }
    }
    if (!hit) {                                  // Segmente als $Name
      auto si = ctx.segments.find(path);
      if (si != ctx.segments.end() && !ctx.segExpanding.count(path)) {
        ctx.segExpanding.insert(path);
        for (size_t k2 = 0; k2 < si->second.size(); ++k2) {
          if (k2) val += '\n';
          val += mergeSubst(si->second[k2], ctx);
        }
        ctx.segExpanding.erase(path);
        hit = true;
      }
    }
    if (!hit) { out += c; ++i; continue; }
    size_t after = e + (braced ? 1 : 0);
    std::string ranged;
    if (applyVarRange(line, after, path, val, ctx, ranged)) {
      out += ranged;                               // $name::0 / :#0:0: ...
      i = after;
      continue;
    }
    out += val;
    i = e + (braced ? 1 : 0);
  }
  return out;
}

// Bedingung mit $: "$pfad", "!$pfad", "$pfad=wert", "$pfad!=wert".
// applies sagt, ob der Ausdruck ueberhaupt eine Merge-Bedingung ist.
bool mergeCond(const std::string& exprIn, MergeCtx& ctx, bool& applies) {
  std::string expr = exprIn;
  applies = expr.find('$') != std::string::npos;
  if (!applies) return true;
  bool neg = !expr.empty() && expr[0] == '!';
  if (neg) expr = expr.substr(1);
  std::string lit;
  bool eq = true, hasCmp = false;
  size_t ne = expr.find("!=");
  size_t qe = expr.find('=');
  if (ne != std::string::npos) {
    hasCmp = true; eq = false;
    lit = expr.substr(ne + 2); expr = expr.substr(0, ne);
  } else if (qe != std::string::npos) {
    hasCmp = true;
    lit = expr.substr(qe + 1); expr = expr.substr(0, qe);
  }
  expr = trim(expr); lit = trim(lit);
  if (!expr.empty() && expr[0] == '$') expr = expr.substr(1);
  const Value* v = ctx.lookup(expr);
  std::string varVal;
  bool varHit = false;
  if (!v) {
    auto vi = ctx.vars.find(expr);
    if (vi != ctx.vars.end()) { varVal = vi->second; varHit = true; }
  }
  bool r;
  if (hasCmp) {
    std::string val = v ? v->toText() : varVal;
    std::string want = mergeSubst(lit, ctx);        // $k.email=$_ erlaubt
    r = (val == want) == eq;
  } else {
    r = v ? v->truthy() : (varHit && !varVal.empty());
  }
  return neg ? !r : r;
}

const Value* mergeLoadJson(MergeCtx& ctx, const std::string& baseDir,
                           const std::string& file,
                           std::vector<std::string>& warnings) {
  std::string path = baseDir.empty() ? file : baseDir + "/" + file;
  auto it = ctx.files.find(path);
  if (it == ctx.files.end()) {
    std::string txt = markup::readDataFile(baseDir, file);
    if (txt.empty()) {
      warnings.push_back("\\jloop/\\jload: kann '" + path + "' nicht lesen");
      return nullptr;
    }
    std::string err;
    Value v = Value::parse(txt, err);
    if (!err.empty()) {
      warnings.push_back("\\jloop/\\jload " + file + ": " + err);
      return nullptr;
    }
    it = ctx.files.emplace(path, std::move(v)).first;
  }
  return &it->second;
}

size_t mergeFindClose(const std::vector<std::string>& lines, size_t open,
                      const char* openTag, const char* closeTag) {
  int depth = 1;
  for (size_t j = open + 1; j < lines.size(); ++j) {
    std::string lt = toLower(trim(lines[j]));
    bool opens = lt.rfind(openTag, 0) == 0 &&
                 lt.rfind(closeTag, 0) != 0;
    if (opens) ++depth;
    else if (lt == closeTag && --depth == 0) return j;
  }
  return lines.size();
}

std::string expandMergeJson(const std::vector<std::string>& lines,
                            size_t from, size_t to, MergeCtx& ctx,
                            const std::string& baseDir,
                            std::vector<std::string>& warnings) {
  std::string out;
  auto emit = [&](const std::string& l) { out += l; out += '\n'; };
  size_t frameFloor = ctx.frames.size();            // \jload-Frames hier lokal
  auto savedVars = ctx.vars;                        // Tag-Scope: Variablen
                                                    // beim Verlassen zurueck
  std::vector<std::map<std::string, std::string>> scopeStack;   // \c / \mail

  for (size_t li = from; li < to; ++li) {
    std::string t = trim(lines[li]);
    std::string low = toLower(t);

    // ---- \virtual name ... \\virtual: inline-Datei registrieren ----
    // (Aliasse: \json, \txt, \text, \data -- gleiche Bedeutung)
    std::string vword;
    for (const char* w : { "virtual", "json", "txt", "text", "data",
                           "list", "nb" }) {
      size_t wl = std::strlen(w);
      if (low.size() > wl && low[0] == '\\' &&
          low.compare(1, wl, w) == 0 &&
          (low.size() == wl + 1 || low[wl + 1] == ' ' || low[wl + 1] == '\t')) {
        vword = w;
        break;
      }
    }
    if (!vword.empty()) {
      std::string name = trim(mergeSubst(trim(t.substr(vword.size() + 1)), ctx));
      const std::string openT = "\\" + vword, closeT = "\\\\" + vword;
      size_t end = li + 1;
      bool closedV = false;
      int dv = 1;
      for (; end < to; ++end) {
        std::string et = toLower(trim(lines[end]));
        if (et.rfind(openT, 0) == 0 && et.rfind(closeT, 0) != 0)
          ++dv;
        else if ((et == closeT || et == "\\\\virtual") && --dv == 0)
          { closedV = true; break; }
      }
      if (!closedV) {
        warnings.push_back("\\" + vword + " " + name + ": \\\\" + vword + " fehlt");
        break;
      }
      if (name.empty()) {
        warnings.push_back("\\" + vword + " braucht einen Namen: \\" + vword + " datei.txt");
      } else {
        std::string content;
        for (size_t j = li + 1; j < end; ++j) {
          content += mergeSubst(lines[j], ctx);     // $vars/$keys einsetzen
          content += '\n';
        }
        if (!content.empty()) content.pop_back();
        markup::setVirtualFile(name, content);
      }
      li = end;                                     // Block verschwindet
      continue;
    }

    // ---- \e quelle.txt, Name=datei, ...: Embed mit DATEN-PARAMETERN ----
    // Jeder unbekannte Name=Wert wird als Variable an das Kind gereicht:
    // existiert eine (virtuelle) Datei dieses Namens, ist der Wert deren
    // INHALT, sonst der Wert selbst. Das Kind laeuft hier durch den Walker
    // ($Name, Ranges, \jloop/\jread), seine Projekt-Tags (\mail...\\mail,
    // \attach, einzeilige \pdfgen/\doc/\html mit Quelle) werden auf die
    // HAUPTEBENE gehoben (-> Feedback/--gen), der Rest geht als virtuelle
    // Datei in den normalen \e-Pfad (embedded-Condition, Raender).
    if ((low.rfind("\\e", 0) == 0 &&
         (t.size() == 2 || t[2] == ' ' || t[2] == ',')) ||
        (low.rfind("\\embed", 0) == 0 &&
         (t.size() == 6 || t[6] == ' ' || t[6] == ','))) {
      size_t hl = low[2] == 'm' || (low.size() > 2 && low[1] == 'e' &&
                                    low[2] == 'm') ? 6 : 2;
      hl = low.rfind("\\embed", 0) == 0 ? 6 : 2;
      std::string rest = trim(t.substr(hl));
      if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));
      std::vector<std::string> parts;
      {
        std::stringstream ps(rest);
        std::string part;
        while (std::getline(ps, part, ',')) parts.push_back(trim(part));
      }
      auto knownOpt = [](const std::string& kl) {
        return kl == "newpage" || kl == "neueseite" ||
               kl == "newpage_no_blank" || kl == "newpage_noblank" ||
               kl == "neueseite_keine_leerseite" || kl == "keepmargins" ||
               kl == "raenderbehalten" || kl.rfind("condition=", 0) == 0 ||
               kl.rfind("bedingung=", 0) == 0;
      };
      std::vector<std::pair<std::string, std::string>> dataParams;
      std::vector<std::string> keepOpts;
      std::string src = parts.empty() ? "" : parts[0];
      for (size_t pi = 1; pi < parts.size(); ++pi) {
        std::string kl = toLower(parts[pi]);
        size_t eq = parts[pi].find('=');
        if (knownOpt(kl) || eq == std::string::npos) {
          keepOpts.push_back(parts[pi]);
          continue;
        }
        dataParams.push_back({trim(parts[pi].substr(0, eq)),
                              trim(parts[pi].substr(eq + 1))});
      }
      if (dataParams.empty()) {                    // klassisches \e
        emit(mergeSubst(lines[li], ctx));
        continue;
      }
      src = trim(mergeSubst(src, ctx));
      std::string childRaw = markup::readDataFile(baseDir, src);
      if (childRaw.empty()) {
        warnings.push_back("\\e " + src + ": Quelle nicht lesbar");
        continue;
      }
      auto savedV = ctx.vars;
      for (auto& dp : dataParams) {
        std::string val = trim(mergeSubst(dp.second, ctx));
        std::string content = markup::readDataFile(baseDir, val);
        ctx.vars[dp.first] = content.empty() ? val : content;
      }
      std::vector<std::string> cl;
      {
        std::stringstream cs(markup::translateBracketTags(childRaw));
        std::string l;
        while (std::getline(cs, l)) {
          if (!l.empty() && l.back() == '\r') l.pop_back();
          cl.push_back(l);
        }
      }
      std::string childOut =
          expandMergeJson(cl, 0, cl.size(), ctx, baseDir, warnings);
      ctx.vars = savedV;
      // Projekt-Tags heben
      std::vector<std::string> ol;
      {
        std::stringstream os(childOut);
        std::string l;
        while (std::getline(os, l)) ol.push_back(l);
      }
      std::string rest2;
      for (size_t j = 0; j < ol.size(); ++j) {
        std::string jt = trim(ol[j]);
        std::string jl = toLower(jt);
        bool mailTag = (jl.rfind("\\mail", 0) == 0 &&
                        (jt.size() == 5 || jt[5] == ' ' || jt[5] == ',')) ||
                       jl.rfind("[mail", 0) == 0;
        bool attachTag = (jl.rfind("\\attach", 0) == 0) ||
                         jl.rfind("[attach", 0) == 0;
        if (mailTag) {
          size_t me = j;
          bool found = false;
          for (size_t k = j; k < ol.size(); ++k) {
            std::string kl = toLower(trim(ol[k]));
            if (kl == "\\\\mail" || kl == "[/mail]") { me = k; found = true; break; }
          }
          if (!found) {
            warnings.push_back("\\e " + src + ": \\mail im Kind ohne "
                               "\\\\mail -- Mail bis Dateiende gehoben");
            me = ol.size() - 1;
          }
          for (size_t k = j; k <= me; ++k) emit(ol[k]);
          j = me;
          continue;
        }
        if (attachTag) { emit(ol[j]); continue; }
        bool docTag = false;
        for (const char* w : { "\\pdfgen", "\\doc", "\\html" }) {
          size_t wl = std::strlen(w);
          if (jl.rfind(w, 0) == 0 &&
              (jt.size() == wl || jt[wl] == ' ' || jt[wl] == ',')) {
            // nur einzeilig MIT Quelldatei heben
            std::string arg = trim(jt.substr(wl));
            if (!arg.empty() && arg[0] == ',') arg = trim(arg.substr(1));
            size_t ce = arg.find(',');
            std::string first = toLower(trim(ce == std::string::npos ?
                                             arg : arg.substr(0, ce)));
            if (first.size() > 4 &&
                (first.compare(first.size() - 4, 4, ".txt") == 0 ||
                 first.compare(first.size() - 3, 3, ".md") == 0)) {
              docTag = true;
            }
            break;
          }
        }
        if (docTag) { emit(ol[j]); continue; }
        rest2 += ol[j];
        rest2 += '\n';
      }
      std::string vname = "__embed" + std::to_string(++ctx.embedSeq) + "_" +
                          src;
      for (auto& c : vname) if (c == '/' || c == '\\') c = '_';
      markup::setVirtualFile(vname, rest2);
      std::string eline = "\\e " + vname;
      for (const auto& o : keepOpts) eline += ", " + o;
      emit(eline);
      continue;
    }

    // ---- \render name.txt ... \\render: DYNAMISCHE virtuelle Datei ----
    // (Aliasse: \dynamic, \dynamicfile, \d) Der Inhalt laeuft erst durch
    // die komplette Text-Pipeline (Variablen, \jloop/\jread, \loop/\file,
    // Segmente); das RESULTAT wird als virtuelle Datei registriert und
    // kann als Daten ODER als Source (\e) weitergereicht werden.
    std::string dword;
    for (const char* w : { "render", "dynamicfile", "dynamic", "d" }) {
      size_t wl = std::strlen(w);
      if (low.size() > wl && low[0] == '\\' && low.compare(1, wl, w) == 0 &&
          (low.size() == wl + 1 || low[wl + 1] == ' ' || low[wl + 1] == '\t')) {
        dword = w;
        break;
      }
    }
    if (!dword.empty()) {
      std::string name = trim(mergeSubst(trim(t.substr(dword.size() + 1)), ctx));
      const std::string openT = "\\" + dword, closeT = "\\\\" + dword;
      size_t end = li + 1;
      bool closedD = false;
      int dd = 1;
      for (; end < to; ++end) {
        std::string et = toLower(trim(lines[end]));
        if (et.rfind(openT, 0) == 0 && et.rfind(closeT, 0) != 0) ++dd;
        else if ((et == closeT || et == "\\\\render" || et == "\\\\dynamic") &&
                 --dd == 0) { closedD = true; break; }
      }
      if (!closedD) {
        warnings.push_back("\\" + dword + " " + name + ": Schliesser fehlt");
        break;
      }
      if (name.empty()) {
        warnings.push_back("\\" + dword + " braucht einen Dateinamen");
      } else {
        std::vector<std::string> body(lines.begin() + (long)li + 1,
                                      lines.begin() + (long)end);
        std::string rendered =
            expandMergeJson(body, 0, body.size(), ctx, baseDir, warnings);
        rendered = markup::expandLoops(rendered, baseDir, warnings);
        while (!rendered.empty() && rendered.back() == '\n')
          rendered.pop_back();
        markup::setVirtualFile(name, rendered);
      }
      li = end;
      continue;
    }

    // ---- \jread Name quelle.json: JSON laden und als $Name binden ----
    if (low.rfind("\\jread", 0) == 0 &&
        (t.size() == 6 || t[6] == ' ' || t[6] == '\t' || t[6] == ',')) {
      std::string rest = trim(t.substr(6));
      std::string asName, src;
      {
        size_t sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) { src = rest; asName = "j"; }
        else { asName = trim(rest.substr(0, sp)); src = trim(rest.substr(sp + 1)); }
      }
      src = trim(mergeSubst(src, ctx));
      static const Value kNullJ;
      const Value* root = nullptr;
      if (!src.empty() && src[0] == '$') {
        root = ctx.lookup(src.substr(1));
        if (!root) {
          auto vi = ctx.vars.find(src.substr(1));   // Variableninhalt = JSON
          if (vi != ctx.vars.end()) {
            std::string err;
            Value v = Value::parse(vi->second, err);
            if (err.empty()) {
              ctx.files.emplace("$var:" + src, std::move(v));
              root = &ctx.files["$var:" + src];
            } else warnings.push_back("\\jread " + src + ": " + err);
          }
        }
      } else {
        root = mergeLoadJson(ctx, baseDir, src, warnings);
      }
      // Array-Wurzel: den ERSTEN Datensatz binden ($j.vorname); fuer alle
      // Datensaetze gibt es \jloop. Objekt-Wurzel: direkt binden.
      if (root && root->kind() == Value::Kind::Array && root->size() > 0)
        root = &root->index(0);
      ctx.frames.push_back({asName, root ? root : &kNullJ, 0});
      continue;
    }

    // ---- \segment Name: Textbaustein definieren ODER einsetzen ----
    if (low.rfind("\\segment", 0) == 0 &&
        (t.size() == 8 || t[8] == ' ' || t[8] == '\t')) {
      std::string name = trim(mergeSubst(trim(t.substr(8)), ctx));
      // Definition oder Verwendung? Kommt als NAECHSTES segment-Tag ein
      // \\segment (Schliesser), ist dies eine DEFINITION mit Inhalt.
      size_t close = 0;
      bool isDef = false;
      for (size_t j = li + 1; j < to; ++j) {
        std::string et = toLower(trim(lines[j]));
        if (et == "\\\\segment") { close = j; isDef = true; break; }
        if (et.rfind("\\segment", 0) == 0) break;         // naechste Nutzung
      }
      if (name.empty()) {
        warnings.push_back("\\segment braucht einen Namen");
        if (isDef) li = close;
        continue;
      }
      if (isDef) {                                 // speichern (roh, Subst
        std::vector<std::string> body;             //  erst beim Einsetzen)
        for (size_t j = li + 1; j < close; ++j) body.push_back(lines[j]);
        ctx.segments[name] = body;                 // Ueberschreiben erlaubt
        li = close;
        continue;
      }
      auto it = ctx.segments.find(name);           // Verwendung
      if (it == ctx.segments.end()) {
        warnings.push_back("\\segment " + name + ": nicht definiert");
        emit(lines[li]);
        continue;
      }
      if (ctx.segExpanding.count(name)) {
        warnings.push_back("\\segment " + name + ": zyklische Verwendung");
        continue;
      }
      ctx.segExpanding.insert(name);
      // durch den Walker schicken: $vars/Bedingungen/innere Segmente wirken
      out += expandMergeJson(it->second, 0, it->second.size(), ctx, baseDir,
                             warnings);
      ctx.segExpanding.erase(name);
      continue;
    }

    // ---- $name=wert: Variable setzen (Zeile verschwindet) ----
    if (!t.empty() && t[0] == '$' && t.size() > 1 &&
        (std::isalpha((unsigned char)t[1]) || t[1] == '_')) {
      size_t e = 1;
      while (e < t.size() &&
             (std::isalnum((unsigned char)t[e]) || t[e] == '_')) ++e;
      if (e < t.size() && t[e] == '=' &&
          // NICHT die \loop-Mementos $a='< / $a='> anfassen
          !(e + 2 <= t.size() - 1 && t[e + 1] == '\'' &&
            (t[e + 2] == '<' || t[e + 2] == '>'))) {
        std::string name = t.substr(1, e - 1);
        ctx.vars[name] = mergeSubst(trim(t.substr(e + 1)), ctx);
        continue;
      }
    }

    if (low.rfind("\\c", 0) == 0 &&
        (t.size() == 2 || t[2] == ' ' || t[2] == '\t')) {
      std::string expr = trim(t.substr(2));
      bool applies = false;
      bool hold = mergeCond(expr, ctx, applies);
      if (applies) {
        size_t end = mergeFindClose(lines, li, "\\c", "\\\\c");
        if (end == lines.size()) {
          warnings.push_back("\\c " + expr + ": \\\\c fehlt");
          continue;
        }
        if (hold)
          out += expandMergeJson(lines, li + 1, end, ctx, baseDir, warnings);
        li = end;
        continue;
      }
      // normale (nicht-$) Bedingung: Zeile durchreichen, aber Variablen,
      // die IM Block gesetzt werden, am \\c wieder zuruecknehmen
      scopeStack.push_back(ctx.vars);
      emit(mergeSubst(lines[li], ctx));
      continue;
    }
    if (low == "\\\\c" || low == "[/c]") {
      if (!scopeStack.empty()) { ctx.vars = scopeStack.back(); scopeStack.pop_back(); }
      emit(lines[li]);
      continue;
    }
    if ((low.rfind("\\mail", 0) == 0 &&
         (t.size() == 5 || t[5] == ' ' || t[5] == ',')) ||
        low.rfind("[mail", 0) == 0) {
      scopeStack.push_back(ctx.vars);               // Mail = eigener Scope
      emit(mergeSubst(lines[li], ctx));
      continue;
    }
    if (low == "\\\\mail" || low == "[/mail]") {
      if (!scopeStack.empty()) { ctx.vars = scopeStack.back(); scopeStack.pop_back(); }
      emit(lines[li]);
      continue;
    }

    if (low.rfind("\\jload", 0) == 0 &&
        (t.size() == 6 || t[6] == ' ' || t[6] == '\t' || t[6] == ',')) {
      std::string rest = trim(t.substr(6));
      if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));
      std::string file, matchKey, matchVal, asName = "k";
      std::stringstream ps(rest);
      std::string part;
      bool first = true;
      while (std::getline(ps, part, ',')) {
        part = trim(part);
        size_t eq = part.find('=');
        std::string key = eq == std::string::npos ? "" :
            toLower(trim(part.substr(0, eq)));
        if (first && key != "match" && key != "treffer" &&
            key != "as" && key != "als") {
          file = key == "file" || key == "datei" ? trim(part.substr(eq + 1))
                                                 : part;
        } else if (key == "match" || key == "treffer") {
          std::string m = trim(part.substr(eq + 1));
          size_t meq = m.find('=');
          matchKey = meq == std::string::npos ? m : trim(m.substr(0, meq));
          matchVal = meq == std::string::npos ? "" : trim(m.substr(meq + 1));
        } else if (key == "as" || key == "als") {
          asName = trim(part.substr(eq + 1));
        } else if (!part.empty()) {
          warnings.push_back("\\jload: unbekannte Option: " + part);
        }
        first = false;
      }
      static const Value kNull;
      const Value* hit = &kNull;
      if (const Value* root = mergeLoadJson(ctx, baseDir, file, warnings)) {
        std::string want = mergeSubst(matchVal, ctx);
        for (size_t i = 0; i < root->size(); ++i) {
          const Value& rec = root->index(i);
          if (matchKey.empty() || rec.at(matchKey).toText() == want) {
            hit = &rec;
            break;
          }
        }
      }
      ctx.frames.push_back({asName, hit, 0});
      continue;
    }

    if (low == "\\\\jload") {
      if (ctx.frames.size() > frameFloor && !ctx.frames.back().name.empty())
        ctx.frames.pop_back();
      continue;
    }

    if (low.rfind("\\jloop", 0) == 0 &&
        (t.size() == 6 || t[6] == ' ' || t[6] == '\t' || t[6] == ',')) {
      std::string rest = trim(t.substr(6));
      if (!rest.empty() && rest[0] == ',') rest = trim(rest.substr(1));
      std::string src, filter;
      {
        std::stringstream ps(rest);
        std::string part;
        bool first = true;
        while (std::getline(ps, part, ',')) {
          part = trim(part);
          size_t eq = part.find('=');
          std::string key = eq == std::string::npos ? "" :
              toLower(trim(part.substr(0, eq)));
          if (first && key != "filter") src = part;
          else if (key == "filter") filter = trim(part.substr(eq + 1));
          else if (!part.empty())
            warnings.push_back("\\jloop: unbekannte Option: " + part);
          first = false;
        }
      }
      size_t end = mergeFindClose(lines, li, "\\jloop", "\\\\jloop");
      if (end == lines.size()) {
        warnings.push_back("\\jloop " + src + ": \\\\jloop fehlt");
        break;
      }
      const Value* arr = nullptr;
      if (!src.empty() && src[0] == '$') {
        arr = ctx.lookup(src.substr(1));
        if (!arr) {                                // Variableninhalt = JSON
          auto vi = ctx.vars.find(src.substr(1));
          if (vi != ctx.vars.end()) {
            auto fi = ctx.files.find("$var:" + src);
            if (fi == ctx.files.end()) {
              std::string err;
              Value v = Value::parse(vi->second, err);
              if (!err.empty())
                warnings.push_back("\\jloop " + src + ": " + err);
              else
                fi = ctx.files.emplace("$var:" + src, std::move(v)).first;
            }
            if (fi != ctx.files.end()) arr = &fi->second;
          }
        }
        if (!arr)
          warnings.push_back("\\jloop " + src + ": unbekannter Pfad");
      } else {
        arr = mergeLoadJson(ctx, baseDir, src, warnings);
      }
      if (arr) {
        size_t base = ctx.frames.size();
        long pos = 0;
        for (size_t i = 0; i < arr->size(); ++i) {
          const Value& rec = arr->index(i);
          if (!filter.empty()) {
            MergeCtx probe;
            probe.frames = ctx.frames;
            probe.frames.push_back({"", &rec, pos + 1});
            bool applies = false;
            if (!mergeCond(filter, probe, applies)) continue;
          }
          ++pos;
          ctx.frames.push_back({"", &rec, pos});
          out += expandMergeJson(lines, li + 1, end, ctx, baseDir, warnings);
          ctx.frames.resize(base);
        }
      }
      li = end;
      continue;
    }

    emit(mergeSubst(lines[li], ctx));
  }
  ctx.frames.resize(frameFloor);                    // \jload endet am Body
  ctx.vars = savedVars;                             // Variablen-Scope zu
  return out;
}

}  // namespace

std::string expandMailMerge(const std::string& text, const std::string& baseDir,
                            std::vector<std::string>& warnings) {
  std::string t = markup::translateBracketTags(text);
  if (t.find("\\mloop") != std::string::npos)
    t = markup::expandMergeTextLoops(t, baseDir, warnings);
  if (t.find("\\jloop") == std::string::npos &&
      t.find("\\jload") == std::string::npos &&
      t.find("\\virtual") == std::string::npos &&
      t.find('$') == std::string::npos)
    return t;
  std::vector<std::string> lines;
  {
    std::stringstream ss(t);
    std::string l;
    while (std::getline(ss, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  MergeCtx ctx;
  return expandMergeJson(lines, 0, lines.size(), ctx, baseDir, warnings);
}

Project parseProject(const std::string& text, const std::string& baseDir,
                     const std::string& defaultOutFile,
                     std::vector<std::string>& warnings) {
  Project prj;
  DocSpec* curDoc = nullptr;
  MailSpec* curMail = nullptr;      // collecting subject/body when non-null
  bool mailHasSubject = false;
  bool skipSection = false;         // a control tag whose condition failed

  // leading blank lines before the first content carry meaning (each one
  // adds a gap unit above the first flowable) and must reach the document
  std::string pendingLead;
  auto startImplicitDoc = [&]() {
    prj.docs.push_back({defaultOutFile, "", pendingLead});
    pendingLead.clear();
    curDoc = &prj.docs.back();
  };

  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // multi-line control tag: starts with '[' + pdfgen/mail/attach, may
    // continue over following lines until the closing bracket
    std::string t = trim(line);
    std::string low = toLower(t);
    bool tagStart = !t.empty() && t[0] == '[' &&
        (low.rfind("[pdfgen", 0) == 0 || low.rfind("[mail", 0) == 0 ||
         low.rfind("[attach", 0) == 0 || low.rfind("[doc", 0) == 0 ||
         low.rfind("[html", 0) == 0);
    if (tagStart) {
      std::string tagText = t;
      while (tagText.find(']') == std::string::npos && std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tagText += " " + trim(line);
      }
      Tag tag;
      if (parseTag(tagText, tag)) {
        pendingLead.clear();                     // blanks before a tag: void
        skipSection = false;
        if (!controlConditionPasses(tag, warnings)) {
          if (tag.kind == "pdfgen" || tag.kind == "mail" || tag.kind == "doc" || tag.kind == "html") {
            skipSection = true;               // drop the whole section
            curDoc = nullptr;
            curMail = nullptr;
          }
          continue;                            // failing [attach]: ignored
        }
        // [pdfgen..., include] belongs to the document content
        if (tag.kind == "pdfgen" && hasFlag(tag, "include")) {
          if (!curDoc) startImplicitDoc();
          curMail = nullptr;
          curDoc->inlineText += tagText + "\n";
          continue;
        }
        if (tag.kind == "pdfgen" || tag.kind == "doc" || tag.kind == "html") {
          // [pdfgen=Quelle.txt, file=X.pdf]   wie bisher (format=pdf)
          // [doc=Name, format=pdf,html]       Name ohne Endung = Ausgabename,
          //                                   mit .txt/.md = Quelldatei
          // [html=Name]                       = [doc=Name, format=html]
          curMail = nullptr;
          DocSpec spec;
          std::string head = tag.headValue, lowHead = toLower(head);
          const bool headIsSource = lowHead.size() > 3 && (lowHead.compare(lowHead.size() - 4, 4, ".txt") == 0 || lowHead.compare(lowHead.size() - 3, 3, ".md") == 0);
          if (tag.kind == "pdfgen" || headIsSource) spec.sourceFile = head;
          else spec.name = head;
          std::string file = opt(tag, "file") ? *opt(tag, "file") : "";
          if (file.empty()) {
            if (!head.empty()) { file = head; size_t dot = file.rfind('.'); if (dot != std::string::npos && headIsSource) file.erase(dot); }
            else { warnings.push_back("[" + tag.kind + "] without file= — using " + defaultOutFile); file = defaultOutFile; }
          }
          // Basisname ohne .pdf/.html -- die Endung kommt je Format
          { std::string lf = toLower(file); for (const char* e : { ".pdf", ".html", ".htm" }) { size_t n = std::strlen(e); if (lf.size() > n && lf.compare(lf.size() - n, n, e) == 0) { file.erase(file.size() - n); break; } } }
          spec.outFile = file;
          if (spec.name.empty()) spec.name = file;
          spec.formats.clear();
          std::string fmt = tag.kind == "html" ? "html" : "pdf";
          spec.unique = hasFlag(tag, "unique") || hasFlag(tag, "eindeutig");
          if (auto* v = opt(tag, "format")) fmt = toLower(*v);
          // "format=pdf,html": der Tag-Parser trennt an Kommas, das "html"
          // kommt als nackte Option an -- hier wieder einsammeln.
          // (format=pdf+html und format=pdf html gehen ebenfalls.)
          for (auto& kv : tag.opts) if (kv.second.empty() && (kv.first == "pdf" || kv.first == "html")) fmt += "," + kv.first;
          { std::string cur; for (char c : fmt + ",") { if (c == ',' || c == ' ' || c == '+' || c == '/') { cur = trim(cur); if (cur == "pdf" || cur == "html") spec.formats.push_back(cur); else if (!cur.empty()) warnings.push_back("[" + tag.kind + "]: unknown format '" + cur + "' (pdf, html)"); cur.clear(); } else cur += c; } }
          if (spec.formats.empty()) spec.formats.push_back("pdf");
          applyMarginOpts(tag.opts, "", spec.layout, warnings);
          prj.docs.push_back(std::move(spec));
          curDoc = &prj.docs.back();
          continue;
        }
        if (tag.kind == "mail") {
          // curDoc bleibt stehen: der Mail-Koerper laeuft ueber curMail,
          // nach [/mail] gehoert der Inhalt wieder zum selben Dokument
          // (sonst entstuenden zwei implizite Dokumente gleichen Namens).
          MailSpec m;
          m.name = tag.headValue;
          m.baseDir = baseDir;
          if (auto* v = opt(tag, "to"))         m.to = splitWs(*v);
          if (auto* v = opt(tag, "cc"))         m.cc = splitWs(*v);
          if (auto* v = opt(tag, "attachment")) m.attachments = splitWs(*v);
          if (auto* v = opt(tag, "test"))       m.testAddr = *v;
          if (auto* v = opt(tag, "from"))       m.from = *v;   // Absenderkonto
          if (auto* v = opt(tag, "format"))     { const std::string f = toLower(*v); m.html = f == "html"; if (f != "html" && f != "text" && !f.empty()) warnings.push_back("[mail] format=" + *v + ": use text or html"); }
          if (auto* v = opt(tag, "body"))       m.bodyDoc = *v;  // Koerper = Dokument Name
          if (m.name.empty())
            warnings.push_back("[mail] without a name — use [mail=Name, ...]");
          prj.mails.push_back(std::move(m));
          curMail = &prj.mails.back();
          mailHasSubject = false;
          continue;
        }
        if (tag.kind == "attach") {
          const std::string* mn = opt(tag, "mail");
          const std::string* fp = opt(tag, "file");
          if (!mn || !fp) {
            warnings.push_back("[attach] needs mail= and file=");
          } else {
            bool found = false;
            for (auto& m : prj.mails)
              if (m.name == *mn) { m.attachments.push_back(*fp); found = true; }
            if (!found)
              warnings.push_back("[attach]: no mail named '" + *mn + "' defined (yet)");
          }
          continue;                              // control line, not content
        }
      }
    }

    // ordinary line: mail subject/body or document content
    if (skipSection) continue;
    {
      std::string tl = toLower(trim(line));
      if (tl == "\\\\mail" || tl == "[/mail]") {   // explizites Mail-Ende
        curMail = nullptr;
        continue;
      }
    }
    if (curMail) {
      if (!mailHasSubject) {
        if (trim(line).empty()) continue;        // skip blanks before subject
        curMail->subject = trim(line);
        mailHasSubject = true;
      } else {
        curMail->body += line + "\n";
      }
      continue;
    }
    if (!curDoc) {
      if (trim(line).empty()) { pendingLead += "\n"; continue; }
      startImplicitDoc();
    }
    curDoc->inlineText += line + "\n";
  }

  for (auto& m : prj.mails) m.body = trim(m.body);
  // drop an implicit doc that stayed empty
  if (!prj.docs.empty() && prj.docs[0].sourceFile.empty() &&
      trim(prj.docs[0].inlineText).empty() && prj.docs[0].outFile == defaultOutFile &&
      prj.docs.size() > 1)
    prj.docs.erase(prj.docs.begin());
  return prj;
}

// ---- rendering -------------------------------------------------------------
bool renderDocument(const std::string& baseDir, const std::string& sourceText,
                    const std::string& outPdfPath, std::string* outBytes,
                    std::vector<std::string>& warnings, int depth,
                    PageLayout layout, bool embedded) {
  using namespace pdf;
  if (depth > 6) {
    warnings.push_back("include depth limit reached (circular [pdfgen, include]?)");
    return false;
  }

  std::string text = sourceText;

  // per-page header/footer blocks
  std::string headerTxt = extractBlock(text, "[header]", "[/header]");
  std::string footerTxt = extractBlock(text, "[bottom]", "[/bottom]");

  // a [margins...] line inside the source overrides the tag options
  extractMarginsTag(text, layout, warnings);

  // [embed=...] blocks (their conditions run with embedded=true)
  text = expandEmbeds(text, baseDir, layout, warnings, depth);
  // \loop / \file blocks over data files
  text = markup::expandLoops(text, baseDir, warnings);

  markup::Options opt;
  opt.pdfPath = outPdfPath;                       // for [date, modified=1]

  markup::BuildContext ctx;
  ctx.renderInclude = [&](const std::string& src, std::string& bytes,
                          std::string& err) {
    std::string path = baseDir.empty() ? src : baseDir + "/" + src;
    std::string childText = readFile(path);
    if (childText.empty()) { err = "cannot read " + path; return false; }
    bool incMd = src.size() > 3 &&
                 toLower(src).compare(src.size() - 3, 3, ".md") == 0;
    childText = markup::preprocessSource(childText, /*embedded=*/true, warnings,
                                         incMd);
    std::vector<std::string> subWarn;
    bool ok = renderDocument(baseDir, childText, "", &bytes, subWarn,
                             depth + 1, PageLayout{}, /*embedded=*/true);
    for (auto& w : subWarn) warnings.push_back(src + ": " + w);
    if (!ok) err = "rendering failed";
    return ok;
  };

  std::string raw = markup::expandIncludes(text, baseDir, warnings, embedded);
  raw = markup::expandDates(raw, opt, warnings);
  headerTxt = markup::expandDates(headerTxt, opt, warnings);
  footerTxt = markup::expandDates(footerTxt, opt, warnings);
  auto tokens = markup::tokenize(raw, opt, warnings);

  std::string docName = outPdfPath;
  {
    size_t slash = docName.find_last_of("/\\");
    if (slash != std::string::npos) docName = docName.substr(slash + 1);
  }

  auto runPass = [&](int totalPages, Writer& writer) {
    Document doc(writer, layout.left, layout.right, layout.top, layout.bottom);
    PageDecor decor;
    decor.headerMarkup = headerTxt;
    decor.footerMarkup = footerTxt;
    decor.docName = docName;
    decor.totalPages = totalPages;
    decor.build = [&](const std::string& mtext) {
      std::vector<std::string> dw;                 // decor warnings: dropped
      std::string dtext = markup::expandIncludes(mtext, baseDir, dw, embedded);
      auto dtokens = markup::tokenize(dtext, opt, dw);
      return markup::buildFlowables(dtokens, baseDir, dw, nullptr);
    };
    doc.setDecor(std::move(decor));
    for (auto& f : markup::buildFlowables(tokens, baseDir, warnings, &ctx))
      doc.add(std::move(f));
    doc.build();
    return doc.totalPagesRendered();
  };

  // [pages] needs the final page count -> counting pass first
  auto lowAll = toLower(headerTxt + footerTxt);
  bool needTwoPass = lowAll.find("[pages]") != std::string::npos;
  int total = 0;
  if (needTwoPass) {
    Writer counting;
    total = runPass(0, counting);
  }
  Writer writer;
  runPass(total, writer);

  if (outBytes) return writer.saveToString(*outBytes);
  if (!writer.save(outPdfPath)) {
    warnings.push_back("cannot write " + outPdfPath);
    return false;
  }
  PDFGEN_LOGI("wrote %s", outPdfPath.c_str());
  return true;
}

// ---- CLI source processing --------------------------------------------------
bool renderHtmlDocument(const std::string& baseDir, const std::string& sourceText,
                        const std::string& outHtmlPath, std::vector<std::string>& warnings) {
  markup::Options opt; opt.pdfPath = outHtmlPath;
  std::vector<markup::Token> toks = markup::tokenize(sourceText, opt, warnings);
  std::string title = outHtmlPath;
  { size_t sl = title.find_last_of("/\\"); if (sl != std::string::npos) title = title.substr(sl + 1); size_t dot = title.rfind('.'); if (dot != std::string::npos) title.erase(dot); }
  htmlout::Result r = htmlout::render(toks, baseDir, title, /*forMail=*/false, warnings);
  std::ofstream f(outHtmlPath, std::ios::binary);
  if (!f) { warnings.push_back("cannot write " + outHtmlPath); return false; }
  f << r.html;
  PDFGEN_LOGI("html: %s", outHtmlPath.c_str());
  return true;
}
// MAIL-SCHABLONEN: \loop-Bloecke, die Projekt-Tags enthalten ([mail=, [doc=,
// [html=, [pdfgen=, [attach), werden VOR der Projektaufteilung je Datensatz
// ausgerollt -- so entsteht aus einer Vorlage je Kunde eine eigene Mail
// (to=:1 nimmt die Adresse aus dem Datensatz). Loops ohne Projekt-Tags
// bleiben, wie sie sind; die rollt der Renderer je Dokument aus.
static std::string expandProjectLoops(const std::string& text, const std::string& baseDir,
                                      std::vector<std::string>& warnings) {
  std::vector<std::string> lines; { std::stringstream ss(text); std::string l; while (std::getline(ss, l)) lines.push_back(l); }
  auto istProjektTag = [](const std::string& l) {
    std::string t = toLower(trim(l));
    return t.rfind("[mail", 0) == 0 || t.rfind("[doc", 0) == 0 || t.rfind("[html", 0) == 0 || t.rfind("[pdfgen", 0) == 0 || t.rfind("[attach", 0) == 0;
  };
  std::string out; bool geaendert = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string t = toLower(trim(lines[i]));
    if (t.rfind("\\loop", 0) == 0 && t.rfind("\\\\loop", 0) != 0) {
      // Blockende suchen (verschachtelt)
      int tiefe = 0; size_t ende = i; bool hatTag = false;
      for (size_t j = i; j < lines.size(); ++j) {
        const std::string u = toLower(trim(lines[j]));
        if (u.rfind("\\\\loop", 0) == 0) { if (--tiefe == 0) { ende = j; break; } }
        else if (u.rfind("\\loop", 0) == 0) ++tiefe;
        else if (istProjektTag(lines[j])) hatTag = true;
      }
      if (hatTag && ende > i) {
        std::string block; for (size_t j = i; j <= ende; ++j) block += lines[j] + "\n";
        out += markup::expandLoops(block, baseDir, warnings); geaendert = true;
        i = ende; continue;
      }
    }
    out += lines[i] + "\n";
  }
  return geaendert ? out : text;
}

// EMPFAENGER-POOLS: "@name" in to=/cc= steht fuer die Adressen aus
// <baseDir>/name.txt (jede Zeile mit einem @ ist eine Adresse; Zeilen ohne @
// sind Namen/Notizen und werden ueberlesen). So lassen sich Kundenpools in
// einer Datei pflegen und per @stammkunden ansprechen.
static void poolsAufloesen(std::vector<std::string>& adressen, const std::string& baseDir,
                           const std::string& mailName, std::vector<std::string>& warnings) {
  std::vector<std::string> aus;
  for (const std::string& a : adressen) {
    if (a.size() < 2 || a[0] != '@' || a.find('@', 1) != std::string::npos) { aus.push_back(a); continue; }
    const std::string datei = baseDir + "/" + a.substr(1) + ".txt";
    std::string inhalt = markup::readDataFile(baseDir, a.substr(1) + ".txt");
    if (inhalt.empty()) { warnings.push_back("[mail=" + mailName + "] pool " + a + ": " + datei + " not found or empty"); continue; }
    std::stringstream ss(inhalt); std::string l; size_t n = 0;
    while (std::getline(ss, l)) { l = trim(l); if (l.find('@') != std::string::npos) { aus.push_back(l); ++n; } }
    if (!n) warnings.push_back("[mail=" + mailName + "] pool " + a + ": no addresses in " + datei);
  }
  adressen.swap(aus);
}

bool processSource(const std::string& cliArg, std::vector<MailSpec>& mails,
                   std::vector<std::string>& warnings,
                   const RunOptions* opts) {
  std::string baseDir, mainFile, defaultOut;
  std::string lowArg = toLower(cliArg);
  bool isTxt = lowArg.size() > 4 &&
               lowArg.compare(lowArg.size() - 4, 4, ".txt") == 0;
  bool isMd  = lowArg.size() > 3 &&
               lowArg.compare(lowArg.size() - 3, 3, ".md") == 0;
  if (isTxt || isMd) {
    size_t slash = cliArg.find_last_of("/\\");
    baseDir = slash == std::string::npos ? "." : cliArg.substr(0, slash);
    mainFile = cliArg;
    std::string base = slash == std::string::npos ? cliArg : cliArg.substr(slash + 1);
    base.erase(base.size() - (isTxt ? 4 : 3));
    defaultOut = base + ".pdf";                  // Example.md -> Example.pdf
  } else {
    baseDir = cliArg;
    mainFile = cliArg + "/text.txt";
    {   // a directory may carry text.md instead
      std::ifstream probe(mainFile);
      if (!probe) {
        std::ifstream md(cliArg + "/text.md");
        if (md) mainFile = cliArg + "/text.md";
      }
    }
    defaultOut = "Bewerbung.pdf";                // old directory behavior
  }

  PDFGEN_LOGD("processSource: %s", mainFile.c_str());
  std::string text = readFile(mainFile);
  text = expandMailMerge(text, baseDir, warnings);   // \mloop/\jloop/\jload
  if (text.empty()) {
    warnings.push_back("cannot read " + mainFile);
    return false;
  }
  bool mdMain = mainFile.size() > 3 &&
                toLower(mainFile).compare(mainFile.size() - 3, 3, ".md") == 0;
  text = markup::preprocessSource(text, /*embedded=*/false, warnings, mdMain);

  text = expandProjectLoops(text, baseDir, warnings);      // Mail-Schablonen je Datensatz
  Project prj = parseProject(text, baseDir, defaultOut, warnings);
  for (auto& m : prj.mails) { poolsAufloesen(m.to, baseDir, m.name, warnings); poolsAufloesen(m.cc, baseDir, m.name, warnings); }
  bool ok = true;

  // ---- Auswahl (--gen) + Dry-Run: erst planen, dann rendern --------------
  const bool dryRun = opts && opts->dryRun;
  const bool haveSel = opts && !opts->gen.empty();
  auto basisOf = [&](const DocSpec& d) {
    std::string basis = d.outFile;
    std::string lf = toLower(basis);
    for (const char* e : { ".pdf", ".html", ".htm" }) {
      size_t n = std::strlen(e);
      if (lf.size() > n && lf.compare(lf.size() - n, n, e) == 0) {
        basis.erase(basis.size() - n);
        break;
      }
    }
    return basis;
  };
  auto wanted = [&](const std::string& id) {
    if (!haveSel) return true;
    for (const auto& g : opts->gen) if (g == id) return true;
    return false;
  };
  // Mail-Auswahl zuerst: eine gewaehlte Mail zieht ihre Dokumente nach
  std::vector<std::string> pulled;                  // Dateinamen (Basename)
  for (auto& m : prj.mails) {
    m.selected = wanted("mail." + m.name);
    if (m.selected && haveSel) {
      for (const auto& a : m.attachments) {
        size_t sl = a.find_last_of("/\\");
        pulled.push_back(sl == std::string::npos ? a : a.substr(sl + 1));
      }
      if (!m.bodyDoc.empty()) pulled.push_back(m.bodyDoc);
    }
  }
  auto pulledBy = [&](const DocSpec& d, const std::string& fname) {
    for (const auto& pn : pulled)
      if (pn == fname || pn == d.name || pn == basisOf(d)) return true;
    return false;
  };

  for (auto& d : prj.docs) {
    std::string basis = basisOf(d);
    std::string src;
    bool srcLoaded = false;
    for (const std::string& fmt : d.formats) {
      std::string fname = basis + "." + fmt;
      std::string outPath = baseDir + "/" + fname;
      if (d.unique) {                               // nie ueberschreiben
        int k = 2;
        while (std::ifstream(outPath).good()) {
          fname = basis + "-" + std::to_string(k++) + "." + fmt;
          outPath = baseDir + "/" + fname;
        }
      }
      std::string id = fmt + "." + basis;
      bool sel = wanted(id) || pulledBy(d, fname);
      GenItem gi;
      gi.id = id;
      gi.kind = fmt;
      gi.file = fname;
      gi.selected = sel;
      if (sel && !dryRun) {
        if (!srcLoaded) {
          src = d.inlineText;
          if (!d.sourceFile.empty()) {
            src = markup::readDataFile(baseDir, d.sourceFile);
            if (src.empty()) {
              warnings.push_back("cannot read source " + d.sourceFile);
              ok = false;
            } else {
              src = markup::preprocessSource(src, /*embedded=*/false, warnings);
            }
          }
          srcLoaded = true;
        }
        bool one = true;
        if (!src.empty() || d.sourceFile.empty()) {
          if (fmt == "html")
            one = renderHtmlDocument(baseDir, src, outPath, warnings);
          else
            one = renderDocument(baseDir, src, outPath, nullptr, warnings, 0,
                                 d.layout);
        } else one = false;
        ok = one && ok;
        gi.generated = one;
      }
      if (opts && opts->listing) opts->listing->push_back(gi);
    }
  }
  if (opts && opts->listing)
    for (const auto& m : prj.mails) {
      GenItem gi;
      gi.id = "mail." + m.name;
      gi.kind = "mail";
      gi.mailName = m.name;
      gi.selected = m.selected;
      opts->listing->push_back(gi);
    }
  if (dryRun) {
    for (auto& m : prj.mails) { m.baseDir = baseDir; mails.push_back(m); }
    return ok;
  }
  // MAILS: body=Name nimmt den Inhalt des Dokuments; format=html rendert den
  // Koerper durch dieselbe Pipeline (HTML + Textalternative + cid-Bilder).
  for (auto& m : prj.mails) {
    if (!m.bodyDoc.empty()) {
      bool found = false;
      for (const auto& d : prj.docs) {
        if (d.name != m.bodyDoc && d.outFile != m.bodyDoc) continue;
        found = true;
        std::string src = d.inlineText;
        if (!d.sourceFile.empty()) { src = readFile(baseDir + "/" + d.sourceFile); src = markup::preprocessSource(src, false, warnings); }
        m.body = src;
        { std::string b2 = d.outFile; std::string lf = toLower(b2); for (const char* e : { ".pdf", ".html", ".htm" }) { size_t n = std::strlen(e); if (lf.size() > n && lf.compare(lf.size() - n, n, e) == 0) { b2.erase(b2.size() - n); break; } }
          for (const std::string& fmt : d.formats) if (fmt == "pdf") m.attachments.push_back(b2 + ".pdf"); }   // das PDF haengt mit
        break;
      }
      if (!found) warnings.push_back("[mail=" + m.name + "] body=" + m.bodyDoc + ": no such document");
    }
    if (m.html) {
      markup::Options mo;
      std::vector<markup::Token> toks = markup::tokenize(m.body, mo, warnings);
      htmlout::Result r = htmlout::render(toks, baseDir, m.subject, /*forMail=*/true, warnings);
      m.bodyHtml = r.html; m.body = r.text;
      for (auto& im : r.images) m.inlineImages.push_back({ im.cid, im.path });
    }
  }
  for (auto& m : prj.mails) mails.push_back(std::move(m));
  return ok;
}

} // namespace project
