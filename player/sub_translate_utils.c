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

#include "mpv_talloc.h"

#include "common/common.h"
#include "misc/bstr.h"

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

char *sub_translate_escape_ass(void *talloc_parent, const char *text)
{
    bstr escaped = {0};
    for (const unsigned char *cursor =
             (const unsigned char *)(text ? text : "");
         *cursor; cursor++)
    {
        if (*cursor == '\r') {
            if (cursor[1] == '\n')
                continue;
            bstr_xappend(talloc_parent, &escaped, bstr0("\\N"));
        } else if (*cursor == '\n') {
            bstr_xappend(talloc_parent, &escaped, bstr0("\\N"));
        } else if (*cursor == '{') {
            bstr_xappend(talloc_parent, &escaped, bstr0("\\{"));
        } else if (*cursor == '\\') {
            bstr_xappend(talloc_parent, &escaped, bstr0("\\"));
            mp_append_utf8_bstr(talloc_parent, &escaped, 0x2060);
        } else {
            bstr_xappend(talloc_parent, &escaped,
                         (bstr){(char *)cursor, 1});
        }
    }
    char *result = escaped.start
        ? bstrto0(talloc_parent, escaped)
        : talloc_strdup(talloc_parent, "");
    talloc_free(escaped.start);
    return result;
}
