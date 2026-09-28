#include "pdfimport.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <zlib.h>

namespace pdf {
namespace {

// ---- object model ---------------------------------------------------------
struct PObj {
  enum Kind { Null, Bool, Num, Str, HexStr, Name, Arr, Dict, Stream, Ref } kind = Null;
  bool b = false;
  double num = 0;
  bool isInt = false;
  std::string s;                       // Str/HexStr raw bytes | Name text
  std::vector<PObj> arr;
  std::vector<std::pair<std::string, PObj>> dict;  // ordered
  std::string stream;                  // raw (still encoded) stream bytes
  int refNum = 0, refGen = 0;

  const PObj* get(const std::string& key) const {
    for (auto& kv : dict)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  void set(const std::string& key, PObj v) {
    for (auto& kv : dict)
      if (kv.first == key) { kv.second = std::move(v); return; }
    dict.push_back({key, std::move(v)});
  }
  void erase(const std::string& key) {
    for (size_t i = 0; i < dict.size(); ++i)
      if (dict[i].first == key) { dict.erase(dict.begin() + i); return; }
  }
};

bool isWs(int c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c==0; }
bool isDelim(int c) { return c=='('||c==')'||c=='<'||c=='>'||c=='['||c==']'||c=='{'||c=='}'||c=='/'||c=='%'; }

// ---- lexer/parser over the raw file --------------------------------------
struct Lexer {
  const std::string& d;
  size_t p = 0;
  explicit Lexer(const std::string& data, size_t pos = 0) : d(data), p(pos) {}

  void skipWs() {
    while (p < d.size()) {
      if (isWs((unsigned char)d[p])) { ++p; continue; }
      if (d[p] == '%') { while (p < d.size() && d[p] != '\n' && d[p] != '\r') ++p; continue; }
      break;
    }
  }
  bool lit(const char* kw) {                 // consume keyword if present
    skipWs();
    size_t n = std::strlen(kw);
    if (d.compare(p, n, kw) != 0) return false;
    size_t e = p + n;
    if (e < d.size() && !isWs((unsigned char)d[e]) && !isDelim((unsigned char)d[e])) return false;
    p = e;
    return true;
  }
};

class Reader;
PObj parseObj(Lexer& lx, Reader* rd);

std::string parseName(Lexer& lx) {
  std::string out;
  ++lx.p;                                    // '/'
  while (lx.p < lx.d.size()) {
    unsigned char c = lx.d[lx.p];
    if (isWs(c) || isDelim(c)) break;
    if (c == '#' && lx.p + 2 < lx.d.size()) {
      auto hex = [](char h)->int{ if(h>='0'&&h<='9')return h-'0'; h|=32; if(h>='a'&&h<='f')return h-'a'+10; return -1; };
      int a = hex(lx.d[lx.p+1]), b = hex(lx.d[lx.p+2]);
      if (a >= 0 && b >= 0) { out += (char)(a*16+b); lx.p += 3; continue; }
    }
    out += (char)c;
    ++lx.p;
  }
  return out;
}

PObj parseString(Lexer& lx) {
  PObj o; o.kind = PObj::Str;
  ++lx.p;                                    // '('
  int depth = 1;
  while (lx.p < lx.d.size() && depth > 0) {
    char c = lx.d[lx.p++];
    if (c == '\\') {
      if (lx.p < lx.d.size()) { o.s += '\\'; o.s += lx.d[lx.p++]; }
      continue;
    }
    if (c == '(') depth++;
    if (c == ')') { depth--; if (!depth) break; }
    o.s += c;
  }
  return o;
}

PObj parseHexString(Lexer& lx) {
  PObj o; o.kind = PObj::HexStr;
  ++lx.p;                                    // '<'
  while (lx.p < lx.d.size() && lx.d[lx.p] != '>') {
    char c = lx.d[lx.p++];
    if (!isWs((unsigned char)c)) o.s += c;
  }
  if (lx.p < lx.d.size()) ++lx.p;            // '>'
  return o;
}

// ---- xref + object access -------------------------------------------------
struct XEntry { int type = 0; long a = 0; long b = 0; };  // 1: offset,gen  2: objstm,idx

class Reader {
public:
  std::string d;
  std::map<int, XEntry> xref;
  PObj trailer;
  std::map<int, PObj> cache;
  std::map<int, std::vector<std::pair<int, PObj>>> objStmCache;

  bool load(std::string bytes, std::string& err);
  const PObj* object(int num);
  const PObj* resolve(const PObj& o) {
    if (o.kind != PObj::Ref) return &o;
    return object(o.refNum);
  }
  bool decodeStream(const PObj& st, std::string& out, std::string& err);

private:
  bool parseXrefAt(long pos, std::string& err, int depth);
  bool parseClassicXref(Lexer& lx, std::string& err, int depth);
  bool parseXrefStream(PObj st, std::string& err, int depth);
};

PObj parseObj(Lexer& lx, Reader* rd) {
  lx.skipWs();
  if (lx.p >= lx.d.size()) return PObj{};
  char c = lx.d[lx.p];
  PObj o;
  if (c == '/') { o.kind = PObj::Name; o.s = parseName(lx); return o; }
  if (c == '(') return parseString(lx);
  if (c == '[') {
    ++lx.p;
    o.kind = PObj::Arr;
    for (;;) {
      lx.skipWs();
      if (lx.p >= lx.d.size()) break;
      if (lx.d[lx.p] == ']') { ++lx.p; break; }
      o.arr.push_back(parseObj(lx, rd));
    }
    return o;
  }
  if (c == '<') {
    if (lx.p + 1 < lx.d.size() && lx.d[lx.p+1] == '<') {
      lx.p += 2;
      o.kind = PObj::Dict;
      for (;;) {
        lx.skipWs();
        if (lx.p + 1 < lx.d.size() && lx.d[lx.p] == '>' && lx.d[lx.p+1] == '>') { lx.p += 2; break; }
        if (lx.p >= lx.d.size() || lx.d[lx.p] != '/') { ++lx.p; if (lx.p >= lx.d.size()) break; continue; }
        std::string key = parseName(lx);
        o.dict.push_back({key, parseObj(lx, rd)});
      }
      // stream?
      Lexer probe = lx;
      if (probe.lit("stream")) {
        // EOL after 'stream': CRLF or LF
        if (probe.p < probe.d.size() && probe.d[probe.p] == '\r') ++probe.p;
        if (probe.p < probe.d.size() && probe.d[probe.p] == '\n') ++probe.p;
        long len = -1;
        if (const PObj* L = o.get("Length")) {
          const PObj* r = rd ? rd->resolve(*L) : (L->kind == PObj::Num ? L : nullptr);
          if (r && r->kind == PObj::Num) len = (long)r->num;
        }
        if (len < 0) {                        // fallback: search endstream
          size_t e = lx.d.find("endstream", probe.p);
          len = e == std::string::npos ? 0 : (long)(e - probe.p);
        }
        o.kind = PObj::Stream;
        o.stream = lx.d.substr(probe.p, (size_t)len);
        probe.p += (size_t)len;
        probe.lit("endstream");
        lx.p = probe.p;
      }
      return o;
    }
    return parseHexString(lx);
  }
  if (lx.lit("true"))  { o.kind = PObj::Bool; o.b = true;  return o; }
  if (lx.lit("false")) { o.kind = PObj::Bool; o.b = false; return o; }
  if (lx.lit("null"))  { o.kind = PObj::Null; return o; }
  // number — or "N G R" reference
  {
    size_t start = lx.p;
    std::string tok;
    while (lx.p < lx.d.size() && !isWs((unsigned char)lx.d[lx.p]) && !isDelim((unsigned char)lx.d[lx.p]))
      tok += lx.d[lx.p++];
    char* endp = nullptr;
    double v = std::strtod(tok.c_str(), &endp);
    if (endp == tok.c_str()) { return PObj{}; }   // unknown token — treat as null
    bool isInt = tok.find('.') == std::string::npos;
    if (isInt && v >= 0) {                        // possible reference
      Lexer probe = lx;
      probe.skipWs();
      size_t s2 = probe.p;
      std::string tok2;
      while (probe.p < probe.d.size() && !isWs((unsigned char)probe.d[probe.p]) && !isDelim((unsigned char)probe.d[probe.p]))
        tok2 += probe.d[probe.p++];
      char* e2 = nullptr;
      double g = std::strtod(tok2.c_str(), &e2);
      if (e2 != tok2.c_str() && tok2.find('.') == std::string::npos && g >= 0) {
        if (probe.lit("R")) {
          o.kind = PObj::Ref; o.refNum = (int)v; o.refGen = (int)g;
          lx.p = probe.p;
          return o;
        }
        (void)s2;
      }
    }
    (void)start;
    o.kind = PObj::Num; o.num = v; o.isInt = isInt;
    return o;
  }
}

// zlib inflate
bool zInf(const std::string& in, std::string& out) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return false;
  zs.next_in = (Bytef*)in.data();
  zs.avail_in = in.size();
  char buf[65536];
  int ret;
  do {
    zs.next_out = (Bytef*)buf;
    zs.avail_out = sizeof buf;
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return false; }
    out.append(buf, sizeof buf - zs.avail_out);
  } while (ret != Z_STREAM_END && zs.avail_in > 0);
  inflateEnd(&zs);
  return true;
}

// PNG-style predictor decode (Predictor >= 10)
bool unpredict(std::string& data, int columns, int colors, int bpc) {
  int bpp = std::max(1, colors * bpc / 8);
  size_t rowlen = (size_t)columns * colors * bpc / 8;
  size_t stride = rowlen + 1;
  if (rowlen == 0 || data.size() % stride != 0) return false;
  size_t rows = data.size() / stride;
  std::string out(rowlen * rows, '\0');
  for (size_t r = 0; r < rows; ++r) {
    unsigned char ft = data[r * stride];
    const unsigned char* src = (const unsigned char*)data.data() + r * stride + 1;
    unsigned char* dst = (unsigned char*)&out[r * rowlen];
    const unsigned char* prev = r ? (unsigned char*)&out[(r-1) * rowlen] : nullptr;
    for (size_t i = 0; i < rowlen; ++i) {
      int a = i >= (size_t)bpp ? dst[i - bpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
      int v = src[i];
      switch (ft) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: { int p=a+b-c, pa=std::abs(p-a), pb=std::abs(p-b), pc=std::abs(p-c);
                  v += (pa<=pb && pa<=pc) ? a : (pb<=pc ? b : c); break; }
      }
      dst[i] = (unsigned char)v;
    }
  }
  data = std::move(out);
  return true;
}

bool Reader::decodeStream(const PObj& st, std::string& out, std::string& err) {
  out = st.stream;
  const PObj* f = st.get("Filter");
  if (!f) return true;
  const PObj* fr = resolve(*f);
  std::vector<std::string> filters;
  if (fr->kind == PObj::Name) filters.push_back(fr->s);
  else if (fr->kind == PObj::Arr)
    for (auto& e : fr->arr) { const PObj* er = resolve(e); if (er->kind == PObj::Name) filters.push_back(er->s); }
  for (auto& name : filters) {
    if (name == "FlateDecode" || name == "Fl") {
      std::string dec;
      if (!zInf(out, dec)) { err = "FlateDecode failed"; return false; }
      out = std::move(dec);
    } else { err = "unsupported stream filter: " + name; return false; }
  }
  if (const PObj* dp = st.get("DecodeParms")) {
    const PObj* d1 = resolve(*dp);
    if (d1->kind == PObj::Arr && !d1->arr.empty()) d1 = resolve(d1->arr[0]);
    if (d1->kind == PObj::Dict) {
      long pred = 1, cols = 1, colors = 1, bpc = 8;
      if (auto* v = d1->get("Predictor")) { auto* r = resolve(*v); if (r->kind==PObj::Num) pred = (long)r->num; }
      if (auto* v = d1->get("Columns"))   { auto* r = resolve(*v); if (r->kind==PObj::Num) cols = (long)r->num; }
      if (auto* v = d1->get("Colors"))    { auto* r = resolve(*v); if (r->kind==PObj::Num) colors = (long)r->num; }
      if (auto* v = d1->get("BitsPerComponent")) { auto* r = resolve(*v); if (r->kind==PObj::Num) bpc = (long)r->num; }
      if (pred >= 10 && !unpredict(out, (int)cols, (int)colors, (int)bpc)) {
        err = "predictor decode failed"; return false;
      }
    }
  }
  return true;
}

bool Reader::parseClassicXref(Lexer& lx, std::string& err, int depth) {
  for (;;) {
    lx.skipWs();
    if (lx.lit("trailer")) break;
    // "start count"
    PObj a = parseObj(lx, this), b = parseObj(lx, this);
    if (a.kind != PObj::Num || b.kind != PObj::Num) { err = "bad xref section"; return false; }
    long start = (long)a.num, count = (long)b.num;
    lx.skipWs();
    for (long i = 0; i < count; ++i) {
      if (lx.p + 18 > lx.d.size()) { err = "truncated xref"; return false; }
      long off = std::strtol(lx.d.c_str() + lx.p, nullptr, 10);
      long gen = std::strtol(lx.d.c_str() + lx.p + 11, nullptr, 10);
      char type = lx.d[lx.p + 17];
      int num = (int)(start + i);
      if (type == 'n' && !xref.count(num)) xref[num] = {1, off, gen};
      lx.p += 20;
      while (lx.p < lx.d.size() && (lx.d[lx.p-1] != ' ' && lx.d[lx.p-1] != '\r' && lx.d[lx.p-1] != '\n')) break;
    }
  }
  PObj tr = parseObj(lx, this);
  if (tr.kind != PObj::Dict) { err = "bad trailer"; return false; }
  if (trailer.kind != PObj::Dict) trailer = tr;
  if (const PObj* xs = tr.get("XRefStm")) {          // hybrid file
    const PObj* r = resolve(*xs);
    if (r && r->kind == PObj::Num && depth < 32) parseXrefAt((long)r->num, err, depth + 1);
  }
  if (const PObj* pv = tr.get("Prev")) {
    const PObj* r = resolve(*pv);
    if (r && r->kind == PObj::Num && depth < 32)
      return parseXrefAt((long)r->num, err, depth + 1);
  }
  return true;
}

bool Reader::parseXrefStream(PObj st, std::string& err, int depth) {
  std::string data;
  if (!decodeStream(st, data, err)) return false;
  const PObj* W = st.get("W");
  if (!W || W->kind != PObj::Arr || W->arr.size() < 3) { err = "bad /W"; return false; }
  int w[3];
  for (int i = 0; i < 3; ++i) w[i] = (int)W->arr[i].num;
  int rec = w[0] + w[1] + w[2];
  std::vector<std::pair<long,long>> index;
  if (const PObj* I = st.get("Index")) {
    for (size_t i = 0; i + 1 < I->arr.size(); i += 2)
      index.push_back({(long)I->arr[i].num, (long)I->arr[i+1].num});
  } else {
    long size = 0;
    if (const PObj* S = st.get("Size")) size = (long)S->num;
    index.push_back({0, size});
  }
  size_t p = 0;
  auto field = [&](int width) -> long {
    long v = 0;
    for (int i = 0; i < width; ++i) v = (v << 8) | (unsigned char)data[p++];
    return v;
  };
  for (auto& [start, count] : index) {
    for (long i = 0; i < count; ++i) {
      if (p + rec > data.size()) { err = "truncated xref stream"; return false; }
      long t = w[0] ? field(w[0]) : 1;
      long a = field(w[1]);
      long b = w[2] ? field(w[2]) : 0;
      int num = (int)(start + i);
      if (!xref.count(num)) {
        if (t == 1) xref[num] = {1, a, b};
        else if (t == 2) xref[num] = {2, a, b};
      }
    }
  }
  if (trailer.kind != PObj::Dict) { trailer = st; trailer.kind = PObj::Dict; }
  if (const PObj* pv = st.get("Prev")) {
    if (pv->kind == PObj::Num && depth < 32)
      return parseXrefAt((long)pv->num, err, depth + 1);
  }
  return true;
}

bool Reader::parseXrefAt(long pos, std::string& err, int depth) {
  if (pos < 0 || (size_t)pos >= d.size()) { err = "bad xref offset"; return false; }
  Lexer lx(d, (size_t)pos);
  if (lx.lit("xref")) return parseClassicXref(lx, err, depth);
  // else: an indirect object holding a cross-reference stream
  PObj a = parseObj(lx, this), b = parseObj(lx, this);
  (void)a; (void)b;
  if (!lx.lit("obj")) { err = "no xref at startxref position"; return false; }
  PObj st = parseObj(lx, this);
  if (st.kind != PObj::Stream) { err = "xref stream expected"; return false; }
  return parseXrefStream(std::move(st), err, depth);
}

bool Reader::load(std::string bytes, std::string& err) {
  d = std::move(bytes);
  if (d.compare(0, 5, "%PDF-") != 0) { err = "not a PDF file"; return false; }
  size_t sx = d.rfind("startxref");
  if (sx == std::string::npos) { err = "startxref not found"; return false; }
  Lexer lx(d, sx + 9);
  PObj off = parseObj(lx, this);
  if (off.kind != PObj::Num) { err = "bad startxref"; return false; }
  if (!parseXrefAt((long)off.num, err, 0)) return false;
  if (trailer.get("Encrypt")) { err = "encrypted PDFs are not supported"; return false; }
  return true;
}

const PObj* Reader::object(int num) {
  auto it = cache.find(num);
  if (it != cache.end()) return &it->second;
  auto xe = xref.find(num);
  if (xe == xref.end()) { cache[num] = PObj{}; return &cache[num]; }
  if (xe->second.type == 1) {
    Lexer lx(d, (size_t)xe->second.a);
    PObj a = parseObj(lx, this), g = parseObj(lx, this);
    (void)a; (void)g;
    if (!lx.lit("obj")) { cache[num] = PObj{}; return &cache[num]; }
    cache[num] = parseObj(lx, this);
    return &cache[num];
  }
  // type 2: inside an object stream
  int stmNum = (int)xe->second.a, idx = (int)xe->second.b;
  auto& entries = objStmCache[stmNum];
  if (entries.empty()) {
    const PObj* stm = object(stmNum);
    if (stm->kind != PObj::Stream) { cache[num] = PObj{}; return &cache[num]; }
    std::string data, err;
    if (!decodeStream(*stm, data, err)) { cache[num] = PObj{}; return &cache[num]; }
    long n = 0, first = 0;
    if (const PObj* v = stm->get("N")) n = (long)v->num;
    if (const PObj* v = stm->get("First")) first = (long)v->num;
    Lexer hl(data, 0);
    std::vector<std::pair<int, long>> table;
    for (long i = 0; i < n; ++i) {
      PObj on = parseObj(hl, nullptr), of = parseObj(hl, nullptr);
      table.push_back({(int)on.num, (long)of.num});
    }
    for (auto& [onum, ooff] : table) {
      Lexer ol(data, (size_t)(first + ooff));
      entries.push_back({onum, parseObj(ol, nullptr)});
    }
  }
  if (idx >= 0 && idx < (int)entries.size()) cache[num] = entries[idx].second;
  else cache[num] = PObj{};
  return &cache[num];
}

// ---- serialization back into the target document --------------------------
std::string escStr(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') { out += '\\'; if (i+1 < s.size()) out += s[++i]; }
    else if (c == '(' || c == ')') { out += '\\'; out += c; }
    else out += c;
  }
  return out;
}

std::string escName(const std::string& s) {
  std::string out;
  char b[8];
  for (unsigned char c : s) {
    if (c <= 32 || c >= 127 || isDelim(c) || c == '#') {
      std::snprintf(b, sizeof b, "#%02X", c);
      out += b;
    } else out += (char)c;
  }
  return out;
}

struct Copier {
  Reader& rd;
  const std::function<int()>& reserve;
  const std::function<void(int, std::string)>& fill;
  std::map<int, int> remap;

  int copyRef(int num) {
    auto it = remap.find(num);
    if (it != remap.end()) return it->second;
    int newNum = reserve();
    remap[num] = newNum;                 // register BEFORE recursing (cycles!)
    const PObj* o = rd.object(num);
    fill(newNum, serialize(*o));
    return newNum;
  }

  std::string serialize(const PObj& o) {
    char b[64];
    switch (o.kind) {
      case PObj::Null: return "null";
      case PObj::Bool: return o.b ? "true" : "false";
      case PObj::Num:
        if (o.isInt) { std::snprintf(b, sizeof b, "%lld", (long long)o.num); return b; }
        std::snprintf(b, sizeof b, "%.6f", o.num);
        return b;
      case PObj::Str:    return "(" + escStr(o.s) + ")";
      case PObj::HexStr: return "<" + o.s + ">";
      case PObj::Name:   return "/" + escName(o.s);
      case PObj::Ref: {
        std::snprintf(b, sizeof b, "%d 0 R", copyRef(o.refNum));
        return b;
      }
      case PObj::Arr: {
        std::string out = "[ ";
        for (auto& e : o.arr) { out += serialize(e); out += ' '; }
        return out + "]";
      }
      case PObj::Dict:
      case PObj::Stream: {
        std::string out = "<< ";
        for (auto& kv : o.dict) {
          if (o.kind == PObj::Stream && kv.first == "Length") continue;
          out += "/" + escName(kv.first) + " " + serialize(kv.second) + " ";
        }
        if (o.kind == PObj::Stream) {
          std::snprintf(b, sizeof b, "/Length %zu ", o.stream.size());
          out += b;
          out += ">>\nstream\n" + o.stream + "\nendstream";
          return out;
        }
        return out + ">>";
      }
    }
    return "null";
  }
};

} // namespace

// ---- ImportedPdf ----------------------------------------------------------
class ImportedPdfImpl {
public:
  Reader rd;
  // page object number + effective inherited attributes
  struct PageInfo { int num; PObj mediaBox, resources, rotate, cropBox; };
  std::vector<PageInfo> pages;

  bool collect(std::string& err) {
    const PObj* root = rd.trailer.get("Root");
    if (!root) { err = "no /Root"; return false; }
    const PObj* cat = rd.resolve(*root);
    const PObj* pagesRef = cat ? cat->get("Pages") : nullptr;
    if (!pagesRef) { err = "no /Pages"; return false; }
    PObj inheritedNull;
    return walk(*pagesRef, inheritedNull, inheritedNull, inheritedNull, inheritedNull, err, 0);
  }

  bool walk(const PObj& nodeRef, PObj mb, PObj res, PObj rot, PObj crop,
            std::string& err, int depth) {
    if (depth > 64) { err = "page tree too deep"; return false; }
    int num = nodeRef.kind == PObj::Ref ? nodeRef.refNum : 0;
    const PObj* node = rd.resolve(nodeRef);
    if (!node || node->kind != PObj::Dict) return true;
    auto inherit = [&](const char* key, PObj& slot) {
      if (const PObj* v = node->get(key)) slot = *v;
    };
    inherit("MediaBox", mb);
    inherit("Resources", res);
    inherit("Rotate", rot);
    inherit("CropBox", crop);
    const PObj* type = node->get("Type");
    bool isPage = type && rd.resolve(*type) && rd.resolve(*type)->s == "Page";
    if (isPage || (!node->get("Kids") && node->get("Contents"))) {
      pages.push_back({num, mb, res, rot, crop});
      return true;
    }
    if (const PObj* kids = node->get("Kids")) {
      const PObj* ka = rd.resolve(*kids);
      if (ka && ka->kind == PObj::Arr)
        for (auto& k : ka->arr)
          if (!walk(k, mb, res, rot, crop, err, depth + 1)) return false;
    }
    return true;
  }
};

ImportedPdf::ImportedPdf() : impl_(new ImportedPdfImpl) {}
ImportedPdf::~ImportedPdf() = default;

bool ImportedPdf::load(std::string bytes, std::string& err) {
  if (!impl_->rd.load(std::move(bytes), err)) return false;
  return impl_->collect(err);
}

int ImportedPdf::pageCount() const { return (int)impl_->pages.size(); }

int ImportedPdf::copyPage(int index,
                          const std::function<int()>& reserve,
                          const std::function<void(int, std::string)>& fill,
                          int parentObjNum, std::string& err) {
  if (index < 0 || index >= (int)impl_->pages.size()) { err = "page index out of range"; return 0; }
  auto& pi = impl_->pages[index];
  const PObj* orig = impl_->rd.object(pi.num);
  if (!orig || orig->kind != PObj::Dict) { err = "page object missing"; return 0; }

  PObj page = *orig;                       // shallow copy, then adjust
  page.erase("Parent");
  page.erase("B");                         // article beads may point outside
  if (!page.get("Type")) { PObj t; t.kind = PObj::Name; t.s = "Page"; page.set("Type", t); }
  if (!page.get("MediaBox")  && pi.mediaBox.kind  != PObj::Null) page.set("MediaBox",  pi.mediaBox);
  if (!page.get("Resources") && pi.resources.kind != PObj::Null) page.set("Resources", pi.resources);
  if (!page.get("Rotate")    && pi.rotate.kind    != PObj::Null) page.set("Rotate",    pi.rotate);
  if (!page.get("CropBox")   && pi.cropBox.kind   != PObj::Null) page.set("CropBox",   pi.cropBox);

  Copier cp{impl_->rd, reserve, fill, {}};
  cp.remap[pi.num] = reserve();            // the page object itself
  int pageNum = cp.remap[pi.num];
  std::string body = cp.serialize(page);
  // splice in the parent reference
  char b[48];
  std::snprintf(b, sizeof b, "<< /Parent %d 0 R ", parentObjNum);
  if (body.compare(0, 3, "<< ") == 0) body = std::string(b) + body.substr(3);
  fill(pageNum, body);
  return pageNum;
}

} // namespace pdf
