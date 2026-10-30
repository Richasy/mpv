/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <libbluray/filesystem.h>

#include "common/common.h"
#include "misc/bstr.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "stream.h"
#include "bluray_files.h"

struct mp_bluray_files {
    struct stream *owner;
    char **files;
};

struct file {
    BD_FILE_H bd;
    struct stream *stream;
};

struct directory {
    BD_DIR_H bd;
    char **names;
    int count;
    int pos;
};

static bool valid_path(const char *path)
{
    if (!path[0] || path[0] == '/' || strstr(path, "//") || strlen(path) > 4096)
        return false;
    bstr rest = bstr0(path);
    while (rest.len) {
        bstr part = bstr_split(rest, "/", &rest);
        if (!part.len || part.len > 255 || bstr_equals0(part, ".") ||
            bstr_equals0(part, ".."))
            return false;
        for (int i = 0; i < part.len; i++) {
            unsigned char c = part.start[i];
            if (c < 32 || c == 127 || strchr("\\:?*\"<>|", c))
                return false;
        }
    }
    return path[strlen(path) - 1] != '/';
}

struct mp_bluray_files *mp_bluray_files_create(struct stream *owner,
                                               char **files)
{
    if (!owner->access_references || !files || !files[0])
        return NULL;
    for (int i = 0; files[i]; i += 2) {
        if (!valid_path(files[i]) || !files[i + 1][0]) {
            MP_ERR(owner, "Invalid Blu-ray file map entry %d.\n", i / 2);
            return NULL;
        }
        if (strncasecmp(files[i + 1], "http://", 7) == 0 ||
            strncasecmp(files[i + 1], "https://", 8) == 0) {
            owner->streaming = true;
            owner->is_network = true;
        }
    }
    struct mp_bluray_files *fs = talloc_zero(owner, struct mp_bluray_files);
    fs->owner = owner;
    // Keep a session snapshot; later option changes must not alter callbacks
    // while libbluray owns files from this disc.
    int count = 0;
    while (files[count])
        count++;
    fs->files = talloc_zero_array(fs, char *, count + 1);
    for (int i = 0; i < count; i++)
        fs->files[i] = talloc_strdup(fs->files, files[i]);
    return fs;
}

// libbluray uses the platform separator, while file-map keys always use '/'.
static char *normalize_path(void *parent, const char *path)
{
    char *result = talloc_strdup(parent, path);
    for (char *p = result; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return result;
}

static void file_close(BD_FILE_H *bd)
{
    struct file *f = bd->internal;
    free_stream(f->stream);
    talloc_free(f);
}

static int64_t file_tell(BD_FILE_H *bd)
{
    struct file *f = bd->internal;
    return stream_tell(f->stream);
}

static int64_t file_seek(BD_FILE_H *bd, int64_t offset, int32_t origin)
{
    struct file *f = bd->internal;
    int64_t base;
    switch (origin) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = stream_tell(f->stream); break;
    case SEEK_END: base = stream_get_size(f->stream); break;
    default: return -1;
    }
    if (base < 0 || offset < -base || offset > INT64_MAX - base)
        return -1;
    int64_t target = base + offset;
    return stream_seek(f->stream, target) ? target : -1;
}

static int64_t file_read(BD_FILE_H *bd, uint8_t *buffer, int64_t size)
{
    struct file *f = bd->internal;
    if (size < 0 || mp_cancel_test(f->stream->cancel))
        return -1;
    int64_t done = 0;
    while (done < size) {
        int want = MPMIN(size - done, INT_MAX);
        int got = stream_read(f->stream, buffer + done, want);
        done += got;
        if (got < want)
            return done ? done : f->stream->error ? -1 : 0;
    }
    return done;
}

struct bd_file_s *mp_bluray_open_file(void *handle, const char *path)
{
    struct mp_bluray_files *fs = handle;
    if (mp_cancel_test(fs->owner->cancel))
        return NULL;
    char *key = normalize_path(NULL, path);
    const char *url = NULL;
    for (int i = 0; fs->files[i]; i += 2) {
        if (strcmp(key, fs->files[i]) == 0) {
            url = fs->files[i + 1];
            break;
        }
    }
    talloc_free(key);
    if (!url)
        return NULL;
    struct stream *s = stream_open_disc_file(fs->owner, url);
    if (!s)
        return NULL;
    struct file *f = talloc_zero(NULL, struct file);
    f->stream = s;
    f->bd = (BD_FILE_H){
        .internal = f,
        .close = file_close,
        .seek = file_seek,
        .tell = file_tell,
        .read = file_read,
    };
    return &f->bd;
}

static void dir_close(BD_DIR_H *bd)
{
    talloc_free(bd->internal);
}

static int dir_read(BD_DIR_H *bd, BD_DIRENT *entry)
{
    struct directory *dir = bd->internal;
    if (dir->pos == dir->count)
        return 1;
    snprintf(entry->d_name, sizeof(entry->d_name), "%s", dir->names[dir->pos++]);
    return 0;
}

struct bd_dir_s *mp_bluray_open_dir(void *handle, const char *path)
{
    struct mp_bluray_files *fs = handle;
    if (mp_cancel_test(fs->owner->cancel))
        return NULL;
    struct directory *dir = talloc_zero(NULL, struct directory);
    char *prefix = normalize_path(dir, path);
    size_t len = strlen(prefix);
    while (len && prefix[len - 1] == '/')
        prefix[--len] = 0;
    for (int i = 0; fs->files[i]; i += 2) {
        const char *key = fs->files[i];
        if (len && (strncmp(key, prefix, len) || key[len] != '/'))
            continue;
        const char *name = key + (len ? len + 1 : 0);
        size_t size = strcspn(name, "/");
        if (!size || size >= sizeof(((BD_DIRENT *)0)->d_name))
            continue;
        bool duplicate = false;
        for (int n = 0; n < dir->count; n++)
            duplicate |= strlen(dir->names[n]) == size &&
                         strncmp(dir->names[n], name, size) == 0;
        if (!duplicate)
            MP_TARRAY_APPEND(dir, dir->names, dir->count,
                             talloc_strndup(dir, name, size));
    }
    if (!dir->count) {
        talloc_free(dir);
        return NULL;
    }
    dir->bd = (BD_DIR_H){
        .internal = dir,
        .close = dir_close,
        .read = dir_read,
    };
    return &dir->bd;
}
