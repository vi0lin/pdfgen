#include "project.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

#include "flowables.h"
#include "markup.h"
#include "pdfwriter.h"

// ---- release consistency check ----
#ifndef PDFGEN_MARKUP_API
#error "stale markup.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_MARKUP_API != 8
#error "version mismatch in markup.h: replace ALL pdfgen source files from the same release."
#endif
#ifndef PDFGEN_FLOWABLES_API
#error "stale flowables.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_FLOWABLES_API != 7
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
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
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
  if (out.kind != "pdfgen" && out.kind != "mail" && out.kind != "attach") return false;
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

// Finds a whole-line [margins...] tag in the source, applies it to `lay`,
// and removes the line. The first occurrence wins.
void extractMarginsTag(std::string& text, PageLayout& lay,
                       std::vector<std::string>& warnings) {
  std::stringstream ss(text);
  std::string line, out;
  bool done = false;
  while (std::getline(ss, line)) {
    std::string t = trim(line);
    std::string low = toLower(t);
    if (!done && low.rfind("[margins", 0) == 0 && !t.empty() && t.back() == ']') {
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
    bool newpage = false, keepmargins = false;
    for (size_t i = 1; i < parts.size(); ++i) {
      std::string k = toLower(parts[i]);
      if (k == "newpage" || k == "neueseite") newpage = true;
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
    child = markup::preprocessSource(child, /*embedded=*/true, warnings);
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

    if (newpage) out += "[newpage]\n";
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
Project parseProject(const std::string& text, const std::string& baseDir,
                     const std::string& defaultOutFile,
                     std::vector<std::string>& warnings) {
  Project prj;
  DocSpec* curDoc = nullptr;
  MailSpec* curMail = nullptr;      // collecting subject/body when non-null
  bool mailHasSubject = false;
  bool skipSection = false;         // a control tag whose condition failed

  auto startImplicitDoc = [&]() {
    prj.docs.push_back({defaultOutFile, "", ""});
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
         low.rfind("[attach", 0) == 0);
    if (tagStart) {
      std::string tagText = t;
      while (tagText.find(']') == std::string::npos && std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tagText += " " + trim(line);
      }
      Tag tag;
      if (parseTag(tagText, tag)) {
        skipSection = false;
        if (!controlConditionPasses(tag, warnings)) {
          if (tag.kind == "pdfgen" || tag.kind == "mail") {
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
        if (tag.kind == "pdfgen") {
          curMail = nullptr;
          std::string file = opt(tag, "file") ? *opt(tag, "file") : "";
          if (file.empty()) {
            if (!tag.headValue.empty()) {          // derive from source name
              file = tag.headValue;
              size_t dot = file.rfind('.');
              if (dot != std::string::npos) file.erase(dot);
              file += ".pdf";
            } else {
              warnings.push_back("[pdfgen] without file= — using " + defaultOutFile);
              file = defaultOutFile;
            }
          }
          DocSpec spec;
          spec.outFile = file;
          spec.sourceFile = tag.headValue;
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
      if (trim(line).empty()) continue;          // ignore leading blank lines
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

  markup::Options opt;
  opt.pdfPath = outPdfPath;                       // for [date, modified=1]

  markup::BuildContext ctx;
  ctx.renderInclude = [&](const std::string& src, std::string& bytes,
                          std::string& err) {
    std::string path = baseDir.empty() ? src : baseDir + "/" + src;
    std::string childText = readFile(path);
    if (childText.empty()) { err = "cannot read " + path; return false; }
    childText = markup::preprocessSource(childText, /*embedded=*/true, warnings);
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
  std::cout << "wrote " << outPdfPath << "\n";
  return true;
}

// ---- CLI source processing --------------------------------------------------
bool processSource(const std::string& cliArg, std::vector<MailSpec>& mails,
                   std::vector<std::string>& warnings) {
  std::string baseDir, mainFile, defaultOut;
  std::string lowArg = toLower(cliArg);
  bool isTxt = lowArg.size() > 4 &&
               lowArg.compare(lowArg.size() - 4, 4, ".txt") == 0;
  if (isTxt) {
    size_t slash = cliArg.find_last_of("/\\");
    baseDir = slash == std::string::npos ? "." : cliArg.substr(0, slash);
    mainFile = cliArg;
    std::string base = slash == std::string::npos ? cliArg : cliArg.substr(slash + 1);
    base.erase(base.size() - 4);
    defaultOut = base + ".pdf";                  // Example.txt -> Example.pdf
  } else {
    baseDir = cliArg;
    mainFile = cliArg + "/text.txt";
    defaultOut = "Bewerbung.pdf";                // old directory behavior
  }

  std::string text = readFile(mainFile);
  if (text.empty()) {
    warnings.push_back("cannot read " + mainFile);
    return false;
  }
  text = markup::preprocessSource(text, /*embedded=*/false, warnings);

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
    std::string outPath = baseDir + "/" + d.outFile;
    ok = renderDocument(baseDir, src, outPath, nullptr, warnings, 0, d.layout) && ok;
  }
  for (auto& m : prj.mails) mails.push_back(std::move(m));
  return ok;
}

} // namespace project
