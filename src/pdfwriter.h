// pdfwriter.h — minimal low-level PDF 1.4 writer (no external deps except zlib
// for image recompression, see image.h). Produces uncompressed content streams
// so the output stays human-readable / debuggable.
//
// Responsibilities of this layer ONLY:
//   * PDF object bookkeeping (numbers, offsets, xref, trailer)
//   * pages, page content streams
//   * the 4 built-in Helvetica fonts (WinAnsi encoded)
//   * image XObjects (raw stream data prepared by image.cpp)
//
// All layout logic (line breaking, justification, page breaking) lives in
// flowables.h — this file knows nothing about paragraphs.
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_PDFWRITER_API 7
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pdf {

// ---- units -----------------------------------------------------------------
constexpr double PT   = 1.0;
constexpr double INCH = 72.0;
constexpr double CM   = 72.0 / 2.54;
constexpr double MM   = CM / 10.0;
constexpr double PICA = 12.0;

constexpr double A4_W = 595.2756;   // 210mm
constexpr double A4_H = 841.8898;   // 297mm

// ---- fonts ------------------------------------------------------------------
enum class Font : uint8_t {
  Helvetica = 0,
  HelveticaBold,
  HelveticaOblique,
  HelveticaBoldOblique,
  Courier,                      // monospaced (code spans); F5, not embedded
};
const char* fontBaseName(Font f);       // e.g. "Helvetica-Bold"
const char* fontResourceName(Font f);   // e.g. "F1" (name used in content streams)

// ---- images -----------------------------------------------------------------
// Prepared by image.cpp; this struct is what the writer embeds verbatim.
struct ImageXObject {
  int         widthPx  = 0;
  int         heightPx = 0;
  std::string colorSpace;          // "DeviceRGB" or "DeviceGray"
  int         bitsPerComponent = 8;
  std::string filter;              // "DCTDecode" or "FlateDecode"
  std::string data;                // stream bytes (already encoded)
  std::string smaskData;           // optional 8-bit gray alpha, FlateDecode ("" = none)
};

class ImportedPdf;   // pdfimport.h

// ---- writer -----------------------------------------------------------------
class Writer {
public:
  explicit Writer(double pageW = A4_W, double pageH = A4_H);
  ~Writer();

  // Registers an image, returns the resource name ("Im1", "Im2", ...) to use
  // with the `Do` operator in content streams.
  std::string addImage(const ImageXObject& img);

  // Starts a fresh page; subsequent content() calls append to it.
  void beginPage();
  // Appends raw content-stream operators to the current page.
  void content(const std::string& ops);
  // Appends to the page's BACK layer: painted before (i.e. underneath) all
  // regular content of that page. Used for page backgrounds and images with
  // layer=back.
  void contentBack(const std::string& ops);

  // Inserts all pages of an existing PDF at the current position: the
  // current page ends here, the foreign pages follow, and the next
  // beginPage()/content() continues after them. An empty current page is
  // dropped. Returns false (with `error`) for unparseable/encrypted input.
  bool appendExternalPdf(std::string pdfBytes, std::string& error,
                         int* pagesOut = nullptr);
  // Drops the current own page entirely (incl. header/footer/background
  // already painted on it). Used by the document when a page that only
  // carries decor must not survive before a merged PDF.
  void discardCurrentOwnPage();
  // Clickable link rectangle on the CURRENT page (PDF user-space coords).
  void addLink(double x, double y, double w, double h, const std::string& url);

  // Writes the whole document. Returns false on I/O error.
  bool save(const std::string& path);
  // Same, but into a byte string (used for [pdfgen=..., include]).
  bool saveToString(std::string& out);

  double pageWidth()  const { return pageW_; }
  double pageHeight() const { return pageH_; }

  // Escapes ()\ in a WinAnsi byte string for use inside PDF ( ) literals.
  static std::string escapeString(const std::string& winAnsiBytes);
  // Formats a number with sane precision for content streams.
  static std::string num(double v);

private:
  double pageW_, pageH_;
  // page order: own content streams interleaved with imported pages
  struct Link { double x, y, w, h; std::string url; };
  struct Slot { int extDoc = -1; int extPage = -1;
                std::string back; std::string content;
                std::vector<Link> links; };
  std::vector<Slot> slots_;
  std::vector<std::unique_ptr<ImportedPdf>> externalDocs_;
  std::vector<ImageXObject> images_;

  struct Obj { std::string body; size_t offset = 0; };  // body excludes "N 0 obj"
  int addObject(std::vector<Obj>& objs, std::string body);
  bool buildDocument(std::string& result);
};

// ---- UTF-8 → WinAnsi (CP1252) -----------------------------------------------
// Converts UTF-8 input to the single-byte encoding used by the base-14 fonts.
// Unmappable code points become '?'.
std::string utf8ToWinAnsi(const std::string& utf8);

} // namespace pdf
