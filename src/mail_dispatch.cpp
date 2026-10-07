#include "mail_dispatch.h"

#include "gmail_send.h"
#include "mail_config.h"
#include "pdfgen_host.h"

namespace pdfgen {

int sendMail(const project::MailSpec& m, bool testRun) {
  const MailConfig cfg = mailConfig();
  const MailAccount* acc = cfg.find(m.from);
  if (!acc) {
    if (m.from.empty())
      PDFGEN_LOGE("mail '%s': kein Absender konfiguriert "
                  "(pdfgen.conf, --sender oder Einstellungen der App)",
                  m.name.c_str());
    else
      PDFGEN_LOGE("mail '%s': from=%s ist kein konfiguriertes Konto",
                  m.name.c_str(), m.from.c_str());
    return 1;
  }
  if (acc->token.empty()) {
    PDFGEN_LOGE("mail '%s': Konto %s hat kein Token", m.name.c_str(),
                acc->address.c_str());
    return 1;
  }
  MailAccount resolved = *acc;
  {
    std::vector<std::string> rw;
    if (!resolveAccount(resolved, rw)) {
      for (auto& w : rw) PDFGEN_LOGE("%s", w.c_str());
      PDFGEN_LOGE("mail '%s': Konto %s hat keinen aufloesbaren SMTP-Server",
                  m.name.c_str(), acc->address.c_str());
      return 1;
    }
  }

  std::vector<std::string> to = m.to, cc = m.cc;
  std::string subject = m.subject;
  if (testRun) {
    if (!m.testAddr.empty())        to = {m.testAddr};
    else if (!cfg.testTo.empty())   to = cfg.testTo;
    else {
      PDFGEN_LOGW("mail '%s': keine Testadresse (test= oder [test]), uebersprungen",
                  m.name.c_str());
      return 1;
    }
    cc.clear();
    subject = "[TEST] " + subject;
  }
  if (to.empty()) {
    PDFGEN_LOGE("mail '%s': keine Empfaenger (to=...)", m.name.c_str());
    return 1;
  }

  std::vector<std::string> attachments;
  for (const auto& a : m.attachments)
    attachments.push_back(resolvePath(m.baseDir.empty() ? a : m.baseDir + "/" + a));

  PDFGEN_LOGD("mail '%s': von %s an %zu Empfaenger, %zu Anhaenge",
              m.name.c_str(), acc->address.c_str(), to.size(), attachments.size());
  for (const auto& a : attachments) PDFGEN_LOGD("  Anhang: %s", a.c_str());

  pdfgen_mail::Transport tr{resolved.host, resolved.port, resolved.ssl,
                            resolved.xoauth2};
  PDFGEN_LOGD("mail '%s': %s:%d %s%s", m.name.c_str(), tr.host.c_str(),
              tr.port, tr.ssl ? "smtps" : "starttls",
              tr.xoauth2 ? " xoauth2" : "");
  int rc = m.bodyHtml.empty()
         ? pdfgen_mail::send(tr, acc->address, acc->token, to, cc, subject, m.body, attachments)
         : pdfgen_mail::sendHtml(tr, acc->address, acc->token, to, cc, subject, m.body, m.bodyHtml, m.inlineImages, attachments);
  if (rc == 0) PDFGEN_LOGI("mail '%s': gesendet (von %s)", m.name.c_str(),
                           acc->address.c_str());
  else         PDFGEN_LOGE("mail '%s': Versand fehlgeschlagen", m.name.c_str());
  return rc;
}

bool sendMails(const std::vector<project::MailSpec>& mails,
               const std::vector<std::string>& names, bool testRun) {
  bool ok = true;
  pdfgen_mail_global_init();
  if (names.empty()) {
    if (mails.empty()) {
      PDFGEN_LOGW("keine [mail=...]-Definitionen in den Quellen gefunden");
      ok = false;
    }
    for (const auto& m : mails)
      if (sendMail(m, testRun) != 0) ok = false;
  } else {
    for (const auto& name : names) {
      bool found = false;
      for (const auto& m : mails)
        if (m.name == name) {
          found = true;
          if (sendMail(m, testRun) != 0) ok = false;
        }
      if (!found) {
        PDFGEN_LOGE("keine Mail namens '%s' definiert", name.c_str());
        ok = false;
      }
    }
  }
  pdfgen_mail_global_cleanup();
  return ok;
}

} // namespace pdfgen
