#include "image.h"
#include <algorithm>   // std::count/min/max (MSVC does not pull this in transitively)
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <zlib.h>
#ifndef PDFGEN_NO_WEBP
#include <webp/decode.h>

// ---- release consistency check (see header comment) ----
#ifndef PDFGEN_IMAGE_API
#error "stale image.h: it lacks PDFGEN_IMAGE_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_IMAGE_API != 3
#error "version mismatch in image.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif
#ifndef PDFGEN_PDFWRITER_API
#error "stale pdfwriter.h: it lacks PDFGEN_PDFWRITER_API. Replace ALL pdfgen source files from the same release (delete the old src/ first), then wipe the CMake build directory."
#elif PDFGEN_PDFWRITER_API != 6
#error "version mismatch in pdfwriter.h: replace ALL pdfgen source files from the same release and wipe the build directory."
#endif

#endif

namespace pdf {
namespace {

bool readFile(const std::string& path, std::string& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

uint32_t be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Area-average (box filter) downscale of interleaved `comps`-byte pixels.
void boxScale(std::string& buf, int comps, int w, int h, int nw, int nh) {
  std::string out((size_t)nw * nh * comps, '\0');
  const unsigned char* src = (const unsigned char*)buf.data();
  unsigned char* dst = (unsigned char*)&out[0];
  for (int dy = 0; dy < nh; ++dy) {
    int y0 = (int)((long long)dy * h / nh);
    int y1 = (int)(((long long)dy + 1) * h / nh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int dx = 0; dx < nw; ++dx) {
      int x0 = (int)((long long)dx * w / nw);
      int x1 = (int)(((long long)dx + 1) * w / nw);
      if (x1 <= x0) x1 = x0 + 1;
      for (int c = 0; c < comps; ++c) {
        long long sum = 0;
        for (int sy = y0; sy < y1; ++sy)
          for (int sx = x0; sx < x1; ++sx)
            sum += src[((size_t)sy * w + sx) * comps + c];
        dst[((size_t)dy * nw + dx) * comps + c] =
            (unsigned char)(sum / ((y1 - y0) * (long long)(x1 - x0)));
      }
    }
  }
  buf = std::move(out);
}

// Applies the pixel budget: scales rgb (+ optional alpha) down when needed.
void applyPixelBudget(std::string& rgb, std::string* alpha, int rgbComps,
                      int& w, int& h, int maxW, int maxH) {
  double scale = 1.0;
  if (maxW > 0 && w > maxW) scale = std::min(scale, (double)maxW / w);
  if (maxH > 0 && h > maxH) scale = std::min(scale, (double)maxH / h);
  if (scale >= 1.0) return;
  int nw = std::max(1, (int)(w * scale + 0.5));
  int nh = std::max(1, (int)(h * scale + 0.5));
  boxScale(rgb, rgbComps, w, h, nw, nh);
  if (alpha && !alpha->empty()) boxScale(*alpha, 1, w, h, nw, nh);
  w = nw;
  h = nh;
}

std::string zDeflate(const std::string& raw) {
  uLongf cap = compressBound(raw.size());
  std::string out(cap, '\0');
  compress2((Bytef*)&out[0], &cap, (const Bytef*)raw.data(), raw.size(), 6);
  out.resize(cap);
  return out;
}

bool zInflate(const std::string& comp, std::string& raw) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return false;
  zs.next_in  = (Bytef*)comp.data();
  zs.avail_in = comp.size();
  char buf[65536];
  int ret;
  do {
    zs.next_out  = (Bytef*)buf;
    zs.avail_out = sizeof buf;
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return false; }
    raw.append(buf, sizeof buf - zs.avail_out);
  } while (ret != Z_STREAM_END);
  inflateEnd(&zs);
  return true;
}

// ---- JPEG -------------------------------------------------------------------
bool loadJpeg(const std::string& data, ImageXObject& out, std::string& err) {
  size_t i = 2, n = data.size();               // skip FFD8
  while (i + 9 < n) {
    if ((unsigned char)data[i] != 0xFF) { ++i; continue; }
    unsigned char marker = data[i + 1];
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD9)) { i += 2; continue; }
    uint16_t seglen = ((unsigned char)data[i+2] << 8) | (unsigned char)data[i+3];
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      out.heightPx = ((unsigned char)data[i+5] << 8) | (unsigned char)data[i+6];
      out.widthPx  = ((unsigned char)data[i+7] << 8) | (unsigned char)data[i+8];
      int comps    = (unsigned char)data[i+9];
      out.colorSpace = comps == 1 ? "DeviceGray" : "DeviceRGB";  // 3 = YCbCr→RGB
      out.filter = "DCTDecode";
      out.data   = data;
      return true;
    }
    i += 2 + seglen;
  }
  err = "could not find JPEG SOF marker";
  return false;
}

// ---- PNG --------------------------------------------------------------------
int paeth(int a, int b, int c) {
  int p = a + b - c, pa = std::abs(p-a), pb = std::abs(p-b), pc = std::abs(p-c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

bool loadPng(const std::string& data, ImageXObject& out, std::string& err,
             int maxW, int maxH) {
  size_t i = 8;                                 // skip signature
  int w = 0, h = 0, bitDepth = 0, colorType = -1, interlace = 0;
  std::string idat, plteRGB, trns;

  while (i + 8 <= data.size()) {
    uint32_t len = be32((const unsigned char*)data.data() + i);
    std::string type = data.substr(i + 4, 4);
    if (i + 8 + len > data.size()) break;
    const char* payload = data.data() + i + 8;
    if (type == "IHDR") {
      w = be32((const unsigned char*)payload);
      h = be32((const unsigned char*)payload + 4);
      bitDepth  = (unsigned char)payload[8];
      colorType = (unsigned char)payload[9];
      interlace = (unsigned char)payload[12];
    } else if (type == "PLTE") plteRGB.assign(payload, len);
    else if (type == "tRNS")   trns.assign(payload, len);
    else if (type == "IDAT")   idat.append(payload, len);
    else if (type == "IEND")   break;
    i += 12 + len;
  }
  if (w <= 0 || h <= 0)  { err = "bad PNG header"; return false; }
  if (interlace)         { err = "interlaced PNG not supported — re-save without interlacing"; return false; }
  if (bitDepth != 8)     { err = "only 8-bit PNGs supported — re-save as 8-bit"; return false; }

  static const int chanPer[7] = {1,0,3,1,2,0,4};   // by color type
  if (colorType < 0 || colorType > 6 || chanPer[colorType] == 0) {
    err = "unsupported PNG color type"; return false;
  }
  int channels = chanPer[colorType];
  size_t stride = (size_t)w * channels;

  std::string raw;
  if (!zInflate(idat, raw) || raw.size() < (stride + 1) * h) {
    err = "PNG decompression failed"; return false;
  }

  // un-filter scanlines in place into `pixels`
  std::vector<unsigned char> pixels(stride * h);
  const unsigned char* src = (const unsigned char*)raw.data();
  for (int y = 0; y < h; ++y) {
    unsigned char filter = src[y * (stride + 1)];
    const unsigned char* line = src + y * (stride + 1) + 1;
    unsigned char* dst  = pixels.data() + y * stride;
    unsigned char* prev = y ? pixels.data() + (y - 1) * stride : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      int a = x >= (size_t)channels ? dst[x - channels] : 0;
      int b = prev ? prev[x] : 0;
      int c = (prev && x >= (size_t)channels) ? prev[x - channels] : 0;
      int v = line[x];
      switch (filter) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: v += paeth(a, b, c); break;
      }
      dst[x] = (unsigned char)v;
    }
  }

  // expand to RGB / Gray (+ alpha)
  std::string rgb, alpha;
  rgb.reserve((size_t)w * h * 3);
  bool hasAlpha = colorType == 4 || colorType == 6;
  if (hasAlpha) alpha.reserve((size_t)w * h);

  for (size_t p = 0; p < (size_t)w * h; ++p) {
    const unsigned char* px = pixels.data() + p * channels;
    switch (colorType) {
      case 0: rgb += (char)px[0]; break;                              // gray
      case 2: rgb.append((const char*)px, 3); break;                  // rgb
      case 3: {                                                       // palette
        size_t idx = (size_t)px[0] * 3;
        if (idx + 2 < plteRGB.size()) rgb.append(plteRGB, idx, 3);
        else rgb.append(3, '\0');
        break;
      }
      case 4: rgb += (char)px[0]; alpha += (char)px[1]; break;        // gray+a
      case 6: rgb.append((const char*)px, 3); alpha += (char)px[3]; break; // rgb+a
    }
  }

  int comps = (colorType == 0 || colorType == 4) ? 1 : 3;
  applyPixelBudget(rgb, hasAlpha ? &alpha : nullptr, comps, w, h, maxW, maxH);
  out.widthPx  = w;
  out.heightPx = h;
  out.colorSpace = (colorType == 0 || colorType == 4) ? "DeviceGray" : "DeviceRGB";
  out.filter = "FlateDecode";
  out.data   = zDeflate(rgb);
  if (hasAlpha) out.smaskData = zDeflate(alpha);
  return true;
}

// ---- WebP -------------------------------------------------------------------
#ifndef PDFGEN_NO_WEBP
bool loadWebp(const std::string& d, ImageXObject& out, std::string& err,
              int maxW, int maxH) {
  const uint8_t* bytes = (const uint8_t*)d.data();
  int w = 0, h = 0;
  if (!WebPGetInfo(bytes, d.size(), &w, &h)) {
    err = "invalid or unsupported WebP file"; return false;
  }
  // RGBA covers every WebP flavor: lossy (VP8), lossless (VP8L), extended
  // (VP8X) with or without an alpha channel. Animated files decode to their
  // first frame only via the demux API — plain WebPDecodeRGBA rejects them,
  // which is fine for a document generator.
  uint8_t* rgba = WebPDecodeRGBA(bytes, d.size(), &w, &h);
  if (!rgba) { err = "WebP decoding failed (animated WebP is not supported)"; return false; }

  std::string rgb, alpha;
  rgb.reserve((size_t)w * h * 3);
  alpha.reserve((size_t)w * h);
  bool anyTransparency = false;
  for (size_t p = 0; p < (size_t)w * h; ++p) {
    const uint8_t* px = rgba + p * 4;
    rgb += (char)px[0]; rgb += (char)px[1]; rgb += (char)px[2];
    alpha += (char)px[3];
    if (px[3] != 0xFF) anyTransparency = true;
  }
  WebPFree(rgba);

  applyPixelBudget(rgb, anyTransparency ? &alpha : nullptr, 3, w, h, maxW, maxH);
  out.widthPx = w; out.heightPx = h;
  out.colorSpace = "DeviceRGB";
  out.filter = "FlateDecode";
  out.data = zDeflate(rgb);
  if (anyTransparency) out.smaskData = zDeflate(alpha);   // skip fully opaque
  return true;
}
#else
bool loadWebp(const std::string&, ImageXObject&, std::string& err, int, int) {
  err = "this build has WebP support disabled (CMake: -DPDFGEN_WITH_WEBP=ON, "
        "Make: rebuild without NO_WEBP=1 -- or convert the image to PNG)";
  return false;
}
#endif

// ---- BMP --------------------------------------------------------------------
bool loadBmp(const std::string& d, ImageXObject& out, std::string& err,
             int maxW, int maxH) {
  auto u32 = [&](size_t o){ return (uint32_t)(unsigned char)d[o] | ((unsigned char)d[o+1]<<8) | ((unsigned char)d[o+2]<<16) | ((unsigned char)d[o+3]<<24); };
  auto s32 = [&](size_t o){ return (int32_t)u32(o); };
  if (d.size() < 54) { err = "truncated BMP"; return false; }
  uint32_t offBits = u32(10);
  int w = s32(18), h = s32(22);
  int bpp = (unsigned char)d[28] | ((unsigned char)d[29] << 8);
  uint32_t compression = u32(30);
  bool topDown = h < 0; if (topDown) h = -h;
  if (compression != 0 || (bpp != 24 && bpp != 32)) {
    err = "only uncompressed 24/32-bit BMP supported"; return false;
  }
  size_t rowSize = ((size_t)w * (bpp / 8) + 3) & ~3u;
  if (d.size() < offBits + rowSize * h) { err = "truncated BMP data"; return false; }

  std::string rgb; rgb.reserve((size_t)w * h * 3);
  for (int y = 0; y < h; ++y) {
    int srcRow = topDown ? y : h - 1 - y;
    const unsigned char* row = (const unsigned char*)d.data() + offBits + rowSize * srcRow;
    for (int x = 0; x < w; ++x) {
      const unsigned char* px = row + x * (bpp / 8);      // BGR(A)
      rgb += (char)px[2]; rgb += (char)px[1]; rgb += (char)px[0];
    }
  }
  applyPixelBudget(rgb, nullptr, 3, w, h, maxW, maxH);
  out.widthPx = w; out.heightPx = h;
  out.colorSpace = "DeviceRGB";
  out.filter = "FlateDecode";
  out.data = zDeflate(rgb);
  return true;
}

} // namespace

bool loadImage(const std::string& path, ImageXObject& out, std::string& error,
               int maxWidthPx, int maxHeightPx) {
  std::string data;
  if (!readFile(path, data)) { error = "cannot open file: " + path; return false; }
  if (data.size() >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8)
    return loadJpeg(data, out, error);            // verbatim, never resampled
  if (data.size() >= 8 && !std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8))
    return loadPng(data, out, error, maxWidthPx, maxHeightPx);
  if (data.size() >= 12 && !std::memcmp(data.data(), "RIFF", 4) &&
      !std::memcmp(data.data() + 8, "WEBP", 4))
    return loadWebp(data, out, error, maxWidthPx, maxHeightPx);
  if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
    return loadBmp(data, out, error, maxWidthPx, maxHeightPx);
  error = "unsupported image format (supported: JPEG, PNG, WebP, BMP): " + path;
  return false;
}

} // namespace pdf
