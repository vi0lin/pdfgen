#include "project.h"
#include "htmlwriter.h"
#include <cstring>
#include "jsondata.h"

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
#elif PDFGEN_MARKUP_API != 18
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
std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string w;
  while (ss >> w) out.push_back(w);
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
    std::string child = readFile(baseDir + "/" + src);
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

// $pfad / ${pfad} / $# ersetzen; unbekannte Namen bleiben woertlich
// (so ueberleben \loop-Mementos wie $a und gewoehnliche $-Zeichen).
std::string mergeSubst(const std::string& line, const MergeCtx& ctx) {
  std::string out;
  size_t i = 0, n = line.size();
  while (i < n) {
    char c = line[i];
    if (c != '$') { out += c; ++i; continue; }
    if (i + 1 < n && line[i + 1] == '#') {
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
    if (!v) { out += c; ++i; continue; }
    out += v->toText();
    i = e + (braced ? 1 : 0);
  }
  return out;
}

// Bedingung mit $: "$pfad", "!$pfad", "$pfad=wert", "$pfad!=wert".
// applies sagt, ob der Ausdruck ueberhaupt eine Merge-Bedingung ist.
bool mergeCond(const std::string& exprIn, const MergeCtx& ctx, bool& applies) {
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
  bool r;
  if (hasCmp) {
    std::string val = v ? v->toText() : "";
    std::string want = mergeSubst(lit, ctx);        // $k.email=$_ erlaubt
    r = (val == want) == eq;
  } else {
    r = v && v->truthy();
  }
  return neg ? !r : r;
}

const Value* mergeLoadJson(MergeCtx& ctx, const std::string& baseDir,
                           const std::string& file,
                           std::vector<std::string>& warnings) {
  std::string path = baseDir.empty() ? file : baseDir + "/" + file;
  auto it = ctx.files.find(path);
  if (it == ctx.files.end()) {
    std::string txt = readFile(path);
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

  for (size_t li = from; li < to; ++li) {
    std::string t = trim(lines[li]);
    std::string low = toLower(t);

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
      emit(mergeSubst(lines[li], ctx));             // normale Bedingung
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
  return out;
}

}  // namespace

std::string expandMailMerge(const std::string& text, const std::string& baseDir,
                            std::vector<std::string>& warnings) {
  std::string t = text;
  if (t.find("\\mloop") != std::string::npos)
    t = markup::expandMergeTextLoops(t, baseDir, warnings);
  if (t.find("\\jloop") == std::string::npos &&
      t.find("\\jload") == std::string::npos)
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
          curDoc = nullptr;
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
bool processSource(const std::string& cliArg, std::vector<MailSpec>& mails,
                   std::vector<std::string>& warnings) {
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

  Project prj = parseProject(text, baseDir, defaultOut, warnings);
  bool ok = true;
  for (auto& d : prj.docs) {
    std::string src = d.inlineText;
    if (!d.sourceFile.empty()) {
      src = readFile(baseDir + "/" + d.sourceFile);
      if (src.empty()) {
        warnings.push_back("cannot read source " + d.sourceFile);
        ok = false;
        continue;
      }
      src = markup::preprocessSource(src, /*embedded=*/false, warnings);
    }
    std::string basis = d.outFile;   // implizites Dokument traegt noch ".pdf" (defaultOutFile)
    { std::string lf = toLower(basis); for (const char* e : { ".pdf", ".html", ".htm" }) { size_t n = std::strlen(e); if (lf.size() > n && lf.compare(lf.size() - n, n, e) == 0) { basis.erase(basis.size() - n); break; } } }
    for (const std::string& fmt : d.formats) {
      std::string outPath = baseDir + "/" + basis + "." + fmt;
      if (fmt == "html") ok = renderHtmlDocument(baseDir, src, outPath, warnings) && ok;
      else               ok = renderDocument(baseDir, src, outPath, nullptr, warnings, 0, d.layout) && ok;
    }
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
