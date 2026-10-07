/* gmail_send.c — SMTP mail via libcurl, matching the interface the project's
 * main.cpp expects (see gmail_send.h for the extern "C" details).
 *
 * Gmail specifics:
 *   - host: smtps://smtp.gmail.com:465 (implicit TLS)
 *   - auth: the account's e-mail address + an "app password"
 *     (Google account -> Security -> 2-step verification -> App passwords).
 *     Regular account passwords are rejected by Google for SMTP.
 */
#include "gmail_send.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mail body is streamed to curl via this callback */
struct payload {
  const char* data;
  size_t pos, len;
};

static size_t payload_read(char* buffer, size_t size, size_t nitems, void* userp) {
  struct payload* p = (struct payload*)userp;
  size_t room = size * nitems;
  size_t left = p->len - p->pos;
  size_t n = left < room ? left : room;
  if (n) {
    memcpy(buffer, p->data + p->pos, n);
    p->pos += n;
  }
  return n;
}

static const char* basename_of(const char* path) {
  const char* s1 = strrchr(path, '/');
  const char* s2 = strrchr(path, '\\');
  const char* s = s1 > s2 ? s1 : s2;
  return s ? s + 1 : path;
}

/* "a@x, b@y" joined header value; caller frees */
static char* join_addrs(const char** a, int n) {
  size_t len = 1;
  for (int i = 0; i < n; ++i) len += strlen(a[i]) + 2;
  char* out = (char*)malloc(len);
  if (!out) return NULL;
  out[0] = 0;
  for (int i = 0; i < n; ++i) {
    if (i) strcat(out, ", ");
    strcat(out, a[i]);
  }
  return out;
}

int send_mail(const char* user, const char* app_password,
              const char** to, int to_count,
              const char** cc, int cc_count,
              const char* subject, const char* body,
              const char** attachments, int attachment_count) {
  UIP_LOG("Initializing Curl");
  CURL* curl = curl_easy_init();
  if (!curl) {
    UIP_LOG("No Curl!");
    fprintf(stderr, "gmail_send: curl_easy_init failed\n");
    return CURLE_FAILED_INIT;
  }

  struct curl_slist* recipients = NULL;
  struct curl_slist* headers = NULL;
  curl_mime* mime = NULL;
  char* to_hdr = NULL;
  char* cc_hdr = NULL;
  CURLcode res = CURLE_OK;

  curl_easy_setopt(curl, CURLOPT_URL, "smtps://smtp.gmail.com:465");
  curl_easy_setopt(curl, CURLOPT_USERNAME, user);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, app_password);
  curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
  curl_easy_setopt(curl, CURLOPT_MAIL_FROM, user);

  /* envelope recipients: To + Cc */
  for (int i = 0; i < to_count; ++i)
    recipients = curl_slist_append(recipients, to[i]);
  for (int i = 0; i < cc_count; ++i)
    recipients = curl_slist_append(recipients, cc[i]);
  curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

  /* visible headers */
  {
    char line[1024];
    to_hdr = join_addrs(to, to_count);
    snprintf(line, sizeof line, "To: %s", to_hdr ? to_hdr : "");
    headers = curl_slist_append(headers, line);
    if (cc_count > 0) {
      cc_hdr = join_addrs(cc, cc_count);
      snprintf(line, sizeof line, "Cc: %s", cc_hdr ? cc_hdr : "");
      headers = curl_slist_append(headers, line);
    }
    snprintf(line, sizeof line, "From: %s", user);
    headers = curl_slist_append(headers, line);
    snprintf(line, sizeof line, "Subject: %s", subject ? subject : "");
    headers = curl_slist_append(headers, line);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  /* MIME: text part + attachments */
  mime = curl_mime_init(curl);
  {
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_data(part, body ? body : "", CURL_ZERO_TERMINATED);
    curl_mime_type(part, "text/plain; charset=utf-8");
  }
  for (int i = 0; i < attachment_count; ++i) {
    curl_mimepart* part = curl_mime_addpart(mime);
    if (curl_mime_filedata(part, attachments[i]) != CURLE_OK) {
      fprintf(stderr, "gmail_send: cannot attach %s\n", attachments[i]);
      continue;
    }
    curl_mime_filename(part, basename_of(attachments[i]));
    curl_mime_encoder(part, "base64");
  }
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    fprintf(stderr, "gmail_send: %s\n", curl_easy_strerror(res));

  curl_slist_free_all(recipients);
  curl_slist_free_all(headers);
  curl_mime_free(mime);
  free(to_hdr);
  free(cc_hdr);
  curl_easy_cleanup(curl);
  return (int)res;
}

int send_test_mail(void) {
  const char* user = getenv("GMAIL_USER");
  const char* pass = getenv("GMAIL_APP_PASSWORD");
  const char* to_env = getenv("GMAIL_TO");
  const char* attach = getenv("GMAIL_ATTACH");

  if (!user || !pass) {
    fprintf(stderr,
        "gmail_send: set GMAIL_USER and GMAIL_APP_PASSWORD to send mail\n"
        "  (an app password requires 2-step verification on the account:\n"
        "   Google account -> Security -> App passwords)\n");
    return 1;
  }
  const char* to[1] = { to_env ? to_env : user };
  const char* att[1] = { attach };
  int natt = attach ? 1 : 0;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  int rc = send_mail(user, pass, to, 1, NULL, 0,
                     "pdfgen test mail",
                     "Diese Testmail wurde von pdfgen verschickt.\n",
                     att, natt);
  curl_global_cleanup();
  if (rc == 0) printf("gmail_send: test mail sent to %s\n", to[0]);
  return rc;
}
