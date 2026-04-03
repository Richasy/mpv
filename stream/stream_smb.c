/*
 * SMB stream handler for Windows
 *
 * Converts smb:// URLs to UNC paths and uses Windows native SMB support.
 * Authenticates via WNetAddConnection2W when credentials are provided.
 *
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

#include "config.h"

#ifdef _WIN32

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <windows.h>
#include <winnetwk.h>
#include <io.h>

#include "osdep/io.h"

#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "stream.h"

struct priv {
    int fd;
    wchar_t *unc_share; // e.g. L"\\\\host\\share" for WNetCancelConnection2W
    bool has_connection;
};

static int64_t smb_get_size(stream_t *s)
{
    struct priv *p = s->priv;
    struct stat st;
    if (fstat(p->fd, &st) == 0 && st.st_size >= 0)
        return st.st_size;
    return -1;
}

static int smb_fill_buffer(stream_t *s, void *buffer, int max_len)
{
    struct priv *p = s->priv;
    int r = read(p->fd, buffer, max_len);
    return r <= 0 ? 0 : r;
}

static int smb_seek(stream_t *s, int64_t newpos)
{
    struct priv *p = s->priv;
    return lseek(p->fd, newpos, SEEK_SET) != (off_t)-1;
}

static void smb_close(stream_t *s)
{
    struct priv *p = s->priv;
    if (p->fd >= 0)
        close(p->fd);
    if (p->has_connection && p->unc_share) {
        WNetCancelConnection2W(p->unc_share, 0, FALSE);
    }
}

// Convert a UTF-8 string to a wide string. Caller must talloc_free.
static wchar_t *utf8_to_wchar(void *talloc_ctx, const char *str)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len <= 0)
        return NULL;
    wchar_t *wstr = talloc_array(talloc_ctx, wchar_t, len);
    MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, len);
    return wstr;
}

static int smb_open(stream_t *stream)
{
    struct priv *p = talloc_zero(stream, struct priv);
    p->fd = -1;
    stream->priv = p;

    // stream->path is the URL without "smb://"
    // Expected formats:
    //   user:pass@host/share/path
    //   host/share/path
    char *path = talloc_strdup(stream, stream->path);
    mp_url_unescape_inplace(path);

    char *username = NULL;
    char *password = NULL;
    char *host = NULL;

    // Parse user:pass@host/share/path
    char *at = strchr(path, '@');
    char *hostpath;
    if (at) {
        *at = '\0';
        char *userinfo = path;
        hostpath = at + 1;

        char *colon = strchr(userinfo, ':');
        if (colon) {
            *colon = '\0';
            username = userinfo;
            password = colon + 1;
        } else {
            username = userinfo;
        }
    } else {
        hostpath = path;
    }

    // hostpath = "host/share/path/to/file"
    // We need to split into host, share, and remaining path
    char *first_slash = strchr(hostpath, '/');
    if (!first_slash) {
        MP_ERR(stream, "SMB URL must contain at least host/share: %s\n",
               stream->url);
        return STREAM_ERROR;
    }

    *first_slash = '\0';
    host = hostpath;
    char *share_and_path = first_slash + 1;

    // Split share from remaining path
    char *share = share_and_path;
    char *second_slash = strchr(share_and_path, '/');
    char *remaining_path = NULL;
    if (second_slash) {
        *second_slash = '\0';
        remaining_path = second_slash + 1;
    }

    if (!host[0] || !share[0]) {
        MP_ERR(stream, "SMB URL missing host or share name: %s\n",
               stream->url);
        return STREAM_ERROR;
    }

    // Build UNC share path: \\host\share
    char *unc_share = talloc_asprintf(stream, "\\\\%s\\%s", host, share);

    // Build full UNC path: \\host\share\path\to\file
    char *unc_path;
    if (remaining_path && remaining_path[0]) {
        unc_path = talloc_asprintf(stream, "%s\\%s", unc_share, remaining_path);
        // Convert forward slashes to backslashes
        for (char *c = unc_path + strlen(unc_share); *c; c++) {
            if (*c == '/')
                *c = '\\';
        }
    } else {
        unc_path = unc_share;
    }

    // Authenticate if credentials provided
    if (username && username[0]) {
        wchar_t *w_share = utf8_to_wchar(stream, unc_share);
        wchar_t *w_user = utf8_to_wchar(stream, username);
        wchar_t *w_pass = password ? utf8_to_wchar(stream, password) : NULL;

        if (!w_share || !w_user) {
            MP_ERR(stream, "Failed to convert SMB credentials to wide string\n");
            return STREAM_ERROR;
        }

        NETRESOURCEW nr = {
            .dwType = RESOURCETYPE_DISK,
            .lpRemoteName = w_share,
        };

        DWORD ret = WNetAddConnection2W(&nr, w_pass, w_user,
                                         CONNECT_TEMPORARY);
        if (ret != NO_ERROR && ret != ERROR_ALREADY_ASSIGNED &&
            ret != ERROR_SESSION_CREDENTIAL_CONFLICT) {
            MP_ERR(stream, "SMB authentication failed for %s (error %lu)\n",
                   unc_share, ret);
            return STREAM_ERROR;
        }

        p->unc_share = utf8_to_wchar(p, unc_share);
        p->has_connection = (ret == NO_ERROR);
        MP_VERBOSE(stream, "SMB connection established to %s\n", unc_share);
    }

    // Open the file via UNC path
    wchar_t *w_path = utf8_to_wchar(stream, unc_path);
    if (!w_path) {
        MP_ERR(stream, "Failed to convert UNC path to wide string\n");
        return STREAM_ERROR;
    }

    p->fd = _wopen(w_path, O_RDONLY | O_BINARY);
    if (p->fd < 0) {
        MP_ERR(stream, "Cannot open SMB file '%s': %s\n",
               unc_path, mp_strerror(errno));
        return STREAM_ERROR;
    }

    // Check if seekable
    off_t len = lseek(p->fd, 0, SEEK_END);
    lseek(p->fd, 0, SEEK_SET);
    if (len != (off_t)-1) {
        stream->seek = smb_seek;
        stream->seekable = true;
    }

    stream->fill_buffer = smb_fill_buffer;
    stream->get_size = smb_get_size;
    stream->close = smb_close;
    stream->streaming = true;
    stream->fast_skip = true;

    MP_VERBOSE(stream, "Opened SMB file: %s\n", unc_path);
    return STREAM_OK;
}

const stream_info_t stream_info_smb = {
    .name = "smb",
    .open = smb_open,
    .protocols = (const char *const[]){ "smb", NULL },
    .stream_origin = STREAM_ORIGIN_NET,
};

#endif /* _WIN32 */
