#include "mail_config.h"
#include "pdfgen_host.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace pdfgen {
namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string noSpaces(const std::string& s) {
  std::string o;
  for (char c : s) if (!std::isspace((unsigned char)c)) o += c;
  return o;
}
bool isComment(const std::string& t) {
  return t.empty() || t[0] == '#' || t[0] == ';' || t.rfind("//", 0) == 0;
}

std::mutex g_mx;
MailConfig g_cfg;

} // namespace

const MailAccount* MailConfig::find(const std::string& from) const {
  if (accounts.empty()) return nullptr;
  if (trim(from).empty()) {
    for (const auto& a : accounts) if (a.isDefault) return &a;
    return &accounts.front();
  }
  const std::string want = lower(trim(from));
  for (const auto& a : accounts) if (lower(a.address) == want) return &a;
  return nullptr;
}

void MailConfig::merge(const MailConfig& o) {
  bool newDefault = false;
  for (const auto& a : o.accounts) if (a.isDefault) newDefault = true;
  if (newDefault) for (auto& a : accounts) a.isDefault = false;
  for (const auto& a : o.accounts) {
    bool replaced = false;
    for (auto& mine : accounts)
      if (lower(mine.address) == lower(a.address)) {
        bool keepDef = mine.isDefault && !newDefault;
        mine = a;
        mine.isDefault = a.isDefault || keepDef;
        replaced = true;
        break;
      }
    if (!replaced) accounts.push_back(a);
  }
  for (const auto& t : o.testTo) {
    bool dup = false;
    for (const auto& x : testTo) if (lower(x) == lower(t)) dup = true;
    if (!dup) testTo.push_back(t);
  }
}

bool MailConfig::setDefault(const std::string& address) {
  const std::string want = lower(trim(address));
  MailAccount* hit = nullptr;
  for (auto& a : accounts) if (lower(a.address) == want) hit = &a;
  if (!hit) return false;
  for (auto& a : accounts) a.isDefault = false;
  hit->isDefault = true;
  return true;
}

bool parseMailConfig(const std::string& text, MailConfig& cfg,
                     std::vector<std::string>& warnings) {
  enum class Sec { None, Account, Test, Unknown } sec = Sec::None;
  MailAccount cur;
  int curLines = 0;                    // 0 = Adresse fehlt, 1 = Token fehlt
  int lineNo = 0;
  bool sawDefault = false;
  for (const auto& a : cfg.accounts) if (a.isDefault) sawDefault = true;

  auto flush = [&]() {
    if (sec != Sec::Account) return;
    if (curLines == 0) {
      warnings.push_back("mail config: [" + cur.provider + "] ohne Adresse, ignoriert");
    } else {
      if (cur.token.empty())
        warnings.push_back("mail config: " + cur.address + " hat kein Token");
      if (cur.isDefault && sawDefault) {
        warnings.push_back("mail config: mehrere Standardkonten, " +
                           cur.address + " ist es nicht");
        cur.isDefault = false;
      }
      if (cur.isDefault) sawDefault = true;
      MailConfig one;
      one.accounts.push_back(cur);
      // merge() haelt Adressen eindeutig (spaetere Angabe gewinnt)
      bool def = cur.isDefault;
      one.accounts[0].isDefault = false;
      cfg.merge(one);
      if (def) cfg.setDefault(cur.address);
    }
  };

  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    ++lineNo;
    std::string t = trim(line);
    if (isComment(t)) continue;
    if (t.front() == '[' && t.back() == ']') {
      flush();
      std::stringstream hs(lower(t.substr(1, t.size() - 2)));
      std::string w, provider, host;
      int port = 0;
      bool def = false, ssl = false, starttls = false, xo = false;
      while (hs >> w) {
        if      (w == "default" || w == "standard") def = true;
        else if (w == "ssl" || w == "smtps")        ssl = true;
        else if (w == "starttls")                    starttls = true;
        else if (w == "xoauth2" || w == "oauth2")   xo = true;
        else if (provider.empty())                   provider = w;
        else if (host.empty()) {                     // smtp HOST[:PORT]
          size_t c = w.find(':');
          host = w.substr(0, c);
          if (c != std::string::npos) port = std::atoi(w.c_str() + c + 1);
        } else if (port == 0 && std::isdigit((unsigned char)w[0])) {
          port = std::atoi(w.c_str());               // smtp HOST PORT
        }
      }
      (void)starttls;                                // default anyway
      if (provider == "test") {
        sec = Sec::Test;
      } else if (provider.empty() && !def) {
        warnings.push_back("mail config Zeile " + std::to_string(lineNo) +
                           ": leerer Abschnitt");
        sec = Sec::Unknown;
      } else {
        sec = Sec::Account;
        cur = MailAccount{};
        cur.provider = provider.empty() ? "gmail" : provider;
        cur.host = host;
        cur.port = port;
        cur.ssl = ssl;
        cur.xoauth2 = xo;
        cur.isDefault = def;
        curLines = 0;
      }
      continue;
    }
    switch (sec) {
      case Sec::Account:
        if (curLines == 0)      { cur.address = t; curLines = 1; }
        else if (curLines == 1) { cur.token = noSpaces(t); curLines = 2; }
        else warnings.push_back("mail config Zeile " + std::to_string(lineNo) +
                                ": ueberzaehlige Zeile im Konto " + cur.address);
        break;
      case Sec::Test: {
        std::stringstream ws(t);
        std::string addr;
        MailConfig one;
        while (ws >> addr) one.testTo.push_back(addr);
        cfg.merge(one);
        break;
      }
      case Sec::None:
        warnings.push_back("mail config Zeile " + std::to_string(lineNo) +
                           ": Text vor dem ersten [Abschnitt]");
        break;
      case Sec::Unknown:
        break;
    }
  }
  flush();
  return true;
}

bool loadMailConfigFile(const std::string& path, MailConfig& cfg,
                        std::vector<std::string>& warnings) {
  std::string text;
  if (!readFile(path, text)) return false;
  return parseMailConfig(text, cfg, warnings);
}

std::string formatMailConfig(const MailConfig& cfg) {
  std::string out;
  for (const auto& a : cfg.accounts) {
    if (trim(a.address).empty()) continue;
    out += "[" + (a.provider.empty() ? std::string("gmail") : a.provider) +
           (a.isDefault ? " default" : "") + "]\n";
    out += trim(a.address) + "\n";
    if (!a.token.empty()) out += noSpaces(a.token) + "\n";
  }
  if (!cfg.testTo.empty()) {
    out += "[test]\n";
    for (const auto& t : cfg.testTo) out += t + "\n";
  }
  return out;
}

void setMailConfig(const MailConfig& cfg) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_cfg = cfg;
  PDFGEN_LOGD("mail config: %zu Konto/Konten, %zu Testempfaenger",
              cfg.accounts.size(), cfg.testTo.size());
}

MailConfig mailConfig() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_cfg;
}


// ---- Anbieter-Profile ---------------------------------------------------
namespace {
struct Provider { const char* name; const char* host; int port; bool ssl; };
const Provider kProviders[] = {
  {"gmail",   "smtp.gmail.com",          465, true },
  {"outlook", "smtp.office365.com",      587, false},
  {"office365","smtp.office365.com",     587, false},
  {"gmx",     "mail.gmx.net",            587, false},
  {"webde",   "smtp.web.de",             587, false},
  {"web.de",  "smtp.web.de",             587, false},
  {"tonline", "securesmtp.t-online.de",  465, true },
  {"t-online","securesmtp.t-online.de",  465, true },
  {"yahoo",   "smtp.mail.yahoo.com",     465, true },
  {"icloud",  "smtp.mail.me.com",        587, false},
};
} // namespace

bool resolveAccount(MailAccount& a, std::vector<std::string>& warnings) {
  if (!a.host.empty()) {
    if (a.port == 0) a.port = a.ssl ? 465 : 587;
    return true;
  }
  for (const auto& p : kProviders) {
    if (a.provider == p.name) {
      a.host = p.host;
      if (a.port == 0) a.port = p.port;
      // explizites ssl/starttls im Header gewinnt gegen das Profil
      if (!a.ssl) a.ssl = p.ssl;
      return true;
    }
  }
  warnings.push_back("mail config: unbekannter Anbieter '" + a.provider +
                     "' fuer " + a.address +
                     " -- [smtp HOST:PORT (ssl)] verwenden");
  return false;
}

std::string providerForAddress(const std::string& address) {
  size_t at = address.find('@');
  if (at == std::string::npos) return "";
  std::string d = lower(address.substr(at + 1));
  auto has = [&](const char* dom) {
    size_t n = std::strlen(dom);
    return d == dom ||
           (d.size() > n && d.compare(d.size() - n - 1, n + 1,
                                      std::string(".") + dom) == 0);
  };
  if (has("gmail.com") || has("googlemail.com"))            return "gmail";
  if (has("outlook.com") || has("outlook.de") ||
      has("hotmail.com") || has("hotmail.de") ||
      has("live.com") || has("live.de") || has("office365.com"))
    return "outlook";
  if (d == "gmx.de" || d == "gmx.net" || d == "gmx.at" ||
      d == "gmx.ch" || d == "gmx.com")                      return "gmx";
  if (has("web.de"))                                        return "webde";
  if (has("t-online.de") || has("magenta.de"))              return "tonline";
  if (has("yahoo.com") || has("yahoo.de") || has("ymail.com"))
    return "yahoo";
  if (has("icloud.com") || has("me.com") || has("mac.com")) return "icloud";
  return "";
}

} // namespace pdfgen
