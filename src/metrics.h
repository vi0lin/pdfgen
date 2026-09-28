// metrics.h — advance widths of the built-in Helvetica family (Adobe AFM data),
// needed for word wrapping and justification. Widths are in 1/1000 em.
#pragma once
// API version of this header. The .cpp files verify that all headers come
// from the same release -- mixing files from different downloads otherwise
// causes confusing "has no member" errors.
#define PDFGEN_METRICS_API 1
#include <string>
#include "pdfwriter.h"

namespace pdf {

// Width of a single WinAnsi byte in 1/1000 em for the given font.
int glyphWidth(Font f, unsigned char code);

// Width of a WinAnsi byte string at `fontSize` points.
double textWidth(Font f, const std::string& winAnsiBytes, double fontSize);

} // namespace pdf
