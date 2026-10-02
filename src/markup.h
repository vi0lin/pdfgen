// markup.h — text.txt parsing, restructured as a flat two-stage pipeline.
//
// The Python original nested parse_images → parse_headings → parse_datum
// inside each other, which made the control flow hard to follow (and hid a
// bug: parse_datum returned a variable from parse_headings' scope). Here the
// stages are strictly sequential and never call each other:
//
//   stage 0  expandIncludes()  [datei.txt] tags → file contents
//   stage 1  tokenize()        raw text  →  flat std::vector<Token>
//   stage 2  buildFlowables()  tokens    →  layout objects
//
// Adding a new tag type = one new Token::Kind + one case in each stage.
//
// Recognized input syntax (unchanged from the Python version):
//   \[  \]                       literal square brackets in the output —
//                                 never interpreted as a tag anywhere
//   // Kommentar                  a line starting with // (after optional
//                                 indentation) is dropped entirely
//   [newpage]                     forces a page break ([neueseite] works too)
//   [newpage, no_blank]           page break that is skipped when the current
//                                 page is still empty (never creates a fully
//                                 blank page)
//   [row, pagebreak]              between two table rows: the table breaks to
//                                 a new page exactly here (header repeats),
//                                 even when the rest would still have fit;
//                                 combinable with spacing=...
//   [group] rows [/group]         rows between the markers are never split
//                                 across pages: if the natural break falls
//                                 inside, the whole group moves to the next
//                                 page ([gruppe] works too)
//   [margins, left=2*cm, ...]     ANYWHERE in the content: changes the page
//                                 margins from this point on (left/right
//                                 immediately, top/bottom from the next page)
//   condition=embedded            any [...] tag may carry a condition option:
//   condition=!embedded           the tag only takes effect when the content
//                                 is (not) being embedded into another
//                                 document ([embed=...] / [pdfgen, include]);
//                                 a failing [table...] tag hides the whole
//                                 following table
//   [table, rowspacing=2*mm,...]  default vertical gap between all rows
//   [row, spacing=6*mm]           on its own line between two table rows:
//                                 overrides the gap at exactly this position
//   blank line                    paragraph break; EVERY ADDITIONAL blank
//                                 line adds one more gap unit, so stacked
//                                 blank lines raise the spacing evenly --
//                                 also at the very top of the file (extra
//                                 space above the first heading)
//   single newline                hard line break within a paragraph
//   # .. #####  Heading           heading levels 1–5 (line must start with #)
//   [date] / [datum]              today's date, default format DD.MM.YYYY
//   [date, format="WEEKDAY, D. MONTH YYYY", lang=de]
//                                 format tokens: YYYY YY MM M DD D,
//                                 MONTH/MON (full/short month name),
//                                 WEEKDAY/WD (full/short weekday name);
//                                 lang = en | de | ru | pl | es
//                                 (quote the format if it contains commas)
//   [date, modified=1, ...]       fixed date: if the output PDF already
//                                 exists, its file creation date is used,
//                                 otherwise today. Same formatting options.
//   [pic.png]                     image, default 5×5 cm
//   [pic.png, width=3*cm]         proportional height
//   [pic.png, height=40*mm]       proportional width
//   [pic.png, width=3*cm, height=2*cm]
//   [pic.png, width=3*cm, float=left]   text of the NEXT paragraph wraps
//                                       around the image (also: float=right)
//   [pic.png, width=3*cm, x=12*cm, y=2*cm]
//                                       absolute position measured from the
//                                       LEFT/TOP page edge; the image floats
//                                       OVER the text and occupies no space
//                                       in the flow (current page)
//   [pic.png, ..., dx=5*mm, dy=-2*mm]   nudges a flow image visually
//                                       (dx right, dy down; negatives work);
//                                       the occupied space stays unchanged
//   [pic.webp, width=5*cm, dpi=150]     decoded images (PNG/WebP/BMP) are
//                                       downscaled to the display size at
//                                       this resolution before embedding —
//                                       drastically smaller PDFs at good
//                                       print quality (150 = fine for
//                                       documents, 300 = print-perfect;
//                                       alias: aufloesung). JPEG stays
//                                       verbatim. Requires width= or height=.
//   [pic.png, ..., layer=back]          painted UNDERNEATH the text (above a
//                                       page background) — e.g. watermarks;
//                                       default is layer=front
//   [absender.txt]                include a text file. Alone on a line: block
//                                 include (may itself contain headings, tables,
//                                 images, further includes). Inline (e.g. in a
//                                 table cell): newlines become <br/>.
//   [table, widths=4*cm:2:1, padding=2*mm:1*mm, spacing=0.5*cm,
//           indent=1*cm, width=12*cm, grid=on, frame=on,
//           linewidth=1*pt, linecolor=#333333]
//                                 options for the table starting on the next
//                                 line: fixed column widths (with unit) and
//                                 relative weights (bare numbers) mixable;
//                                 padding=h:v inner cell padding; spacing =
//                                 outer gap above+below; indent = left offset;
//                                 width = total table width; grid=off hides
//                                 the lines. ("tabelle" works too.)
//   [pdf=Site3.pdf]               merge all pages of an existing PDF at this
//                                 point; following content starts on a fresh
//                                 page
//   [pdfgen=Site3.txt, include]   render Site3.txt with pdfgen and merge its
//                                 pages here (recursion depth is limited)
//   images inside table cells work, incl. float=left/right beside cell text;
//   an image alone in a cell with float=right is right-aligned. Images wider
//   than their column are scaled down to fit.
//   | Titel über zwei Spalten || C |
//                                 column span: pipes DIRECTLY after another
//                                 (zero-width segment) extend the cell before
//                                 them by one column. "| |" with a space
//                                 stays an ordinary empty cell.
//   | Kopf A | Kopf B |           table rows (markdown pipes). Tables have NO
//   | :--- | ---: |               borders by default. The FIRST separator row
//   | Zelle | 12,50 |             makes the rows above it a bold header, sets
//   | --- | --- |                 column alignment, and draws a rule at its
//   | Summe | 25,00 |             position; FURTHER separator rows just draw
//                                 a rule there (e.g. above a sum row).
//   <b> <i> <u> </...> <br/>      inline styling inside paragraph text
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_MARKUP_API 15
#include <functional>
#include <string>
#include <vector>
#include "flowables.h"

namespace markup {

struct Token {
  enum class Kind { Text, Heading, Image, Table, ParagraphEnd,
                    ExternalPdf,       // text = path of an existing PDF
                    IncludeSource,     // text = pdfgen source file to render
                    NewPage,
                    MarginsChange,     // margins[l r t b], -1 = keep
                    VSpace,            // vspacePts of vertical space
                    GroupMark };       // @ outside tables
  Token() = default;
  Kind kind;
  std::string text;        // Text: paragraph markup | Heading: title | Image: path
  int level = 0;           // Heading only (1..5)
  double widthPt  = -1;    // Image only, -1 = not given
  double heightPt = -1;    // Image only, -1 = not given
  int floatSide = 0;       // Image only: 0 none, 1 left, 2 right
  int align = 0;           // Image: 0 left, 1 center, 2 right
  bool hasAbs = false;     // Image: x=/y= given
  double absX = 0, absY = 0;
  double dx = 0, dy = 0;
  bool layerBack = false;
  double dpi = 0;          // Image: resample budget (0 = keep full size)
  double vspacePts = 0;    // VSpace: amount in points

  std::vector<std::vector<std::string>> rows;  // Table: cell markup per row
  std::vector<std::vector<int>> spans;         // Table: column span per cell
  std::vector<int> aligns;                     // Table: 0 left, 1 center, 2 right
  int headerRows = 0;                          // Table: leading bold rows
  pdf::TableOpts topts;                        // Table: layout options
  int blankLines = 1;                          // ParagraphEnd: gap multiplier
  bool noBlank = false;                        // NewPage: skip on empty page
  double margins[4] = {-1, -1, -1, -1};        // MarginsChange: l r t b
};

struct Options {
  std::string pdfPath;     // output PDF path, used by [date, modified=1]
};

// Stage -1: drops //-comment lines and evaluates condition= options on all
// [...] tags for the given embedding state (failing tags disappear; a failing
// [table...] tag also hides the following table rows; passing tags lose the
// condition option so downstream parsers see clean tags).
std::string preprocessSource(const std::string& text, bool embedded,
                             std::vector<std::string>& warnings);

// Stage 0a: expands \loop blocks (body repeated per record of a data file,
// : placeholders replaced). Runs after embeds, before file includes.
std::string expandLoops(const std::string& text, const std::string& baseDir,
                        std::vector<std::string>& warnings);

// Stage 0b: replaces [date...] / [datum...] tags with formatted date strings.
std::string expandDates(const std::string& text, const Options& opt,
                        std::vector<std::string>& warnings);

// Stage 0: replaces [datei.txt] tags with the file's contents (relative to
// baseDir). Nested includes are followed up to a fixed depth; missing files
// produce a warning and are dropped. Whole-line tags insert content verbatim,
// inline tags convert newlines to <br/>.
// `embedded` is applied (via preprocessSource) to every included file.
std::string expandIncludes(const std::string& text, const std::string& baseDir,
                           std::vector<std::string>& warnings,
                           bool embedded = false);

// Stage 1: raw UTF-8 text → flat token list. Never recurses.
std::vector<Token> tokenize(const std::string& rawText, const Options& opt,
                            std::vector<std::string>& warnings);

// Renders another pdfgen source file into PDF bytes; used for
// [pdfgen=..., include]. Implemented by the project layer.
struct BuildContext {
  std::function<bool(const std::string& sourceFile, std::string& pdfBytes,
                     std::string& err)> renderInclude;
};

// Stage 2: tokens → flowables. `baseDir` is prepended to image/pdf paths.
// Load failures are reported in `warnings` and skipped. `ctx` may be null;
// [pdfgen=..., include] then produces a warning.
pdf::FlowList buildFlowables(const std::vector<Token>& tokens,
                             const std::string& baseDir,
                             std::vector<std::string>& warnings,
                             const BuildContext* ctx = nullptr);

// "3*cm", "40*mm", "1.5*inch", "12" (pt), "cm" (=1cm) → points; false if invalid.
bool evalDimension(const std::string& expr, double& outPt);

// Today as DD.MM.YYYY.
std::string todayString();

} // namespace markup
