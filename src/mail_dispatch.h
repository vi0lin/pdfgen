// mail_dispatch.h -- versendet [mail=...]-Definitionen mit den Konten aus
// mailConfig() (mail_config.h). Gehoert zum Target pdfgen::mail (braucht
// curl). Frueher stand dieselbe Logik zweimal da (main.cpp und im Gast);
// jetzt nur noch hier.
#pragma once
#define PDFGEN_MAIL_DISPATCH_API 1

#include <string>
#include <vector>

#include "project.h"

namespace pdfgen {

// Eine Mail senden. Absender = m.from (oder Standardkonto).
// testRun: nur an m.testAddr bzw. die [test]-Adressen der Konfiguration,
// Betreff mit "[TEST] ". 0 = gesendet. Fehler gehen ins Log.
int sendMail(const project::MailSpec& m, bool testRun);

// Mehrere senden: names leer = alle, sonst nur die genannten.
// Kuemmert sich selbst um curl_global_init/cleanup. true = alles gesendet.
bool sendMails(const std::vector<project::MailSpec>& mails,
               const std::vector<std::string>& names, bool testRun);

} // namespace pdfgen
