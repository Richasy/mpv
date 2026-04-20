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

/*
 * Whisper lookahead pipeline (faster-whisper.exe backend).
 *
 * High level:
 *   - User supplies fastwhisper_dir pointing at the Purfview
 *     whisper-standalone-win folder (faster-whisper.exe + cu* dlls).
 *   - We spawn faster-whisper.exe as a subprocess, feed it audio,
 *     parse the SRT it writes, and inject each segment into a
 *     dedicated subtitle stream of the primary demuxer via
 *     demuxer_feed_af_sub().
 *
 * Two operating modes selected automatically from the source URL:
 *
 *   LOCAL_FILE  - source is a local path. Single subprocess takes
 *                 the source file directly; one SRT covers the whole
 *                 video. All segments are injected once transcription
 *                 finishes.
 *
 *   NETWORK     - source is a network URL. Not yet implemented in this
 *                 revision; init returns an explanatory error.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "mpv_talloc.h"

#include "common/msg.h"
#include "common/common.h"
#include "common/global.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/subprocess.h"
#include "osdep/threads.h"
#include "misc/bstr.h"
#include "misc/thread_tools.h"
#include "stream/stream.h"
#include "demux/demux.h"
#include "demux/stheader.h"

#include "core.h"
#include "whisper_srt.h"
#include "whisper_translate.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ----------------------------------------------------------------------------
// Internal state
// ----------------------------------------------------------------------------

enum wl_mode {
    WL_MODE_LOCAL_FILE = 0,
    WL_MODE_NETWORK    = 1,
};

struct whisper_lookahead {
    struct MPContext *mpctx;
    struct mp_log *log;

    // Captured at start (immutable on background threads after init)
    char *filename;             // full path or URL (user-expanded)
    char *whisper_opts;         // raw opts string
    enum wl_mode mode;

    // Parsed opts
    char *fastwhisper_dir;
    char *fastwhisper_exe;
    char *model;
    char *device;
    char *language;
    int chunk_sec;
    char *initial_prompt;

    struct whisper_translator *translator;

    // ASS injection target
    struct sh_stream *primary_stream;
    struct demuxer *primary_demuxer;

    // Lifecycle
    struct mp_cancel *cancel;          // parent cancel
    struct mp_cancel *child_cancel;    // for in-flight subprocess

    mp_thread init_thread;
    bool init_thread_valid;
    atomic_bool init_done;
    bool init_ok;

    mp_thread work_thread;
    bool work_thread_valid;
    atomic_int terminate;

    bool track_selected;

    int subtitles_injected;
    int translations_injected;
    int chunks_processed;

    // Chunked work-loop state (LOCAL mode)
    mp_mutex seek_mutex;
    bool seek_pending;
    double seek_target;
    int64_t last_injected_end_ms;  // de-dup across chunk boundaries
};

// ----------------------------------------------------------------------------
// Subtitle injection helpers
// ----------------------------------------------------------------------------

static void inject_one(struct whisper_lookahead *wl,
                       const char *text, double pts, double dur)
{
    if (!wl->primary_stream || !wl->primary_demuxer || !text || !text[0])
        return;

    char *sub_text = NULL;
    if (wl->translator) {
        char *translated = whisper_translate(wl->translator, wl, text);
        if (translated) {
            sub_text = talloc_asprintf(wl,
                "{\\fs72\\c&H00FFFFFF&\\3c&H00000000&\\bord3}%s"
                "\\N{\\fs48\\c&H00E0FFFF&\\3c&H00000000&\\bord2}%s",
                translated, text);
            wl->translations_injected++;
            MP_INFO(wl, "translated #%d: %.40s%s\n",
                    wl->translations_injected, translated,
                    strlen(translated) > 40 ? "..." : "");
            talloc_free(translated);
        }
    }
    if (!sub_text)
        sub_text = talloc_strdup(wl, text);

    char *ass_line = talloc_asprintf(wl,
        "%d,0,Default,,0,0,0,,%s",
        wl->subtitles_injected, sub_text);
    talloc_free(sub_text);

    size_t ass_len = strlen(ass_line);
    struct demux_packet *dp = new_demux_packet_from(
        wl->primary_demuxer->packet_pool, (void *)ass_line, ass_len);
    if (dp) {
        dp->pts = pts;
        dp->dts = pts;
        dp->duration = dur;
        dp->sub_duration = dur;
        demuxer_feed_af_sub(wl->primary_stream, dp);
        wl->subtitles_injected++;
        MP_INFO(wl, "subtitle #%d @ %.3f (dur=%.1f): %.40s%s\n",
                wl->subtitles_injected, pts, dur, text,
                strlen(text) > 40 ? "..." : "");
        mp_wakeup_core(wl->mpctx);
    }
    talloc_free(ass_line);
}

// ----------------------------------------------------------------------------
// Temp directory helpers
// ----------------------------------------------------------------------------

static char *make_temp_outdir(void *tctx, struct mp_log *log)
{
    const char *tmp = NULL;
#ifdef _WIN32
    static char winbuf[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof(winbuf), winbuf);
    if (n > 0 && n < sizeof(winbuf)) {
        if (winbuf[n - 1] == '\\' || winbuf[n - 1] == '/')
            winbuf[n - 1] = '\0';
        tmp = winbuf;
    }
#endif
    if (!tmp) tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = "/tmp";

    static atomic_int counter;
#ifdef _WIN32
    static const char SEP = '\\';
#else
    static const char SEP = '/';
#endif
    for (int attempt = 0; attempt < 20; attempt++) {
        unsigned pid;
#ifdef _WIN32
        pid = (unsigned)GetCurrentProcessId();
#else
        pid = (unsigned)getpid();
#endif
        int seq = atomic_fetch_add(&counter, 1);
        char *path = talloc_asprintf(tctx, "%s%cmpv-fw-%u-%d-%d", tmp,
                                     SEP, pid, seq, rand());
#ifdef _WIN32
        if (CreateDirectoryA(path, NULL))
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            mp_warn(log, "make_temp_outdir: CreateDirectory failed for '%s' "
                    "(err=%lu)\n", path, GetLastError());
            talloc_free(path);
            return NULL;
        }
#else
        if (mkdir(path, 0700) == 0)
            return path;
#endif
        talloc_free(path);
    }
    mp_err(log, "make_temp_outdir: failed after 20 attempts\n");
    return NULL;
}

static void rmtree_quiet(struct mp_log *log, const char *dir)
{
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    char *pat = talloc_asprintf(NULL, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &ffd);
    talloc_free(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(ffd.cFileName, ".") == 0 ||
                strcmp(ffd.cFileName, "..") == 0)
                continue;
            char *full = talloc_asprintf(NULL, "%s\\%s", dir, ffd.cFileName);
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rmtree_quiet(log, full);
            else
                DeleteFileA(full);
            talloc_free(full);
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
#else
    (void)log; (void)dir;
#endif
}

// ----------------------------------------------------------------------------
// Subprocess invocation
// ----------------------------------------------------------------------------

struct stderr_capture {
    struct mp_log *log;
    char buf[1024];
    size_t fill;
};

static void on_child_stderr(void *ctx, char *data, size_t size)
{
    struct stderr_capture *c = ctx;
    if (size == 0) {
        if (c->fill > 0) {
            c->buf[c->fill] = '\0';
            mp_verbose(c->log, "fw.stderr: %s\n", c->buf);
            c->fill = 0;
        }
        return;
    }
    while (size > 0) {
        size_t take = size;
        if (c->fill + take >= sizeof(c->buf) - 1)
            take = sizeof(c->buf) - 1 - c->fill;
        memcpy(c->buf + c->fill, data, take);
        c->fill += take;
        data += take;
        size -= take;
        char *nl;
        while ((nl = memchr(c->buf, '\n', c->fill))) {
            *nl = '\0';
            char *line = c->buf;
            if (nl > line && nl[-1] == '\r') nl[-1] = '\0';
            if (line[0])
                mp_info(c->log, "fw: %s\n", line);
            size_t consumed = (nl - c->buf) + 1;
            memmove(c->buf, c->buf + consumed, c->fill - consumed);
            c->fill -= consumed;
        }
        if (c->fill >= sizeof(c->buf) - 1) {
            c->buf[c->fill] = '\0';
            mp_info(c->log, "fw: %s\n", c->buf);
            c->fill = 0;
        }
    }
}

static int run_faster_whisper(struct whisper_lookahead *wl,
                              void *tctx,
                              const char *audio_path,
                              const char *out_dir,
                              double clip_t0_sec,
                              double clip_t1_sec,
                              char **out_srt_path)
{
    *out_srt_path = NULL;

    char **args = NULL;
    int n = 0;
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, wl->fastwhisper_exe));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--model"));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, wl->model));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--device"));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, wl->device));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--output_dir"));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, out_dir));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--output_format"));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "srt"));
    if (wl->language && wl->language[0] &&
        strcmp(wl->language, "auto") != 0) {
        MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--language"));
        MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, wl->language));
    }
    if (wl->initial_prompt && wl->initial_prompt[0]) {
        MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--initial_prompt"));
        MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, wl->initial_prompt));
    }
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--beep_off"));
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--print_progress"));
    if (clip_t1_sec > clip_t0_sec) {
        MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, "--clip_timestamps"));
        MP_TARRAY_APPEND(tctx, args, n,
            talloc_asprintf(tctx, "%.3f,%.3f", clip_t0_sec, clip_t1_sec));
    }
    MP_TARRAY_APPEND(tctx, args, n, talloc_strdup(tctx, audio_path));
    MP_TARRAY_APPEND(tctx, args, n, NULL);

    MP_INFO(wl, "spawn: clip [%.2f, %.2f] -> %s\n",
            clip_t0_sec, clip_t1_sec, out_dir);

    struct stderr_capture cap_err = { .log = wl->log };
    struct stderr_capture cap_out = { .log = wl->log };

    struct mp_subprocess_opts opts = {
        .exe = wl->fastwhisper_exe,
        .args = args,
        .cancel = wl->child_cancel,
        .num_fds = 3,
        .fds = {
            { .fd = 0, .src_fd = -1 },
            { .fd = 1, .on_read = on_child_stderr, .on_read_ctx = &cap_out },
            { .fd = 2, .on_read = on_child_stderr, .on_read_ctx = &cap_err },
        },
    };

    struct mp_subprocess_result res = {0};
    mp_subprocess(wl->log, &opts, &res);
    on_child_stderr(&cap_err, NULL, 0);
    on_child_stderr(&cap_out, NULL, 0);

    if (res.error != MP_SUBPROCESS_OK &&
        res.error != MP_SUBPROCESS_EKILLED_BY_US) {
        MP_ERR(wl, "spawn failed: %s\n", mp_subprocess_err_str(res.error));
        return -1;
    }
    if (res.error == MP_SUBPROCESS_EKILLED_BY_US)
        return -2;  // aborted (likely a seek)

    // NOTE: Purfview's faster-whisper.exe (PyInstaller-bundled) frequently
    // crashes on shutdown (0xC0000409 STACK_BUFFER_OVERRUN) after writing
    // the SRT successfully. We deliberately ignore the exit status and
    // trust the SRT file: caller treats "missing SRT" as an empty (silent)
    // chunk, not a fatal error.
    if (res.exit_status != 0) {
        MP_VERBOSE(wl, "fw.exe exited with status %u (ignored if SRT exists)\n",
                   res.exit_status);
    }

    // Compute the SRT name: <basename without extension>.srt in out_dir
    const char *base = strrchr(audio_path, '/');
    const char *base2 = strrchr(audio_path, '\\');
    if (base2 && base2 > base) base = base2;
    base = base ? base + 1 : audio_path;

    const char *dot = strrchr(base, '.');
    char *stem = dot ? talloc_strndup(tctx, base, dot - base)
                     : talloc_strdup(tctx, base);
    char *srt = talloc_asprintf(tctx, "%s%c%s.srt", out_dir,
#ifdef _WIN32
                                '\\',
#else
                                '/',
#endif
                                stem);

    FILE *f = fopen(srt, "rb");
    if (!f) {
        // Silent clip (VAD filtered everything) — not an error.
        MP_VERBOSE(wl, "no SRT produced for this chunk (silent clip?)\n");
        *out_srt_path = NULL;
        return 0;
    }
    fclose(f);
    *out_srt_path = srt;
    return 0;
}

// ----------------------------------------------------------------------------
// LOCAL_FILE mode (chunked, lookahead-style)
// ----------------------------------------------------------------------------

// inject only segments overlapping [t0_sec, t1_sec); de-dup using
// last_injected_end_ms so VAD-extended boundary segments aren't repeated.
static void inject_segments_clipped(struct whisper_lookahead *wl,
                                    struct ws_srt_segments *segs,
                                    double t0_sec, double t1_sec)
{
    if (!segs || segs->count == 0)
        return;
    int64_t t0_ms = (int64_t)(t0_sec * 1000.0);
    int64_t t1_ms = (int64_t)(t1_sec * 1000.0);
    int injected = 0;
    for (int i = 0; i < segs->count; i++) {
        if (atomic_load(&wl->terminate))
            return;
        struct ws_srt_segment *s = &segs->items[i];
        // Reject segments fully outside the clip range.
        if (s->end_ms <= t0_ms || s->start_ms >= t1_ms)
            continue;
        // De-dup: skip anything not strictly past last injected end.
        if (s->start_ms < wl->last_injected_end_ms)
            continue;
        double pts = s->start_ms / 1000.0;
        double dur = (s->end_ms - s->start_ms) / 1000.0;
        if (dur < 0.1) dur = 0.1;
        inject_one(wl, s->text, pts, dur);
        wl->last_injected_end_ms = s->end_ms;
        injected++;
    }
    if (injected == 0)
        MP_VERBOSE(wl, "chunk [%.2f,%.2f]: 0 new segments\n", t0_sec, t1_sec);
}

static int do_local_transcribe(struct whisper_lookahead *wl)
{
    struct MPContext *mpctx = wl->mpctx;
    double duration = mpctx->demuxer ? mpctx->demuxer->duration : -1.0;
    if (duration <= 0.0) {
        MP_WARN(wl, "duration unknown; falling back to single-shot transcription\n");
        duration = 0.0;  // 0 = no upper bound
    }

    double chunk_sec = wl->chunk_sec > 0 ? (double)wl->chunk_sec : 60.0;

    // Snap initial chunk start to a multiple of chunk_sec at or before
    // current playback position so we don't redo earlier audio.
    double start_pts = mpctx->playback_pts;
    if (start_pts == MP_NOPTS_VALUE || start_pts < 0) start_pts = 0;
    double chunk_t0 = floor(start_pts / chunk_sec) * chunk_sec;
    wl->last_injected_end_ms = (int64_t)(chunk_t0 * 1000.0);

    MP_INFO(wl, "local: starting chunked transcription, duration=%.1f, "
            "chunk_sec=%.1f, start_t0=%.2f\n",
            duration, chunk_sec, chunk_t0);

    while (!atomic_load(&wl->terminate)) {
        // Honor a pending seek (highest priority).
        mp_mutex_lock(&wl->seek_mutex);
        if (wl->seek_pending) {
            double tgt = wl->seek_target;
            wl->seek_pending = false;
            mp_mutex_unlock(&wl->seek_mutex);
            chunk_t0 = floor(tgt / chunk_sec) * chunk_sec;
            if (chunk_t0 < 0) chunk_t0 = 0;
            wl->last_injected_end_ms = (int64_t)(chunk_t0 * 1000.0);
            MP_INFO(wl, "local: seek -> %.2f, chunk_t0 reset to %.2f\n",
                    tgt, chunk_t0);
        } else {
            mp_mutex_unlock(&wl->seek_mutex);
        }

        if (duration > 0 && chunk_t0 >= duration) {
            MP_INFO(wl, "local: reached EOF (chunk_t0=%.2f, duration=%.2f)\n",
                    chunk_t0, duration);
            break;
        }

        // If playback head has already raced past us by more than 2 chunks,
        // jump ahead to avoid wasting work on stale audio.
        double pp = mpctx->playback_pts;
        if (pp != MP_NOPTS_VALUE && pp > chunk_t0 + 2 * chunk_sec) {
            double new_t0 = floor(pp / chunk_sec) * chunk_sec;
            MP_INFO(wl, "local: playhead at %.2f outpaced chunk_t0 %.2f; "
                    "skipping to %.2f\n", pp, chunk_t0, new_t0);
            chunk_t0 = new_t0;
            wl->last_injected_end_ms = (int64_t)(chunk_t0 * 1000.0);
        }

        double chunk_t1 = chunk_t0 + chunk_sec;
        if (duration > 0 && chunk_t1 > duration)
            chunk_t1 = duration;

        void *tctx = talloc_new(NULL);
        char *out_dir = make_temp_outdir(tctx, wl->log);
        if (!out_dir) {
            talloc_free(tctx);
            return -1;
        }

        // Reset per-spawn cancel so a previous abort doesn't carry over.
        mp_cancel_reset(wl->child_cancel);

        char *srt_path = NULL;
        int rc = run_faster_whisper(wl, tctx, wl->filename, out_dir,
                                    chunk_t0, chunk_t1, &srt_path);

        if (rc == -2) {
            // Aborted (likely seek). Drop output, loop continues.
            rmtree_quiet(wl->log, out_dir);
            talloc_free(tctx);
            MP_INFO(wl, "local: chunk aborted, continuing\n");
            continue;
        }
        if (rc != 0) {
            rmtree_quiet(wl->log, out_dir);
            talloc_free(tctx);
            MP_ERR(wl, "local: chunk failed at t=%.2f, stopping\n", chunk_t0);
            return -1;
        }

        if (srt_path) {
            struct ws_srt_segments *segs =
                ws_srt_parse_file(tctx, wl->log, srt_path);
            if (segs && segs->count > 0)
                inject_segments_clipped(wl, segs, chunk_t0, chunk_t1);
        }

        rmtree_quiet(wl->log, out_dir);
        talloc_free(tctx);

        wl->chunks_processed++;
        chunk_t0 = chunk_t1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Mode detection
// ----------------------------------------------------------------------------

static enum wl_mode detect_mode(const char *path)
{
    if (!path || !path[0])
        return WL_MODE_LOCAL_FILE;
    if (strncasecmp(path, "file://", 7) == 0)
        return WL_MODE_LOCAL_FILE;
    const char *colon = strstr(path, "://");
    if (colon)
        return WL_MODE_NETWORK;
    return WL_MODE_LOCAL_FILE;
}

// ----------------------------------------------------------------------------
// Options parsing
// ----------------------------------------------------------------------------

static bool parse_opts(struct whisper_lookahead *wl, char **errmsg_out)
{
    *errmsg_out = NULL;

    wl->model    = talloc_strdup(wl, "small");
    wl->device   = talloc_strdup(wl, "auto");
    wl->language = talloc_strdup(wl, "auto");
    wl->chunk_sec = 30;

    char *translate_to = NULL;
    enum wt_provider translate_provider = WT_PROVIDER_NONE;

    if (!wl->whisper_opts || !wl->whisper_opts[0]) {
        *errmsg_out = talloc_strdup(wl, "empty whisper-lookahead opts");
        return false;
    }

    char *opts_copy = talloc_strdup(wl, wl->whisper_opts);
    char *p = opts_copy;
    while (p && *p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            const char *key = p;
            const char *val = eq + 1;
            if (!strcmp(key, "fastwhisper_dir")) {
                talloc_free(wl->fastwhisper_dir);
                wl->fastwhisper_dir = talloc_strdup(wl, val);
            } else if (!strcmp(key, "model")) {
                talloc_free(wl->model);
                wl->model = talloc_strdup(wl, val);
            } else if (!strcmp(key, "device")) {
                talloc_free(wl->device);
                wl->device = talloc_strdup(wl, val);
            } else if (!strcmp(key, "language")) {
                talloc_free(wl->language);
                wl->language = talloc_strdup(wl, val);
            } else if (!strcmp(key, "chunk_sec")) {
                int v = atoi(val);
                if (v >= 5 && v <= 300) wl->chunk_sec = v;
            } else if (!strcmp(key, "initial_prompt")) {
                talloc_free(wl->initial_prompt);
                wl->initial_prompt = talloc_strdup(wl, val);
            } else if (!strcmp(key, "translate_to")) {
                translate_to = talloc_strdup(wl, val);
            } else if (!strcmp(key, "translate_provider")) {
                if (!strcmp(val, "google"))
                    translate_provider = WT_PROVIDER_GOOGLE;
                else if (!strcmp(val, "azure"))
                    translate_provider = WT_PROVIDER_AZURE;
            } else {
                MP_VERBOSE(wl, "ignoring unknown opt: %s=%s\n", key, val);
            }
        }
        p = comma ? comma + 1 : NULL;
    }

    if (!wl->fastwhisper_dir || !wl->fastwhisper_dir[0]) {
        *errmsg_out = talloc_strdup(wl,
            "fastwhisper_dir is required in whisper-lookahead opts");
        return false;
    }

#ifdef _WIN32
    wl->fastwhisper_exe =
        talloc_asprintf(wl, "%s\\faster-whisper.exe", wl->fastwhisper_dir);
#else
    wl->fastwhisper_exe =
        talloc_asprintf(wl, "%s/faster-whisper", wl->fastwhisper_dir);
#endif

    FILE *f = fopen(wl->fastwhisper_exe, "rb");
    if (!f) {
        *errmsg_out = talloc_asprintf(wl,
            "faster-whisper executable not found at %s", wl->fastwhisper_exe);
        return false;
    }
    fclose(f);

    if (translate_to && translate_to[0] && translate_provider != WT_PROVIDER_NONE) {
        const char *src = wl->language ? wl->language : "auto";
        wl->translator = whisper_translator_create(wl, wl->log,
                                                   translate_provider,
                                                   src, translate_to);
        if (wl->translator) {
            MP_INFO(wl, "translator enabled (%s -> %s, %s)\n",
                    src, translate_to,
                    translate_provider == WT_PROVIDER_GOOGLE ? "google" : "azure");
        } else {
            MP_WARN(wl, "failed to create translator\n");
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Init + work threads
// ----------------------------------------------------------------------------

static MP_THREAD_VOID work_thread_fn(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/work");

    if (wl->mode == WL_MODE_LOCAL_FILE) {
        do_local_transcribe(wl);
    } else {
        MP_ERR(wl, "NETWORK mode is not yet implemented in this build. "
               "Source '%s' looks like a network URL — please use a local "
               "file for now.\n", wl->filename);
    }

    MP_INFO(wl, "work: thread exiting (%d chunks, %d subs, %d trans)\n",
            wl->chunks_processed, wl->subtitles_injected,
            wl->translations_injected);
    MP_THREAD_RETURN();
}

static MP_THREAD_VOID init_thread_fn(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/init");

    MP_INFO(wl, "init: starting (mode=%s, file='%s')\n",
            wl->mode == WL_MODE_LOCAL_FILE ? "local" : "network",
            wl->filename);

    char *errmsg = NULL;
    if (!parse_opts(wl, &errmsg)) {
        MP_ERR(wl, "init: opts parse failed: %s\n",
               errmsg ? errmsg : "unknown");
        goto fail;
    }

    MP_INFO(wl, "init: model=%s device=%s language=%s chunk_sec=%d exe=%s\n",
            wl->model, wl->device, wl->language, wl->chunk_sec,
            wl->fastwhisper_exe);

    if (mp_cancel_test(wl->cancel))
        goto fail;

    if (mp_thread_create(&wl->work_thread, work_thread_fn, wl)) {
        MP_ERR(wl, "init: failed to create work thread\n");
        goto fail;
    }
    wl->work_thread_valid = true;

    wl->init_ok = true;
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);
    MP_INFO(wl, "init: done\n");
    MP_THREAD_RETURN();

fail:
    wl->init_ok = false;
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);
    MP_THREAD_RETURN();
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void whisper_lookahead_start(struct MPContext *mpctx, const char *whisper_opts)
{
    whisper_lookahead_stop(mpctx);

    if (!mpctx->demuxer || !mpctx->filename) {
        MP_ERR(mpctx, "whisper lookahead: no file loaded\n");
        return;
    }

    struct track *audio_track = mpctx->current_track[0][STREAM_AUDIO];
    if (!audio_track || !audio_track->stream || !audio_track->demuxer) {
        MP_ERR(mpctx, "whisper lookahead: no audio track selected\n");
        return;
    }

    MP_INFO(mpctx, "whisper lookahead: starting (async), opts='%s'\n",
            whisper_opts ? whisper_opts : "");

    struct whisper_lookahead *wl = talloc_zero(NULL, struct whisper_lookahead);
    wl->mpctx = mpctx;
    wl->log = mp_log_new(wl, mpctx->log, "whisper-fw");
    wl->primary_stream = audio_track->stream;
    wl->primary_demuxer = audio_track->demuxer;
    atomic_store(&wl->init_done, false);
    atomic_store(&wl->terminate, 0);

    wl->cancel = mp_cancel_new(wl);
    mp_cancel_set_parent(wl->cancel, mpctx->playback_abort);
    wl->child_cancel = mp_cancel_new(wl);
    mp_cancel_set_parent(wl->child_cancel, wl->cancel);
    mp_mutex_init(&wl->seek_mutex);

    char *path = mp_get_user_path(wl, mpctx->global, mpctx->filename);
    wl->filename = talloc_strdup(wl, path);
    wl->whisper_opts = talloc_strdup(wl, whisper_opts ? whisper_opts : "");
    wl->mode = detect_mode(wl->filename);

    if (mp_thread_create(&wl->init_thread, init_thread_fn, wl)) {
        MP_ERR(mpctx, "whisper lookahead: failed to create init thread\n");
        talloc_free(wl);
        return;
    }
    wl->init_thread_valid = true;

    mpctx->whisper_lookahead = wl;
    MP_INFO(mpctx, "whisper lookahead: init thread launched\n");
}

void whisper_lookahead_stop(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    MP_INFO(mpctx, "whisper lookahead: stopping (chunks=%d subs=%d trans=%d)\n",
            wl->chunks_processed, wl->subtitles_injected,
            wl->translations_injected);

    atomic_store(&wl->terminate, 1);
    mp_cancel_trigger(wl->cancel);

    if (wl->init_thread_valid)
        mp_thread_join(wl->init_thread);
    wl->init_thread_valid = false;

    if (wl->work_thread_valid)
        mp_thread_join(wl->work_thread);
    wl->work_thread_valid = false;

    whisper_translator_destroy(&wl->translator);

    mp_mutex_destroy(&wl->seek_mutex);

    mpctx->whisper_lookahead = NULL;
    talloc_free(wl);

    MP_INFO(mpctx, "whisper lookahead: stopped\n");
}

void whisper_lookahead_seek(struct MPContext *mpctx, double pts)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || pts == MP_NOPTS_VALUE)
        return;
    mp_mutex_lock(&wl->seek_mutex);
    wl->seek_pending = true;
    wl->seek_target = pts;
    mp_mutex_unlock(&wl->seek_mutex);
    mp_cancel_trigger(wl->child_cancel);
    MP_VERBOSE(wl, "seek: requested %.3f (will abort current chunk)\n", pts);
}

bool whisper_lookahead_track_selected(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && wl->track_selected;
}

void whisper_lookahead_set_track_selected(struct MPContext *mpctx, bool val)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (wl) wl->track_selected = val;
}

bool whisper_lookahead_ready(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && atomic_load(&wl->init_done) && wl->init_ok;
}

bool whisper_lookahead_failed(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && atomic_load(&wl->init_done) && !wl->init_ok;
}
