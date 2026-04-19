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
 * Minimal SRT (SubRip) parser used by player/whisper.c to consume the
 * output of faster-whisper.exe.
 *
 * Format:
 *   <index>\n
 *   HH:MM:SS,mmm --> HH:MM:SS,mmm\n
 *   <text line 1>\n
 *   [<text line 2>\n ...]
 *   \n
 *
 * faster-whisper writes UTF-8 with optional BOM and uses LF or CRLF.
 * Multi-line text segments are joined with a single space (mpv ASS
 * dialogue lines are single-line; we drop hard breaks).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "whisper_srt.h"

static char *strip_bom(char *s)
{
    if ((unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        return s + 3;
    return s;
}

// Parse "HH:MM:SS,mmm" or "HH:MM:SS.mmm" into milliseconds.
// Returns -1 on parse failure.
static int64_t parse_timecode(const char *s)
{
    int h = 0, m = 0, sec = 0, ms = 0;
    char sep = ',';
    if (sscanf(s, "%d:%d:%d%c%d", &h, &m, &sec, &sep, &ms) < 5)
        return -1;
    if (h < 0 || m < 0 || m >= 60 || sec < 0 || sec >= 60 || ms < 0 || ms >= 1000)
        return -1;
    return ((int64_t)h * 3600 + m * 60 + sec) * 1000 + ms;
}

// Trim leading/trailing whitespace in-place. Returns the (possibly shifted)
// pointer for convenience.
static char *trim(char *s)
{
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

// Tokenize buf into lines (in-place: replaces \n with \0). Returns array
// of pointers and stores count in *out_count. Caller frees array via talloc.
static char **split_lines(void *tctx, char *buf, int *out_count)
{
    char **lines = NULL;
    int count = 0, cap = 0;
    char *p = buf;
    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n') p++;
        bool has_nl = (*p == '\n');
        if (has_nl) *p = '\0';
        // Strip a trailing \r if present (CRLF input)
        size_t len = strlen(line_start);
        if (len && line_start[len - 1] == '\r')
            line_start[len - 1] = '\0';
        if (count >= cap) {
            cap = cap ? cap * 2 : 64;
            lines = talloc_realloc(tctx, lines, char *, cap);
        }
        lines[count++] = line_start;
        if (has_nl) p++;
    }
    *out_count = count;
    return lines;
}

struct ws_srt_segments *ws_srt_parse_buffer(void *talloc_parent,
                                            struct mp_log *log,
                                            const char *data, size_t len)
{
    struct ws_srt_segments *out = talloc_zero(talloc_parent,
                                              struct ws_srt_segments);

    char *buf = talloc_strndup(out, data, len);
    buf = strip_bom(buf);

    int line_count = 0;
    char **lines = split_lines(out, buf, &line_count);

    int i = 0;
    while (i < line_count) {
        // Skip blank lines
        while (i < line_count && lines[i][0] == '\0') i++;
        if (i >= line_count) break;

        // Optional index line: a pure integer. If next line looks like a
        // timecode, treat current line as index and consume it.
        if (i + 1 < line_count && strstr(lines[i + 1], "-->"))
            i++;

        if (i >= line_count) break;

        // Timecode line: "HH:MM:SS,mmm --> HH:MM:SS,mmm"
        char *arrow = strstr(lines[i], "-->");
        if (!arrow) {
            // Not a timecode — skip and resync at the next blank line
            while (i < line_count && lines[i][0] != '\0') i++;
            continue;
        }
        char *left = lines[i];
        char *right = arrow + 3;
        // Null-terminate the left side at the first space before "-->"
        char *lend = arrow;
        while (lend > left && (lend[-1] == ' ' || lend[-1] == '\t'))
            lend--;
        char saved = *lend;
        *lend = '\0';

        int64_t s_ms = parse_timecode(trim(left));
        int64_t e_ms = parse_timecode(trim(right));
        *lend = saved;

        i++;
        if (s_ms < 0 || e_ms < 0) {
            // Bad timecode — skip text block
            while (i < line_count && lines[i][0] != '\0') i++;
            continue;
        }

        // Text lines until blank or EOF
        char *text = NULL;
        while (i < line_count && lines[i][0] != '\0') {
            char *t = trim(lines[i]);
            if (t[0]) {
                if (text) {
                    text = talloc_strdup_append(text, " ");
                    text = talloc_strdup_append(text, t);
                } else {
                    text = talloc_strdup(out, t);
                }
            }
            i++;
        }

        if (!text || !text[0]) {
            talloc_free(text);
            continue;
        }
        if (e_ms <= s_ms) {
            // Some segments come back zero-length — extend slightly
            e_ms = s_ms + 500;
        }

        struct ws_srt_segment seg = {
            .start_ms = s_ms,
            .end_ms = e_ms,
            .text = text,
        };
        MP_TARRAY_APPEND(out, out->items, out->count, seg);
    }

    if (log) {
        mp_verbose(log, "ws_srt: parsed %d segments from %zu bytes\n",
                   out->count, len);
    }
    return out;
}

struct ws_srt_segments *ws_srt_parse_file(void *talloc_parent,
                                          struct mp_log *log,
                                          const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (log)
            mp_err(log, "ws_srt: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024 * 1024) {
        fclose(f);
        if (log)
            mp_warn(log, "ws_srt: empty or oversized file %s (%ld bytes)\n",
                    path, sz);
        return NULL;
    }
    char *buf = talloc_array(NULL, char, sz);
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    struct ws_srt_segments *segs =
        ws_srt_parse_buffer(talloc_parent, log, buf, rd);
    talloc_free(buf);
    return segs;
}

void ws_srt_segments_free(struct ws_srt_segments **segs)
{
    if (segs && *segs) {
        talloc_free(*segs);
        *segs = NULL;
    }
}
