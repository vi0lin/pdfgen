// ttffont.h — minimal TrueType parsing + a registry for embedding a Unicode
// fallback font (DejaVu Sans) as a PDF Type0/CIDFontType2 with Identity-H
// encoding. Needed because the built-in Helvetica fonts only cover WinAnsi
// (CP1252): Cyrillic (Russian) and most Polish letters (ą ć ę ł ń ś ź ż)
// would otherwise render as '?'.
//
// Text runs that contain such characters are automatically shaped with this
// font; everything WinAnsi-representable keeps using the built-in fonts.
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_TTFFONT_API 1
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pdf {

// One parsed TrueType face.
struct TTFace {
  std::string raw;                      // whole file (embedded verbatim)
  std::string psName = "Embedded";      // PostScript-ish name for /BaseFont
  int unitsPerEm = 1000;
  int numGlyphs = 0;
  int ascent = 800, descent = -200;     // font units
  int bbox[4] = {0, 0, 1000, 1000};     // xMin yMin xMax yMax

  bool load(const std::string& path, std::string& err);
  uint16_t gidFor(uint32_t codepoint) const;
  int advance(uint16_t gid) const;      // font units

  // registry of glyphs actually used (gid -> one representative codepoint),
  // filled by UnicodeFonts::shape/measure; drives /W and /ToUnicode
  mutable std::map<uint16_t, uint32_t> used;

private:
  std::vector<uint16_t> advances_;      // per gid
  // cmap format 4 segments
  std::vector<uint32_t> seg4_;          // packed: not used; we keep parsed arrays
  std::vector<uint16_t> end4_, start4_, rangeOff4_;
  std::vector<int16_t>  delta4_;
  size_t cmap4Base_ = 0;                // offset of idRangeOffset array in raw
  // cmap format 12 groups: {start, end, startGid}
  struct Grp { uint32_t start, end, gid; };
  std::vector<Grp> groups12_;
};

// Singleton holding the regular + bold face. Loads lazily from the first
// existing candidate: $PDFGEN_FONT / $PDFGEN_FONT_BOLD, ./fonts/, then the
// usual DejaVu / Liberation system locations.
class UnicodeFonts {
public:
  static UnicodeFonts& inst();

  bool available();                                   // tries to load once
  const std::string& loadError() const { return loadError_; }

  // width of UTF-8 text at `size` points (also records used glyphs)
  double measure(const std::string& utf8, bool bold, double size);
  // 2-byte big-endian GID string as PDF hex (no <>), records used glyphs
  std::string hexString(const std::string& utf8, bool bold);

  bool faceUsed(bool bold) const;
  const TTFace* face(bool bold) const;
  // content-stream resource name matching pick(): "FU1" or "FU2"
  const char* resourceName(bool bold) const { return (bold && haveBold_) ? "FU2" : "FU1"; }

private:
  UnicodeFonts() = default;
  bool tried_ = false, ok_ = false;
  TTFace reg_, bold_;
  bool haveBold_ = false;
  std::string loadError_;
  const TTFace& pick(bool bold) const { return (bold && haveBold_) ? bold_ : reg_; }
};

// Decodes one UTF-8 code point at s[i]; advances i. Invalid bytes yield '?'.
uint32_t decodeUtf8(const std::string& s, size_t& i);
// CP1252 byte for a code point, or -1 if not representable.
int cpToWinAnsi(uint32_t cp);

} // namespace pdf
