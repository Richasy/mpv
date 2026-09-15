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
