#include "ttffont.h"
#include <cstdlib>
#include <cstring>
#include <fstream>

// ---- release consistency check (see header comment) ----
#ifndef PDFGEN_TTFFONT_API
#error "stale ttffont.h: it lacks PDFGEN_TTFFONT_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_TTFFONT_API != 1
#error "version mismatch in ttffont.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif


namespace pdf {
namespace {

uint16_t be16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
int16_t sbe16(const unsigned char* p) { return (int16_t)be16(p); }

} // namespace

// ---- TTFace ------------------------------------------------------------------
bool TTFace::load(const std::string& path, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { err = "cannot open font: " + path; return false; }
  raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  const unsigned char* d = (const unsigned char*)raw.data();
  if (raw.size() < 12) { err = "truncated font file"; return false; }

  uint32_t ver = be32(d);
  if (ver != 0x00010000 && ver != 0x74727565 /*'true'*/) {
    err = "not a TrueType font (OpenType/CFF is unsupported): " + path;
    return false;
  }
  uint16_t numTables = be16(d + 4);
  size_t headOff = 0, hheaOff = 0, maxpOff = 0, hmtxOff = 0, cmapOff = 0, nameOff = 0;
  for (uint16_t i = 0; i < numTables; ++i) {
    const unsigned char* rec = d + 12 + 16 * i;
    if (12 + 16 * (size_t)(i + 1) > raw.size()) break;
    std::string tag((const char*)rec, 4);
    uint32_t off = be32(rec + 8);
    if      (tag == "head") headOff = off;
    else if (tag == "hhea") hheaOff = off;
    else if (tag == "maxp") maxpOff = off;
    else if (tag == "hmtx") hmtxOff = off;
    else if (tag == "cmap") cmapOff = off;
    else if (tag == "name") nameOff = off;
  }
  if (!headOff || !hheaOff || !maxpOff || !hmtxOff || !cmapOff) {
    err = "font misses required tables"; return false;
  }

  unitsPerEm = be16(d + headOff + 18);
  bbox[0] = sbe16(d + headOff + 36); bbox[1] = sbe16(d + headOff + 38);
  bbox[2] = sbe16(d + headOff + 40); bbox[3] = sbe16(d + headOff + 42);
  ascent  = sbe16(d + hheaOff + 4);
  descent = sbe16(d + hheaOff + 6);
  uint16_t numHMetrics = be16(d + hheaOff + 34);
  numGlyphs = be16(d + maxpOff + 4);

  advances_.assign(numGlyphs, 0);
  uint16_t last = 0;
  for (int g = 0; g < numGlyphs; ++g) {
    if (g < numHMetrics) last = be16(d + hmtxOff + 4 * g);
    advances_[g] = last;
  }

  // PostScript name (name table, nameID 6) — ASCII best effort
  if (nameOff) {
    uint16_t count = be16(d + nameOff + 2);
    uint16_t strOff = be16(d + nameOff + 4);
    for (uint16_t i = 0; i < count; ++i) {
      const unsigned char* r = d + nameOff + 6 + 12 * i;
      if (be16(r + 6) != 6) continue;                       // nameID
      uint16_t len = be16(r + 8), off = be16(r + 10);
      const unsigned char* sp = d + nameOff + strOff + off;
      std::string nm;
      for (uint16_t k = 0; k < len; ++k)
        if (sp[k] >= 33 && sp[k] < 127) nm += (char)sp[k];  // skips UTF-16 zeros
      if (!nm.empty()) { psName = nm; break; }
    }
  }

  // cmap: prefer (3,10) format 12, then (3,1)/(0,*) format 4
  uint16_t nSub = be16(d + cmapOff + 2);
  size_t best4 = 0, best12 = 0;
  for (uint16_t i = 0; i < nSub; ++i) {
    const unsigned char* r = d + cmapOff + 4 + 8 * i;
    uint16_t plat = be16(r), enc = be16(r + 2);
    uint32_t off = be32(r + 4);
    uint16_t fmt = be16(d + cmapOff + off);
    if (fmt == 12 && plat == 3 && enc == 10) best12 = cmapOff + off;
    if (fmt == 4 && ((plat == 3 && enc == 1) || plat == 0) && !best4)
      best4 = cmapOff + off;
  }
  if (best12) {
    uint32_t n = be32(d + best12 + 12);
    for (uint32_t g = 0; g < n; ++g) {
      const unsigned char* r = d + best12 + 16 + 12 * g;
      groups12_.push_back({be32(r), be32(r + 4), be32(r + 8)});
    }
  } else if (best4) {
    uint16_t segX2 = be16(d + best4 + 6);
    uint16_t segs = segX2 / 2;
    size_t endOff = best4 + 14;
    size_t startOff = endOff + segX2 + 2;
    size_t deltaOff = startOff + segX2;
    size_t roOff = deltaOff + segX2;
    end4_.resize(segs); start4_.resize(segs); delta4_.resize(segs); rangeOff4_.resize(segs);
    for (uint16_t sIdx = 0; sIdx < segs; ++sIdx) {
      end4_[sIdx]      = be16(d + endOff + 2 * sIdx);
      start4_[sIdx]    = be16(d + startOff + 2 * sIdx);
      delta4_[sIdx]    = sbe16(d + deltaOff + 2 * sIdx);
      rangeOff4_[sIdx] = be16(d + roOff + 2 * sIdx);
    }
    cmap4Base_ = roOff;
  } else {
    err = "no usable cmap subtable"; return false;
  }
  return true;
}

uint16_t TTFace::gidFor(uint32_t cp) const {
  if (!groups12_.empty()) {
    size_t lo = 0, hi = groups12_.size();
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (cp < groups12_[mid].start) hi = mid;
      else if (cp > groups12_[mid].end) lo = mid + 1;
      else return (uint16_t)(groups12_[mid].gid + (cp - groups12_[mid].start));
    }
    return 0;
  }
  if (cp > 0xFFFF || end4_.empty()) return 0;
  size_t sIdx = 0;
  while (sIdx < end4_.size() && end4_[sIdx] < cp) ++sIdx;
  if (sIdx >= end4_.size() || start4_[sIdx] > cp) return 0;
  if (rangeOff4_[sIdx] == 0)
    return (uint16_t)((cp + delta4_[sIdx]) & 0xFFFF);
  size_t addr = cmap4Base_ + 2 * sIdx + rangeOff4_[sIdx] + 2 * (cp - start4_[sIdx]);
  if (addr + 1 >= raw.size()) return 0;
  uint16_t g = be16((const unsigned char*)raw.data() + addr);
  return g ? (uint16_t)((g + delta4_[sIdx]) & 0xFFFF) : 0;
}

int TTFace::advance(uint16_t gid) const {
  return gid < advances_.size() ? advances_[gid] : unitsPerEm / 2;
}

// ---- UnicodeFonts -------------------------------------------------------------
UnicodeFonts& UnicodeFonts::inst() {
  static UnicodeFonts u;
  return u;
}

bool UnicodeFonts::available() {
  if (tried_) return ok_;
  tried_ = true;

  auto tryLoad = [](TTFace& f, const std::vector<std::string>& paths) {
    std::string err;
    for (const auto& p : paths)
      if (!p.empty() && f.load(p, err)) return true;
    return false;
  };
  const char* envR = std::getenv("PDFGEN_FONT");
  const char* envB = std::getenv("PDFGEN_FONT_BOLD");
  ok_ = tryLoad(reg_, {
      envR ? envR : "",
      "fonts/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "C:/Windows/Fonts/arial.ttf",
  });
  if (!ok_) {
    loadError_ = "no Unicode font found -- non-Latin text renders as '?'. "
                 "Install DejaVu Sans or set PDFGEN_FONT=/path/to/font.ttf";
    return false;
  }
  haveBold_ = tryLoad(bold_, {
      envB ? envB : "",
      "fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "C:/Windows/Fonts/arialbd.ttf",
  });
  return true;
}

double UnicodeFonts::measure(const std::string& utf8, bool bold, double size) {
  if (!available()) return 0;
  const TTFace& f = pick(bold);
  long units = 0;
  size_t i = 0;
  while (i < utf8.size()) {
    uint32_t cp = decodeUtf8(utf8, i);
    uint16_t g = f.gidFor(cp);
    f.used.emplace(g, cp);
    units += f.advance(g);
  }
  return (double)units * size / f.unitsPerEm;
}

std::string UnicodeFonts::hexString(const std::string& utf8, bool bold) {
  if (!available()) return "";
  const TTFace& f = pick(bold);
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  size_t i = 0;
  while (i < utf8.size()) {
    uint32_t cp = decodeUtf8(utf8, i);
    uint16_t g = f.gidFor(cp);
    f.used.emplace(g, cp);
    out += hex[(g >> 12) & 0xF]; out += hex[(g >> 8) & 0xF];
    out += hex[(g >> 4) & 0xF];  out += hex[g & 0xF];
  }
  return out;
}

bool UnicodeFonts::faceUsed(bool bold) const {
  const TTFace& f = (bold && haveBold_) ? bold_ : reg_;
  if (bold && !haveBold_) return false;      // reported under regular instead
  return ok_ && !f.used.empty();
}

const TTFace* UnicodeFonts::face(bool bold) const {
  if (!ok_) return nullptr;
  return (bold && haveBold_) ? &bold_ : &reg_;
}

// ---- shared UTF-8 / CP1252 helpers --------------------------------------------
uint32_t decodeUtf8(const std::string& s, size_t& i) {
  unsigned char c = s[i];
  uint32_t cp; int len;
  size_t n = s.size();
  if      (c < 0x80)                    { cp = c; len = 1; }
  else if ((c >> 5) == 0x6  && i+1 < n) { cp = ((c & 0x1F) << 6)  | (s[i+1] & 0x3F); len = 2; }
  else if ((c >> 4) == 0xE  && i+2 < n) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3; }
  else if ((c >> 3) == 0x1E && i+3 < n) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4; }
  else                                  { cp = '?'; len = 1; }
  i += len;
  return cp;
}

int cpToWinAnsi(uint32_t cp) {
  if (cp < 0x80) return (int)cp;
  if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;
  switch (cp) {
    case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83;
    case 0x201E: return 0x84; case 0x2026: return 0x85; case 0x2020: return 0x86;
    case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89;
    case 0x0160: return 0x8A; case 0x2039: return 0x8B; case 0x0152: return 0x8C;
    case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
    case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95;
    case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x02DC: return 0x98;
    case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B;
    case 0x0153: return 0x9C; case 0x017E: return 0x9E; case 0x0178: return 0x9F;
    default: return -1;
  }
}

} // namespace pdf
