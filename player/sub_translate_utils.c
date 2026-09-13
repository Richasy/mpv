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

#include "mpv_talloc.h"

#include "sub_translate.h"

char *sub_translate_escape_ass(void *talloc_parent, const char *text)
{
    size_t length = sub_translate_escape_ass_buffer(NULL, 0, text);
    char *result = talloc_array(talloc_parent, char, length + 1);
    sub_translate_escape_ass_buffer(result, length + 1, text);
    return result;
}
