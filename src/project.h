// project.h — the layer above the markup pipeline: one source file (text.txt
// or any .txt passed on the command line) may define SEVERAL PDF documents
// and SEVERAL e-mails.
//
// Control tags (each on its own line; a tag may wrap across lines until its
// closing bracket):
//
//   [pdfgen, file=Bewerbung.pdf]        start a document; the following lines
//                                       are its content (the previous document
//                                       is finalized here)
//   [pdfgen, file=X.pdf, margins=1.4*cm:1*cm:0.5*cm:0.5*cm]
//                                       page margins left:right:top:bottom;
//                                       also as single options left= right=
//                                       top= bottom= (German: links rechts
//                                       oben unten). Unset values keep the
//                                       defaults 1.4cm / 1.0cm / 0.5cm /
//                                       0.5cm. Alternatively a standalone
//                                       line INSIDE the document content:
//                                       [margins, left=2*cm, top=1*cm]
//                                       (works for the implicit document and
//                                       inside [pdfgen, include] sources;
//                                       it wins over the tag options)
//   [pdfgen=Lebenslauf.txt, file=Lebenslauf.pdf]
//                                       start a document whose content comes
//                                       from Lebenslauf.txt instead
//   [mail=Name, to=a@x b@y, cc=c@z, attachment=Bewerbung.pdf, test=me@x,
//         from=ich@gmail.com]
//                                       define a mail; the next non-empty
//                                       line is the Subject, the lines after
//                                       it (until the next control tag) are
//                                       the body. Address lists are space-
//                                       separated. `test` is an alternative
//                                       recipient used with --mail-test
//                                       (fallback: [test] in pdfgen.conf).
//                                       `from` picks the sender account from
//                                       the mail config (mail_config.h);
//                                       without it the default account is
//                                       used. Backslash form:
//                                       \mail Name, to=..., from=...
//   [attach, mail=Name, file=X.pdf]     add another attachment to mail `Name`
//
// A [pdfgen...] tag WITH the `include` flag is NOT a control tag — it stays
// inside the document content and merges rendered pages there (markup.h).
//
//   [header] ... [/header]              per-page header block; rendered on
//   [bottom] ... [/bottom]              every own page (not on merged foreign
//                                       pages). [document] = output file name,
//                                       [pages] = "current/total". The blocks
//                                       shrink the content area; page margins
//                                       stay visually intact. Any markup
//                                       (tables, images, headings) works.
//   [embed=Quelle.txt]                  inline the file into THIS document's
//                                       flow with the `embedded` condition
//                                       active. Options: `newpage` starts it
//                                       on a fresh page; `newpage_no_blank`
//                                       does the same but skips the break if
//                                       the current page is still empty (no
//                                       blank page); `keepmargins` drops
//                                       the file's own [margins] tags so the
//                                       parent margins stay (default: its
//                                       [margins] apply from that point and
//                                       the parent margins are restored after
//                                       the block).
//   condition=embedded|!embedded        works on the control tags too: a
//                                       failing [pdfgen]/[mail] skips its
//                                       whole section, a failing [attach] is
//                                       ignored.
//   // Kommentar                        lines starting with // are dropped
//
// If a source contains no control tags at all, the whole file is one
// document: DIR sources produce DIR/Bewerbung.pdf (old behavior), .txt
// sources produce <basename>.pdf next to the source, e.g.
// `pdfgen Example.txt` -> Example.pdf.
#pragma once
#define PDFGEN_PROJECT_API 5

#include <string>
#include <vector>
#include <utility>

namespace project {

// Page margins in points; defaults match the original hard-coded values.
struct PageLayout {
  double left   = 1.4  * 28.3465;   // 1.4 cm
  double right  = 1.0  * 28.3465;
  double top    = 0.5  * 28.3465;
  double bottom = 0.5  * 28.3465;
};

struct DocSpec {
  std::string outFile;          // relative to baseDir -- Basisname; je Format kommt .pdf/.html
  std::string sourceFile;       // "" = inline content
  std::string inlineText;
  PageLayout layout;            // from [pdfgen ...] tag options
  std::vector<std::string> formats{"pdf"};   // "pdf", "html" -- [doc, format=pdf,html]
  std::string name;             // [doc=Name] / [html=Name]: Name fuer body= in Mails
};

struct MailSpec {
  std::string name;
  std::vector<std::string> to, cc, attachments;
  std::string testAddr;         // for --mail-test
  std::string from;             // sender account ("" = default account)
  std::string subject, body;
  std::string baseDir;          // attachments resolve relative to this
  bool html = false;            // format=html: body durch die pdfgen-Pipeline nach HTML
  std::string bodyDoc;          // body=Name: Inhalt des Dokuments Name als Koerper
  std::string bodyHtml;         // gerendert (processSource), "" = reine Textmail
  std::vector<std::pair<std::string, std::string>> inlineImages;   // cid -> Datei (HTML-Mail)
};

struct Project {
  std::vector<DocSpec> docs;
  std::vector<MailSpec> mails;
};

// Splits a source text into document sections and mail definitions.
// `defaultOutFile` names the implicit document used when no [pdfgen] tag
// appears (or for content before the first tag).
Project parseProject(const std::string& text, const std::string& baseDir,
                     const std::string& defaultOutFile,
                     std::vector<std::string>& warnings);

// Renders one document source text into a PDF file (or into `outBytes` when
// non-null, for includes). Handles [pdfgen=..., include] recursively with a
// depth limit. Returns false on failure.
bool renderDocument(const std::string& baseDir, const std::string& sourceText,
                    const std::string& outPdfPath, std::string* outBytes,
                    std::vector<std::string>& warnings, int depth = 0,
                    PageLayout layout = {}, bool embedded = false);
// Renders one document source text into an HTML file (same tokens as the
// PDF path, see htmlwriter.h). Returns false on failure.
bool renderHtmlDocument(const std::string& baseDir, const std::string& sourceText,
                        const std::string& outHtmlPath, std::vector<std::string>& warnings);

// Convenience: process one CLI source (directory or .txt file): parse,
// render every document, and append the mails to `mails`.
// Mail-merge expansion (\mloop, \jloop, \jload, $placeholders) -- runs on
// the raw main source before documents/mails are split. processSource calls
// this itself; exposed for hosts that assemble sources in memory.
std::string expandMailMerge(const std::string& text, const std::string& baseDir,
                            std::vector<std::string>& warnings);

bool processSource(const std::string& cliArg, std::vector<MailSpec>& mails,
                   std::vector<std::string>& warnings);

} // namespace project
