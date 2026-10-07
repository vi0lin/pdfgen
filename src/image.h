// image.h — loads image files into pdf::ImageXObject ready for embedding.
//   JPEG : embedded verbatim (DCTDecode), header parsed only for dimensions
//   PNG  : fully decoded via zlib (all color types, incl. palette + alpha),
//          re-compressed as FlateDecode RGB/Gray, alpha becomes an /SMask
//   WebP : decoded via libwebp (lossy + lossless + alpha), FlateDecode RGB,
//          alpha becomes an /SMask. Disable with `make NO_WEBP=1` if libwebp
//          is unavailable (WebP files then fail with a clear message).
//   BMP  : uncompressed 24/32-bit, converted to FlateDecode RGB
// Returns false and fills `error` for unsupported formats (gif, ...).
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_IMAGE_API 3
#include <string>
#include "pdfwriter.h"

namespace pdf {

// maxWidthPx/maxHeightPx (0 = unlimited) cap the pixel size of decoded
// images (PNG/WebP/BMP): larger bitmaps are downscaled with a box filter
// before being embedded, which shrinks the PDF drastically. JPEG files are
// embedded verbatim and are never resampled.
bool loadImage(const std::string& path, ImageXObject& out, std::string& error,
               int maxWidthPx = 0, int maxHeightPx = 0);

} // namespace pdf
