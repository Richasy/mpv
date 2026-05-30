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

#ifndef MPLAYER_THUMBNAIL_H
#define MPLAYER_THUMBNAIL_H

// Handler for the user-facing `thumbnail-raw` command. Decodes a single video
// frame near a requested timestamp directly from the demuxer cache (no low
// level seek, no disturbance to the playing decoder) and returns it as a raw
// RGB node map, mirroring `screenshot-raw`.
void cmd_thumbnail_raw(void *p);

#endif /* MPLAYER_THUMBNAIL_H */
