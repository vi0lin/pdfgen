#include "metrics.h"
#include <cstring>

// ---- release consistency check (see header comment) ----
#ifndef PDFGEN_METRICS_API
#error "stale metrics.h: it lacks PDFGEN_METRICS_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_METRICS_API != 1
#error "version mismatch in metrics.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_PDFWRITER_API
#error "stale pdfwriter.h: it lacks PDFGEN_PDFWRITER_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_PDFWRITER_API != 6
#error "version mismatch in pdfwriter.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif


namespace pdf {

// Adobe AFM widths for ASCII 32..126.
// Oblique variants share widths with their upright counterparts.
static const int kHelv[95] = {
  278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278, // 32..47
  556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556, // 48..63
 1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778, // 64..79
  667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556, // 80..95
  333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556, // 96..111
  556,556,333,500,278,556,500,722,500,500,500,334,260,334,584      //112..126
};
static const int kHelvBold[95] = {
  278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,
  556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,
  975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,
  667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,
  333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,
  611,611,389,556,333,611,556,778,556,556,500,389,280,389,584
};

// For accented Latin-1 letters the advance width equals the base letter's
// width in Helvetica, so map 0xC0..0xFF to base ASCII letters.
static char baseLetter(unsigned char code) {
  static const char* map =
    //  C0        C8        D0        D8        E0        E8        F0        F8
    "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  if (code >= 0xC0) return map[code - 0xC0];
  return 0;
}

int glyphWidth(Font f, unsigned char code) {
  bool bold = (f == Font::HelveticaBold || f == Font::HelveticaBoldOblique);
  const int* tab = bold ? kHelvBold : kHelv;

  if (code >= 32 && code <= 126) return tab[code - 32];

  if (char b = baseLetter(code)) {
    if (b == '*' || b == '/') return 584;                 // × ÷
    return tab[(unsigned char)b - 32];
  }

  switch (code) {                       // common CP1252 / Latin-1 punctuation
    case 0x80: return 556;              // €
    case 0x82: case 0x91: case 0x92: return bold ? 278 : 222;   // ‚ ‘ ’
    case 0x84: case 0x93: case 0x94: return bold ? 500 : 333;   // „ “ ”
    case 0x85: return 1000;             // …
    case 0x95: return 350;              // •
    case 0x96: return 556;              // –
    case 0x97: return 1000;             // —
    case 0xA0: return 278;              // nbsp
    case 0xA7: return 556;              // §
    case 0xAB: case 0xBB: return 556;   // « »
    case 0xB0: return 400;              // °
    case 0xB7: return 278;              // ·
    case 0xDF: return 611;              // ß
    default:   return 556;              // safe fallback
  }
}

double textWidth(Font f, const std::string& s, double fontSize) {
  long w = 0;
  for (unsigned char c : s) w += glyphWidth(f, c);
  return w * fontSize / 1000.0;
}

} // namespace pdf
