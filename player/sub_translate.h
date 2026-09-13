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

#ifndef MP_SUB_TRANSLATE_H
#define MP_SUB_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>

struct MPContext;
struct track;

enum sub_translate_source_class {
    SUB_TRANSLATE_SOURCE_TEXT = 0,
    SUB_TRANSLATE_SOURCE_GENERATED,
    SUB_TRANSLATE_SOURCE_BITMAP,
};

bool sub_translate_get_enabled(struct MPContext *mpctx);
void sub_translate_set_enabled(struct MPContext *mpctx, bool enabled);
int sub_translate_set_config(struct MPContext *mpctx, const char *json,
                             char **error);
char *sub_translate_get_config(struct MPContext *mpctx, void *talloc_parent);
char *sub_translate_get_status(struct MPContext *mpctx, void *talloc_parent);

void sub_translate_update(struct MPContext *mpctx);
void sub_translate_seek(struct MPContext *mpctx);
void sub_translate_on_sub_reinit(struct MPContext *mpctx,
                                 struct track *track);
void sub_translate_on_sub_uninit(struct MPContext *mpctx,
                                 struct track *track);
void sub_translate_stop_file(struct MPContext *mpctx);
void sub_translate_destroy(struct MPContext *mpctx);

bool sub_translate_is_generated_profile(const char *profile);
enum sub_translate_source_class sub_translate_classify_source(
    const char *profile, bool text_supported);
size_t sub_translate_escape_ass_buffer(char *buffer, size_t size,
                                       const char *text);
char *sub_translate_escape_ass(void *talloc_parent, const char *text);

#endif
