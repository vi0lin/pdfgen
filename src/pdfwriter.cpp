#include "pdfwriter.h"
#include <algorithm>   // std::count/min/max (MSVC does not pull this in transitively)
#include "pdfimport.h"
#include "ttffont.h"
#include <zlib.h>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "pdfgen_host.h"   // writeFile, Log

// ---- release consistency check (see header comment) ----
#ifndef PDFGEN_PDFWRITER_API
#error "stale pdfwriter.h: it lacks PDFGEN_PDFWRITER_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_PDFWRITER_API != 6
#error "version mismatch in pdfwriter.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_TTFFONT_API
#error "stale ttffont.h: it lacks PDFGEN_TTFFONT_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_TTFFONT_API != 1
#error "version mismatch in ttffont.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif


namespace pdf {

const char* fontBaseName(Font f) {
  switch (f) {
    case Font::Helvetica:            return "Helvetica";
    case Font::HelveticaBold:        return "Helvetica-Bold";
    case Font::HelveticaOblique:     return "Helvetica-Oblique";
    case Font::HelveticaBoldOblique: return "Helvetica-BoldOblique";
  }
  return "Helvetica";
}
const char* fontResourceName(Font f) {
  switch (f) {
    case Font::Helvetica:            return "F1";
    case Font::HelveticaBold:        return "F2";
    case Font::HelveticaOblique:     return "F3";
    case Font::HelveticaBoldOblique: return "F4";
  }
  return "F1";
}

Writer::Writer(double pageW, double pageH) : pageW_(pageW), pageH_(pageH) {}
Writer::~Writer() = default;

std::string Writer::addImage(const ImageXObject& img) {
  images_.push_back(img);
  return "Im" + std::to_string(images_.size());
}

void Writer::beginPage() { slots_.emplace_back(); }

void Writer::content(const std::string& ops) {
  if (slots_.empty() || slots_.back().extDoc >= 0) beginPage();
  slots_.back().content += ops;
}

void Writer::contentBack(const std::string& ops) {
  if (slots_.empty() || slots_.back().extDoc >= 0) beginPage();
  slots_.back().back += ops;
}

void Writer::discardCurrentOwnPage() {
  if (!slots_.empty() && slots_.back().extDoc < 0) slots_.pop_back();
}

bool Writer::appendExternalPdf(std::string pdfBytes, std::string& error,
                               int* pagesOut) {
  auto doc = std::make_unique<ImportedPdf>();
  if (!doc->load(std::move(pdfBytes), error)) return false;
  if (doc->pageCount() == 0) { error = "PDF has no pages"; return false; }
  // an untouched current page would otherwise become a stray blank page
  if (!slots_.empty() && slots_.back().extDoc < 0 && slots_.back().content.empty())
    slots_.pop_back();
  int idx = (int)externalDocs_.size();
  externalDocs_.push_back(std::move(doc));
  for (int pg = 0; pg < externalDocs_[idx]->pageCount(); ++pg) {
    Slot sl; sl.extDoc = idx; sl.extPage = pg;
    slots_.push_back(std::move(sl));
  }
  if (pagesOut) *pagesOut = externalDocs_[idx]->pageCount();
  return true;
}

std::string Writer::escapeString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '(' || c == ')' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += (char)c;
  }
  return out;
}

std::string Writer::num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  // trim trailing zeros / dot
  std::string s(buf);
  size_t dot = s.find('.');
  if (dot != std::string::npos) {
    size_t last = s.find_last_not_of('0');
    if (s[last] == '.') last--;
    s.erase(last + 1);
  }
  return s;
}

int Writer::addObject(std::vector<Obj>& objs, std::string body) {
  objs.push_back({std::move(body), 0});
  return (int)objs.size();          // object numbers are 1-based
}

bool Writer::buildDocument(std::string& result) {
  // Object layout:
  //   1            Catalog
  //   2            Pages
  //   3..6         Fonts F1..F4
  //   then per image: XObject (+ optional SMask object)
  //   then per page: Page dict + content stream
  std::vector<Obj> objs;

  addObject(objs, "<< /Type /Catalog /Pages 2 0 R >>");  // obj 1, /Kids patched below
  int pagesObjIdx = addObject(objs, "");                 // obj 2, filled last

  for (int i = 0; i < 4; ++i) {
    std::ostringstream f;
    f << "<< /Type /Font /Subtype /Type1 /BaseFont /" << fontBaseName((Font)i)
      << " /Encoding /WinAnsiEncoding >>";
    addObject(objs, f.str());
  }

  // embedded Unicode fonts (Type0 / CIDFontType2, Identity-H), only if used
  auto zdeflate = [](const std::string& rawData) {
    uLongf cap = compressBound(rawData.size());
    std::string out(cap, '\0');
    compress2((Bytef*)&out[0], &cap, (const Bytef*)rawData.data(), rawData.size(), 6);
    out.resize(cap);
    return out;
  };
  auto emitUnicodeFont = [&](const TTFace& f) -> int {
    double k = 1000.0 / f.unitsPerEm;
    std::string name;
    for (char c : f.psName)
      if (std::isalnum((unsigned char)c) || c == '-') name += c;
    if (name.empty()) name = "EmbeddedFont";

    std::string comp = zdeflate(f.raw);
    std::ostringstream ff;
    ff << "<< /Filter /FlateDecode /Length " << comp.size()
       << " /Length1 " << f.raw.size() << " >>\nstream\n" << comp << "\nendstream";
    int fileObj = addObject(objs, ff.str());

    std::ostringstream fd;
    fd << "<< /Type /FontDescriptor /FontName /" << name
       << " /Flags 32 /FontBBox [" << (int)(f.bbox[0]*k) << " " << (int)(f.bbox[1]*k)
       << " " << (int)(f.bbox[2]*k) << " " << (int)(f.bbox[3]*k) << "]"
       << " /ItalicAngle 0 /Ascent " << (int)(f.ascent*k)
       << " /Descent " << (int)(f.descent*k)
       << " /CapHeight " << (int)(f.ascent*k)
       << " /StemV 80 /FontFile2 " << fileObj << " 0 R >>";
    int descObj = addObject(objs, fd.str());

    // /W array: widths (in 1000-units) for the glyphs actually used,
    // grouped into runs of consecutive GIDs
    std::ostringstream wArr;
    wArr << "[";
    int runStart = -1, prev = -2;
    std::string runWidths;
    auto flushRun = [&]() {
      if (runStart >= 0) wArr << " " << runStart << " [" << runWidths << "]";
      runWidths.clear();
      runStart = -1;
    };
    for (const auto& [gid, cp] : f.used) {
      (void)cp;
      if ((int)gid != prev + 1) { flushRun(); runStart = gid; }
      runWidths += std::to_string((int)(f.advance(gid) * k)) + " ";
      prev = gid;
    }
    flushRun();
    wArr << " ]";

    std::ostringstream cid;
    cid << "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" << name
        << " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity)"
           " /Supplement 0 >> /FontDescriptor " << descObj
        << " 0 R /DW " << (int)(f.unitsPerEm * k / 2)
        << " /W " << wArr.str() << " /CIDToGIDMap /Identity >>";
    int cidObj = addObject(objs, cid.str());

    // ToUnicode CMap so text extraction / copy-paste works
    std::ostringstream tu;
    tu << "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
          "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
          "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
          "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<uint16_t, uint32_t>> pairs(f.used.begin(), f.used.end());
    for (size_t i = 0; i < pairs.size(); i += 100) {
      size_t nBlock = std::min<size_t>(100, pairs.size() - i);
      tu << nBlock << " beginbfchar\n";
      for (size_t j = i; j < i + nBlock; ++j) {
        char b[32];
        std::snprintf(b, sizeof b, "<%04X> ", pairs[j].first);
        tu << b;
        uint32_t cp = pairs[j].second;
        if (cp <= 0xFFFF) { std::snprintf(b, sizeof b, "<%04X>\n", cp); tu << b; }
        else {
          cp -= 0x10000;
          std::snprintf(b, sizeof b, "<%04X%04X>\n",
                        0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
          tu << b;
        }
      }
      tu << "endbfchar\n";
    }
    tu << "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";
    std::string tuStr = tu.str();
    std::ostringstream tuObj;
    tuObj << "<< /Length " << tuStr.size() << " >>\nstream\n" << tuStr << "\nendstream";
    int toUniObj = addObject(objs, tuObj.str());

    std::ostringstream t0;
    t0 << "<< /Type /Font /Subtype /Type0 /BaseFont /" << name
       << " /Encoding /Identity-H /DescendantFonts [" << cidObj
       << " 0 R] /ToUnicode " << toUniObj << " 0 R >>";
    return addObject(objs, t0.str());
  };

  auto& uf = UnicodeFonts::inst();
  int fu1Obj = 0, fu2Obj = 0;
  if (uf.faceUsed(false)) fu1Obj = emitUnicodeFont(*uf.face(false));
  if (uf.faceUsed(true))  fu2Obj = emitUnicodeFont(*uf.face(true));

  // images (record their object numbers for the shared resource dict)
  std::vector<int> imageObjNums;
  for (const auto& im : images_) {
    int smaskNum = 0;
    if (!im.smaskData.empty()) {
      std::ostringstream s;
      s << "<< /Type /XObject /Subtype /Image /Width " << im.widthPx
        << " /Height " << im.heightPx
        << " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode"
        << " /Length " << im.smaskData.size() << " >>\nstream\n"
        << im.smaskData << "\nendstream";
      smaskNum = addObject(objs, s.str());
    }
    std::ostringstream s;
    s << "<< /Type /XObject /Subtype /Image /Width " << im.widthPx
      << " /Height " << im.heightPx
      << " /ColorSpace /" << im.colorSpace
      << " /BitsPerComponent " << im.bitsPerComponent
      << " /Filter /" << im.filter;
    if (smaskNum) s << " /SMask " << smaskNum << " 0 R";
    s << " /Length " << im.data.size() << " >>\nstream\n" << im.data << "\nendstream";
    imageObjNums.push_back(addObject(objs, s.str()));
  }

  // shared /Resources dictionary text
  std::ostringstream res;
  res << "/Resources << /Font << ";
  for (int i = 0; i < 4; ++i)
    res << "/" << fontResourceName((Font)i) << " " << (3 + i) << " 0 R ";
  if (fu1Obj) res << "/FU1 " << fu1Obj << " 0 R ";
  if (fu2Obj) res << "/FU2 " << fu2Obj << " 0 R ";
  res << ">> ";
  if (!imageObjNums.empty()) {
    res << "/XObject << ";
    for (size_t i = 0; i < imageObjNums.size(); ++i)
      res << "/Im" << (i + 1) << " " << imageObjNums[i] << " 0 R ";
    res << ">> ";
  }
  res << ">>";

  // pages (own content pages interleaved with imported ones, in order)
  std::vector<int> pageObjNums;
  auto reserve = [&objs]() {
    objs.push_back({std::string(), 0});
    return (int)objs.size();
  };
  auto fill = [&objs](int n, std::string body) {
    objs[n - 1].body = std::move(body);
  };
  // deliberately empty pages (double [newpage]) stay; only TRAILING empty
  // own pages are dropped (they stem from the automatic fresh page after an
  // external-PDF insert when nothing follows)
  size_t lastUsed = 0;
  for (size_t si = 0; si < slots_.size(); ++si)
    if (slots_[si].extDoc >= 0 || !slots_[si].content.empty() ||
        !slots_[si].back.empty())
      lastUsed = si;
  for (size_t si = 0; si < slots_.size(); ++si) {
    const Slot& sl = slots_[si];
    if (sl.extDoc >= 0) {
      std::string cerr;
      int pn = externalDocs_[sl.extDoc]->copyPage(sl.extPage, reserve, fill, 2, cerr);
      if (pn) pageObjNums.push_back(pn);
      continue;
    }
    if (sl.content.empty() && sl.back.empty() && si > lastUsed &&
        slots_.size() > 1)
      continue;
    std::string stream = sl.back + sl.content;   // back layer paints first
    std::ostringstream cs;
    cs << "<< /Length " << stream.size() << " >>\nstream\n" << stream << "\nendstream";
    int contentNum = addObject(objs, cs.str());

    std::ostringstream pg;
    pg << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
       << num(pageW_) << " " << num(pageH_) << "] "
       << res.str() << " /Contents " << contentNum << " 0 R >>";
    pageObjNums.push_back(addObject(objs, pg.str()));
  }

  {
    std::ostringstream pgs;
    pgs << "<< /Type /Pages /Count " << pageObjNums.size() << " /Kids [ ";
    for (int n : pageObjNums) pgs << n << " 0 R ";
    pgs << "] >>";
    objs[pagesObjIdx - 1].body = pgs.str();
  }

  // serialize with xref
  std::string outBuf;
  outBuf.reserve(1 << 16);
  outBuf += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";   // binary marker comment

  for (size_t i = 0; i < objs.size(); ++i) {
    objs[i].offset = outBuf.size();
    std::ostringstream o;
    o << (i + 1) << " 0 obj\n" << objs[i].body << "\nendobj\n";
    outBuf += o.str();
  }

  size_t xrefPos = outBuf.size();
  {
    std::ostringstream o;
    o << "xref\n0 " << (objs.size() + 1) << "\n" << "0000000000 65535 f \n";
    for (const auto& ob : objs) {
      char line[32];
      std::snprintf(line, sizeof line, "%010zu 00000 n \n", ob.offset);
      o << line;
    }
    o << "trailer\n<< /Size " << (objs.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n" << xrefPos << "\n%%EOF\n";
    outBuf += o.str();
  }
  result = std::move(outBuf);
  return true;
}

bool Writer::saveToString(std::string& out) { return buildDocument(out); }

// bool Writer::save(const std::string& path) {
//   std::string bytes;
//   if (!buildDocument(bytes)) return false;
//   std::ofstream out(path, std::ios::binary);
//   if (!out) return false;
//   out.write(bytes.data(), (std::streamsize)bytes.size());
//   return (bool)out;
// }
bool Writer::save(const std::string& path) {
  std::string bytes;
  if (!buildDocument(bytes)) return false;
  PDFGEN_LOGD("Writer::save %s", pdfgen::resolvePath(path).c_str());
  // Den STRING uebergeben, nicht bytes.data(): ein PDF enthaelt Nullbytes,
  // ein daraus gebauter std::string endete am ersten davon (-> kaputtes
  // PDF ohne xref/trailer). writeFile schreibt data.size() Bytes.
  return pdfgen::writeFile(path, bytes);
}

// ---- UTF-8 → CP1252 ----------------------------------------------------------
std::string utf8ToWinAnsi(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    uint32_t cp = decodeUtf8(utf8, i);
    if (cp == '\r') continue;
    int b = cpToWinAnsi(cp);
    out += b >= 0 ? (char)b : '?';
  }
  return out;
}

} // namespace pdf
