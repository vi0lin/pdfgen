/* gmail_send.h — mail sending via libcurl (SMTP), C module used from C++.
 *
 * The extern "C" block is what makes the C/C++ boundary work: without it the
 * C++ compiler name-mangles the declarations and the linker never matches
 * them against the plain C symbols in gmail_send.c. Include THIS HEADER from
 * C++ code — never the .c file.
 *
 * send_mail() takes the credentials as parameters. Which account is used is
 * decided at RUNTIME by mail_config.h / mail_dispatch.h (pdfgen.conf, -c,
 * --sender, or the host application) -- not by compile-time #defines.
 * Gmail requires an app password (Google account -> Security -> 2-step
 * verification -> App passwords); the normal account password is rejected.
 */
#ifndef PDFGEN_GMAIL_SEND_H
#define PDFGEN_GMAIL_SEND_H
#define PDFGEN_GMAIL_SEND_API 4

#ifdef __cplusplus
extern "C" {
#endif

/* Transport parameters: host/port/TLS variant and auth mechanism. The
 * providers' values live in mail_config.cpp; this struct keeps the C module
 * provider-agnostic. */
typedef struct {
  const char *host;      /* e.g. "smtp.gmail.com" */
  int         port;      /* 465 (ssl=1) or 587 (ssl=0, STARTTLS) */
  int         ssl;       /* 1 = implicit TLS (smtps://), 0 = STARTTLS */
  int         xoauth2;   /* 1 = token is an OAuth2 bearer (XOAUTH2) */
} smtp_transport;

/* Sends one mail over an explicit transport. Counts must match the array
 * sizes. Returns 0 on success, 1 on failure (message via log sink). */
int send_mail_ex(const smtp_transport *tr,
                 const char *from,
                 const char *password,
                 const char **to,   int n_to,
                 const char **cc,   int n_cc,
                 const char *subject,
                 const char *body_text,
                 const char **files, int n_files);

/* Sends one mail via Gmail (kept for compatibility; forwards to
 * send_mail_ex with the Gmail transport). */
int send_mail(const char *from,
              const char *password,
              const char **to,   int n_to,     /* visible recipients   */
              const char **cc,   int n_cc,     /* carbon copies        */
              const char *subject,
              const char *body_text,
              const char **files, int n_files);/* attachment paths     */

/* Sends a short test mail using the compiled-in credentials.
 * `attachment` may be a file path (e.g. the generated PDF) or NULL. */
int send_test_mail(const char *attachment);

/* The compiled-in credentials (FROM_ADDR / APP_PASSWORD), so C++ code can
 * pass them to send_mail without duplicating the #defines. */
const char *pdfgen_mail_from(void);
const char *pdfgen_mail_password(void);

/* curl_global_init/cleanup wrappers for callers sending several mails. */
void pdfgen_mail_global_init(void);
void pdfgen_mail_global_cleanup(void);

#ifdef __cplusplus
} /* extern "C" */

/* ---- C++ convenience wrapper -------------------------------------------
 * Call the C function with std::string / std::vector instead of juggling
 * const char** arrays by hand:
 *
 *   pdfgen_mail::send("me@gmail.com", "apppassword",
 *                     {"a@x.de", "b@y.de"},        // to
 *                     {},                          // cc
 *                     "Bewerbung", "Anbei mein PDF.",
 *                     {dir + "/Bewerbung.pdf"});   // attachments
 */
#include <string>
#include <vector>

namespace pdfgen_mail {

struct Transport {
  std::string host;
  int port = 465;
  bool ssl = true;
  bool xoauth2 = false;
};

inline int send(const Transport& tr,
                const std::string& from, const std::string& password,
                const std::vector<std::string>& to,
                const std::vector<std::string>& cc,
                const std::string& subject, const std::string& body,
                const std::vector<std::string>& attachments) {
  auto ptrs = [](const std::vector<std::string>& v) {
    std::vector<const char*> p;
    p.reserve(v.size());
    for (const auto& s : v) p.push_back(s.c_str());
    return p;
  };
  std::vector<const char*> pt = ptrs(to), pc = ptrs(cc), pa = ptrs(attachments);
  smtp_transport c{tr.host.c_str(), tr.port, tr.ssl ? 1 : 0,
                   tr.xoauth2 ? 1 : 0};
  return send_mail_ex(&c, from.c_str(), password.c_str(),
                      pt.data(), (int)pt.size(),
                      pc.data(), (int)pc.size(),
                      subject.c_str(), body.c_str(),
                      pa.data(), (int)pa.size());
}

inline int send(const std::string& from, const std::string& password,
                const std::vector<std::string>& to,
                const std::vector<std::string>& cc,
                const std::string& subject, const std::string& body,
                const std::vector<std::string>& attachments) {
  auto ptrs = [](const std::vector<std::string>& v) {
    std::vector<const char*> p;
    p.reserve(v.size());
    for (const auto& s : v) p.push_back(s.c_str());
    return p;
  };
  std::vector<const char*> pt = ptrs(to), pc = ptrs(cc), pa = ptrs(attachments);
  return send_mail(from.c_str(), password.c_str(),
                   pt.data(), (int)pt.size(),
                   pc.data(), (int)pc.size(),
                   subject.c_str(), body.c_str(),
                   pa.data(), (int)pa.size());
}

} // namespace pdfgen_mail
#endif /* __cplusplus */

#endif /* PDFGEN_GMAIL_SEND_H */
