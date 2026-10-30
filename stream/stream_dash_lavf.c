/* Typed DASH transport. Each observed AVIO owns exactly one HTTP response.
 * Subsequent ranges are explicit new requests, never FFmpeg internal retries.
 * See the consumer's ADR-0087 and SPEC-0036. No URL/header/body logging. */
#include <libavformat/avio.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "osdep/timer.h"
#include "player/dash_source.h"
#include "stream.h"
#include "dash_http.h"
#include "stream_dash_lavf.h"

struct dash_lavf {
    struct stream *s;
    struct mp_dash_track_config config;
    mpv_dash_track_kind kind;
    AVIOContext *io;
    int64_t deadline;
    int64_t total;
    uint64_t offset, start, expected, received;
    int status, interim_count, requests;
    bool interim, final_seen, headers_ok, length_known, seekable, finished;
    char *range, *length, *encoding, *accept_ranges, *transfer_encoding;
};

static bool stopped(struct dash_lavf *p)
{
    return mp_cancel_test(p->s->cancel) ||
           mp_dash_source_terminal(p->s->global);
}

static int interrupt(void *opaque)
{
    struct dash_lavf *p = opaque;
    return stopped(p) || mp_dash_source_io_expired(p->s->global, p->deadline,
                                                   p->s->network_timeout);
}

static void begin_io(struct dash_lavf *p)
{
    p->deadline = stream_get_io_deadline(p->s);
    if (!p->deadline && p->s->network_timeout > 0)
        p->deadline = mp_time_ns_add(mp_time_ns(), p->s->network_timeout);
}

static int fail(struct dash_lavf *p, mpv_dash_source_failure failure,
                mpv_dash_failure_origin origin)
{
    if (!stopped(p))
        mp_dash_source_fail_with_origin(p->s->global, p->kind, failure, origin);
    p->s->error = AVERROR(EIO);
    return AVERROR(EIO);
}

static int admit(void *opaque)
{
    struct dash_lavf *p = opaque;
    if (interrupt(p)) return AVERROR_EXIT;
    if (p->requests++) return AVERROR(EIO);
    return 0;
}

static int header_value(struct dash_lavf *p, char **target, const char *value)
{
    if (*target) return fail(p, MPV_DASH_FAILURE_HTTP_RANGE,
                             MPV_DASH_ORIGIN_INVALID_PARTIAL_RANGE);
    bstr text = bstr_strip(bstr0(value));
    *target = bstrto0(p, text);
    return 0;
}

static int header(void *opaque, const char *line, size_t length)
{
    struct dash_lavf *p = opaque;
    /* Observed auth/risk precedes a later caller stop, as in curl. */
    bool cancelled = mp_cancel_test(p->s->cancel);
    if (mp_dash_source_terminal(p->s->global)) return AVERROR_EXIT;
    if (length >= 5 && !strncmp(line, "HTTP/", 5)) {
        const char *status = strchr(line, ' ');
        if (!status || strlen(++status) < 3 || status[0] < '0' || status[0] > '9' ||
            status[1] < '0' || status[1] > '9' || status[2] < '0' || status[2] > '9' ||
            (status[3] && status[3] != ' '))
            return fail(p, MPV_DASH_FAILURE_HTTP_STATUS, MPV_DASH_ORIGIN_MALFORMED_STATUS);
        int code = (status[0] - '0') * 100 + (status[1] - '0') * 10 + status[2] - '0';
        if (cancelled && code != 401 && code != 403 && code != 412) return AVERROR_EXIT;
        if (code >= 100 && code < 200 && !p->final_seen && p->interim_count++ < 8) {
            p->interim = true; return 0;
        }
        if (p->final_seen || code < 200)
            return fail(p, MPV_DASH_FAILURE_HTTP_STATUS, p->final_seen ?
                        MPV_DASH_ORIGIN_DUPLICATE_STATUS : MPV_DASH_ORIGIN_INTERIM_LIMIT);
        p->final_seen = true; p->interim = false; p->status = code;
        mpv_dash_source_failure failure = MPV_DASH_FAILURE_NONE;
        mpv_dash_failure_origin origin = MPV_DASH_ORIGIN_OTHER;
        if (code == 401 || code == 403) failure = MPV_DASH_FAILURE_HTTP_AUTH;
        else if (code == 412) failure = MPV_DASH_FAILURE_HTTP_RISK;
        else if (code >= 300 && code < 400) { failure = MPV_DASH_FAILURE_HTTP_STATUS; origin = MPV_DASH_ORIGIN_REDIRECT; }
        else if (code != 206 && p->start) { failure = MPV_DASH_FAILURE_HTTP_RANGE; origin = MPV_DASH_ORIGIN_NONZERO_OFFSET; }
        mp_dash_source_response(p->s->global, p->kind, code, failure, origin);
        return failure == MPV_DASH_FAILURE_NONE ? 0 : AVERROR(EIO);
    }
    if (cancelled) return AVERROR_EXIT;
    if (p->interim) {
        if (!length) p->interim = false;
        return 0;
    }
    if (length) {
        const char *value = strchr(line, ':');
        if (!value) return fail(p, MPV_DASH_FAILURE_HTTP_STATUS, MPV_DASH_ORIGIN_MALFORMED_STATUS);
        size_t name = value++ - line;
        if (name == 13 && !strncasecmp(line, "Content-Range", name)) return header_value(p, &p->range, value);
        if (name == 14 && !strncasecmp(line, "Content-Length", name)) return header_value(p, &p->length, value);
        if (name == 16 && !strncasecmp(line, "Content-Encoding", name)) return header_value(p, &p->encoding, value);
        if (name == 13 && !strncasecmp(line, "Accept-Ranges", name)) return header_value(p, &p->accept_ranges, value);
        if (name == 17 && !strncasecmp(line, "Transfer-Encoding", name)) return header_value(p, &p->transfer_encoding, value);
        return 0;
    }
    if (!p->final_seen) return fail(p, MPV_DASH_FAILURE_HTTP_STATUS, MPV_DASH_ORIGIN_MALFORMED_STATUS);
    uint64_t declared = 0;
    if (p->length) {
        const char *at = p->length;
        if (!mp_dash_http_number(&at, &declared) || *at)
            return fail(p, MPV_DASH_FAILURE_TRANSPORT, MPV_DASH_ORIGIN_DECLARED_LENGTH);
    }
    if (p->encoding && strcasecmp(p->encoding, "identity"))
        return fail(p, MPV_DASH_FAILURE_HTTP_RANGE, MPV_DASH_ORIGIN_INVALID_PARTIAL_RANGE);
    if (p->transfer_encoding && (p->length || strcasecmp(p->transfer_encoding, "chunked")))
        return fail(p, MPV_DASH_FAILURE_TRANSPORT, MPV_DASH_ORIGIN_DECLARED_LENGTH);
    if (p->status == 206) {
        int64_t total;
        if (!mp_dash_http_range(p->range, p->start, INT64_MAX, p->total,
                                &p->expected, &total) || (p->length && declared != p->expected))
            return fail(p, MPV_DASH_FAILURE_HTTP_RANGE, MPV_DASH_ORIGIN_INVALID_PARTIAL_RANGE);
        p->total = total; p->length_known = true; p->seekable = true;
        mp_dash_source_range_validated(p->s->global, p->kind);
    } else {
        p->length_known = p->length != NULL;
        p->expected = declared;
        if (p->length_known) p->total = declared;
        p->seekable = p->accept_ranges && !strcasecmp(p->accept_ranges, "bytes");
    }
    p->headers_ok = true;
    return 0;
}

static void close_io(struct dash_lavf *p)
{
    if (p->io) avio_closep(&p->io);
}

static int open_at(struct dash_lavf *p, uint64_t offset)
{
    while (!stopped(p) && mp_dash_source_idle_paused(p->s->global))
        mp_cancel_wait(p->s->cancel, 0.05);
    if (stopped(p)) return AVERROR_EXIT;
    close_io(p);
    talloc_free(p->range); talloc_free(p->length);
    talloc_free(p->encoding); talloc_free(p->accept_ranges);
    talloc_free(p->transfer_encoding);
    p->range = p->length = p->encoding = p->accept_ranges = NULL;
    p->transfer_encoding = NULL;
    p->start = p->offset = offset; p->received = p->expected = 0;
    p->status = p->interim_count = p->requests = 0;
    p->interim = p->final_seen = p->headers_ok = p->length_known = p->finished = false;
    begin_io(p);
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", p->config.allow_loopback_http ? "http,https,tcp,tls" : "https,tcp,tls", 0);
    av_dict_set(&options, "http_proxy", "", 0);
    av_dict_set(&options, "user_agent", p->config.user_agent, 0);
    av_dict_set(&options, "referer", p->config.referer, 0);
    av_dict_set(&options, "headers", "Accept-Encoding: identity\r\n", 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    av_dict_set(&options, "max_redirects", "0", 0);
    av_dict_set(&options, "reconnect", "0", 0);
    av_dict_set(&options, "multiple_requests", "0", 0);
    av_dict_set(&options, "seekable", "1", 0);
    av_dict_set_int(&options, "offset", offset, 0);
    AVIOInterruptCB cancel = {interrupt, p};
    AVIOHTTPObserver observer = {sizeof(observer), p, admit, header};
    int result = avio_open2_observed(&p->io, p->config.url, AVIO_FLAG_READ,
                                    &cancel, &options, &observer);
    av_dict_free(&options);
    if (result < 0 || !p->headers_ok || stopped(p)) {
        close_io(p);
        if (!stopped(p)) fail(p, MPV_DASH_FAILURE_TRANSPORT, MPV_DASH_ORIGIN_OTHER);
        return result < 0 ? result : AVERROR(EIO);
    }
    p->s->seekable = p->seekable;
    return 0;
}

static int finish_response(struct dash_lavf *p)
{
    if (p->finished) return 0;
    uint8_t extra;
    int end = avio_read_partial(p->io, &extra, 1);
    if ((end != AVERROR_EOF && end != 0) ||
        (p->io->error && p->io->error != AVERROR_EOF) || stopped(p))
        return fail(p, MPV_DASH_FAILURE_TRANSPORT, MPV_DASH_ORIGIN_BODY_LENGTH);
    p->finished = true;
    return 0;
}

static int fill(struct stream *s, void *buffer, int length)
{
    struct dash_lavf *p = s->priv;
    if (stopped(p)) { s->error = AVERROR_EXIT; return -1; }
    if (p->length_known && p->received == p->expected) {
        if (!p->finished) {
            begin_io(p);
            if (finish_response(p) < 0) return -1;
        }
        if (p->total >= 0 && p->offset == p->total) { s->error = 0; return -1; }
        if (p->status != 206 || open_at(p, p->offset) < 0) { s->error = AVERROR(EIO); return -1; }
    }
    if (p->length_known) length = MPMIN((uint64_t)length, p->expected - p->received);
    begin_io(p);
    int read = avio_read_partial(p->io, buffer, length);
    if (read > 0 && !stopped(p)) {
        p->received += read; p->offset += read;
        // A demuxer can stop at the known object size without asking for EOF.
        // Validate framing before releasing the final bytes to that demuxer.
        if (p->length_known && p->received == p->expected && finish_response(p) < 0)
            return -1;
        s->error = 0;
        return read;
    }
    bool eof = read == AVERROR_EOF || (!read && !p->io->error);
    if (!stopped(p) && (!eof || (p->length_known && p->received != p->expected)))
        fail(p, MPV_DASH_FAILURE_TRANSPORT, MPV_DASH_ORIGIN_BODY_LENGTH);
    s->error = stopped(p) ? AVERROR(EIO) : 0;
    return -1;
}

static int seek_to(struct stream *s, int64_t offset)
{
    struct dash_lavf *p = s->priv;
    if (offset < 0 || !p->seekable || (p->total >= 0 && offset > p->total)) return 0;
    if (offset == p->offset) return 1;
    if (offset == p->total) { close_io(p); p->offset = offset; p->received = p->expected = 0; p->length_known = p->finished = true; return 1; }
    return open_at(p, offset) >= 0;
}

static int64_t size(struct stream *s) { return ((struct dash_lavf *)s->priv)->total; }
static void close_stream(struct stream *s) { close_io(s->priv); }

int mp_dash_lavf_open(struct stream *s, const struct stream_open_args *args)
{
    if (s->mode != STREAM_READ) return STREAM_UNSUPPORTED;
    if (args->special_arg) return STREAM_UNSUPPORTED;
    if (args->open_offset < 0) return STREAM_ERROR;
    struct dash_lavf *p = talloc_zero(s, struct dash_lavf);
    p->s = s; p->total = -1; s->priv = p;
    if (!mp_dash_source_get_track(s->global, s->url, p, &p->kind, &p->config)) return STREAM_ERROR;
    s->close = close_stream;
    if (open_at(p, args->open_offset) < 0) return STREAM_ERROR;
    s->fill_buffer = fill; s->seek = seek_to; s->get_size = size;
    s->is_network = true; s->open_offset_ok = true;
    s->mime_type = talloc_strdup(s, p->kind == MPV_DASH_TRACK_VIDEO ? "video/mp4" : "audio/mp4");
    s->effective_url = talloc_strdup(s, s->url);
    mp_dash_source_transport_started(s->global, p->kind, false);
    return STREAM_OK;
}
