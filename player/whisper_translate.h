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

struct mp_log;

enum wt_provider {
    WT_PROVIDER_NONE = 0,
    WT_PROVIDER_GOOGLE,
    WT_PROVIDER_AZURE,
};

struct whisper_translator;

// Create a translator instance. Caller owns the returned pointer.
// source_lang: source language code (e.g. "auto", "en")
// target_lang: target language code (e.g. "zh", "ja", "en")
struct whisper_translator *whisper_translator_create(
    void *talloc_parent, struct mp_log *log,
    enum wt_provider provider,
    const char *source_lang, const char *target_lang);

// Destroy a translator instance, closing WinHTTP handles.
void whisper_translator_destroy(struct whisper_translator **tr);

// Translate text synchronously. Returns a talloc-allocated string on success,
// or NULL on failure. Safe to call from the lookahead background thread.
char *whisper_translate(struct whisper_translator *tr,
                        void *talloc_ctx, const char *text);

#endif
