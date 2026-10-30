/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MPV_BLURAY_FILES_H
#define MPV_BLURAY_FILES_H

struct stream;
struct mp_bluray_files;
struct bd_file_s;
struct bd_dir_s;

// The caller supplies a complete disc-relative path -> source URL map. This
// permits directory enumeration even on HTTP servers without directory lists,
// and does not assume that sibling cloud-drive files share a signed URL.
struct mp_bluray_files *mp_bluray_files_create(struct stream *owner,
                                               char **files);
struct bd_file_s *mp_bluray_open_file(void *handle, const char *path);
struct bd_dir_s *mp_bluray_open_dir(void *handle, const char *path);

#endif
