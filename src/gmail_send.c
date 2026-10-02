/*
 * gmail_send.c — Send an email through Gmail (SMTP) with libcurl.
 *
 * Supports: multiple To/Cc recipients, subject, plain-text body,
 *           and any number of file attachments.
 *
 * IMPORTANT — Gmail credentials:
 *   Google blocks plain account passwords for third-party apps.
 *   1. Enable 2-Step Verification on your Google account.
 *   2. Create an "App Password": Google Account -> Security -> App passwords.
 *   3. Use that 16-character password below (spaces optional).
 *
 * Build:
 *   gcc gmail_send.c -o gmail_send -lcurl
 *   (g++ works identically for C++ projects)
 */

#include "gmail_send.h"

#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>   /* access(), R_OK -- POSIX; ohne diesen Header
                         nimmt C99 keine implizite Deklaration mehr an
                         (clang: "call to undeclared function 'access'"). */
#endif
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

#include "pdfgen_host.h"   /* PDFGEN_LOGx -- Ziel bestimmt der Gast */

/* release consistency check (see gmail_send.h) */
#ifndef PDFGEN_GMAIL_SEND_API
#error "stale gmail_send.h: replace ALL pdfgen source files from the same release."
#elif PDFGEN_GMAIL_SEND_API != 5
#error "version mismatch in gmail_send.h: replace ALL pdfgen source files from the same release."
#endif

/* ------------------------------------------------------------------ */
/*  Configuration — edit these                                         */
/* ------------------------------------------------------------------ */
#ifndef SMTP_URL
#define SMTP_URL     "smtps://smtp.gmail.com:465"   /* implicit TLS  */
#endif
/* Both can also be set at build time instead of editing this file:
 *   cmake -B build -DPDFGEN_MAIL_FROM=me@gmail.com \
 *                  -DPDFGEN_MAIL_APP_PASSWORD=xxxxxxxxxxxxxxxx           */
#ifndef FROM_ADDR
#define FROM_ADDR    "@gmail.com"
#endif
#ifndef APP_PASSWORD
#define APP_PASSWORD "app_password"             /* 16-char app password */
#endif

/* ------------------------------------------------------------------ */
/*  Reusable send function                                             */
/* ------------------------------------------------------------------ */
int send_mail_ex(const smtp_transport *tr,
                 const char *from,
                 const char *password,
                 const char **to,   int n_to,
                 const char **cc,   int n_cc,
                 const char *subject,
                 const char *body_text,
                 const char **files, int n_files)
{
    int i;
    char url[512];
    smtp_transport gmail = { "smtp.gmail.com", 465, 1, SMTP_AUTH_PASSWORD };
    if (!tr) tr = &gmail;
    PDFGEN_LOGD("send_mail: via %s:%d (%s%s) von %s, %d an, %d cc, %d Anhaenge",
                tr->host, tr->port, tr->ssl ? "smtps" : "starttls",
                tr->auth == SMTP_AUTH_XOAUTH2 ? ", xoauth2" : "",
                from, n_to, n_cc, n_files);
    CURL *curl = curl_easy_init();
    if (!curl) {
        PDFGEN_LOGE("curl_easy_init failed");
        return 1;
    }

    struct curl_slist *recipients = NULL;
    struct curl_slist *headers    = NULL;
    curl_mime *mime = NULL;
    char buf[1024];

    /* --- connection & authentication --- */
    snprintf(url, sizeof url, "%s://%s:%d",
             tr->ssl ? "smtps" : "smtp", tr->host, tr->port);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, from);
    switch (tr->auth) {
    case SMTP_AUTH_XOAUTH2:
        /* OAuth2 bearer: curl builds the XOAUTH2 SASL string itself */
        curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, password);
#ifdef CURLOPT_LOGIN_OPTIONS
        curl_easy_setopt(curl, CURLOPT_LOGIN_OPTIONS, "AUTH=XOAUTH2");
#endif
        break;
    case SMTP_AUTH_PASSWORD:
    default:
        /* unknown future values fall back to password auth instead of
         * failing, so an older lib behind a newer config stays usable */
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
        break;
    }
    /* STARTTLS on 587 and implicit TLS on 465 both end up encrypted;
     * CURLUSESSL_ALL makes plain 587 upgrade mandatory. */
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    /* Without timeouts, an unreachable SMTP server (firewalled network,
     * offline machine) makes curl hang forever. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);


    /* --- envelope (who actually receives the mail) --- */
    snprintf(buf, sizeof buf, "<%s>", from);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, buf);

    for (i = 0; i < n_to; i++) {
        snprintf(buf, sizeof buf, "<%s>", to[i]);
        recipients = curl_slist_append(recipients, buf);
    }
    for (i = 0; i < n_cc; i++) {
        snprintf(buf, sizeof buf, "<%s>", cc[i]);
        recipients = curl_slist_append(recipients, buf);
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);


    /* --- visible headers (what the mail client displays) --- */
    snprintf(buf, sizeof buf, "From: %s", from);
    headers = curl_slist_append(headers, buf);

    /* Join To: addresses into one header line */
    {
        char line[1024] = "To: ";
        for (i = 0; i < n_to; i++) {
            if (i) strncat(line, ", ", sizeof line - strlen(line) - 1);
            strncat(line, to[i], sizeof line - strlen(line) - 1);
        }
        headers = curl_slist_append(headers, line);
    }
    if (n_cc > 0) {
        char line[1024] = "Cc: ";
        for (i = 0; i < n_cc; i++) {
            if (i) strncat(line, ", ", sizeof line - strlen(line) - 1);
            strncat(line, cc[i], sizeof line - strlen(line) - 1);
        }
        headers = curl_slist_append(headers, line);
    }
    snprintf(buf, sizeof buf, "Subject: %s", subject);
    headers = curl_slist_append(headers, buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);


    /* --- MIME body: text part + attachments --- */
    mime = curl_mime_init(curl);

    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_data(part, body_text, CURL_ZERO_TERMINATED);
    curl_mime_type(part, "text/plain; charset=utf-8");

    for (i = 0; i < n_files; i++) {
        if (!files[i]) continue;
        part = curl_mime_addpart(mime);
        if (curl_mime_filedata(part, files[i]) != CURLE_OK) {
            PDFGEN_LOGW("cannot attach %s", files[i]);
            continue;
        }
        /* filename shown to the recipient = basename of the path */
        const char *b1 = strrchr(files[i], '/');
        const char *b2 = strrchr(files[i], '\\');
        const char *base = b1 > b2 ? b1 : b2;
        curl_mime_filename(part, base ? base + 1 : files[i]);
        curl_mime_encoder(part, "base64");
        /* Optional: set an explicit type, otherwise it's guessed:
           curl_mime_type(part, "application/pdf"); */
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    /* Nur zum Debuggen einschalten: VERBOSE schreibt den kompletten
     * SMTP-Dialog auf stderr -- INKLUSIVE "AUTH PLAIN <base64>", also das
     * Token praktisch im Klartext. Deshalb nie dauerhaft an.
     * curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); */

    /* --- send --- */

#ifdef __ANDROID__
    /* CA-Verzeichnis: reine Android-Pfade -- auf Linux/Windows nutzt
       curl seine eingebauten Standardorte. access()/R_OK sind POSIX und
       existieren unter mingw nicht; deshalb ist der Block hier statt
       hinter #ifndef _WIN32 gleich komplett Android-exklusiv. */
    const char *p = "/apex/com.android.conscrypt/cacerts";
    if (access(p, R_OK) != 0) p = "/system/etc/security/cacerts";
    curl_easy_setopt(curl, CURLOPT_CAPATH, p);
#endif

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        PDFGEN_LOGE("send failed: %s", curl_easy_strerror(res));
    }
    else {
        PDFGEN_LOGD("mail sent successfully");
    }

    /* --- cleanup --- */
    curl_slist_free_all(recipients);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : 1;
}

const char *pdfgen_mail_from(void)     { return FROM_ADDR; }
const char *pdfgen_mail_password(void) { return APP_PASSWORD; }
void pdfgen_mail_global_init(void)     { curl_global_init(CURL_GLOBAL_ALL); }
void pdfgen_mail_global_cleanup(void)  { curl_global_cleanup(); }

int send_mail(const char *from,
              const char *password,
              const char **to,   int n_to,
              const char **cc,   int n_cc,
              const char *subject,
              const char *body_text,
              const char **files, int n_files)
{
    return send_mail_ex(NULL, from, password, to, n_to, cc, n_cc,
                        subject, body_text, files, n_files);
}

int send_test_mail(const char* attachment)
{
    curl_global_init(CURL_GLOBAL_ALL);
    /* Counts MUST match the array sizes — passing 2 with one-element
     * arrays (as in an earlier draft) reads past the array: undefined
     * behavior, and whatever garbage pointer follows ends up as an SMTP
     * envelope recipient. */
    // const char *to[] = { "email1@gmail.com", "email2@gmail.com" };
    // const char *files[] = { "report.pdf", "photo.jpg" };
    const char *to[]    = { "@gmail.com" };
    int to_len=sizeof(to)/sizeof(to[0]);
    const char *cc[]    = { "@gmail.com" };
    int cc_len=sizeof(cc)/sizeof(cc[0]);
    const char *files[] = { attachment };            /* may be NULL */
    int files_len=sizeof(files)/sizeof(files[0]);
    int n_files = attachment ? 1 : 0;
    int rc = send_mail(FROM_ADDR, APP_PASSWORD,
                       to,    to_len,
                       cc,    cc_len,
                       "Bewerbung als Berufskraftfahrer",
                       "Sehr geehrte Damen und Herren,\r\n\r\nhiermit Bewerbe ich mich auf die Ausgeschriebene Stelle als Berufskraftfahrer. Anbei erhalten Sie meinen Lebenslauf und die formale Bewerbung. Für das weitere Vorgehen kontaktieren Sie mich gerne telefonisch. Vielen Dank für Ihr Interesse. \r\n\r\nMit freundlichen Grüßen\r\nVorname Nachname",
                       files, files_len);
    curl_global_cleanup();
    return rc;
}
