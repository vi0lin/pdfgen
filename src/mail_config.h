// mail_config.h -- Absenderkonten und Test-Empfaenger zur LAUFZEIT.
//
// Ersetzt die frueher einkompilierten FROM_ADDR / APP_PASSWORD. Es gibt
// genau EIN Format, egal woher die Daten kommen:
//
//   # pdfgen.conf -- Kommentare mit #, ; oder //
//   [gmail default]        <- Anbieter + optional "default"
//   ich@gmail.com          <- 1. Zeile: Adresse
//   abcd efgh ijkl mnop    <- 2. Zeile: App-Passwort/Token (Leerzeichen egal)
//   [outlook]              <- weitere Anbieter: outlook/office365, gmx,
//   chef@outlook.com          web.de, t-online, yahoo, icloud -- Host,
//   app-passwort              Port und TLS-Variante sind hinterlegt
//   [gmx]
//   ich@gmx.de
//   app-passwort
//   [smtp mail.firma.de:587 starttls]   <- beliebiger SMTP-Server;
//   ich@firma.de                           Varianten: HOST:465 ssl,
//   passwort                               HOST:587 starttls (Standard)
//   [gmail xoauth2]        <- Flag xoauth2: die Token-Zeile ist ein
//   oauth@gmail.com           OAuth2-Bearer-Token (XOAUTH2) statt eines
//   ya29....                  Passworts -- fuer Konten ohne App-Passwort
//   [test]                 <- Empfaenger fuer --mail-test (wenn die
//   test@example.com          [mail]-Definition kein test= hat)
//
// Quellen:
//   * Standalone: pdfgen.conf neben der Binary, oder -c /pfad/datei,
//     plus --sender ADDR TOKEN auf der Kommandozeile (siehe main.cpp).
//   * Eingebettet: der Gast baut eine MailConfig (z.B. aus Textboxen) und
//     uebergibt sie mit setMailConfig(). Er kann dafuer parseMailConfig()/
//     formatMailConfig() fuer seine eigene Speicherung mitbenutzen.
//
// Ausgewaehlt wird ein Konto per [mail=Name, from=adresse] bzw.
// \mail Name, from=adresse. Ohne from= gilt das Standardkonto.
#pragma once
#define PDFGEN_MAIL_CONFIG_API 2

#include <string>
#include <vector>

namespace pdfgen {

struct MailAccount {
  std::string provider = "gmail";   // gmail, outlook, gmx, webde, tonline,
                                    // yahoo, icloud, smtp (= eigener Server)
  std::string address;
  std::string token;                // App-Passwort / OAuth2-Bearer
  std::string host;                 // leer = Anbieter-Standard
  int         port = 0;             // 0 = Anbieter-Standard
  bool        ssl = false;          // true: implizites TLS (465, smtps://)
                                    // false: STARTTLS auf 587
  bool        xoauth2 = false;      // Token ist OAuth2-Bearer (XOAUTH2)
  bool        isDefault = false;
};

// Traegt Host/Port/TLS des Anbieters ein, wo das Konto nichts vorgibt.
// Unbekannter provider ohne host -> false (+ Warnung).
bool resolveAccount(MailAccount& a, std::vector<std::string>& warnings);

// Anbieter anhand der Mail-Domain raten (fuer --sender ADDR TOKEN):
// gmail.com/googlemail.com -> gmail; outlook/hotmail/live/office365 ->
// outlook; gmx.* -> gmx; web.de -> webde; t-online.de -> tonline;
// yahoo.* -> yahoo; icloud.com/me.com/mac.com -> icloud; sonst "".
std::string providerForAddress(const std::string& address);

struct MailConfig {
  std::vector<MailAccount> accounts;
  std::vector<std::string> testTo;  // Standard-Testempfaenger

  // from leer -> Standardkonto (markiert, sonst das erste).
  // Vergleich der Adresse ohne Gross-/Kleinschreibung. nullptr = keins.
  const MailAccount* find(const std::string& from) const;

  // Fuegt `other` hinzu; gleiche Adresse wird ersetzt. Markiert `other`
  // ein Standardkonto, verliert das bisherige die Markierung.
  void merge(const MailConfig& other);

  // Markiert genau `address` als Standard. false = gibt es nicht.
  bool setDefault(const std::string& address);
};

// Text im obigen Format -> cfg (wird ERGAENZT, nicht geleert).
// Probleme landen in `warnings`; Rueckgabe false nur bei grobem Unsinn.
bool parseMailConfig(const std::string& text, MailConfig& cfg,
                     std::vector<std::string>& warnings);

// Datei lesen und parsen. false = Datei nicht lesbar.
bool loadMailConfigFile(const std::string& path, MailConfig& cfg,
                        std::vector<std::string>& warnings);

// cfg -> Text im selben Format (Konten ohne Adresse werden weggelassen).
std::string formatMailConfig(const MailConfig& cfg);

// ---- globale, thread-sichere Ablage ----------------------------------------
// Hierhin schreibt der Gast (oder main.cpp); mail_dispatch liest von hier.
void setMailConfig(const MailConfig& cfg);
MailConfig mailConfig();              // Kopie

} // namespace pdfgen
