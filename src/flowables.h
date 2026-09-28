// flowables.h — layout engine (the C++ counterpart of reportlab.platypus).
//
// A Flowable knows how to (a) compute its height for a given width and
// (b) draw itself at an absolute position. The Document walks the flowable
// list, breaks pages, and splits paragraphs that straddle a page boundary.
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_FLOWABLES_API 12
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "pdfwriter.h"

namespace pdf {

enum class Align : uint8_t { Left, Center, Right, Justify };

struct Style {
  Font   font        = Font::Helvetica;
  double size        = 11.0;
  double leading     = 16.0;
  Align  align       = Align::Left;
  double spaceBefore = 0.0;    // gap inserted above (skipped at top of page)
  double spaceAfter  = 0.0;
};

// Predefined styles roughly matching reportlab's sample stylesheet.
namespace styles {
  Style normal();
  Style body();                // 11pt / 16 leading, justified (Python's Body)
  Style dateRight();           // right-aligned normal
  Style heading(int level);    // 1..5
}

// ---- flowables ----------------------------------------------------------------
class Flowable;
using FlowPtr  = std::unique_ptr<Flowable>;
using FlowList = std::vector<FlowPtr>;

class Flowable {
public:
  virtual ~Flowable() = default;
  // Lays out for `width`, returns total height (excl. spaceBefore/After).
  virtual double wrap(double width) = 0;
  // Draws with the top edge at absolute `yTop`, left edge at `x`.
  virtual void draw(Writer& w, double x, double yTop) = 0;

  virtual const Style* style() const { return nullptr; }

  // Width the flowable would like on a single line; used for table column
  // sizing. 0 = no preference.
  virtual double naturalWidth() const { return 0; }
  // Deep copy (used to repeat table header rows across pages).
  // nullptr = not clonable.
  virtual FlowPtr cloneFlow() const { return nullptr; }
  // Non-null for the pseudo-flowable that inserts the pages of an existing
  // PDF at this position (Document::build handles it specially).
  virtual const std::string* externalPdfBytes() const { return nullptr; }

  // Page-break splitting: returns the part that fits into `availHeight`
  // (nullptr if nothing fits) and leaves the remainder in *this.
  virtual bool splittable() const { return false; }
  virtual std::unique_ptr<Flowable> splitTop(double width, double availHeight)
  { (void)width; (void)availHeight; return nullptr; }
  // Split demanded by the content itself (e.g. [row, pagebreak] in a table)
  // even though everything would fit; nullptr = no forced break pending.
  virtual std::unique_ptr<Flowable> splitTopForced(double width)
  { (void)width; return nullptr; }
};

// Paragraph with minimal inline markup: <b> <i> <u> </...> and <br/>.
// Input text must be UTF-8; conversion to WinAnsi happens internally.
//
// Lines are normally all broken to the same width. For floats, callers can
// install per-line boxes (width + x offset, last entry repeating) via
// setLineBoxes() -- that's how FloatFlow makes text wrap around an image.
class Paragraph : public Flowable {
public:
  Paragraph(std::string utf8Markup, Style style);
  double wrap(double width) override;
  void   draw(Writer& w, double x, double yTop) override;
  const Style* style() const override { return &style_; }
  // Paragraphs with custom line boxes stay atomic (floats never straddle pages).
  bool splittable() const override { return boxes_.empty(); }
  std::unique_ptr<Flowable> splitTop(double width, double availHeight) override;

  struct LineBox { double width; double xOffset; };
  void setLineBoxes(std::vector<LineBox> boxes);   // empty = uniform width
  // Width if laid out on a single line (longest <br/>-segment); used by
  // TableFlow to size columns proportionally to their content.
  double naturalWidth() const override;
  std::unique_ptr<Paragraph> clone() const { return std::unique_ptr<Paragraph>(new Paragraph(*this)); }
  FlowPtr cloneFlow() const override { return clone(); }
  size_t lineCount() const { return lines_.size(); }

private:
  // uni=false: text is WinAnsi bytes for the built-in fonts.
  // uni=true:  text is UTF-8, rendered with the embedded Unicode font.
  struct Frag { std::string text; Font font; bool underline; bool uni; };
  struct Word { std::vector<Frag> frags; double width; bool forcedBreakAfter; };
  struct Line { std::vector<Frag> frags; double width; int spaces; bool last;
                double boxWidth; double xOff; bool hasUni; };

  Style style_;
  std::vector<Word> words_;      // built once in ctor
  std::vector<Line> lines_;      // rebuilt by wrap()
  std::vector<LineBox> boxes_;   // per-line geometry, empty = uniform
  double wrappedWidth_ = -1;

  Paragraph(Style style) : style_(style) {}   // for splitTop
  void buildWords(const std::string& utf8Markup);
  void breakLines(double width);
};

// Fixed vertical gap.
class Spacer : public Flowable {
public:
  explicit Spacer(double h) : h_(h) {}
  double wrap(double) override { return h_; }
  void draw(Writer&, double, double) override {}
  FlowPtr cloneFlow() const override { return std::make_unique<Spacer>(h_); }
private:
  double h_;
};

// An image scaled to a box of width×height points. If the requested width
// exceeds the available width at wrap() time (e.g. a wide image inside a
// narrow table column), it is scaled down proportionally to fit. Alignment
// within the available width: Left (default), Center, Right.
class ImageFlow : public Flowable {
public:
  // Loads the file; check ok()/error() before adding to the document.
  // dpi > 0: decoded images (PNG/WebP/BMP) are downscaled so the embedded
  // bitmap matches the requested display size at that resolution — e.g.
  // width=5*cm with dpi=150 embeds at most ~295 px width. JPEGs are always
  // embedded verbatim.
  ImageFlow(const std::string& path, double wPt, double hPt, double dpi = 0);
  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  int pixelWidth()  const { return img_.widthPx; }
  int pixelHeight() const { return img_.heightPx; }
  void setAlign(Align a) { align_ = a; }
  // Nudges the drawn position relative to the flow (positive dx = right,
  // positive dy = down); the occupied layout space stays unchanged.
  void setOffset(double dx, double dy) { dx_ = dx; dy_ = dy; }
  // Absolute position on the page, measured from the LEFT and TOP page
  // edges. The image then occupies no space in the text flow at all.
  void setAbsolute(double xFromLeft, double yFromTop) {
    absolute_ = true; absX_ = xFromLeft; absY_ = yFromTop;
  }
  bool isAbsolute() const { return absolute_; }
  // layer=back: painted underneath the page's text (above the background).
  void setBackLayer(bool b) { backLayer_ = b; }

  // Effective (possibly downscaled) size after the last wrap().
  double boxWidth()  const { return effW_; }
  double boxHeight() const { return effH_; }
  double naturalWidth() const override { return reqW_; }
  FlowPtr cloneFlow() const override { return FlowPtr(new ImageFlow(*this)); }

  double wrap(double width) override;
  void draw(Writer& w, double x, double yTop) override;

private:
  ImageXObject img_;
  bool ok_ = false;
  std::string error_, resource_;
  double reqW_ = 0, reqH_ = 0;     // requested size
  double effW_ = 0, effH_ = 0;     // clamped to available width
  double availW_ = 0;
  Align align_ = Align::Left;
  double dx_ = 0, dy_ = 0;
  bool absolute_ = false, backLayer_ = false;
  double absX_ = 0, absY_ = 0;
};

// Internal marker used while building: separates @/[group] flow groups.
// Never reaches the Document (the builder replaces the groups by StackFlows).
class GroupMarkFlow : public Flowable {
public:
  explicit GroupMarkFlow(bool start) : start_(start) {}
  double wrap(double) override { return 0; }
  void draw(Writer&, double, double) override {}
  bool start() const { return start_; }
private:
  bool start_;
};

// Forces a page break ([newpage]). With noBlank ([newpage, no_blank]) the
// break is skipped when the current page is still empty, so no fully blank
// page is ever produced by it.
class PageBreakFlow : public Flowable {
public:
  explicit PageBreakFlow(bool noBlank = false) : noBlank_(noBlank) {}
  double wrap(double) override { return 0; }
  void draw(Writer&, double, double) override {}
  bool noBlank() const { return noBlank_; }
private:
  bool noBlank_;
};

// Changes the page margins from this point on ([margins, ...] mid-document,
// used by [embed] without keepmargins). Values < 0 keep the current margin.
// Left/right apply immediately; top/bottom from the next page break.
class MarginsFlow : public Flowable {
public:
  MarginsFlow(double l, double r, double t, double b)
      : l_(l), r_(r), t_(t), b_(b) {}
  double wrap(double) override { return 0; }
  void draw(Writer&, double, double) override {}
  double l_, r_, t_, b_;
};

// Inserts all pages of an existing PDF at this point of the document
// ([pdf=Site3.pdf] / [pdfgen=Quelle.txt, include]). Content before it ends
// on the current page; content after it starts on a fresh page.
class ExternalPdfFlow : public Flowable {
public:
  explicit ExternalPdfFlow(std::string pdfBytes) : bytes_(std::move(pdfBytes)) {}
  double wrap(double) override { return 0; }
  void draw(Writer&, double, double) override {}
  const std::string* externalPdfBytes() const override { return &bytes_; }
  // newpage_no_blank: an empty page (even one only carrying header/footer/
  // background) directly before the merge is discarded.
  void setNoBlankBefore(bool b) { noBlankBefore_ = b; }
  bool noBlankBefore() const { return noBlankBefore_; }
private:
  std::string bytes_;
  bool noBlankBefore_ = false;
};

// Several flowables stacked vertically (used for mixed-content table cells:
// text above/below images within one cell).
class StackFlow : public Flowable {
public:
  explicit StackFlow(FlowList kids, double gap = 2.0);
  double wrap(double width) override;
  void   draw(Writer& w, double x, double yTop) override;
  double naturalWidth() const override;
  FlowPtr cloneFlow() const override;
private:
  FlowList kids_;
  double gap_;
  double lastWidth_ = 0;
};

// Layout options for TableFlow, parsed from a [table, ...] tag.
struct TableOpts {
  // Per column: {value, isFixed}. Fixed = points; non-fixed = relative
  // weight for distributing the remaining width. Missing columns get
  // weight 1. Empty = size all columns by their natural content width.
  std::vector<std::pair<double, bool>> widths;
  double padH = 4.0, padV = 3.0;   // inner cell padding
  double spacing = -1;             // outer vertical gap above+below (-1 = default)
  double indent  = 0;              // left offset of the whole table
  double totalWidth = -1;          // table width (-1 = full text width)

  // Borders. Default: none at all. A markdown separator row (|---|---|)
  // draws a single horizontal rule at its position (the first one also
  // makes the rows above it a bold header) -- that covers header underlines
  // and "sum" rules. grid=on draws the full grid, frame=on the outer box.
  bool grid  = false;
  bool frame = false;
  double lineWidth = 0.5;                // points
  std::string lineColor = "#4d4d4d";     // hex
  std::vector<int> rulesAfterRow;        // extra horizontal rules (0-based)

  // Vertical gap between rows: `rowGap` for every pair of rows
  // ([table, rowspacing=...]), overridable at single positions via a
  // [row, spacing=...] line between two table rows. Grid lines close both
  // rows around a gap; the outer frame spans across gaps.
  double rowGap = 0;
  std::vector<std::pair<int, double>> rowGapOverrides;  // gap AFTER row i

  // Forced page breaks inside the table ([row, pagebreak]) and groups of
  // rows that must never be split across pages ([group]...[/group]).
  std::vector<int> breakAfterRow;                       // split AFTER row i
  std::vector<std::pair<int, int>> keepGroups;          // [first, last] incl.
};

// General N-column table. Cells are arbitrary flowables (paragraphs, images,
// stacks, floats) and may span several columns; rows split across pages with
// the header row(s) repeated.
class TableFlow : public Flowable {
public:
  struct Cell { FlowPtr flow; int span = 1; };
  using Row = std::vector<Cell>;
  TableFlow(std::vector<Row> rows, int headerRows, TableOpts opts = {});
  double wrap(double width) override;
  void   draw(Writer& w, double x, double yTop) override;
  const Style* style() const override { return &style_; }
  bool splittable() const override { return true; }
  std::unique_ptr<Flowable> splitTop(double width, double availHeight) override;
  std::unique_ptr<Flowable> splitTopForced(double width) override;

private:
  std::unique_ptr<Flowable> splitAtRow(size_t fit);
  std::vector<Row> rows_;
  int headerRows_;
  TableOpts opts_;
  Style style_;                    // carries spaceBefore/After from opts
  size_t nCols_ = 0;
  std::vector<double> colW_;       // computed by wrap (or frozen by splitTop)
  std::vector<double> rowH_;
  bool colWFrozen_ = false;
  double wrappedWidth_ = -1;
  double tableWidth(double avail) const;
  void computeColumnWidths(double width);
  void layoutRows();
};

// Image with text flowing around it ("float: left/right"): the paragraph's
// first lines are narrowed beside the image, the rest continues full width.
class FloatFlow : public Flowable {
public:
  FloatFlow(std::unique_ptr<ImageFlow> img, std::unique_ptr<Paragraph> para,
            bool floatRight, double gutter = 0.4 * CM);
  double wrap(double width) override;
  void   draw(Writer& w, double x, double yTop) override;
  double naturalWidth() const override;
  FlowPtr cloneFlow() const override;
private:
  std::unique_ptr<ImageFlow> img_;
  std::unique_ptr<Paragraph> para_;
  bool right_;
  double gutter_, height_ = 0, imgW_ = 0, imgH_ = 0, lastWidth_ = 0;
};

// Per-page header/footer ([header]...[/header] / [bottom]...[/bottom]).
// The markup may contain [pages] (current/total) and [document] (file name);
// both are substituted per page before `build` turns the markup into
// flowables. totalPages = 0 during the counting pass.
struct PageDecor {
  std::string headerMarkup;   // "" = none
  std::string footerMarkup;   // "" = none
  std::string docName;
  int totalPages = 0;
  std::function<FlowList(const std::string& markupText)> build;
};

// ---- document -------------------------------------------------------------------
class Document {
public:
  Document(Writer& w,
           double marginLeft, double marginRight,
           double marginTop,  double marginBottom);

  void add(FlowPtr f) { flow_.push_back(std::move(f)); }
  // Optional solid page background, e.g. "#f5eee4" ("" = none).
  void setBackground(const std::string& hexColor) { bg_ = hexColor; }
  void setDecor(PageDecor d) { decor_ = std::move(d); }
  void build();

  // Total pages produced by build(), incl. imported ones (for the
  // two-pass [pages] rendering).
  int totalPagesRendered() const { return curPage_; }

  double textWidthAvail() const { return w_.pageWidth() - mL_ - mR_; }

private:
  Writer& w_;
  double mL_, mR_, mT_, mB_;
  std::string bg_;
  FlowList flow_;
  PageDecor decor_;
  int curPage_ = 0;

  std::string substitute(const std::string& markup) const;
  double decorHeight(const std::string& markup, double width);
  void drawDecor(const std::string& markup, double x, double yTopEdge, double width);
  void startPage();
};

} // namespace pdf
