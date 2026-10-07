// main.cpp — CLI entry point.
//
//   pdfgen DIR                  reads DIR/text.txt (DIR/Bewerbung.pdf default)
//   pdfgen Example.txt          reads that file; with no [pdfgen] tags the
//                               whole file becomes Example.pdf
//   pdfgen a.txt b.txt          several sources in one run
//   pdfgen ... --mail           additionally send ALL mails defined via
//                               [mail=...] tags in the sources
//   pdfgen ... --mail N1 N2     send only the mails named N1 and N2
//   pdfgen ... --mail --mail-test
//                               send to each mail's test= address (or the
//                               [test] addresses of the config) instead of
//                               its real recipients (subject gets "[TEST] ")
//
// Sender accounts (see mail_config.h for the file format):
//   default: pdfgen.conf in the SAME DIRECTORY AS THE BINARY (if present)
//   -c FILE / --config FILE     use FILE instead (must exist)
//   --sender ADDR TOKEN         add/replace an account (repeatable)
//   --default-sender ADDR       make ADDR the default account
//   --test-to ADDR              add a test recipient (repeatable)
// Command-line values are merged ON TOP of the file.
//
// Sources may define several documents and mails; see project.h for the
// [pdfgen] / [mail] / [attach] tag syntax.
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
#else
  #include <unistd.h>
#endif

#include "jsondata.h"
#include "markup.h"
#include "flowables.h"
#include "pdfwriter.h"
#include "metrics.h"
#include "image.h"
#include "ttffont.h"
#include "pdfimport.h"
#include "gmail_send.h"
#include "mail_config.h"
#include "pdfgen_host.h"
#include "project.h"
#ifndef PDFGEN_NO_MAIL
#include "mail_dispatch.h"
#endif

// ---- release consistency check ----
#ifndef PDFGEN_PROJECT_API
#error "stale project.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_PROJECT_API != 6
#error "version mismatch in project.h: replace ALL pdfgen source files from the same release."
#endif

namespace {

// Verzeichnis der laufenden Binary. argv[0] ist dafuer unzuverlaessig
// (Aufruf ueber PATH, Symlinks) -- deshalb zuerst die OS-Mittel.
std::string exeDir(const char* argv0) {
  std::string p;
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) p.assign(buf, n);
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t sz = sizeof buf;
  if (_NSGetExecutablePath(buf, &sz) == 0) p = buf;
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) p.assign(buf, (size_t)n);
#endif
  if (p.empty() && argv0) p = argv0;             // Rueckfall
  size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? "." : p.substr(0, s);
}

void verboseSink(int level, const char* msg, void*) {
  const char* tag = level >= PDFGEN_LOG_ERROR ? "error: "
                  : level == PDFGEN_LOG_WARN  ? "warning: "
                  : level == PDFGEN_LOG_DEBUG ? "debug: " : "";
  std::fprintf(stderr, "[pdfgen] %s%s\n", tag, msg);
}

void usage(const char* argv0) {
  std::cerr <<
      "usage: " << argv0 << " SOURCE [SOURCE ...] [--mail [NAME ...]] [--mail-test]\n"
      "  SOURCE               a directory (reads DIR/text.txt) or a .txt file\n"
      "  --mail               send the [mail=...] definitions found in the sources\n"
      "                       (all of them, or only the listed NAMEs)\n"
      "  --mail-test          send to each mail's test= / [test] address instead\n"
      "  -c, --config FILE    sender config (default: pdfgen.conf next to the binary)\n"
      "  --sender ADDR TOKEN [PROV|HOST:PORT[:ssl]]\n"
      "                       add a sender account; provider is inferred\n"
      "                       from the domain (gmail, outlook, gmx, web.de,\n"
      "                       t-online, yahoo, icloud) or given explicitly\n"
      "  --mail-accounts      list the resolved sender accounts and exit\n"
      "  --mail-list          render, then list the defined mails (recipients,\n"
      "                       subject, attachments) WITHOUT sending\n"
      "  --show, --dry-run    plan only: list every pdf.NAME / html.NAME /\n"
      "                       mail.NAME this source would produce -- nothing\n"
      "                       is rendered, written or sent\n"
      "  --gen ID             generate only ID (repeatable; dot notation\n"
      "                       pdf.Angebot-1, mail.Kunde1). A selected mail\n"
      "                       pulls in the documents it needs\n"
      "  --default-sender ADDR\n"
      "  --test-to ADDR       add a test recipient (repeatable)\n"
      "  -V, --version        print the build's API levels and exit\n"
      "  -v, --verbose        also print debug messages\n";
}

} // namespace

static bool listAccounts = false;
static bool listMails = false;
static bool showRun = false;
static project::RunOptions runOpts;

int main(int argc, char** argv) {
  std::vector<std::string> sources, mailNames;
  bool mailMode = false, mailTest = false;
  std::string configPath, defaultSender;
  pdfgen::MailConfig cli;

  auto need = [&](int& i, int count, const char* flag) -> bool {
    if (i + count < argc) return true;
    std::cerr << flag << ": missing argument\n";
    return false;
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    if (a == "--version" || a == "-V") {
      // Welcher Stand ist diese Binary? Die API-Nummern identifizieren das
      // Delta eindeutig, __DATE__/__TIME__ den Uebersetzungszeitpunkt.
      printf("pdfgen  (gebaut %s %s)\n", __DATE__, __TIME__);
      printf("  markup %d  project %d  flowables %d  pdfwriter %d  metrics %d\n",
             PDFGEN_MARKUP_API, PDFGEN_PROJECT_API, PDFGEN_FLOWABLES_API,
             PDFGEN_PDFWRITER_API, PDFGEN_METRICS_API);
      printf("  image %d  ttffont %d  pdfimport %d  jsondata %d  mail %d/%d/%d\n",
             PDFGEN_IMAGE_API, PDFGEN_TTFFONT_API, PDFGEN_PDFIMPORT_API,
             PDFGEN_JSONDATA_API, PDFGEN_GMAIL_SEND_API,
             PDFGEN_MAIL_CONFIG_API,
#ifdef PDFGEN_MAIL_DISPATCH_API
             PDFGEN_MAIL_DISPATCH_API
#else
             1
#endif
             );
      return 0;
    }
    if (a == "-v" || a == "--verbose") { pdfgen_set_log_sink(verboseSink, nullptr); continue; }
    if (a == "--mail")      { mailMode = true; continue; }
    if (a == "--mail-test") { mailTest = true; continue; }
    if (a == "--path" || a == "--paths") continue;
    if (a == "-c" || a == "--config") {
      if (!need(i, 1, "-c")) return 2;
      configPath = argv[++i];
      continue;
    }
    if (a == "--sender") {
      if (!need(i, 2, "--sender")) return 2;
      pdfgen::MailAccount acc;
      acc.address = argv[++i];
      acc.token   = argv[++i];
      // provider from the mail domain (gmail, outlook, gmx, web.de,
      // t-online, yahoo, icloud); optional third value overrides it:
      // a provider name or HOST:PORT[:ssl] for any other server
      acc.provider = pdfgen::providerForAddress(acc.address);
      if (i + 1 < argc && argv[i + 1][0] != '-' &&
          std::string(argv[i + 1]).find('@') == std::string::npos) {
        std::string ex = argv[++i];
        size_t c = ex.find(':');
        if (c == std::string::npos) {
          acc.provider = ex;                       // named provider
        } else {
          acc.provider = "smtp";
          acc.host = ex.substr(0, c);
          std::string rest = ex.substr(c + 1);
          size_t c2 = rest.find(':');
          acc.port = std::atoi(rest.substr(0, c2).c_str());
          if (c2 != std::string::npos && rest.substr(c2 + 1) == "ssl")
            acc.ssl = true;
        }
      }
      if (acc.provider.empty()) {
        PDFGEN_LOGE("--sender %s: Anbieter nicht erkennbar -- bitte angeben: "
                    "--sender ADRESSE TOKEN gmx  (oder HOST:PORT[:ssl])",
                    acc.address.c_str());
        return 2;
      }
      cli.accounts.push_back(acc);
      continue;
    }
    if (a == "--mail-list") {                    // show parsed mails, no send
      listMails = true;
      continue;
    }
    if (a == "--show" || a == "--dry-run") {     // Feedback-Lauf: nur planen
      runOpts.dryRun = true;
      showRun = true;
      continue;
    }
    if (a == "--gen") {                          // Auswahl per Punktnotation
      if (!need(i, 1, "--gen")) return 2;
      runOpts.gen.push_back(argv[++i]);
      showRun = true;
      continue;
    }
    if (a == "--mail-accounts") {                  // show resolved accounts
      listAccounts = true;
      continue;
    }
    if (a == "--default-sender") {
      if (!need(i, 1, "--default-sender")) return 2;
      defaultSender = argv[++i];
      continue;
    }
    if (a == "--test-to") {
      if (!need(i, 1, "--test-to")) return 2;
      cli.testTo.push_back(argv[++i]);
      continue;
    }
    if (a.rfind("-", 0) == 0 && a.size() > 1) {
      std::cerr << "unknown option " << a << " (ignored)\n";
      continue;
    }
    if (mailMode) mailNames.push_back(a);            // names follow --mail
    else sources.push_back(a);
  }
  if (sources.empty() && !listAccounts) { usage(argv[0]); return 2; }

  // ---- Absenderkonten: Datei, dann Kommandozeile obendrauf ----
  pdfgen::MailConfig cfg;
  std::vector<std::string> cfgWarnings;
  if (!configPath.empty()) {
    if (!pdfgen::loadMailConfigFile(configPath, cfg, cfgWarnings)) {
      std::cerr << "-c: cannot read " << configPath << "\n";
      return 2;
    }
  } else {
    const std::string def = exeDir(argv[0]) + "/pdfgen.conf";
    if (pdfgen::loadMailConfigFile(def, cfg, cfgWarnings))
      PDFGEN_LOGD("config: %s", def.c_str());
    else
      PDFGEN_LOGD("config: %s nicht vorhanden", def.c_str());
  }
  cfg.merge(cli);
  if (!defaultSender.empty() && !cfg.setDefault(defaultSender)) {
    std::cerr << "--default-sender: " << defaultSender << " is not a configured account\n";
    return 2;
  }
  for (const auto& w : cfgWarnings) PDFGEN_LOGW("%s", w.c_str());
  pdfgen::setMailConfig(cfg);
  if (listAccounts) {
    std::vector<std::string> rw;
    printf("Konfigurierte Absenderkonten:\n");
    for (auto a : cfg.accounts) {
      bool ok = pdfgen::resolveAccount(a, rw);
      printf("  %-30s %-9s %s:%d %s%s%s%s\n", a.address.c_str(),
             a.provider.c_str(), ok ? a.host.c_str() : "?",
             a.port, a.ssl ? "smtps" : "starttls",
             a.xoauth2 ? " xoauth2" : "",
             a.isDefault ? "  [default]" : "",
             a.token.empty() ? "  (kein Token!)" : "");
    }
    for (auto& w : rw) PDFGEN_LOGW("%s", w.c_str());
    if (cfg.accounts.empty()) printf("  (keine)\n");
    return 0;
  }

  // ---- PDFs erzeugen ----
  std::vector<project::MailSpec> mails;
  std::vector<std::string> warnings;
  bool ok = true;
  std::vector<project::GenItem> listing;
  runOpts.listing = &listing;
  struct PerSource { std::string src; size_t firstWarn, lastWarn, firstItem, lastItem; };
  std::vector<PerSource> runs;
  for (const auto& s : sources) {
    PerSource ps{s, warnings.size(), 0, listing.size(), 0};
    ok = project::processSource(s, mails, warnings, &runOpts) && ok;
    ps.lastWarn = warnings.size();
    ps.lastItem = listing.size();
    runs.push_back(ps);
  }
  for (const auto& w : warnings) PDFGEN_LOGW("%s", w.c_str());

  if (showRun) {                                    // Feedback-/Auswahllauf
    for (const auto& r : runs) {
      printf("Quelle: %s\n", r.src.c_str());
      for (size_t ii = r.firstItem; ii < r.lastItem; ++ii) {
        const auto& g = listing[ii];
        if (g.kind == "mail") {
          const project::MailSpec* m = nullptr;
          for (const auto& mm : mails)
            if (mm.name == g.mailName) { m = &mm; break; }
          printf("  %-28s %s\n", g.id.c_str(),
                 g.selected ? (runOpts.dryRun ? "[geplant]" : "[ausgewaehlt]")
                            : "[uebersprungen]");
          if (m) {
            std::string to;
            for (const auto& a : m->to) to += (to.empty() ? "" : " ") + a;
            printf("      Email: %s | an: %s%s%s\n", m->subject.c_str(),
                   to.c_str(), m->from.empty() ? "" : " | von: ",
                   m->from.c_str());
            // Body gekuerzt: erste zwei nichtleere Zeilen
            std::stringstream bs(m->body);
            std::string bl;
            int shown = 0;
            while (shown < 2 && std::getline(bs, bl))
              if (!bl.empty()) { printf("      | %.76s\n", bl.c_str()); ++shown; }
            for (const auto& a : m->attachments)
              printf("      Anhang: %s\n", a.c_str());
          }
        } else {
          printf("  %-28s -> %s  %s\n", g.id.c_str(), g.file.c_str(),
                 g.generated ? "[generiert]"
                 : g.selected ? (runOpts.dryRun ? "[geplant]" : "[ausgewaehlt]")
                              : "[uebersprungen]");
        }
      }
      // Fehler je Lauf
      for (size_t wi = r.firstWarn; wi < r.lastWarn; ++wi)
        printf("  Fehler/Hinweis: %s\n", warnings[wi].c_str());
    }
    if (runOpts.dryRun) return ok ? 0 : 1;          // --show/--dry-run: fertig
  }

  if (listMails) {                                  // Serienbrief-Kontrolle
    printf("Definierte Mails (%zu):\n", mails.size());
    for (const auto& m : mails) {
      std::string to;
      for (const auto& a : m.to) to += (to.empty() ? "" : " ") + a;
      printf("  %-18s an: %-34s Betreff: %s\n", m.name.c_str(), to.c_str(),
             m.subject.c_str());
      for (const auto& att : m.attachments)
        printf("  %-18s     Anhang: %s\n", "", att.c_str());
      if (!m.from.empty())
        printf("  %-18s     von: %s\n", "", m.from.c_str());
    }
    if (mails.empty()) printf("  (keine)\n");
    return ok ? 0 : 1;
  }

  // ---- Versand ----
  if (mailMode) {
#ifdef PDFGEN_NO_MAIL
    PDFGEN_LOGE("--mail: this pdfgen was built without mail support (no libcurl)");
    ok = false;
#else
    if (!runOpts.gen.empty()) {
      std::vector<project::MailSpec> sel;
      for (const auto& m : mails) if (m.selected) sel.push_back(m);
      ok = pdfgen::sendMails(sel, mailNames, mailTest) && ok;
    } else {
      ok = pdfgen::sendMails(mails, mailNames, mailTest) && ok;
    }
#endif
  }
  return ok ? 0 : 1;
}
