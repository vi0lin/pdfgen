/* pdfgen_host.h -- DIE EINZIGE STELLE, an der ein Gastprogramm pdfgen
 * anpasst. pdfgen selbst kennt kein Gastprogramm.
 *
 * Prinzip ("Dependency Inversion"): pdfgen ruft nicht den Logger des
 * Gastes auf, sondern einen Funktionszeiger. Standalone zeigt er auf
 * stderr, eingebettet setzt der Gast ihn EINMAL beim Start auf seinen
 * eigenen Logger. Die Abhaengigkeit zeigt damit nur noch in EINE Richtung:
 * Gast -> pdfgen. pdfgen kompiliert ohne eine einzige Datei des Gastes.
 *
 *   // im Gast, einmal beim Start:
 *   pdfgen_set_log_sink([](int lvl, const char* msg, void*) {
 *       MeinLogger(lvl, msg);
 *   }, nullptr);
 *   pdfgen::setPathResolver([](const std::string& p) { return MeinPfad(p); });
 *
 * Der Log-Teil ist C-kompatibel, weil gmail_send.c reines C ist.
 */
#ifndef PDFGEN_HOST_H
#define PDFGEN_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PDFGEN_LOG_DEBUG = 0,   /* Ablaufspuren ("Curl Initialized" ...)   */
  PDFGEN_LOG_INFO  = 1,   /* normale Meldungen ("wrote X.pdf")        */
  PDFGEN_LOG_WARN  = 2,
  PDFGEN_LOG_ERROR = 3
};

/* msg ist nullterminiert und OHNE abschliessenden Zeilenumbruch.
 * Der Zeiger gilt nur waehrend des Aufrufs -- wer ihn behalten will,
 * muss kopieren. */
typedef void (*pdfgen_log_sink)(int level, const char *msg, void *user);

/* NULL setzt den Standard zurueck (stderr, ohne DEBUG). Einmal beim
 * Start setzen; das Umsetzen waehrend eines Laufs ist nicht vorgesehen. */
void pdfgen_set_log_sink(pdfgen_log_sink sink, void *user);

#if defined(__GNUC__) || defined(__clang__)
void pdfgen_logf(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void pdfgen_logf(int level, const char *fmt, ...);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Kurzformen -- printf-Stil. */
#define PDFGEN_LOGD(...) pdfgen_logf(PDFGEN_LOG_DEBUG, __VA_ARGS__)
#define PDFGEN_LOGI(...) pdfgen_logf(PDFGEN_LOG_INFO,  __VA_ARGS__)
#define PDFGEN_LOGW(...) pdfgen_logf(PDFGEN_LOG_WARN,  __VA_ARGS__)
#define PDFGEN_LOGE(...) pdfgen_logf(PDFGEN_LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
#include <string>

namespace pdfgen {

// std::string-Bequemlichkeit: pdfgen::log(PDFGEN_LOG_INFO, "Pfad: " + p)
inline void log(int level, const std::string& s) {
  pdfgen_logf(level, "%s", s.c_str());
}

// ---- Pfade -----------------------------------------------------------------
// Relative Pfade zeigen auf Android ins Wurzelverzeichnis (nicht schreibbar).
// Der Gast kann deshalb eine Funktion setzen, die jeden Pfad vor dem Oeffnen
// umschreibt. Standard: unveraendert. Captureless Lambdas passen direkt.
using PathResolver = std::string (*)(const std::string&);
void setPathResolver(PathResolver fn);           // nullptr = unveraendert
std::string resolvePath(const std::string& path);

// Sichere Datei-E/A (fopen-Ergebnis wird immer geprueft -- auf Android bricht
// bionic bei FILE*==NULL sonst hart ab). Pfade laufen durch resolvePath().
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);

} // namespace pdfgen
#endif /* __cplusplus */

#endif /* PDFGEN_HOST_H */
