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

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

#include "sub_translate.h"

bool sub_translate_is_generated_profile(const char *profile)
{
    return profile &&
        (strcmp(profile, "whisper") == 0 ||
         strcmp(profile, "translated") == 0);
}

enum sub_translate_source_class sub_translate_classify_source(
    const char *profile, bool text_supported)
{
    if (sub_translate_is_generated_profile(profile))
        return SUB_TRANSLATE_SOURCE_GENERATED;
    return text_supported ? SUB_TRANSLATE_SOURCE_TEXT
                          : SUB_TRANSLATE_SOURCE_BITMAP;
}

static bool span_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int sub_translate_ass_spans(const char *text, struct sub_translate_span *spans,
                            int capacity)
{
    int count = 0;
    bool drawing = false;
    const char *p = text;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end)
                return -1; // Do not guess at malformed override boundaries.
            int depth = 0;
            for (const char *tag = p + 1; tag < end; tag++) {
                if (*tag == '(')
                    depth++;
                if (*tag == ')' && depth)
                    depth--;
                if (*tag != '\\' || tag + 1 == end)
                    continue;
                const char *name = tag + 1;
                while (name < end && span_space(*name))
                    name++;
                if (*name != 'p' || !strncmp(name, "pos", 3) ||
                    !strncmp(name, "pbo", 3))
                    continue;
                // Drawing tags nested in transforms or arbitrary arguments
                // have ambiguous applicability. Keep that entire event intact.
                if (depth)
                    return -1;
                const char *arg = name + 1;
                while (arg < end && span_space(*arg))
                    arg++;
                bool parens = *arg == '(';
                if (parens)
                    arg++;
                char *number_end;
                long value = strtol(arg, &number_end, 10);
                const char *tail = number_end;
                while (tail < end && span_space(*tail))
                    tail++;
                if (parens) {
                    if (tail == end || *tail != ')' || number_end == arg)
                        return -1;
                    tail++;
                    while (tail < end && span_space(*tail))
                        tail++;
                }
                if (tail != end && *tail != '\\')
                    return -1;
                drawing = value > 0;
                tag = tail - 1;
            }
            p = end + 1;
        } else if (*p == '\\') {
            // Preserve escape bytes, including literal braces/backslashes.
            p++;
            if (*p && (unsigned char)*p < 0x80)
                p++;
        } else {
            const char *start = p;
            while (*p && *p != '{' && *p != '\\')
                p++;
            const char *end = p;
            while (start < end && span_space(*start))
                start++;
            while (end > start && span_space(end[-1]))
                end--;
            if (!drawing && end > start) {
                if (count == INT_MAX)
                    return -1;
                if (spans && count < capacity) {
                    spans[count] = (struct sub_translate_span){
                        .start = start - text,
                        .length = end - start,
                    };
                }
                count++;
            }
        }
    }
    return count;
}

struct block_style {
    double size;
    bool italic;
};

struct block_boundary {
    size_t offset;
    const char *tag;
    size_t tag_length;
};

static bool block_override(const char *start, const char *end,
                           struct block_style *style)
{
    for (const char *p = start; p < end; p++) {
        if (*p != '\\')
            continue;
        const char *tag = ++p;
        while (tag < end && span_space(*tag))
            tag++;
        const char *next = tag;
        while (next < end && *next != '\\')
            next++;
        // Do not infer top-to-bottom block order through positioning, drawings,
        // transforms, style resets or font geometry overrides.
        if (tag < end && (*tag == 'p' || *tag == 'a' || *tag == 'r' ||
                         *tag == 't' || !strncmp(tag, "move", 4) ||
                         !strncmp(tag, "org", 3) || !strncmp(tag, "fr", 2) ||
                         !strncmp(tag, "fa", 2) || !strncmp(tag, "fsc", 3) ||
                         !strncmp(tag, "clip", 4) || !strncmp(tag, "iclip", 5)))
            return false;
        int prefix = 0;
        bool size = next - tag >= 2 && !strncmp(tag, "fs", 2) &&
                    (next - tag == 2 || tag[2] != 'p');
        if (size)
            prefix = 2;
        else if (tag < next && *tag == 'i')
            prefix = 1;
        if (prefix) {
            const char *number = tag + prefix;
            while (number < next && span_space(*number))
                number++;
            // libass interprets signed fs values relatively, not as points.
            if (size && number < next && (*number == '+' || *number == '-'))
                return false;
            char *tail;
            double value = strtod(number, &tail);
            if (tail == number || !isfinite(value))
                return false;
            while (tail < next && span_space(*tail))
                tail++;
            if (tail != next)
                return false;
            if (size) {
                if (value <= 0)
                    return false;
                style->size = value;
            } else {
                if (value != 0 && value != 1)
                    return false;
                style->italic = value != 0;
            }
        }
        p = next - 1;
    }
    return true;
}

static struct block_boundary block_boundary(
    const struct sub_translate_ass_sample *cue)
{
    struct block_boundary result = {0};
    if (!cue->normal_layout || !cue->text || !isfinite(cue->font_size) ||
        cue->font_size <= 0 || !isfinite(cue->start) ||
        !isfinite(cue->duration) || cue->duration <= 0)
        return result;

    struct block_style style = {.size = cue->font_size};
    double lower_size = 0;
    bool upper_text = false;
    bool lower_text = false;
    const char *p = cue->text;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (!end || !block_override(p + 1, end, &style))
                return (struct block_boundary){0};
            p = end + 1;
        } else if (p[0] == '\\' && p[1] == 'N' && p[2] == '{' &&
                   !result.offset && upper_text &&
                   style.size == cue->font_size && !style.italic) {
            const char *end = strchr(p + 2, '}');
            struct block_style lower = style;
            if (!end || !block_override(p + 3, end, &lower))
                return (struct block_boundary){0};
            if (lower.italic && lower.size <= cue->font_size * 0.8) {
                result = (struct block_boundary){
                    .offset = p - cue->text,
                    .tag = p + 2,
                    .tag_length = end - (p + 2) + 1,
                };
                lower_size = lower.size;
            }
            style = lower;
            p = end + 1;
        } else if (*p == '\\') {
            p++;
            if (*p && (unsigned char)*p < 0x80)
                p++;
        } else {
            if (!span_space(*p)) {
                if (result.offset) {
                    if (!style.italic || style.size != lower_size)
                        return (struct block_boundary){0};
                    lower_text = true;
                } else {
                    upper_text = true;
                }
            }
            p++;
        }
    }
    return lower_text ? result : (struct block_boundary){0};
}

size_t sub_translate_ass_primary_end(
    const struct sub_translate_ass_sample *samples, int num_samples,
    const struct sub_translate_ass_sample *cue)
{
    struct block_boundary boundary = block_boundary(cue);
    if (!boundary.offset)
        return 0;
    if (num_samples > SUB_TRANSLATE_ASS_LAYOUT_SAMPLES)
        num_samples = SUB_TRANSLATE_ASS_LAYOUT_SAMPLES;

    int total = 0;
    int matches = 0;
    for (int n = 0; n < num_samples; n++) {
        const struct sub_translate_ass_sample *sample = &samples[n];
        if (sample->style != cue->style)
            continue;
        total++;
        struct block_boundary candidate = block_boundary(sample);
        if (!candidate.offset || sample->font_size != cue->font_size ||
            candidate.tag_length != boundary.tag_length ||
            memcmp(candidate.tag, boundary.tag, boundary.tag_length))
            continue;
        bool overlaps = false;
        for (int k = 0; k < num_samples; k++) {
            const struct sub_translate_ass_sample *other = &samples[k];
            if (k != n && other->style == sample->style &&
                sample->start < other->start + other->duration &&
                other->start < sample->start + sample->duration)
                overlaps = true;
        }
        if (!overlaps)
            matches++;
    }
    return matches >= 3 && matches * 5 >= total * 4 ? boundary.offset : 0;
}

static void append_byte(char *buffer, size_t size, size_t *length,
                        unsigned char value)
{
    if (buffer && *length + 1 < size)
        buffer[*length] = value;
    (*length)++;
}

size_t sub_translate_escape_ass_buffer(char *buffer, size_t size,
                                       const char *text)
{
    size_t length = 0;
    for (const unsigned char *cursor =
             (const unsigned char *)(text ? text : "");
         *cursor; cursor++)
    {
        if (*cursor == '\r') {
            if (cursor[1] == '\n')
                continue;
            append_byte(buffer, size, &length, '\\');
            append_byte(buffer, size, &length, 'N');
        } else if (*cursor == '\n') {
            append_byte(buffer, size, &length, '\\');
            append_byte(buffer, size, &length, 'N');
        } else if (*cursor == '{') {
            append_byte(buffer, size, &length, '\\');
            append_byte(buffer, size, &length, '{');
        } else if (*cursor == '\\') {
            append_byte(buffer, size, &length, '\\');
            append_byte(buffer, size, &length, 0xe2);
            append_byte(buffer, size, &length, 0x81);
            append_byte(buffer, size, &length, 0xa0);
        } else {
            append_byte(buffer, size, &length, *cursor);
        }
    }
    if (buffer && size)
        buffer[length < size ? length : size - 1] = '\0';
    return length;
}
