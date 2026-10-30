/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <libbluray/filesystem.h>

#include "common/msg.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "stream/stream.h"
#include "stream/stream_iso_cache.h"
#include "stream/bluray_files.h"

static int reads, wanted, opened, closed;
static bool fail_seek, fail_read;
static int available = INT_MAX;
static const int64_t file_size = (int64_t)80 * 1024 * 1024 * 1024;

// Fake transport boundaries: assert that the adapter requests only demanded
// bytes, preserves offsets beyond 4 GiB and propagates errors/cancellation.
bool stream_seek(struct stream *s, int64_t pos)
{
    if (fail_seek)
        return false;
    s->pos = pos;
    return true;
}

int stream_read(struct stream *s, void *buf, int size)
{
    reads++;
    wanted = size;
    s->error = fail_read;
    int got = fail_read ? 0 : size < available ? size : available;
    memset(buf, 0x5a, got);
    s->pos += got;
    return got;
}

int64_t stream_get_size(struct stream *s)
{
    return file_size;
}

struct stream *stream_open_disc_file(struct stream *owner, const char *url)
{
    assert(strcmp(url, "http://example.invalid/clip?token=opaque") == 0);
    struct stream *s = talloc_zero(NULL, struct stream);
    s->cancel = owner->cancel;
    opened++;
    return s;
}

void free_stream(struct stream *s)
{
    closed++;
    talloc_free(s);
}

static void test_iso(void)
{
    struct stream stream = {.cancel = mp_cancel_new(NULL)};
    struct iso_cache *reader = iso_cache_create(NULL, &stream);
    unsigned char buffer[2048];
    assert(iso_cache_read(reader, buffer, file_size - 2048, 2048) == 2048);
    assert(wanted == 2048 && stream.pos == file_size);
    assert(buffer[0] == 0x5a && buffer[2047] == 0x5a);
    int prior = reads;
    assert(iso_cache_read(reader, buffer, -1, 2) == -1);
    assert(iso_cache_read(reader, buffer, INT64_MAX, 2) == -1);
    assert(iso_cache_read(reader, buffer, 0, -1) == -1);
    assert(iso_cache_read(reader, buffer, 0, 0) == 0);
    assert(reads == prior);
    fail_seek = true;
    assert(iso_cache_read(reader, buffer, 1024, 2048) == -1);
    assert(reads == prior);
    fail_seek = false;
    fail_read = true;
    assert(iso_cache_read(reader, buffer, 1024, 2048) == -1);
    fail_read = false;
    available = 1024;
    assert(iso_cache_read(reader, buffer, 0, 2048) == 1024);
    available = 0;
    assert(iso_cache_read(reader, buffer, 0, 2048) == 0);
    available = INT_MAX;
    mp_cancel_trigger(stream.cancel);
    prior = reads;
    assert(iso_cache_read(reader, buffer, 0, 2048) == -1);
    assert(reads == prior);
    iso_cache_free(reader);
    talloc_free(stream.cancel);
}

static void test_files(void)
{
    struct stream *owner = talloc_zero(NULL, struct stream);
    owner->log = mp_null_log;
    owner->cancel = mp_cancel_new(owner);
    owner->access_references = true;
    char *files[] = {
        "BDMV/PLAYLIST/00001.mpls", "unused",
        "BDMV/PLAYLIST/00002.mpls", "unused",
        "BDMV/STREAM/00001.m2ts", "http://example.invalid/clip?token=opaque",
        "CERTIFICATE/id.bdmv", "unused", NULL,
    };
    struct mp_bluray_files *fs = mp_bluray_files_create(owner, files);
    assert(fs && owner->is_network && owner->streaming);
    // The instance owns its snapshot, including source strings.
    files[5] = "replacement must not be read";
    BD_DIR_H *dir = mp_bluray_open_dir(fs, "BDMV\\PLAYLIST");
    assert(dir);
    BD_DIRENT entry;
    assert(dir->read(dir, &entry) == 0 &&
           strcmp(entry.d_name, "00001.mpls") == 0);
    assert(dir->read(dir, &entry) == 0 &&
           strcmp(entry.d_name, "00002.mpls") == 0);
    assert(dir->read(dir, &entry) == 1);
    dir->close(dir);
    dir = mp_bluray_open_dir(fs, "BDMV/");
    assert(dir && dir->read(dir, &entry) == 0 &&
           strcmp(entry.d_name, "PLAYLIST") == 0);
    assert(dir->read(dir, &entry) == 0 &&
           strcmp(entry.d_name, "STREAM") == 0);
    assert(dir->read(dir, &entry) == 1); // no duplicate PLAYLIST directory
    dir->close(dir);
    assert(!mp_bluray_open_dir(fs, "missing"));
    assert(!mp_bluray_open_file(fs, "BDMV/STREAM/99999.m2ts"));
    BD_FILE_H *file = mp_bluray_open_file(fs, "BDMV\\STREAM\\00001.m2ts");
    assert(file && opened == 1);
    assert(file->seek(file, -2048, SEEK_END) == file_size - 2048);
    assert(file->tell(file) == file_size - 2048);
    unsigned char buffer[2048];
    assert(file->read(file, buffer, sizeof(buffer)) == 2048);
    assert(file->tell(file) == file_size);
    assert(file->seek(file, -file_size - 1, SEEK_CUR) == -1);
    assert(file->seek(file, INT64_MAX, SEEK_CUR) == -1);
    assert(file->seek(file, 0, 42) == -1);
    fail_read = true;
    assert(file->read(file, buffer, 1) == -1);
    fail_read = false;
    assert(file->read(file, buffer, -1) == -1);
    mp_cancel_trigger(owner->cancel);
    assert(file->read(file, buffer, 1) == -1);
    assert(!mp_bluray_open_file(fs, "BDMV/STREAM/00001.m2ts"));
    assert(!mp_bluray_open_dir(fs, "BDMV"));
    file->close(file);
    assert(closed == opened);
    talloc_free(owner);
}

static void test_invalid_maps(void)
{
    struct stream *owner = talloc_zero(NULL, struct stream);
    owner->log = mp_null_log;
    owner->access_references = true;
    const char *invalid[] = {
        "../BDMV/file", "/BDMV/file", "BDMV/../file", "BDMV//file",
        "BDMV/./file", "BDMV/", "C:/BDMV/file", "BDMV\\file", "", NULL,
    };
    for (int i = 0; invalid[i]; i++) {
        char *map[] = {(char *)invalid[i], "url", NULL};
        assert(!mp_bluray_files_create(owner, map));
    }
    char *empty_url[] = {"BDMV/index.bdmv", "", NULL};
    assert(!mp_bluray_files_create(owner, empty_url));
    owner->access_references = false;
    char *map[] = {"BDMV/index.bdmv", "url", NULL};
    assert(!mp_bluray_files_create(owner, map));
    talloc_free(owner);
}

int main(void)
{
    test_iso();
    test_files();
    test_invalid_maps();
    return 0;
}
