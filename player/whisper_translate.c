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
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#include <windows.h>
#include <winhttp.h>

#include <mpv/client.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/json.h"
#include "misc/node.h"
#include "whisper_translate.h"

// --- struct definition ---

#define WT_DEFAULT_CONTEXT_SIZE   4
#define WT_DEFAULT_TIMEOUT_MS     30000
#define WT_DEFAULT_MAX_TOKENS     128
#define WT_BACKOFF_FAIL_THRESHOLD 5
#define WT_BACKOFF_MS             30000
#define WT_HISTORY_MAX_PAIRS      32

struct wt_history_pair {
    char *src;
    char *dst;
};

struct whisper_translator {
    struct mp_log *log;
    enum wt_provider provider;
    char *source_lang;
    char *target_lang;
    HINTERNET session;          // shared session for google/azure (default proxy)
    HINTERNET session_noproxy;  // dedicated NO_PROXY session for openai (loopback safe)

    // Azure-only
    char *azure_token;
    int64_t azure_token_expires;

    // OpenAI-only
    char *oa_scheme;            // "http" | "https"
    char *oa_host;              // hostname or IP (literal, no brackets)
    int   oa_port;
    char *oa_path;              // request path including query
    bool  oa_is_secure;
    char *oa_model;
    char *oa_api_key;           // may be ""
    char *oa_system_prompt;     // already-rendered final string (or "")
    int   oa_context_size;
    int   oa_timeout_ms;
    int   oa_max_tokens;
    struct wt_history_pair *oa_history;
    int   oa_history_count;
    int   oa_history_head;      // ring head; oldest entry
    int   oa_history_cap;

    // Common: failure / backoff state
    int fail_count;
    int64_t backoff_until_ms;   // GetTickCount64 epoch
    char last_error[128];
};

// --- Helpers ---

static int64_t wt_now_ms(void)
{
    return (int64_t)GetTickCount64();
}

static void wt_set_error(struct whisper_translator *tr, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tr->last_error, sizeof(tr->last_error), fmt, ap);
    va_end(ap);
}

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

// URL-encode a UTF-8 string (talloc allocated)
static char *url_encode(void *talloc_ctx, const char *src)
{
    if (!src)
        return NULL;
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

// --- URL parser ---
//
// Supports:  http://host[:port]/path[?query]
//            https://host[:port]/path[?query]
//            http://[::1]:11434/v1/chat/completions
// Stores results as talloc-children of `parent`.

static bool parse_endpoint_url(void *parent, const char *url,
                               char **out_scheme, char **out_host,
                               int *out_port, char **out_path,
                               bool *out_secure)
{
    if (!url || !url[0])
        return false;
    const char *p = url;
    bool secure;
    if (strncmp(p, "https://", 8) == 0) {
        secure = true;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        secure = false;
        p += 7;
    } else {
        return false;
    }

    const char *host_start;
    const char *host_end;
    int port = secure ? 443 : 80;

    if (*p == '[') {
        // IPv6 literal: [::1]
        host_start = p + 1;
        const char *close = strchr(p, ']');
        if (!close)
            return false;
        host_end = close;
        p = close + 1;
        if (*p == ':') {
            port = atoi(p + 1);
            while (*p && *p != '/' && *p != '?')
                p++;
        }
    } else {
        host_start = p;
        const char *colon = NULL;
        while (*p && *p != '/' && *p != '?') {
            if (*p == ':' && !colon)
                colon = p;
            p++;
        }
        if (colon) {
            host_end = colon;
            port = atoi(colon + 1);
        } else {
            host_end = p;
        }
    }

    if (host_end == host_start)
        return false;
    if (port <= 0 || port > 65535)
        return false;

    const char *path_start = p;
    if (*path_start == '\0' || *path_start == '?') {
        // No explicit path; build "/" + (optional ?query)
        char *path = talloc_strdup(parent, "/");
        if (*path_start == '?')
            path = talloc_asprintf_append(path, "%s", path_start);
        *out_path = path;
    } else {
        *out_path = talloc_strdup(parent, path_start);
    }

    *out_scheme = talloc_strdup(parent, secure ? "https" : "http");
    *out_host   = talloc_strndup(parent, host_start, host_end - host_start);
    *out_port   = port;
    *out_secure = secure;
    return true;
}

// --- WinHTTP request helpers ---

// Send an HTTP request and read the full response body as UTF-8.
// Returns talloc-allocated string, or NULL on failure.
//
// Pass session=NULL to fall back to tr's default session. `secure` only
// matters when path is for https vs http; pass false for plain http.
static char *winhttp_request(void *talloc_ctx, struct mp_log *log,
                             HINTERNET session,
                             const WCHAR *host, int port, bool secure,
                             const WCHAR *verb,
                             const WCHAR *path,
                             const WCHAR *headers,
                             const char *body, size_t body_len,
                             int *out_status)
{
    if (out_status)
        *out_status = 0;

    HINTERNET conn = WinHttpConnect(session, host, port, 0);
    if (!conn) {
        mp_warn(log, "translate: WinHttpConnect failed (%lu)\n", GetLastError());
        return NULL;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, verb, path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        flags);
    if (!req) {
        mp_warn(log, "translate: WinHttpOpenRequest failed (%lu)\n", GetLastError());
        WinHttpCloseHandle(conn);
        return NULL;
    }

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

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(req,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (out_status)
        *out_status = (int)status_code;

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

    char *path_utf8 = talloc_asprintf(NULL, "/m?tl=%s&sl=%s&q=%s", tl, sl, encoded);
    talloc_free(encoded);

    WCHAR *path_wide = utf8_to_wide(NULL, path_utf8);
    talloc_free(path_utf8);
    if (!path_wide)
        return NULL;

    WCHAR *ua = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                L"AppleWebKit/537.36\r\n";

    int status = 0;
    char *html = winhttp_request(NULL, tr->log, tr->session,
                                  L"translate.google.com",
                                  INTERNET_DEFAULT_HTTPS_PORT, true,
                                  L"GET", path_wide, ua, NULL, 0, &status);
    talloc_free(path_wide);

    if (!html) {
        wt_set_error(tr, "google: HTTP %d", status);
        return NULL;
    }

    const char *marker = "class=\"result-container\">";
    char *start = strstr(html, marker);
    if (!start) {
        mp_warn(tr->log, "translate: google response parse failed\n");
        wt_set_error(tr, "google: parse failed");
        talloc_free(html);
        return NULL;
    }
    start += strlen(marker);
    char *end = strchr(start, '<');
    if (!end) {
        wt_set_error(tr, "google: parse failed");
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

static const char *azure_normalize_lang(const char *lang, bool *is_auto)
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
    time_t now = time(NULL);
    if (tr->azure_token && now < tr->azure_token_expires)
        return true;

    talloc_free(tr->azure_token);
    tr->azure_token = NULL;

    int status = 0;
    char *token = winhttp_request(NULL, tr->log, tr->session,
                                   L"edge.microsoft.com",
                                   INTERNET_DEFAULT_HTTPS_PORT, true,
                                   L"GET", L"/translate/auth",
                                   L"User-Agent: Mozilla/5.0\r\n",
                                   NULL, 0, &status);
    if (!token || !token[0]) {
        mp_warn(tr->log, "translate: azure token fetch failed\n");
        wt_set_error(tr, "azure: token fetch HTTP %d", status);
        talloc_free(token);
        return false;
    }

    tr->azure_token = talloc_steal(tr, token);
    tr->azure_token_expires = now + 8 * 60;
    return true;
}

// Extract the value of the first "text":"..." in a JSON string (azure).
static char *azure_json_extract_text(void *talloc_ctx, const char *json)
{
    const char *key = "\"text\":\"";
    char *pos = strstr(json, key);
    if (!pos)
        return NULL;
    pos += strlen(key);
    char *result = talloc_strdup(talloc_ctx, "");
    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
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

    char *headers_utf8 = talloc_asprintf(NULL,
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n",
        tr->azure_token);
    WCHAR *headers_wide = utf8_to_wide(NULL, headers_utf8);
    talloc_free(headers_utf8);

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

    int status = 0;
    char *response = winhttp_request(NULL, tr->log, tr->session,
                                      L"api-edge.cognitive.microsofttranslator.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, true,
                                      L"POST", path_wide, headers_wide,
                                      body, body_len, &status);
    talloc_free(path_wide);
    talloc_free(headers_wide);
    talloc_free(body);

    if (!response) {
        wt_set_error(tr, "azure: HTTP %d", status);
        // Token might be expired, clear cache and retry next call.
        talloc_free(tr->azure_token);
        tr->azure_token = NULL;
        tr->azure_token_expires = 0;
        return NULL;
    }

    char *result = azure_json_extract_text(talloc_ctx, response);
    talloc_free(response);

    if (!result) {
        wt_set_error(tr, "azure: parse failed");
        mp_warn(tr->log, "translate: azure response parse failed\n");
    }
    return result;
}

// --- OpenAI Translate ---

// Built-in fallback system prompt (used only when JSON system_prompt is empty).
// Single placeholder: {target_lang}.
static const char *OA_FALLBACK_PROMPT =
    "You are a professional subtitle translator. Translate the user's "
    "sentence to {target_lang}. Output ONLY the translated sentence, "
    "no quotes, no explanation, no language tag. Previous user/assistant "
    "messages (if any) are prior subtitle translations for context only; "
    "translate only the latest user message and do not continue the "
    "previous sentence.";

static char *oa_render_fallback_prompt(void *parent, const char *target_lang)
{
    const char *tl = target_lang && target_lang[0] ? target_lang : "the target language";
    char *out = talloc_strdup(parent, "");
    const char *p = OA_FALLBACK_PROMPT;
    while (*p) {
        const char *m = strstr(p, "{target_lang}");
        if (!m) {
            out = talloc_asprintf_append(out, "%s", p);
            break;
        }
        out = talloc_strndup_append(out, p, m - p);
        out = talloc_asprintf_append(out, "%s", tl);
        p = m + strlen("{target_lang}");
    }
    return out;
}

// Push (src,dst) to the ring history (oldest evicted automatically).
static void oa_history_push(struct whisper_translator *tr,
                            const char *src, const char *dst)
{
    if (tr->oa_history_cap <= 0)
        return;
    int idx;
    if (tr->oa_history_count < tr->oa_history_cap) {
        idx = (tr->oa_history_head + tr->oa_history_count) % tr->oa_history_cap;
        tr->oa_history_count++;
    } else {
        idx = tr->oa_history_head;
        tr->oa_history_head = (tr->oa_history_head + 1) % tr->oa_history_cap;
        talloc_free(tr->oa_history[idx].src);
        talloc_free(tr->oa_history[idx].dst);
    }
    tr->oa_history[idx].src = talloc_strdup(tr->oa_history, src);
    tr->oa_history[idx].dst = talloc_strdup(tr->oa_history, dst);
}

static void oa_history_clear(struct whisper_translator *tr)
{
    for (int i = 0; i < tr->oa_history_count; i++) {
        int idx = (tr->oa_history_head + i) % tr->oa_history_cap;
        talloc_free(tr->oa_history[idx].src);
        talloc_free(tr->oa_history[idx].dst);
        tr->oa_history[idx].src = NULL;
        tr->oa_history[idx].dst = NULL;
    }
    tr->oa_history_count = 0;
    tr->oa_history_head = 0;
}

// Strip pairs of leading/trailing matching quotes (ASCII " ', curly “”, 「」, 『』).
static void oa_strip_outer_quotes(char *s)
{
    if (!s)
        return;
    size_t len = strlen(s);
    if (len < 2)
        return;
    static const char *pairs[][2] = {
        {"\"", "\""}, {"'", "'"},
        {"\xe2\x80\x9c", "\xe2\x80\x9d"},   // “ ”
        {"\xe2\x80\x98", "\xe2\x80\x99"},   // ‘ ’
        {"\xe3\x80\x8c", "\xe3\x80\x8d"},   // 「 」
        {"\xe3\x80\x8e", "\xe3\x80\x8f"},   // 『 』
    };
    for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++) {
        size_t lo = strlen(pairs[i][0]);
        size_t lc = strlen(pairs[i][1]);
        if (len >= lo + lc &&
            memcmp(s, pairs[i][0], lo) == 0 &&
            memcmp(s + len - lc, pairs[i][1], lc) == 0)
        {
            memmove(s, s + lo, len - lo - lc);
            s[len - lo - lc] = '\0';
            return;
        }
    }
}

// Strip common "Translation: " / "译文：" / etc. prefixes.
static void oa_strip_prefix(char *s)
{
    if (!s)
        return;
    static const char *prefixes[] = {
        "Translation:", "Translated text:", "Translated:",
        "译文：", "译文:", "翻译结果：", "翻译结果:",
        "翻译：", "翻译:", "中文：", "中文:",
        "English:", "Output:", NULL,
    };
    for (int i = 0; prefixes[i]; i++) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(s, prefixes[i], plen) == 0) {
            // Skip prefix and any trailing whitespace.
            char *p = s + plen;
            while (*p == ' ' || *p == '\t')
                p++;
            memmove(s, p, strlen(p) + 1);
            return;
        }
    }
}

// Strip <think>...</think> blocks (DeepSeek-R1 etc.). If <think> appears
// without a closing tag, drop everything from <think> onward — the model is
// streaming reasoning and the visible answer never came.
static void oa_strip_think(char *s)
{
    if (!s)
        return;
    char *open = strstr(s, "<think>");
    while (open) {
        char *close = strstr(open + 7, "</think>");
        if (close) {
            char *after = close + 8;
            memmove(open, after, strlen(after) + 1);
        } else {
            *open = '\0';
            break;
        }
        open = strstr(s, "<think>");
    }
}

// Strip common chat template residue.
static void oa_strip_template(char *s)
{
    if (!s)
        return;
    static const char *tags[] = {
        "<|assistant|>", "<|user|>", "<|system|>",
        "<|im_start|>", "<|im_end|>", NULL,
    };
    for (int i = 0; tags[i]; i++) {
        char *p;
        while ((p = strstr(s, tags[i])) != NULL) {
            size_t tlen = strlen(tags[i]);
            memmove(p, p + tlen, strlen(p + tlen) + 1);
        }
    }
}

static void oa_strip_markdown_fence(char *s)
{
    if (!s)
        return;
    // ```lang\n...\n```
    if (strncmp(s, "```", 3) != 0)
        return;
    char *nl = strchr(s, '\n');
    if (!nl)
        return;
    char *end = strstr(nl + 1, "```");
    if (!end)
        return;
    // Move content between nl+1 and end into s.
    size_t inner = end - (nl + 1);
    memmove(s, nl + 1, inner);
    s[inner] = '\0';
    // Trim trailing whitespace/newlines.
    while (inner > 0 && (s[inner - 1] == '\n' || s[inner - 1] == '\r' ||
                          s[inner - 1] == ' ' || s[inner - 1] == '\t'))
    {
        s[--inner] = '\0';
    }
}

static void oa_trim(char *s)
{
    if (!s)
        return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                        s[len - 1] == '\n' || s[len - 1] == '\r'))
    {
        s[--len] = '\0';
    }
}

static void oa_clean_response(char *s)
{
    if (!s)
        return;
    oa_trim(s);
    oa_strip_think(s);
    oa_strip_template(s);
    oa_trim(s);
    oa_strip_markdown_fence(s);
    oa_trim(s);
    oa_strip_prefix(s);
    oa_trim(s);
    oa_strip_outer_quotes(s);
    oa_trim(s);
}

// Build the chat-completions JSON request body. Returns talloc-allocated
// UTF-8 string, parented to talloc_ctx.
static char *oa_build_request_body(void *talloc_ctx,
                                   struct whisper_translator *tr,
                                   const char *text)
{
    void *tmp = talloc_new(NULL);
    struct mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    // Reparent root's list to tmp so we can free everything together.
    talloc_steal(tmp, root.u.list);

    node_map_add_string(&root, "model", tr->oa_model);
    node_map_add_flag(&root, "stream", false);

    struct mpv_node *messages = node_map_add(&root, "messages",
                                              MPV_FORMAT_NODE_ARRAY);

    // System message
    {
        struct mpv_node *m = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        const char *prompt = tr->oa_system_prompt && tr->oa_system_prompt[0]
            ? tr->oa_system_prompt
            : oa_render_fallback_prompt(tmp, tr->target_lang);
        node_map_add_string(m, "role", "system");
        node_map_add_string(m, "content", prompt);
    }

    // History pairs (oldest first)
    for (int i = 0; i < tr->oa_history_count; i++) {
        int idx = (tr->oa_history_head + i) % tr->oa_history_cap;
        struct wt_history_pair *h = &tr->oa_history[idx];
        if (!h->src || !h->dst)
            continue;
        struct mpv_node *u = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        node_map_add_string(u, "role", "user");
        node_map_add_string(u, "content", h->src);
        struct mpv_node *a = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        node_map_add_string(a, "role", "assistant");
        node_map_add_string(a, "content", h->dst);
    }

    // Current user turn
    {
        struct mpv_node *m = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "role", "user");
        node_map_add_string(m, "content", text);
    }

    // temperature
    node_map_add_double(&root, "temperature", 0.2);

    // max_tokens (omit if 0)
    if (tr->oa_max_tokens > 0)
        node_map_add_int64(&root, "max_tokens", tr->oa_max_tokens);

    char *out = NULL;
    int n = json_write(&out, &root);
    char *result = NULL;
    if (n >= 0 && out) {
        result = talloc_strdup(talloc_ctx, out);
    }
    talloc_free(out);   // json_write uses talloc internally
    talloc_free(tmp);
    return result;
}

// Parse response and extract choices[0].message.content.
// Returns talloc-allocated UTF-8 (parented to talloc_ctx) or NULL.
static char *oa_extract_content(void *talloc_ctx, struct mp_log *log,
                                const char *response)
{
    if (!response)
        return NULL;
    void *tmp = talloc_new(NULL);
    char *mutable_copy = talloc_strdup(tmp, response);
    char *src = mutable_copy;
    struct mpv_node root = {0};
    int r = json_parse(tmp, &root, &src, MAX_JSON_DEPTH);
    if (r < 0) {
        mp_warn(log, "translate: openai response is not JSON\n");
        talloc_free(tmp);
        return NULL;
    }
    char *out = NULL;
    if (root.format == MPV_FORMAT_NODE_MAP) {
        struct mpv_node *choices = node_map_get(&root, "choices");
        if (choices && choices->format == MPV_FORMAT_NODE_ARRAY &&
            choices->u.list && choices->u.list->num > 0)
        {
            struct mpv_node *first = &choices->u.list->values[0];
            if (first->format == MPV_FORMAT_NODE_MAP) {
                struct mpv_node *msg = node_map_get(first, "message");
                if (msg && msg->format == MPV_FORMAT_NODE_MAP) {
                    struct mpv_node *content = node_map_get(msg, "content");
                    if (content && content->format == MPV_FORMAT_STRING &&
                        content->u.string)
                    {
                        out = talloc_strdup(talloc_ctx, content->u.string);
                    }
                }
            }
        }
    }
    talloc_free(tmp);
    return out;
}

static char *translate_openai(struct whisper_translator *tr,
                              void *talloc_ctx, const char *text)
{
    // Backoff check
    int64_t now = wt_now_ms();
    if (tr->backoff_until_ms && now < tr->backoff_until_ms)
        return NULL;
    if (tr->backoff_until_ms && now >= tr->backoff_until_ms) {
        tr->backoff_until_ms = 0;
        tr->fail_count = 0;
        mp_info(tr->log, "translate: openai backoff window ended, retrying\n");
    }

    char *body = oa_build_request_body(NULL, tr, text);
    if (!body) {
        wt_set_error(tr, "openai: build body failed");
        return NULL;
    }
    size_t body_len = strlen(body);

    char *headers_utf8;
    if (tr->oa_api_key && tr->oa_api_key[0]) {
        headers_utf8 = talloc_asprintf(NULL,
            "Content-Type: application/json\r\n"
            "Accept: application/json\r\n"
            "Authorization: Bearer %s\r\n",
            tr->oa_api_key);
    } else {
        headers_utf8 = talloc_strdup(NULL,
            "Content-Type: application/json\r\n"
            "Accept: application/json\r\n");
    }
    WCHAR *headers_wide = utf8_to_wide(NULL, headers_utf8);
    talloc_free(headers_utf8);

    WCHAR *host_wide = utf8_to_wide(NULL, tr->oa_host);
    WCHAR *path_wide = utf8_to_wide(NULL, tr->oa_path);

    int status = 0;
    char *response = winhttp_request(NULL, tr->log, tr->session_noproxy,
                                      host_wide, tr->oa_port, tr->oa_is_secure,
                                      L"POST", path_wide, headers_wide,
                                      body, body_len, &status);

    talloc_free(host_wide);
    talloc_free(path_wide);
    talloc_free(headers_wide);
    talloc_free(body);

    if (!response) {
        wt_set_error(tr, "openai: HTTP %d", status);
        return NULL;
    }

    char *content = oa_extract_content(talloc_ctx, tr->log, response);
    talloc_free(response);

    if (!content || !content[0]) {
        wt_set_error(tr, "openai: empty content");
        if (content)
            talloc_free(content);
        return NULL;
    }

    oa_clean_response(content);
    if (!content[0]) {
        wt_set_error(tr, "openai: empty after clean");
        talloc_free(content);
        return NULL;
    }

    // Push to history (best-effort).
    oa_history_push(tr, text, content);
    return content;
}

// --- Public API ---

struct whisper_translator *whisper_translator_create(
    void *talloc_parent, struct mp_log *log,
    enum wt_provider provider,
    const char *source_lang, const char *target_lang)
{
    if (provider != WT_PROVIDER_GOOGLE && provider != WT_PROVIDER_AZURE)
        return NULL;
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

    WinHttpSetTimeouts(tr->session, 5000, 5000, 10000, 10000);

    mp_info(log, "translate: created %s translator (%s -> %s)\n",
            provider == WT_PROVIDER_GOOGLE ? "google" : "azure",
            tr->source_lang, tr->target_lang);

    return tr;
}

struct whisper_translator *whisper_translator_create_openai(
    void *talloc_parent, struct mp_log *log,
    const struct wt_openai_config *cfg)
{
    if (!cfg || !cfg->endpoint || !cfg->endpoint[0] ||
        !cfg->model || !cfg->model[0] ||
        !cfg->target_lang || !cfg->target_lang[0])
    {
        mp_err(log, "translate: openai config missing endpoint/model/target_lang\n");
        return NULL;
    }

    struct whisper_translator *tr = talloc_zero(talloc_parent,
                                                struct whisper_translator);
    tr->log = log;
    tr->provider = WT_PROVIDER_OPENAI;
    tr->source_lang = talloc_strdup(tr, cfg->source_lang && cfg->source_lang[0]
                                    ? cfg->source_lang : "auto");
    tr->target_lang = talloc_strdup(tr, cfg->target_lang);

    if (!parse_endpoint_url(tr, cfg->endpoint,
                             &tr->oa_scheme, &tr->oa_host,
                             &tr->oa_port, &tr->oa_path,
                             &tr->oa_is_secure))
    {
        mp_err(log, "translate: openai endpoint URL invalid: %s\n", cfg->endpoint);
        talloc_free(tr);
        return NULL;
    }

    tr->oa_model         = talloc_strdup(tr, cfg->model);
    tr->oa_api_key       = talloc_strdup(tr, cfg->api_key ? cfg->api_key : "");
    tr->oa_system_prompt = talloc_strdup(tr,
        cfg->system_prompt ? cfg->system_prompt : "");
    tr->oa_context_size  = cfg->context_size > 0
                            ? cfg->context_size : WT_DEFAULT_CONTEXT_SIZE;
    if (tr->oa_context_size > WT_HISTORY_MAX_PAIRS)
        tr->oa_context_size = WT_HISTORY_MAX_PAIRS;
    tr->oa_timeout_ms    = cfg->timeout_ms > 0
                            ? cfg->timeout_ms : WT_DEFAULT_TIMEOUT_MS;
    if (cfg->max_tokens == 0) {
        tr->oa_max_tokens = 0;
    } else if (cfg->max_tokens < 0) {
        tr->oa_max_tokens = WT_DEFAULT_MAX_TOKENS;
    } else {
        tr->oa_max_tokens = cfg->max_tokens;
    }

    tr->oa_history_cap = tr->oa_context_size;
    tr->oa_history = talloc_zero_array(tr, struct wt_history_pair,
                                        tr->oa_history_cap);

    // Default-proxy session is unused for openai but kept NULL-safe.
    // Use NO_PROXY for loopback / local services.
    WCHAR *ua = utf8_to_wide(NULL, "mpv-whisper/1.0");
    tr->session_noproxy = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0);
    talloc_free(ua);

    if (!tr->session_noproxy) {
        mp_err(log, "translate: WinHttpOpen (NO_PROXY) failed (%lu)\n",
               GetLastError());
        talloc_free(tr);
        return NULL;
    }

    int recv_to = tr->oa_timeout_ms;
    if (recv_to < 1000) recv_to = 1000;
    WinHttpSetTimeouts(tr->session_noproxy, 5000, 5000, 10000, recv_to);

    mp_info(log, "translate: created openai translator (model=%s, %s -> %s, "
                 "context=%d, timeout=%dms, %s://%s:%d%s)\n",
            tr->oa_model, tr->source_lang, tr->target_lang,
            tr->oa_context_size, tr->oa_timeout_ms,
            tr->oa_scheme, tr->oa_host, tr->oa_port, tr->oa_path);
    return tr;
}

void whisper_translator_destroy(struct whisper_translator **tr)
{
    if (!tr || !*tr)
        return;
    struct whisper_translator *t = *tr;
    if (t->session)
        WinHttpCloseHandle(t->session);
    if (t->session_noproxy)
        WinHttpCloseHandle(t->session_noproxy);
    if (t->oa_history && t->oa_history_cap > 0)
        oa_history_clear(t);
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
    case WT_PROVIDER_OPENAI:
        result = translate_openai(tr, talloc_ctx, text);
        break;
    default:
        break;
    }

    if (result) {
        tr->fail_count = 0;
        tr->backoff_until_ms = 0;
        tr->last_error[0] = '\0';
    } else {
        tr->fail_count++;
        if (tr->fail_count <= 3 || tr->fail_count % 10 == 0)
            mp_warn(tr->log, "translate: failed (count=%d, last=%s)\n",
                    tr->fail_count,
                    tr->last_error[0] ? tr->last_error : "?");

        // Backoff applies only to the OpenAI provider (where each failure is
        // an outbound API call that may be rate-limited or expensive).
        if (tr->provider == WT_PROVIDER_OPENAI &&
            tr->fail_count >= WT_BACKOFF_FAIL_THRESHOLD &&
            !tr->backoff_until_ms)
        {
            tr->backoff_until_ms = wt_now_ms() + WT_BACKOFF_MS;
            mp_warn(tr->log, "translate: openai paused for %dms after %d "
                             "failures\n", WT_BACKOFF_MS, tr->fail_count);
        }
    }

    return result;
}

void whisper_translator_get_status(struct whisper_translator *tr,
                                   struct wt_status *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!tr)
        return;
    out->enabled = true;
    out->fail_count = tr->fail_count;
    int64_t now = wt_now_ms();
    if (tr->backoff_until_ms && now < tr->backoff_until_ms) {
        out->paused = true;
        out->retry_after_ms = (int)(tr->backoff_until_ms - now);
    }
    snprintf(out->last_error, sizeof(out->last_error), "%s", tr->last_error);
}
