// mail_config.h -- Absenderkonten und Test-Empfaenger zur LAUFZEIT.
//
// Ersetzt die frueher einkompilierten FROM_ADDR / APP_PASSWORD. Es gibt
// genau EIN Format, egal woher die Daten kommen:
//
//   # pdfgen.conf -- Kommentare mit #, ; oder //
//   [gmail default]        <- Anbieter + optional "default"
//   ich@gmail.com          <- 1. Zeile: Adresse
//   abcd efgh ijkl mnop    <- 2. Zeile: App-Passwort/Token (Leerzeichen egal)
//   [gmail]
//   zweit@gmail.com
//   qrstuvwxyzabcdef
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
#define PDFGEN_MAIL_CONFIG_API 1

#include <string>
#include <vector>

namespace pdfgen {

struct MailAccount {
  std::string provider = "gmail";   // spaeter: smtp, outlook, ...
  std::string address;
  std::string token;                // App-Passwort / PAT
  bool isDefault = false;
};

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
