#include "pdfgen_host.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

void defaultSink(int level, const char* msg, void*) {
  if (level < PDFGEN_LOG_INFO) return;             // DEBUG nur mit Gast-Sink/-v
  const char* tag = level >= PDFGEN_LOG_ERROR ? "error: "
                  : level == PDFGEN_LOG_WARN  ? "warning: " : "";
  std::fprintf(stderr, "[pdfgen] %s%s\n", tag, msg);
}

pdfgen_log_sink g_sink = defaultSink;
void*           g_user = nullptr;
pdfgen::PathResolver g_resolver = nullptr;

} // namespace

extern "C" void pdfgen_set_log_sink(pdfgen_log_sink sink, void* user) {
  g_sink = sink ? sink : defaultSink;
  g_user = sink ? user : nullptr;
}

extern "C" void pdfgen_logf(int level, const char* fmt, ...) {
  if (!fmt) { g_sink(level, "", g_user); return; }
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) { g_sink(level, "(format error)", g_user); return; }
  // lange Meldungen (z.B. ganze Quelltexte) nicht abschneiden
  if (n >= (int)sizeof buf) {
    std::string big((size_t)n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize((size_t)n);
    while (!big.empty() && big.back() == '\n') big.pop_back();
    g_sink(level, big.c_str(), g_user);
    return;
  }
  while (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
  g_sink(level, buf, g_user);
}

namespace pdfgen {

void setPathResolver(PathResolver fn) { g_resolver = fn; }

std::string resolvePath(const std::string& path) {
  return g_resolver ? g_resolver(path) : path;
}

bool readFile(const std::string& path, std::string& out) {
  out.clear();
  const std::string p = resolvePath(path);
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
  const std::string p = resolvePath(path);
  FILE* f = std::fopen(p.c_str(), "wb");
  if (!f) return false;
  // data.size() statt strlen: PDFs enthalten Nullbytes
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (std::fclose(f) == 0) && ok;
  return ok;
}

} // namespace pdfgen
