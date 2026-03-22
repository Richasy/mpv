/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include <windows.h>
#include <winhttp.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "whisper_translate.h"

// --- struct definition ---

struct whisper_translator {
    struct mp_log *log;
    enum wt_provider provider;
    char *source_lang;
    char *target_lang;
    HINTERNET session;
    char *azure_token;
    int64_t azure_token_expires;
    int fail_count;
};

// --- Helpers ---

// Convert UTF-8 to wide string (talloc allocated)
static WCHAR *utf8_to_wide(void *talloc_ctx, const char *utf8)
{
    if (!utf8 || !utf8[0])
        return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0)
        return NULL;
    WCHAR *wide = talloc_array(talloc_ctx, WCHAR, len);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
    return wide;
}

// Convert wide string to UTF-8 (talloc allocated)
static char *wide_to_utf8(void *talloc_ctx, const WCHAR *wide)
{
    if (!wide || !wide[0])
        return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0)
        return NULL;
    char *utf8 = talloc_array(talloc_ctx, char, len);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, len, NULL, NULL);
    return utf8;
}

// URL-encode a UTF-8 string (talloc allocated)
static char *url_encode(void *talloc_ctx, const char *src)
{
    if (!src)
        return NULL;

    // Worst case: every byte becomes %XX (3x expansion)
    size_t src_len = strlen(src);
    char *buf = talloc_array(talloc_ctx, char, src_len * 3 + 1);
    char *dst = buf;

    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            *dst++ = c;
        } else if (c == ' ') {
            *dst++ = '+';
        } else {
            snprintf(dst, 4, "%%%02X", c);
            dst += 3;
        }
    }
    *dst = '\0';
    return buf;
}

// Decode common HTML entities in-place
static void html_entity_decode(char *s)
{
    if (!s)
        return;

    struct { const char *entity; char replacement; } entities[] = {
        {"&amp;",  '&'},
        {"&lt;",   '<'},
        {"&gt;",   '>'},
        {"&quot;", '"'},
        {"&#39;",  '\''},
    };

    char *read = s;
    char *write = s;
    while (*read) {
        bool found = false;
        for (int i = 0; i < 5; i++) {
            size_t elen = strlen(entities[i].entity);
            if (strncmp(read, entities[i].entity, elen) == 0) {
                *write++ = entities[i].replacement;
                read += elen;
                found = true;
                break;
            }
        }
        if (!found)
            *write++ = *read++;
    }
    *write = '\0';
}

// --- WinHTTP request helpers ---

// Send an HTTP request and read the full response body as UTF-8.
// Returns talloc-allocated string, or NULL on failure.
static char *winhttp_request(void *talloc_ctx, struct mp_log *log,
                             HINTERNET session,
                             const WCHAR *host, int port,
                             const WCHAR *verb,
                             const WCHAR *path,
                             const WCHAR *headers,
                             const char *body, size_t body_len)
{
    HINTERNET conn = WinHttpConnect(session, host, port, 0);
    if (!conn) {
        mp_warn(log, "translate: WinHttpConnect failed (%lu)\n", GetLastError());
        return NULL;
    }

    HINTERNET req = WinHttpOpenRequest(conn, verb, path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!req) {
        mp_warn(log, "translate: WinHttpOpenRequest failed (%lu)\n", GetLastError());
        WinHttpCloseHandle(conn);
        return NULL;
    }

    // Add headers if provided
    if (headers && headers[0]) {
        WinHttpAddRequestHeaders(req, headers, (DWORD)-1,
                                  WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok;
    if (body && body_len > 0) {
        ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)body, (DWORD)body_len,
                                 (DWORD)body_len, 0);
    } else {
        ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    if (!ok) {
        mp_warn(log, "translate: WinHttpSendRequest failed (%lu)\n", GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return NULL;
    }

    ok = WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        mp_warn(log, "translate: WinHttpReceiveResponse failed (%lu)\n", GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return NULL;
    }

    // Check HTTP status
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(req,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);

    // Read response body
    char *result = talloc_strdup(talloc_ctx, "");
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(req, &bytes_available) && bytes_available > 0) {
        char *chunk = talloc_array(NULL, char, bytes_available + 1);
        DWORD bytes_read = 0;
        if (WinHttpReadData(req, chunk, bytes_available, &bytes_read)) {
            chunk[bytes_read] = '\0';
            result = talloc_asprintf_append(result, "%s", chunk);
        }
        talloc_free(chunk);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);

    if (status_code >= 400) {
        mp_warn(log, "translate: HTTP %lu error\n", status_code);
        talloc_free(result);
        return NULL;
    }

    return result;
}

// --- Google Translate ---

static const char *google_normalize_lang(const char *lang)
{
    if (!lang)
        return "auto";
    if (strcmp(lang, "zh") == 0)
        return "zh-CN";
    return lang;
}

static char *translate_google(struct whisper_translator *tr,
                              void *talloc_ctx, const char *text)
{
    const char *sl = google_normalize_lang(tr->source_lang);
    const char *tl = google_normalize_lang(tr->target_lang);
    char *encoded = url_encode(NULL, text);
    if (!encoded)
        return NULL;

    // Build path: /m?tl=xx&sl=xx&q=encoded_text
    char *path_utf8 = talloc_asprintf(NULL, "/m?tl=%s&sl=%s&q=%s", tl, sl, encoded);
    talloc_free(encoded);

    WCHAR *path_wide = utf8_to_wide(NULL, path_utf8);
    talloc_free(path_utf8);
    if (!path_wide)
        return NULL;

    WCHAR *ua = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                L"AppleWebKit/537.36\r\n";

    char *html = winhttp_request(NULL, tr->log, tr->session,
                                  L"translate.google.com",
                                  INTERNET_DEFAULT_HTTPS_PORT,
                                  L"GET", path_wide, ua,
                                  NULL, 0);
    talloc_free(path_wide);

    if (!html)
        return NULL;

    // Parse: class="result-container">TRANSLATED_TEXT<
    const char *marker = "class=\"result-container\">";
    char *start = strstr(html, marker);
    if (!start) {
        mp_warn(tr->log, "translate: google response parse failed\n");
        talloc_free(html);
        return NULL;
    }
    start += strlen(marker);
    char *end = strchr(start, '<');
    if (!end) {
        talloc_free(html);
        return NULL;
    }

    size_t len = end - start;
    char *result = talloc_strndup(talloc_ctx, start, len);
    talloc_free(html);

    html_entity_decode(result);
    return result;
}

// --- Azure Translate ---

static const char *azure_normalize_lang(const char *lang,
                                        bool *is_auto)
{
    *is_auto = false;
    if (!lang || strcmp(lang, "auto") == 0) {
        *is_auto = true;
        return "";
    }
    if (strcmp(lang, "zh") == 0)
        return "zh-Hans";
    if (strcmp(lang, "zh-TW") == 0)
        return "zh-Hant";
    return lang;
}

static bool azure_refresh_token(struct whisper_translator *tr)
{
    // Check if cached token is still valid (8 minute expiry)
    time_t now = time(NULL);
    if (tr->azure_token && now < tr->azure_token_expires)
        return true;

    talloc_free(tr->azure_token);
    tr->azure_token = NULL;

    char *token = winhttp_request(NULL, tr->log, tr->session,
                                   L"edge.microsoft.com",
                                   INTERNET_DEFAULT_HTTPS_PORT,
                                   L"GET",
                                   L"/translate/auth",
                                   L"User-Agent: Mozilla/5.0\r\n",
                                   NULL, 0);
    if (!token || !token[0]) {
        mp_warn(tr->log, "translate: azure token fetch failed\n");
        talloc_free(token);
        return false;
    }

    tr->azure_token = talloc_steal(tr, token);
    tr->azure_token_expires = now + 8 * 60;  // 8 minutes
    return true;
}

// Extract the value of the first "text":"..." in a JSON string.
// Returns talloc-allocated string or NULL.
static char *json_extract_text(void *talloc_ctx, const char *json)
{
    const char *key = "\"text\":\"";
    char *pos = strstr(json, key);
    if (!pos)
        return NULL;
    pos += strlen(key);

    // Find closing quote, handling escaped quotes
    char *result = talloc_strdup(talloc_ctx, "");
    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;  // skip backslash
            switch (*pos) {
            case '"':  result = talloc_asprintf_append(result, "\""); break;
            case '\\': result = talloc_asprintf_append(result, "\\"); break;
            case 'n':  result = talloc_asprintf_append(result, "\n"); break;
            case 't':  result = talloc_asprintf_append(result, "\t"); break;
            case '/':  result = talloc_asprintf_append(result, "/"); break;
            default:
                result = talloc_asprintf_append(result, "\\%c", *pos);
                break;
            }
            pos++;
        } else {
            char c[2] = {*pos, '\0'};
            result = talloc_asprintf_append(result, "%s", c);
            pos++;
        }
    }
    return result;
}

static char *translate_azure(struct whisper_translator *tr,
                             void *talloc_ctx, const char *text)
{
    if (!azure_refresh_token(tr))
        return NULL;

    bool is_auto = false;
    const char *tl = azure_normalize_lang(tr->target_lang, &is_auto);
    bool src_auto = false;
    const char *sl = azure_normalize_lang(tr->source_lang, &src_auto);

    // Build path
    char *path_utf8;
    if (src_auto) {
        path_utf8 = talloc_asprintf(NULL,
            "/translate?api-version=3.0&to=%s", tl);
    } else {
        path_utf8 = talloc_asprintf(NULL,
            "/translate?api-version=3.0&to=%s&from=%s", tl, sl);
    }

    WCHAR *path_wide = utf8_to_wide(NULL, path_utf8);
    talloc_free(path_utf8);
    if (!path_wide)
        return NULL;

    // Build headers
    char *headers_utf8 = talloc_asprintf(NULL,
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n",
        tr->azure_token);
    WCHAR *headers_wide = utf8_to_wide(NULL, headers_utf8);
    talloc_free(headers_utf8);

    // Build JSON body: [{"Text":"..."}]
    // Escape special JSON characters in text
    char *escaped = talloc_strdup(NULL, "");
    for (const char *p = text; *p; p++) {
        switch (*p) {
        case '"':  escaped = talloc_asprintf_append(escaped, "\\\""); break;
        case '\\': escaped = talloc_asprintf_append(escaped, "\\\\"); break;
        case '\n': escaped = talloc_asprintf_append(escaped, "\\n"); break;
        case '\r': escaped = talloc_asprintf_append(escaped, "\\r"); break;
        case '\t': escaped = talloc_asprintf_append(escaped, "\\t"); break;
        default:
            escaped = talloc_asprintf_append(escaped, "%c", *p);
            break;
        }
    }
    char *body = talloc_asprintf(NULL, "[{\"Text\":\"%s\"}]", escaped);
    talloc_free(escaped);
    size_t body_len = strlen(body);

    char *response = winhttp_request(NULL, tr->log, tr->session,
                                      L"api-edge.cognitive.microsofttranslator.com",
                                      INTERNET_DEFAULT_HTTPS_PORT,
                                      L"POST", path_wide, headers_wide,
                                      body, body_len);
    talloc_free(path_wide);
    talloc_free(headers_wide);
    talloc_free(body);

    if (!response) {
        // Token might be expired, clear cache and retry once
        talloc_free(tr->azure_token);
        tr->azure_token = NULL;
        tr->azure_token_expires = 0;
        return NULL;
    }

    char *result = json_extract_text(talloc_ctx, response);
    talloc_free(response);

    if (!result)
        mp_warn(tr->log, "translate: azure response parse failed\n");

    return result;
}

// --- Public API ---

struct whisper_translator *whisper_translator_create(
    void *talloc_parent, struct mp_log *log,
    enum wt_provider provider,
    const char *source_lang, const char *target_lang)
{
    struct whisper_translator *tr = talloc_zero(talloc_parent,
                                                struct whisper_translator);
    tr->log = log;
    tr->provider = provider;
    tr->source_lang = talloc_strdup(tr, source_lang ? source_lang : "auto");
    tr->target_lang = talloc_strdup(tr, target_lang);

    WCHAR *ua = utf8_to_wide(NULL, "mpv-whisper/1.0");
    tr->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                               0);
    talloc_free(ua);

    if (!tr->session) {
        mp_err(log, "translate: WinHttpOpen failed (%lu)\n", GetLastError());
        talloc_free(tr);
        return NULL;
    }

    // Set reasonable timeouts (connect=5s, send=10s, recv=10s)
    WinHttpSetTimeouts(tr->session, 5000, 5000, 10000, 10000);

    mp_info(log, "translate: created %s translator (%s -> %s)\n",
            provider == WT_PROVIDER_GOOGLE ? "google" : "azure",
            tr->source_lang, tr->target_lang);

    return tr;
}

void whisper_translator_destroy(struct whisper_translator **tr)
{
    if (!tr || !*tr)
        return;
    struct whisper_translator *t = *tr;
    if (t->session)
        WinHttpCloseHandle(t->session);
    // azure_token and lang strings are talloc children, freed automatically
    talloc_free(t);
    *tr = NULL;
}

char *whisper_translate(struct whisper_translator *tr,
                        void *talloc_ctx, const char *text)
{
    if (!tr || !text || !text[0])
        return NULL;

    char *result = NULL;
    switch (tr->provider) {
    case WT_PROVIDER_GOOGLE:
        result = translate_google(tr, talloc_ctx, text);
        break;
    case WT_PROVIDER_AZURE:
        result = translate_azure(tr, talloc_ctx, text);
        break;
    default:
        break;
    }

    if (result) {
        tr->fail_count = 0;
    } else {
        tr->fail_count++;
        // Rate-limit warnings: only log every 10th failure
        if (tr->fail_count <= 3 || tr->fail_count % 10 == 0)
            mp_warn(tr->log, "translate: failed (count=%d)\n", tr->fail_count);
    }

    return result;
}
