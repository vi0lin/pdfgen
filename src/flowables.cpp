#include "flowables.h"
#include <cctype>   // std::tolower (nie auf transitive Includes verlassen)
#include <algorithm>
#include <cmath>
#include <sstream>
#include "image.h"
#include "metrics.h"
#include "ttffont.h"

// ---- release consistency check (see header comment) ----
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
#ifndef PDFGEN_METRICS_API
#error "stale metrics.h: it lacks PDFGEN_METRICS_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_METRICS_API != 2
#error "version mismatch in metrics.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_IMAGE_API
#error "stale image.h: it lacks PDFGEN_IMAGE_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_IMAGE_API != 3
#error "version mismatch in image.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_TTFFONT_API
#error "stale ttffont.h: it lacks PDFGEN_TTFFONT_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_TTFFONT_API != 1
#error "version mismatch in ttffont.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif


namespace pdf {

// ---- styles --------------------------------------------------------------------
namespace styles {
Style normal() { return {Font::Helvetica, 10, 12, Align::Left, 0, 0}; }
Style body()   { return {Font::Helvetica, 11, 16, Align::Justify, 0, 0}; }
Style code() {
  Style s;
  s.font = Font::Courier;
  s.size = 9.5;
  s.leading = 13.0;
  s.align = Align::Left;
  return s;
}

Style dateRight() { Style s = normal(); s.align = Align::Right; return s; }
Style heading(int level) {
  level = std::clamp(level, 1, 5);
  static const double sz[] = {18, 14, 12, 10, 9};
  static const double before[] = {12, 12, 12, 10, 8};
  static const double after[]  = {6, 6, 6, 4, 4};
  Style s;
  s.font = Font::HelveticaBold;
  s.size = sz[level - 1];
  s.leading = s.size * 1.2;
  s.spaceBefore = before[level - 1];
  s.spaceAfter  = after[level - 1];
  return s;
}
} // namespace styles

// ---- Paragraph -------------------------------------------------------------------
Paragraph::Paragraph(std::string utf8Markup, Style style) : style_(style) {
  buildWords(utf8Markup);
}

static bool isBoldFont(Font f) {
  return f == Font::HelveticaBold || f == Font::HelveticaBoldOblique;
}

static std::string encodeUtf8(uint32_t cp) {
  std::string o;
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) {
    o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F));
    o += (char)(0x80 | (cp & 0x3F));
  } else {
    o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
    o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
  }
  return o;
}

static double fragWidth(const Paragraph* /*unused*/, Font font, bool uni,
                        const std::string& text, double size) {
  if (uni) return UnicodeFonts::inst().measure(text, isBoldFont(font), size);
  return textWidth(font, text, size);
}

static Font applyFace(Font base, bool bold, bool italic) {
  bool baseBold   = base == Font::HelveticaBold || base == Font::HelveticaBoldOblique;
  bool baseItalic = base == Font::HelveticaOblique || base == Font::HelveticaBoldOblique;
  bold   = bold   || baseBold;
  italic = italic || baseItalic;
  if (bold && italic) return Font::HelveticaBoldOblique;
  if (bold)   return Font::HelveticaBold;
  if (italic) return Font::HelveticaOblique;
  return Font::Helvetica;
}

void Paragraph::buildWords(const std::string& utf8) {
  // 1) parse inline markup into styled runs, splitting on spaces into words
  bool bold = false, italic = false, underline = false;
  bool strike = false, mono = false;
  int  link = -1;
  Word cur;  cur.width = 0; cur.forcedBreakAfter = false;

  auto flushWord = [&](bool forced) {
    if (!cur.frags.empty() || forced) {
      cur.forcedBreakAfter = forced;
      if (!cur.frags.empty() || forced) words_.push_back(std::move(cur));
      cur = Word{};
    }
  };
  auto appendCp = [&](uint32_t cp) {
    if (cp == 0x06) cp = '<';                      // literal '<' from code spans

    Font f = mono ? Font::Courier : applyFace(style_.font, bold, italic);
    int wb = cpToWinAnsi(cp);
    bool uni = false;
    std::string bytes;
    if (wb >= 0) bytes = std::string(1, (char)wb);
    else if (UnicodeFonts::inst().available()) { uni = true; bytes = encodeUtf8(cp); }
    else bytes = "?";
    if (cur.frags.empty() || cur.frags.back().font != f ||
        cur.frags.back().underline != underline || cur.frags.back().uni != uni ||
        cur.frags.back().strike != strike || cur.frags.back().link != link)
      cur.frags.push_back({std::string(), f, underline, uni, strike, link});
    cur.frags.back().text += bytes;
  };

  const std::string& s = utf8;                    // raw UTF-8 (tags are ASCII)
  size_t i = 0, n = s.size();
  while (i < n) {
    char c = s[i];
    if (c == '<') {                              // possible tag
      size_t close = s.find('>', i);
      bool longTag = close != std::string::npos && i + 3 <= n &&
                     (s.compare(i + 1, 2, "a=") == 0 ||
                      s.compare(i + 1, 2, "A=") == 0);
      if (close != std::string::npos && (close - i <= 6 || longTag)) {
        std::string tag = s.substr(i + 1, close - i - 1);
        std::string low;
        for (char t : tag) low += (char)std::tolower((unsigned char)t);
        bool handled = true;
        if (longTag) {
          links_.push_back(tag.substr(2));         // raw URL, case preserved
          link = (int)links_.size() - 1;
          underline = true;                        // links render underlined
          i = close + 1;
          continue;
        }
        if      (low == "b")    bold = true;
        else if (low == "/b")   bold = false;
        else if (low == "i")    italic = true;
        else if (low == "/i")   italic = false;
        else if (low == "u")    underline = true;
        else if (low == "/u")   underline = false;
        else if (low == "s")    strike = true;
        else if (low == "/s")   strike = false;
        else if (low == "code")  mono = true;
        else if (low == "/code") mono = false;
        else if (low == "/a")   { link = -1; underline = false; }
        else if (low == "br" || low == "br/" || low == "br /") flushWord(true);
        else handled = false;
        if (handled) { i = close + 1; continue; }
      }
    }
    if (c == ' ' || c == '\t') { flushWord(false); ++i; continue; }
    if (c == '\n')             { flushWord(true);  ++i; continue; } // hard break
    if (c == '\r')             { ++i; continue; }
    appendCp(decodeUtf8(s, i));                   // advances i
  }
  flushWord(false);

  for (auto& w : words_) {                        // 2) measure
    w.width = 0;
    for (auto& f : w.frags)
      w.width += fragWidth(this, f.font, f.uni, f.text, style_.size);
  }
}

void Paragraph::breakLines(double width) {
  lines_.clear();
  double spaceW = textWidth(style_.font, " ", style_.size);

  auto boxFor = [&](size_t idx) -> LineBox {
    if (boxes_.empty()) return {width, 0.0};
    return boxes_[std::min(idx, boxes_.size() - 1)];
  };

  Line line; line.width = 0; line.spaces = 0; line.last = false; line.hasUni = false;
  line.boxWidth = boxFor(0).width; line.xOff = boxFor(0).xOffset;
  auto pushLine = [&](bool last) {
    line.last = last;
    lines_.push_back(std::move(line));
    line = Line{};
    line.hasUni = false;
    LineBox b = boxFor(lines_.size());
    line.boxWidth = b.width; line.xOff = b.xOffset;
  };

  for (size_t i = 0; i < words_.size(); ++i) {
    const Word& w = words_[i];
    double sep = line.frags.empty() ? 0 : spaceW;
    if (!line.frags.empty() && line.width + sep + w.width > line.boxWidth + 0.01) {
      pushLine(false);
      sep = 0;
    }
    if (!line.frags.empty()) {
      // merge separator space into previous frag if style matches, else new frag
      Frag& pv = line.frags.back();
      double sw = pv.uni
          ? fragWidth(this, pv.font, true, " ", style_.size)
          : spaceW;
      if (!w.frags.empty() && pv.font == w.frags[0].font &&
          pv.underline == w.frags[0].underline && pv.uni == w.frags[0].uni)
        pv.text += ' ';
      else
        line.frags.push_back({" ", pv.font, false, pv.uni});
      line.width += sw;
      line.spaces++;
    }
    for (const auto& f : w.frags) {
      if (f.uni) line.hasUni = true;
      if (!line.frags.empty() && line.frags.back().font == f.font &&
          line.frags.back().underline == f.underline &&
          line.frags.back().uni == f.uni)
        line.frags.back().text += f.text;
      else
        line.frags.push_back(f);
    }
    line.width += w.width;
    if (w.forcedBreakAfter) pushLine(true);       // forced breaks never justify
  }
  if (!line.frags.empty()) pushLine(true);
  if (lines_.empty()) pushLine(true);             // empty paragraph → one blank line
}

double Paragraph::wrap(double width) {
  width -= style_.leftIndent;                     // markdown list/quote indent

  if (width != wrappedWidth_) { breakLines(width); wrappedWidth_ = width; }
  return lines_.size() * style_.leading;
}

void Paragraph::setLineBoxes(std::vector<LineBox> boxes) {
  boxes_ = std::move(boxes);
  wrappedWidth_ = -1;                       // force re-break on next wrap()
}

double Paragraph::naturalWidth() const {  // override
  double spaceW = textWidth(style_.font, " ", style_.size);
  double cur = 0, best = 0;
  for (const auto& w : words_) {
    if (cur > 0) cur += spaceW;
    cur += w.width;
    best = std::max(best, cur);
    if (w.forcedBreakAfter) cur = 0;
  }
  return best;
}

void Paragraph::draw(Writer& w, double x, double yTop) {
  if (wrappedWidth_ < 0) wrap(w.pageWidth());
  std::ostringstream o;
  const double leading = style_.leading;

  for (size_t li = 0; li < lines_.size(); ++li) {
    const Line& L = lines_[li];
    double baseline = yTop - li * leading - 0.8 * leading;

    double indent = style_.leftIndent -
                    (li == 0 ? style_.hangOutdent : 0.0);   // hanging bullet
    double startX = x + indent + L.xOff, tw = 0;
    double slack = L.boxWidth - L.width;
    if (style_.align == Align::Right)  startX = x + L.xOff + slack;
    if (style_.align == Align::Center) startX = x + L.xOff + slack / 2;
    // word-spacing justification only works for 1-byte codes; lines with
    // embedded-font (2-byte CID) fragments are set at natural spacing
    if (style_.align == Align::Justify && !L.last && L.spaces > 0 && slack > 0 &&
        !L.hasUni)
      tw = slack / L.spaces;

    o << "BT\n";
    if (tw > 0) o << Writer::num(tw) << " Tw\n"; else o << "0 Tw\n";
    o << Writer::num(startX) << " " << Writer::num(baseline) << " Td\n";

    double penX = startX;
    for (const auto& f : L.frags) {
      double fw;
      if (f.uni) {
        auto& uf = UnicodeFonts::inst();
        bool bold = isBoldFont(f.font);
        o << "/" << uf.resourceName(bold) << " " << Writer::num(style_.size)
          << " Tf <" << uf.hexString(f.text, bold) << "> Tj\n";
        fw = uf.measure(f.text, bold, style_.size);
      } else {
        o << "/" << fontResourceName(f.font) << " " << Writer::num(style_.size)
          << " Tf (" << Writer::escapeString(f.text) << ") Tj\n";
        fw = textWidth(f.font, f.text, style_.size);
        for (unsigned char c : f.text) if (c == ' ') fw += tw;
      }
      if (f.underline || f.strike) {
        o << "ET\nq 0.5 w ";
        if (f.underline) {
          double uy = baseline - 0.11 * style_.size;
          o << Writer::num(penX) << " " << Writer::num(uy) << " m "
            << Writer::num(penX + fw) << " " << Writer::num(uy) << " l S ";
        }
        if (f.strike) {
          double sy = baseline + 0.26 * style_.size;   // mid x-height
          o << Writer::num(penX) << " " << Writer::num(sy) << " m "
            << Writer::num(penX + fw) << " " << Writer::num(sy) << " l S ";
        }
        o << "Q\nBT\n" << Writer::num(tw > 0 ? tw : 0.0) << " Tw\n"
          << Writer::num(penX + fw) << " " << Writer::num(baseline) << " Td\n";
      }
      if (f.link >= 0 && f.link < (int)links_.size()) {
        w.addLink(penX, baseline - 0.25 * style_.size, fw,
                  1.05 * style_.size, links_[(size_t)f.link]);
      }
      penX += fw;
    }
    o << "ET\n";
  }
  w.content(o.str());
}

std::unique_ptr<Flowable> Paragraph::splitTop(double width, double availHeight) {
  wrap(width);
  size_t fit = (size_t)(availHeight / style_.leading);
  if (fit == 0 || fit >= lines_.size()) return nullptr;   // nothing / everything fits

  auto top = std::unique_ptr<Paragraph>(new Paragraph(style_));
  top->wrappedWidth_ = width;
  top->lines_.assign(lines_.begin(), lines_.begin() + fit);
  top->lines_.back().last = true;                          // don't justify cut line

  lines_.erase(lines_.begin(), lines_.begin() + fit);
  wrappedWidth_ = width;                                   // remainder stays laid out
  return top;
}

void HRuleFlow::draw(Writer& w, double x, double yTop) {
  std::ostringstream o;
  double y = yTop - 5.0;
  o << "q 0.8 w 0.7 G " << Writer::num(x) << " " << Writer::num(y) << " m "
    << Writer::num(x + width_) << " " << Writer::num(y) << " l S Q\n";
  w.content(o.str());
}

// ---- ImageFlow -------------------------------------------------------------------
ImageFlow::ImageFlow(const std::string& path, double wPt, double hPt,
                     double dpi)
    : reqW_(wPt), reqH_(hPt) {
  int maxW = 0, maxH = 0;
  if (dpi > 0) {
    if (wPt > 0) maxW = (int)(wPt / 72.0 * dpi + 0.5);
    if (hPt > 0) maxH = (int)(hPt / 72.0 * dpi + 0.5);
  }
  ok_ = loadImage(path, img_, error_, maxW, maxH);
  if (!ok_) return;
  double aspect = img_.widthPx > 0 ? (double)img_.heightPx / img_.widthPx : 1.0;
  if (reqW_ > 0 && reqH_ <= 0)      reqH_ = reqW_ * aspect;   // proportional
  else if (reqH_ > 0 && reqW_ <= 0) reqW_ = reqH_ / aspect;   // fill-in
  else if (reqW_ <= 0 && reqH_ <= 0) {
    // no dimension given: the image's natural size (pixels at 96 dpi,
    // the CSS/Word convention); wider than the text area is clamped
    // proportionally by wrap() as usual
    reqW_ = img_.widthPx * 72.0 / 96.0;
    reqH_ = img_.heightPx * 72.0 / 96.0;
  }
  effW_ = reqW_; effH_ = reqH_;
}

double ImageFlow::wrap(double width) {
  availW_ = width;
  if (absolute_) {                 // takes no space in the flow
    effW_ = reqW_; effH_ = reqH_;
    return 0;
  }
  // never overflow the available width (e.g. wide image in a narrow column)
  double scale = (reqW_ > width && width > 0) ? width / reqW_ : 1.0;
  effW_ = reqW_ * scale;
  effH_ = reqH_ * scale;
  return effH_;
}

void ImageFlow::draw(Writer& w, double x, double yTop) {
  if (!ok_) return;
  if (resource_.empty()) resource_ = w.addImage(img_);
  double ix, iy;                                    // bottom-left corner
  if (absolute_) {
    ix = absX_;
    iy = w.pageHeight() - absY_ - effH_;            // y measured from the top
  } else {
    ix = x;
    if (availW_ > effW_) {
      if (align_ == Align::Right)  ix = x + availW_ - effW_;
      if (align_ == Align::Center) ix = x + (availW_ - effW_) / 2;
    }
    iy = yTop - effH_;
  }
  ix += dx_;
  iy -= dy_;                                        // positive dy moves down
  std::ostringstream o;
  o << "q\n" << Writer::num(effW_) << " 0 0 " << Writer::num(effH_) << " "
    << Writer::num(ix) << " " << Writer::num(iy) << " cm\n/"
    << resource_ << " Do\nQ\n";
  if (backLayer_) w.contentBack(o.str());
  else            w.content(o.str());
}

// ---- StackFlow -------------------------------------------------------------------
StackFlow::StackFlow(FlowList kids, double gap) : kids_(std::move(kids)), gap_(gap) {}

double StackFlow::wrap(double width) {
  lastWidth_ = width;
  double h = 0;
  for (size_t i = 0; i < kids_.size(); ++i) {
    if (i) h += gap_;
    h += kids_[i]->wrap(width);
  }
  return h;
}

void StackFlow::draw(Writer& w, double x, double yTop) {
  double y = yTop;
  for (size_t i = 0; i < kids_.size(); ++i) {
    if (i) y -= gap_;
    double h = kids_[i]->wrap(lastWidth_);     // cached, returns child height
    kids_[i]->draw(w, x, y);
    y -= h;
  }
}

double StackFlow::naturalWidth() const {
  double n = 0;
  for (const auto& k : kids_) n = std::max(n, k->naturalWidth());
  return n;
}

FlowPtr StackFlow::cloneFlow() const {
  FlowList copies;
  for (const auto& k : kids_) {
    auto c = k->cloneFlow();
    if (!c) return nullptr;
    copies.push_back(std::move(c));
  }
  return std::make_unique<StackFlow>(std::move(copies), gap_);
}

static bool parseHex(const std::string& hex, double rgb[3]);

// ---- TableFlow -------------------------------------------------------------------
TableFlow::TableFlow(std::vector<Row> rows, int headerRows, TableOpts opts)
    : rows_(std::move(rows)), headerRows_(headerRows), opts_(std::move(opts)) {
  for (const auto& r : rows_) {
    size_t cols = 0;
    for (const auto& c : r) cols += std::max(1, c.span);
    nCols_ = std::max(nCols_, cols);
  }
  if (opts_.spacing >= 0) {
    style_.spaceBefore = opts_.spacing;
    style_.spaceAfter  = opts_.spacing;
  }
}

double TableFlow::tableWidth(double avail) const {
  double w = avail - opts_.indent;
  if (opts_.totalWidth > 0) w = std::min(w, opts_.totalWidth);
  return std::max(w, 1.0);
}

void TableFlow::computeColumnWidths(double width) {
  if (colWFrozen_) return;
  const double W = tableWidth(width);
  colW_.assign(nCols_, 0);

  if (!opts_.widths.empty()) {
    // explicit spec: fixed widths (points) consume first, the remainder is
    // distributed over the weighted columns; unspecified columns weigh 1
    double fixedSum = 0, weightSum = 0;
    for (size_t c = 0; c < nCols_; ++c) {
      if (c < opts_.widths.size() && opts_.widths[c].second)
        fixedSum += opts_.widths[c].first;
      else
        weightSum += (c < opts_.widths.size()) ? opts_.widths[c].first : 1.0;
    }
    double leftover = std::max(W - fixedSum, 0.0);
    for (size_t c = 0; c < nCols_; ++c) {
      if (c < opts_.widths.size() && opts_.widths[c].second)
        colW_[c] = opts_.widths[c].first;
      else {
        double wgt = (c < opts_.widths.size()) ? opts_.widths[c].first : 1.0;
        colW_[c] = weightSum > 0 ? leftover * wgt / weightSum : 0;
      }
    }
    return;
  }

  // no spec: proportional to each column's natural content width
  // (cells spanning several columns don't drive individual column widths)
  std::vector<double> desired(nCols_, 1.0);
  for (const auto& row : rows_) {
    size_t col = 0;
    for (const auto& cell : row) {
      int sp = std::max(1, cell.span);
      if (cell.flow && sp == 1 && col < nCols_)
        desired[col] = std::max(desired[col],
                                cell.flow->naturalWidth() + padL(col) + padR(col, sp) + 2);
      col += sp;
    }
  }
  double sum = 0;
  for (double d : desired) sum += d;
  for (size_t c = 0; c < nCols_; ++c)
    colW_[c] = W * desired[c] / sum;
  // keep very narrow columns readable
  const double minW = 1.5 * CM;
  if (W >= minW * nCols_) {
    double excess = 0;
    for (double& cw : colW_) if (cw < minW) { excess += minW - cw; cw = minW; }
    if (excess > 0) {
      double flexible = 0;
      for (double cw : colW_) if (cw > minW) flexible += cw - minW;
      if (flexible > 0)
        for (double& cw : colW_)
          if (cw > minW) cw -= excess * (cw - minW) / flexible;
    }
  }
}

void TableFlow::layoutRows() {
  rowH_.assign(rows_.size(), 0);
  for (size_t r = 0; r < rows_.size(); ++r) {
    double h = 0;
    size_t col = 0;
    for (const auto& cell : rows_[r]) {
      int sp = std::max(1, cell.span);
      double w = 0;
      for (int k = 0; k < sp && col + k < nCols_; ++k) w += colW_[col + k];
      if (cell.flow)
        h = std::max(h, cell.flow->wrap(std::max(w - padL(col) - padR(col, sp), 1.0)));
      col += sp;
    }
    rowH_[r] = h + 2 * opts_.padV;
  }
}

static double tfGapAfter(const TableOpts& o, int row) {
  for (auto& [r, g] : o.rowGapOverrides)
    if (r == row) return g;
  return o.rowGap;
}

double TableFlow::wrap(double width) {
  if (width != wrappedWidth_ || rowH_.size() != rows_.size()) {
    computeColumnWidths(width);
    layoutRows();
    wrappedWidth_ = width;
  }
  double h = 0;
  for (size_t r = 0; r < rowH_.size(); ++r) {
    if (r) h += tfGapAfter(opts_, (int)r - 1);
    h += rowH_[r];
  }
  return h;
}

void TableFlow::draw(Writer& w, double x, double yTop) {
  if (wrappedWidth_ < 0) wrap(w.pageWidth());
  x += opts_.indent;
  double tableW = 0;
  for (double cw : colW_) tableW += cw;
  double tableH = 0;
  for (size_t r = 0; r < rowH_.size(); ++r) {
    if (r) tableH += tfGapAfter(opts_, (int)r - 1);
    tableH += rowH_[r];
  }

  // cells
  double y = yTop;
  for (size_t r = 0; r < rows_.size(); ++r) {
    if (r) y -= tfGapAfter(opts_, (int)r - 1);
    double cx = x;
    size_t col = 0;
    for (const auto& cell : rows_[r]) {
      int sp = std::max(1, cell.span);
      if (cell.flow) cell.flow->draw(w, cx + padL(col), y - opts_.padV);
      for (int k = 0; k < sp && col < nCols_; ++k, ++col) cx += colW_[col];
    }
    y -= rowH_[r];
  }

  // borders: nothing by default; individual rules from separator rows,
  // frame=on for the outer box, grid=on for everything
  bool anyRule = !opts_.rulesAfterRow.empty();
  if (opts_.grid || opts_.frame || anyRule) {
    double rgb[3] = {0.3, 0.3, 0.3};
    parseHex(opts_.lineColor, rgb);
    std::ostringstream o;
    o << "q " << Writer::num(opts_.lineWidth) << " w "
      << Writer::num(rgb[0]) << " " << Writer::num(rgb[1]) << " "
      << Writer::num(rgb[2]) << " RG\n";
    auto hline = [&](double yy) {
      o << Writer::num(x) << " " << Writer::num(yy) << " m "
        << Writer::num(x + tableW) << " " << Writer::num(yy) << " l S\n";
    };
    if (opts_.grid) {
      double yy = yTop;
      for (size_t r = 0; r < rowH_.size(); ++r) {
        if (r) {                                     // gap: close both rows
          double g = tfGapAfter(opts_, (int)r - 1);
          if (g > 0) hline(yy - g);                  // top of the next row
          yy -= g;
        } else hline(yy);                            // table top
        yy -= rowH_[r];
        hline(yy);                                   // bottom of this row
      }
      // verticals per row at the actual cell boundaries, so cells spanning
      // several columns are not cut through
      yy = yTop;
      for (size_t r = 0; r < rows_.size(); ++r) {
        if (r) yy -= tfGapAfter(opts_, (int)r - 1);
        auto vseg = [&](double vx) {
          o << Writer::num(vx) << " " << Writer::num(yy) << " m "
            << Writer::num(vx) << " " << Writer::num(yy - rowH_[r]) << " l S\n";
        };
        vseg(x);
        double cx = x;
        size_t col = 0;
        for (const auto& cell : rows_[r]) {
          int sp = std::max(1, cell.span);
          for (int k = 0; k < sp && col < nCols_; ++k, ++col) cx += colW_[col];
          vseg(cx);
        }
        if (cx < x + tableW - 0.01) vseg(x + tableW);   // ragged short rows
        yy -= rowH_[r];
      }
    } else {
      if (opts_.frame)
        o << Writer::num(x) << " " << Writer::num(yTop - tableH) << " "
          << Writer::num(tableW) << " " << Writer::num(tableH) << " re S\n";
      for (int after : opts_.rulesAfterRow) {
        if (after < 0 || after >= (int)rowH_.size()) continue;
        double yy = yTop;
        for (int r = 0; r <= after; ++r) {
          if (r) yy -= tfGapAfter(opts_, r - 1);
          yy -= rowH_[r];
        }
        hline(yy);
      }
    }
    o << "Q\n";
    w.content(o.str());
  }
}

std::unique_ptr<Flowable> TableFlow::splitTop(double width, double availHeight) {
  wrap(width);
  // how many leading rows fit (incl. the gaps between them)?
  size_t fit = 0;
  double h = 0;
  while (fit < rows_.size()) {
    double add = rowH_[fit] + (fit ? tfGapAfter(opts_, (int)fit - 1) : 0.0);
    if (h + add > availHeight + 0.01) break;
    h += add;
    ++fit;
  }
  // a forced break ([row, pagebreak]) inside the fitting range caps the fit
  for (int b : opts_.breakAfterRow)
    if ((size_t)b + 1 <= fit && (size_t)b + 1 > (size_t)headerRows_)
      fit = std::min(fit, (size_t)b + 1);
  // never split inside a keep-group: back up to the group's first row
  for (auto& [g0, g1] : opts_.keepGroups)
    if (fit > (size_t)g0 && fit <= (size_t)g1)
      fit = (size_t)g0;
  // need at least the header plus one body row on this page, and at least
  // one body row must remain for the next page
  if (fit <= (size_t)headerRows_ || fit >= rows_.size()) return nullptr;
  return splitAtRow(fit);
}

std::unique_ptr<Flowable> TableFlow::splitTopForced(double width) {
  wrap(width);
  for (int b : opts_.breakAfterRow) {
    size_t fit = (size_t)b + 1;
    if (fit > (size_t)headerRows_ && fit < rows_.size())
      return splitAtRow(fit);
  }
  return nullptr;
}

std::unique_ptr<Flowable> TableFlow::splitAtRow(size_t fit) {

  std::vector<Row> topRows;
  for (size_t r = 0; r < fit; ++r) topRows.push_back(std::move(rows_[r]));

  // remainder = cloned header + remaining rows (header repeats on next page);
  // if some header cell isn't clonable, the remainder just goes without header
  int restHeaderRows = headerRows_;
  std::vector<Row> restRows;
  for (size_t r = 0; r < (size_t)headerRows_ && restHeaderRows; ++r) {
    Row hr;
    for (const auto& cell : topRows[r]) {
      Cell c;
      c.span = cell.span;
      c.flow = cell.flow ? cell.flow->cloneFlow() : nullptr;
      if (cell.flow && !c.flow) { restHeaderRows = 0; restRows.clear(); break; }
      hr.push_back(std::move(c));
    }
    if (restHeaderRows) restRows.push_back(std::move(hr));
  }
  for (size_t r = fit; r < rows_.size(); ++r) restRows.push_back(std::move(rows_[r]));

  // split the horizontal rules between the fragments: rules on rows that
  // stayed on this page go to `top`; rules under repeated header rows are
  // duplicated; the rest shifts by the split offset
  TableOpts topOpts = opts_, restOpts = opts_;
  topOpts.rulesAfterRow.clear();
  restOpts.rulesAfterRow.clear();
  for (int after : opts_.rulesAfterRow) {
    if (after < (int)fit) {
      topOpts.rulesAfterRow.push_back(after);
      if (after < restHeaderRows)                          // header underline
        restOpts.rulesAfterRow.push_back(after);
    } else {
      restOpts.rulesAfterRow.push_back(after - (int)fit + restHeaderRows);
    }
  }
  topOpts.rowGapOverrides.clear();
  restOpts.rowGapOverrides.clear();
  for (auto& [row, gap] : opts_.rowGapOverrides) {
    if (row < (int)fit) {
      topOpts.rowGapOverrides.push_back({row, gap});
      if (row < restHeaderRows) restOpts.rowGapOverrides.push_back({row, gap});
    } else {
      restOpts.rowGapOverrides.push_back({row - (int)fit + restHeaderRows, gap});
    }
  }
  topOpts.breakAfterRow.clear();
  restOpts.breakAfterRow.clear();
  for (int b : opts_.breakAfterRow) {
    if (b >= (int)fit)                                   // the one at fit-1 is
      restOpts.breakAfterRow.push_back(b - (int)fit + restHeaderRows);  // consumed
  }
  topOpts.keepGroups.clear();
  restOpts.keepGroups.clear();
  for (auto& [g0, g1] : opts_.keepGroups) {
    if (g1 < (int)fit) topOpts.keepGroups.push_back({g0, g1});
    else if (g0 >= (int)fit)
      restOpts.keepGroups.push_back({g0 - (int)fit + restHeaderRows,
                                     g1 - (int)fit + restHeaderRows});
    // straddling groups can't occur: splitTop backs up before the group
  }
  topOpts.spacing = -1;   // spacing handled once by the original position

  auto top = std::unique_ptr<TableFlow>(
      new TableFlow(std::move(topRows), headerRows_, topOpts));
  top->colW_ = colW_; top->colWFrozen_ = true;           // identical columns
  top->nCols_ = nCols_;

  headerRows_ = restHeaderRows;
  opts_ = restOpts;
  if (opts_.spacing >= 0) { style_.spaceBefore = 0; }    // no extra gap mid-table
  rows_ = std::move(restRows);
  colWFrozen_ = true;                                    // ...on both fragments
  wrappedWidth_ = -1;
  rowH_.clear();
  return top;
}

// ---- FloatFlow -------------------------------------------------------------------
FloatFlow::FloatFlow(std::unique_ptr<ImageFlow> img, std::unique_ptr<Paragraph> para,
                     bool floatRight, double gutter)
    : img_(std::move(img)), para_(std::move(para)), right_(floatRight), gutter_(gutter) {}

double FloatFlow::wrap(double width) {
  lastWidth_ = width;
  imgH_ = img_->wrap(width * 0.7);        // leave at least 30% for the text
  imgW_ = img_->boxWidth();
  double narrowW = std::max(width - imgW_ - gutter_, width * 0.25);
  double leading = para_->style()->leading;
  // number of narrowed lines = lines needed to clear the image (+ small gap)
  size_t narrowLines = (size_t)std::ceil((imgH_ + 0.15 * CM) / leading);

  std::vector<Paragraph::LineBox> boxes(narrowLines + 1);
  for (size_t i = 0; i < narrowLines; ++i)
    boxes[i] = {narrowW, right_ ? 0.0 : imgW_ + gutter_};
  boxes[narrowLines] = {width, 0.0};       // repeats for all further lines
  para_->setLineBoxes(std::move(boxes));

  double paraH = para_->wrap(width);
  height_ = std::max(imgH_, paraH);
  return height_;
}

void FloatFlow::draw(Writer& w, double x, double yTop) {
  double imgX = right_ ? x + lastWidth_ - imgW_ : x;
  img_->draw(w, imgX, yTop);
  para_->draw(w, x, yTop);
}

double FloatFlow::naturalWidth() const {
  return img_->naturalWidth() + gutter_ + para_->naturalWidth();
}

FlowPtr FloatFlow::cloneFlow() const {
  auto ic = img_->cloneFlow();
  auto pc = para_->clone();
  if (!ic || !pc) return nullptr;
  return std::make_unique<FloatFlow>(
      std::unique_ptr<ImageFlow>(static_cast<ImageFlow*>(ic.release())),
      std::move(pc), right_, gutter_);
}

// ---- Document ---------------------------------------------------------------------
Document::Document(Writer& w, double mL, double mR, double mT, double mB)
    : w_(w), mL_(mL), mR_(mR), mT_(mT), mB_(mB) {}

static bool parseHex(const std::string& hex, double rgb[3]) {
  if (hex.size() != 7 || hex[0] != '#') return false;
  auto nib = [](char c)->int {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
  };
  for (int i = 0; i < 3; ++i)
    rgb[i] = (nib(hex[1 + 2*i]) * 16 + nib(hex[2 + 2*i])) / 255.0;
  return true;
}

std::string Document::substitute(const std::string& markup) const {
  auto replaceAll = [](std::string s, const std::string& from, const std::string& to) {
    std::string low;
    for (char c : s) low += (char)std::tolower((unsigned char)c);
    std::string out;
    size_t pos = 0;
    for (;;) {
      size_t hit = low.find(from, pos);
      if (hit == std::string::npos) { out.append(s, pos, std::string::npos); break; }
      out.append(s, pos, hit - pos);
      out += to;
      pos = hit + from.size();
    }
    return out;
  };
  std::string pages = std::to_string(curPage_) + "/" +
      std::to_string(decor_.totalPages > 0 ? decor_.totalPages : curPage_);
  std::string s = replaceAll(markup, "[pages]", pages);
  return replaceAll(s, "[document]", decor_.docName);
}

double Document::decorHeight(const std::string& markup, double width) {
  if (markup.empty() || !decor_.build) return 0;
  double h = 0;
  for (auto& f : decor_.build(substitute(markup)))
    h += f->wrap(width);
  return h;
}

void Document::drawDecor(const std::string& markup, double x, double yTopEdge,
                         double width) {
  if (markup.empty() || !decor_.build) return;
  double y = yTopEdge;
  for (auto& f : decor_.build(substitute(markup))) {
    double h = f->wrap(width);
    f->draw(w_, x, y);
    y -= h;
  }
}

void Document::startPage() {
  w_.beginPage();
  ++curPage_;
  double rgb[3];
  if (parseHex(bg_, rgb)) {
    std::ostringstream o;
    o << "q " << Writer::num(rgb[0]) << " " << Writer::num(rgb[1]) << " "
      << Writer::num(rgb[2]) << " rg 0 0 " << Writer::num(w_.pageWidth())
      << " " << Writer::num(w_.pageHeight()) << " re f Q\n";
    w_.contentBack(o.str());
  }
  double width = textWidthAvail();
  if (!decor_.headerMarkup.empty())
    drawDecor(decor_.headerMarkup, mL_, w_.pageHeight() - mT_, width);
  if (!decor_.footerMarkup.empty()) {
    double fh = decorHeight(decor_.footerMarkup, width);
    drawDecor(decor_.footerMarkup, mL_, mB_ + fh, width);
  }
}

void Document::build() {
  curPage_ = 0;
  auto headerH = [&]() {
    return decor_.headerMarkup.empty() ? 0.0
        : decorHeight(decor_.headerMarkup, textWidthAvail());
  };
  auto footerH = [&]() {
    return decor_.footerMarkup.empty() ? 0.0
        : decorHeight(decor_.footerMarkup, textWidthAvail());
  };
  auto contentTop = [&]() { return w_.pageHeight() - mT_ - headerH(); };
  auto contentBottom = [&]() { return mB_ + footerH(); };

  double yTop = 0;
  bool pageEmpty = true;
  bool anythingDrawn = false;
  // Pages after a merged PDF (and the very first page) are created LAZILY:
  // only when real content arrives. This is what prevents stray pages that
  // carry nothing but header/footer/background — e.g. between two [pdf=...]
  // merges or after a final one.
  bool pendingPage = true;
  auto ensurePage = [&]() {
    if (!pendingPage) return;
    startPage();                       // advances curPage_ and draws decor
    yTop = contentTop();
    pageEmpty = true;
    pendingPage = false;
  };
  auto newPage = [&]() {
    startPage();
    yTop = contentTop();
    pendingPage = false;
  };

  for (auto& fp : flow_) {
    Flowable* f = fp.get();
    const double width = textWidthAvail();
    const double yBottom = contentBottom();

    if (auto* pb = dynamic_cast<PageBreakFlow*>(f)) {
      if (!anythingDrawn && pageEmpty) continue;   // no leading blank page
      if (pendingPage) {
        // after a merged PDF the next own page is already due: a [newpage]
        // here only materializes it instead of adding a second one (a
        // deliberate blank still needs two [newpage] tags, as usual)
        ensurePage();
        continue;
      }
      if (pb->noBlank() && pageEmpty) continue;    // [newpage, no_blank]
      newPage();
      pageEmpty = true;
      continue;
    }
    if (auto* mc = dynamic_cast<MarginsFlow*>(f)) {
      if (mc->l_ >= 0) mL_ = mc->l_;
      if (mc->r_ >= 0) mR_ = mc->r_;
      if (mc->t_ >= 0) mT_ = mc->t_;
      if (mc->b_ >= 0) mB_ = mc->b_;
      if (pageEmpty && !pendingPage) yTop = contentTop();
      continue;
    }

    if (const std::string* ext = f->externalPdfBytes()) {
      auto* epf = dynamic_cast<ExternalPdfFlow*>(f);
      // newpage_no_blank: swallow an explicitly created empty page (which
      // may already carry header/footer/background) before the merge
      if (epf && epf->noBlankBefore() && !pendingPage && pageEmpty) {
        w_.discardCurrentOwnPage();
        --curPage_;
        pendingPage = true;
      }
      std::string err;
      int extPages = 0;
      if (w_.appendExternalPdf(*ext, err, &extPages)) {
        curPage_ += extPages;
        pendingPage = true;            // next own page only if content follows
        pageEmpty = true;
      }
      continue;
    }
    // A spacer that would land at the top of the lazy page after a merged
    // PDF is dropped (gaps don't survive page starts caused by merges);
    // deliberate leading gaps at the very start of a document stay.
    bool isSpacer = dynamic_cast<Spacer*>(f) != nullptr;
    if (isSpacer && pendingPage && anythingDrawn) continue;
    ensurePage();

    const Style* st = f->style();
    double before = st ? st->spaceBefore : 0;
    double after  = st ? st->spaceAfter  : 0;

    if (!pageEmpty && before > 0) yTop -= before;

    double h = f->wrap(width);
    for (;;) {
      if (h > yTop - yBottom + 0.01) {               // doesn't fit
        bool drewPart = false;
        if (f->splittable()) {
          auto top = f->splitTop(width, yTop - yBottom);
          if (top) { top->draw(w_, mL_, yTop); drewPart = true; anythingDrawn = true; }
        }
        if (!drewPart && pageEmpty) break;           // oversized atom: draw anyway
        newPage();
        pageEmpty = true;
        h = f->wrap(width);                          // remainder (or full) height
        continue;
      }
      // fits entirely — but the content may still demand a break inside
      // itself ([row, pagebreak])
      auto top = f->splitTopForced(width);
      if (!top) break;
      top->draw(w_, mL_, yTop);
      anythingDrawn = true;
      newPage();
      pageEmpty = true;
      h = f->wrap(width);
    }
    f->draw(w_, mL_, yTop);
    yTop -= h + after;
    if (!isSpacer) {                   // spacers don't make a page non-empty
      pageEmpty = false;
      anythingDrawn = true;
    }
  }
  if (curPage_ == 0) ensurePage();     // completely empty document: one page
}

} // namespace pdf
