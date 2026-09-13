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
