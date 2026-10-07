#include "htmlwriter.h"
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace htmlout {
namespace {

std::string esc(const std::string& s) {
  std::string o; o.reserve(s.size() + 8);
  for (char c : s) switch (c) {
    case '&': o += "&amp;"; break;  case '<': o += "&lt;"; break;
    case '>': o += "&gt;"; break;   case '"': o += "&quot;"; break;
    default: o += c;
  }
  return o;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string pt(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2fpt", v); return b; }
std::string mime(const std::string& path) {
  std::string l = lower(path); auto ends = [&](const char* e) { size_t n = std::strlen(e); return l.size() >= n && l.compare(l.size() - n, n, e) == 0; };
  if (ends(".png")) return "image/png";
  if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
  if (ends(".gif")) return "image/gif";
  if (ends(".webp")) return "image/webp";
  if (ends(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}
std::string base64(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o; size_t i = 0; unsigned v;
  while (i + 2 < in.size()) { v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2]; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63]; i += 3; }
  if (i + 1 == in.size()) { v = (unsigned char)in[i] << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "=="; }
  else if (i + 2 == in.size()) { v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8); o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '='; }
  return o;
}
bool readAll(const std::string& p, std::string& out) { std::ifstream f(p, std::ios::binary); if (!f) return false; std::ostringstream ss; ss << f.rdbuf(); out = ss.str(); return true; }

// Inline-Markup der Tokens: <b> <i> <u> </b> </i> </u> <br/> -- der Rest ist Text.
template <class F> void inlineWalk(const std::string& m, F&& emit) {
  size_t i = 0;
  while (i < m.size()) {
    if (m[i] == '<') {
      size_t e = m.find('>', i);
      if (e != std::string::npos) {
        std::string tag = lower(m.substr(i + 1, e - i - 1));
        if (tag == "b" || tag == "i" || tag == "u" || tag == "/b" || tag == "/i" || tag == "/u" || tag == "br" || tag == "br/") { emit(tag, true); i = e + 1; continue; }
      }
    }
    size_t n = m.find('<', i); if (n == std::string::npos) n = m.size();
    emit(m.substr(i, n - i), false); i = n;
  }
}

const char* css =
  "body{font-family:Helvetica,Arial,sans-serif;font-size:11pt;line-height:1.45;color:#1a1a1a;margin:0;padding:0}"
  ".page{max-width:17cm;margin:1.5em auto;padding:0 1em}"
  "h1,h2,h3,h4,h5{font-weight:bold;margin:1.2em 0 .4em;line-height:1.25}"
  "h1{font-size:18pt}h2{font-size:14pt}h3{font-size:12pt}h4{font-size:11pt}h5{font-size:10pt}"
  "p{margin:0 0 .8em;text-align:justify}"
  "p.right{text-align:right}p.center{text-align:center}"
  "blockquote{margin:.5em 0 .8em 1.2em;padding-left:.6em;border-left:3px solid #c8c8c8;color:#444}"
  "pre{font-family:Courier,monospace;font-size:9.5pt;line-height:1.35;background:#f4f4f4;padding:.6em;overflow-x:auto}"
  "hr{border:0;border-top:1px solid #999;margin:1em 0}"
  "ul,ol{margin:0 0 .8em 1.4em;padding:0}li{margin:.15em 0}"
  "table{border-collapse:collapse;margin:.6em 0}"
  "td,th{vertical-align:top;padding:3pt 4pt}th{font-weight:bold;text-align:left}"
  "tr.rule td,tr.rule th{border-bottom:1px solid #4d4d4d}"
  "table.grid td,table.grid th{border:1px solid #4d4d4d}table.frame{border:1px solid #4d4d4d}"
  "img{max-width:100%;height:auto}img.left{float:left;margin:0 1em .5em 0}img.right{float:right;margin:0 0 .5em 1em}"
  ".center{text-align:center}.rightalign{text-align:right}.clear{clear:both}"
  ".pagebreak{page-break-before:always;break-before:page}"
  "@media print{.page{max-width:none;margin:0}}";

} // namespace

std::string inlineToHtml(const std::string& m) {
  std::string o; int ob = 0, oi = 0, ou = 0;
  inlineWalk(m, [&](const std::string& s, bool tag) {
    if (!tag) { o += esc(s); return; }
    if (s == "b") { o += "<b>"; ++ob; } else if (s == "/b") { if (ob) { o += "</b>"; --ob; } }
    else if (s == "i") { o += "<i>"; ++oi; } else if (s == "/i") { if (oi) { o += "</i>"; --oi; } }
    else if (s == "u") { o += "<u>"; ++ou; } else if (s == "/u") { if (ou) { o += "</u>"; --ou; } }
    else o += "<br>";
  });
  while (ob-- > 0) o += "</b>";
  while (oi-- > 0) o += "</i>";
  while (ou-- > 0) o += "</u>";
  return o;
}
std::string inlineToText(const std::string& m) {
  std::string o;
  inlineWalk(m, [&](const std::string& s, bool tag) { if (!tag) o += s; else if (s == "br" || s == "br/") o += "\n"; });
  return o;
}

Result render(const std::vector<markup::Token>& tokens, const std::string& baseDir,
              const std::string& title, bool forMail, std::vector<std::string>& warnings) {
  using markup::Token;
  Result r;
  std::string& h = r.html; std::string& t = r.text;
  h += "<!DOCTYPE html>\n<html lang=\"de\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  h += "<title>" + esc(title) + "</title>\n<style>" + css + "</style>\n</head>\n<body>\n<div class=\"page\">\n";
  int cidN = 0;
  int listLevel = 0;                                   // offene <ul>-Ebenen
  auto closeLists = [&](int to) { while (listLevel > to) { h += "</ul>\n"; --listLevel; } };
  bool floatOffen = false;

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token& k = tokens[i];
    if (k.kind != Token::Kind::ListItem) closeLists(0);
    switch (k.kind) {
      case Token::Kind::Heading: {
        const int lv = k.level < 1 ? 1 : (k.level > 5 ? 5 : k.level);
        h += "<h" + std::to_string(lv) + ">" + inlineToHtml(k.text) + "</h" + std::to_string(lv) + ">\n";
        t += "\n" + inlineToText(k.text) + "\n" + std::string(inlineToText(k.text).size(), lv == 1 ? '=' : '-') + "\n";
        break; }
      case Token::Kind::Text: {
        std::string cls = k.align == 2 ? " class=\"right\"" : (k.align == 1 ? " class=\"center\"" : "");
        h += "<p" + cls + ">" + inlineToHtml(k.text) + "</p>\n";
        t += inlineToText(k.text) + "\n";
        break; }
      case Token::Kind::ParagraphEnd:
        for (int b = 1; b < k.blankLines; ++b) h += "<p>&nbsp;</p>\n";
        t += "\n";
        break;
      case Token::Kind::Quote:
        h += "<blockquote>" + inlineToHtml(k.text) + "</blockquote>\n";
        t += "> " + inlineToText(k.text) + "\n";
        break;
      case Token::Kind::CodeBlock:
        h += "<pre>" + esc(k.text) + "</pre>\n";
        t += "\n" + k.text + "\n\n";
        break;
      case Token::Kind::HRule:
        h += "<hr>\n"; t += "----------------------------------------\n";
        break;
      case Token::Kind::ListItem: {
        const int lv = k.level < 1 ? 1 : k.level;
        while (listLevel < lv) { h += "<ul>\n"; ++listLevel; }
        closeLists(lv);
        std::string body = k.text; const size_t sep = body.find('\x07'); std::string bullet;
        if (sep != std::string::npos) { bullet = body.substr(0, sep); body = body.substr(sep + 1); }
        h += "<li>" + inlineToHtml(body) + "</li>\n";
        t += std::string((size_t)(lv - 1) * 2, ' ') + (bullet.empty() ? "-" : bullet) + " " + inlineToText(body) + "\n";
        break; }
      case Token::Kind::Image: {
        const std::string path = (k.text.size() > 1 && (k.text[0] == '/' || k.text[1] == ':')) ? k.text : baseDir + "/" + k.text;
        std::string src;
        if (forMail) { const std::string cid = "img" + std::to_string(++cidN) + "@pdfgen"; r.images.push_back({ cid, path }); src = "cid:" + cid; }
        else { std::string bytes; if (readAll(path, bytes)) src = "data:" + mime(path) + ";base64," + base64(bytes); else { warnings.push_back("html: image not found: " + k.text); src = esc(k.text); } }
        std::string style;
        if (k.widthPt > 0) style += "width:" + pt(k.widthPt) + ";";
        if (k.heightPt > 0) style += "height:" + pt(k.heightPt) + ";";
        const char* cls = k.floatSide == 1 ? "left" : (k.floatSide == 2 ? "right" : "");
        if (k.floatSide) floatOffen = true;
        std::string wrapOpen, wrapClose;
        if (!k.floatSide && k.align == 1) { wrapOpen = "<div class=\"center\">"; wrapClose = "</div>"; }
        if (!k.floatSide && k.align == 2) { wrapOpen = "<div class=\"rightalign\">"; wrapClose = "</div>"; }
        h += wrapOpen + "<img src=\"" + src + "\" alt=\"" + esc(k.text) + "\"" + (cls[0] ? std::string(" class=\"") + cls + "\"" : "") + (style.empty() ? "" : " style=\"" + style + "\"") + ">" + wrapClose + "\n";
        t += "[Bild: " + k.text + "]\n";
        break; }
      case Token::Kind::Table: {
        const pdf::TableOpts& o = k.topts;
        std::string cls; if (o.grid) cls += " grid"; if (o.frame) cls += " frame";
        std::string style = "border-color:" + o.lineColor + ";";
        if (o.totalWidth > 0) style += "width:" + pt(o.totalWidth) + ";"; else if (!o.widths.empty()) style += "width:100%;";
        if (o.indent > 0) style += "margin-left:" + pt(o.indent) + ";";
        h += "<table class=\"tbl" + cls + "\" style=\"" + style + "\">\n";
        if (!o.widths.empty()) {
          double fixed = 0, rel = 0; for (auto& w : o.widths) (w.second ? fixed : rel) += w.first;
          h += "<colgroup>";
          for (auto& w : o.widths) { if (w.second) h += "<col style=\"width:" + pt(w.first) + "\">"; else { char b[32]; std::snprintf(b, sizeof b, "%.1f%%", rel > 0 ? 100.0 * w.first / rel : 100.0 / o.widths.size()); h += std::string("<col style=\"width:") + b + "\">"; } }
          h += "</colgroup>\n";
        }
        for (size_t ri = 0; ri < k.rows.size(); ++ri) {
          const bool kopf = (int)ri < k.headerRows;
          bool rule = (int)ri == k.headerRows - 1; for (int ra : o.rulesAfterRow) if (ra == (int)ri) rule = true;
          h += std::string("<tr") + (rule ? " class=\"rule\"" : "") + ">";
          std::string tz;
          for (size_t ci = 0; ci < k.rows[ri].size(); ++ci) {
            const int span = (ci < k.spans[ri].size() && k.spans[ri][ci] > 1) ? k.spans[ri][ci] : 1;
            const int al = ci < k.aligns.size() ? k.aligns[ci] : 0;
            std::string cs = "padding:" + pt(o.padV) + " " + pt(o.padH) + ";" + (al == 1 ? "text-align:center;" : al == 2 ? "text-align:right;" : "");
            h += std::string(kopf ? "<th" : "<td") + (span > 1 ? " colspan=\"" + std::to_string(span) + "\"" : "") + " style=\"" + cs + "\">" + inlineToHtml(k.rows[ri][ci]) + (kopf ? "</th>" : "</td>");
            if (!tz.empty()) tz += "  |  ";
            tz += inlineToText(k.rows[ri][ci]);
          }
          h += "</tr>\n"; t += tz + "\n";
          if (rule) t += "----------------------------------------\n";
        }
        h += "</table>\n"; t += "\n";
        break; }
      case Token::Kind::NewPage:
        h += "<div class=\"pagebreak\"></div>\n"; t += "\n\n";
        break;
      case Token::Kind::VSpace: {
        h += "<div style=\"height:" + pt(k.vspacePts) + "\"></div>\n"; t += "\n";
        break; }
      case Token::Kind::GroupMark:
        if (floatOffen) { h += "<div class=\"clear\"></div>\n"; floatOffen = false; }
        break;
      case Token::Kind::MarginsChange:
        break;                                                   // Seitenraender: in HTML ohne Bedeutung
      case Token::Kind::ExternalPdf:
        h += "<p><a href=\"" + esc(k.text) + "\">" + esc(k.text) + "</a></p>\n";
        t += "[PDF: " + k.text + "]\n";
        warnings.push_back("html: [pdf=" + k.text + "] becomes a link (cannot merge pages into HTML)");
        break;
      case Token::Kind::IncludeSource:
        warnings.push_back("html: [pdfgen=" + k.text + ", include] is not rendered into HTML");
        break;
    }
  }
  closeLists(0);
  if (floatOffen) h += "<div class=\"clear\"></div>\n";
  h += "</div>\n</body>\n</html>\n";
  // Textfassung: nicht mehr als zwei Leerzeilen in Folge
  std::string tt; int nl = 0;
  for (char c : t) { if (c == '\n') { if (++nl <= 2) tt += c; } else { nl = 0; tt += c; } }
  r.text = tt;
  return r;
}

} // namespace htmlout
