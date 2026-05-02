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

#ifndef MP_WHISPER_TRANSLATE_H
#define MP_WHISPER_TRANSLATE_H

#include <stdbool.h>

struct mp_log;

enum wt_provider {
    WT_PROVIDER_NONE = 0,
    WT_PROVIDER_GOOGLE,
    WT_PROVIDER_AZURE,
    WT_PROVIDER_OPENAI,
};

// OpenAI-compatible Chat Completions configuration. All strings are owned by
// the caller and copied internally on translator creation.
struct wt_openai_config {
    const char *endpoint;       // full URL, e.g. http://127.0.0.1:11434/v1/chat/completions
    const char *model;          // model name
    const char *api_key;        // optional, may be NULL or "" for ollama
    const char *source_lang;    // may be NULL ("auto")
    const char *target_lang;    // required, e.g. "zh"
    const char *system_prompt;  // optional. If NULL/empty, mpv built-in fallback.
    int context_size;           // accepted for backward compat; ignored.
                                // OpenAI history was removed to make the
                                // translator stateless and safely callable
                                // from multiple worker threads in parallel.
    int timeout_ms;             // per-request timeout; <=0 means default.
                                // Hard-clamped to <= 5000 ms internally so
                                // stop/seek can reliably interrupt within a
                                // bounded delay.
    int max_tokens;             // 0 means: don't send max_tokens; <0 means default (128)
};

// Translator status snapshot, filled by whisper_translator_get_status().
struct wt_status {
    bool enabled;
    bool paused;            // backoff active: skipping requests for retry_after_ms
    int  fail_count;
    int  retry_after_ms;    // remaining ms in current backoff window (0 if not paused)
    char last_error[128];   // short ascii reason; "" if none
};

struct whisper_translator;

// Create a Google/Azure translator. Caller owns the returned pointer.
// source_lang: source language code (e.g. "auto", "en")
// target_lang: target language code (e.g. "zh", "ja", "en")
struct whisper_translator *whisper_translator_create(
    void *talloc_parent, struct mp_log *log,
    enum wt_provider provider,
    const char *source_lang, const char *target_lang);

// Create an OpenAI-compatible translator. Returns NULL on bad config
// (missing endpoint/model/target_lang, unsupported scheme, etc.).
struct whisper_translator *whisper_translator_create_openai(
    void *talloc_parent, struct mp_log *log,
    const struct wt_openai_config *cfg);

// Destroy a translator instance, closing WinHTTP handles.
void whisper_translator_destroy(struct whisper_translator **tr);

// Per-call result of whisper_translate_call(). Caller does NOT need to free
// any field individually; `translated` is a talloc child of the talloc_ctx
// passed to whisper_translate_call(). All numeric fields are value types
// and `error` is an inline buffer.
struct wt_call_result {
    char *translated;        // success: talloc string. failure: NULL.
    int   http_status;       // 0 if no HTTP was issued (e.g. backoff)
    bool  rate_limited;      // 429 / equivalent
    bool  http_issued;       // true iff an HTTP request was actually sent
                             // out the wire (used by callers to distinguish
                             // local short-circuits like backoff / config
                             // errors from real provider calls; only the
                             // latter should consume cost-protection budget)
    int   retry_after_ms;    // parsed from Retry-After header (0 if absent)
    char  error[256];        // short reason on failure ("" on success)
};

// Translate text synchronously into `out`. Safe to call concurrently from
// multiple threads on the same translator: internal mutable state is guarded
// by a per-translator state_lock; the HTTP call itself is performed without
// any lock held so multiple in-flight requests can run in parallel.
//
// `talloc_ctx` parents the returned `translated` string (if any).
void whisper_translate_call(struct whisper_translator *tr,
                            void *talloc_ctx,
                            const char *text,
                            struct wt_call_result *out);

// Acquire / release a refcount on the translator. Use these when keeping a
// translator pointer alive across an unlocked HTTP call while another thread
// might destroy the translator. `whisper_translator_destroy()` is equivalent
// to a final release.
struct whisper_translator *whisper_translator_acquire(
    struct whisper_translator *tr);
void whisper_translator_release(struct whisper_translator **tr);

// Translate text synchronously. Returns a talloc-allocated string on success,
// or NULL on failure. Thin wrapper around whisper_translate_call(); kept for
// backward source compatibility with non-pipelined callers.
char *whisper_translate(struct whisper_translator *tr,
                        void *talloc_ctx, const char *text);

// Fill in a status snapshot. Safe to call from any thread that holds the
// caller's translator lifetime (the caller is responsible for serializing
// access to the translator pointer itself).
void whisper_translator_get_status(struct whisper_translator *tr,
                                   struct wt_status *out);

#endif
