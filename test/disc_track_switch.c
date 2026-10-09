// SPDX-License-Identifier: LGPL-2.1-or-later

#include <assert.h>
#include <string.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/stheader.h"
#include "misc/dispatch.h"
#include "player/core.h"
#include "stream/stream.h"

struct fixture {
    struct MPContext player;
    struct demuxer demuxer;
    struct stream stream;
    stream_info_t info;
    struct stream_nav_state nav;
    struct mp_codec_params codec;
    struct sh_stream header;
    struct track track;
};

static int control(struct stream *stream, int command, void *argument)
{
    assert(command == STREAM_CTRL_GET_NAV_STATE);
    *(struct stream_nav_state *)argument =
        *(struct stream_nav_state *)stream->priv;
    return STREAM_OK;
}

static void init(struct fixture *f, enum stream_type type)
{
    memset(f, 0, sizeof(*f));
    f->info.name = "iso/bluray";
    f->stream.info = &f->info;
    f->stream.control = control;
    f->stream.priv = &f->nav;
    f->demuxer.stream = &f->stream;
    f->demuxer.seekable = true;
    f->demuxer.partially_seekable = true;
    f->codec.codec = type == STREAM_SUB ? "hdmv_pgs_subtitle" : "ac3";
    f->header.type = type;
    f->header.codec = &f->codec;
    f->track.type = type;
    f->track.selected = true;
    f->track.demuxer = &f->demuxer;
    f->track.stream = &f->header;
    f->player.demuxer = &f->demuxer;
    f->player.log = mp_null_log;
    f->player.dispatch = mp_dispatch_create(NULL);
    f->player.playback_initialized = true;
    f->player.play_dir = 1;
    f->player.playback_pts = 445.592267;
    f->player.last_seek_pts = MP_NOPTS_VALUE;
    f->player.current_track[0][type] = &f->track;
}

static void finish(struct fixture *f)
{
    talloc_free(f->player.dispatch);
}

static void assert_no_refresh(struct fixture *f)
{
    assert(!disc_nav_refresh_track(&f->player, &f->track));
    assert(f->player.seek.type == MPSEEK_NONE);
}

static void check_audio_refresh(void)
{
    struct fixture f;
    init(&f, STREAM_AUDIO);
    assert(disc_nav_refresh_track(&f.player, &f.track));
    assert(f.player.seek.type == MPSEEK_ABSOLUTE);
    assert(f.player.seek.amount == f.player.playback_pts);
    assert(f.player.seek.exact == MPSEEK_EXACT);
    assert(!(f.player.seek.flags & MPSEEK_FLAG_SUBPREROLL));
    finish(&f);
}

static void check_subtitle_refresh(void)
{
    struct fixture f;
    init(&f, STREAM_SUB);
    f.player.paused = true;
    assert(disc_nav_refresh_track(&f.player, &f.track));
    assert(f.player.seek.type == MPSEEK_ABSOLUTE);
    assert(f.player.seek.amount == f.player.playback_pts);
    assert(f.player.seek.flags & MPSEEK_FLAG_SUBPREROLL);
    assert(f.player.paused);
    finish(&f);
}

static void check_queued_seek_is_retained(void)
{
    struct fixture f;
    init(&f, STREAM_AUDIO);
    f.player.seek = (struct seek_params){
        .type = MPSEEK_ABSOLUTE,
        .exact = MPSEEK_EXACT,
        .amount = 900,
    };
    assert(disc_nav_refresh_track(&f.player, &f.track));
    assert(f.player.seek.amount == 900);
    finish(&f);
}

static void check_in_progress_seek_is_retained(void)
{
    struct fixture f;
    init(&f, STREAM_AUDIO);
    f.player.playback_pts = MP_NOPTS_VALUE;
    f.player.last_seek_pts = 600;
    f.player.current_seek = (struct seek_params){
        .type = MPSEEK_ABSOLUTE,
        .exact = MPSEEK_EXACT,
        .amount = 600,
    };
    assert(disc_nav_refresh_track(&f.player, &f.track));
    assert(f.player.seek.amount == 600);
    finish(&f);
}

static void check_navigation_boundaries(void)
{
    struct fixture f;
    init(&f, STREAM_SUB);
    f.nav.nav_active = true;
    assert_no_refresh(&f);
    f.nav = (struct stream_nav_state){.menu_active = true};
    assert_no_refresh(&f);
    f.nav = (struct stream_nav_state){.still_active = true};
    assert_no_refresh(&f);
    f.nav = (struct stream_nav_state){.drain_pending = true};
    assert_no_refresh(&f);
    finish(&f);
}

#if HAVE_LIBBLURAY
static void check_plain_title_navigation_snapshot(void)
{
    struct fixture f;
    init(&f, STREAM_SUB);
    assert(stream_bluray_test_plain_nav_state(&f.nav) == STREAM_OK);
    assert(!f.nav.nav_active);
    assert(f.nav.still_active);
    assert(f.nav.drain_pending);
    assert(f.nav.discontinuity_id == 7);
    assert_no_refresh(&f);
    finish(&f);
}
#endif

static void check_source_boundaries(void)
{
    struct fixture f;
    init(&f, STREAM_AUDIO);
    const char *unchanged[] = {"file", "ffmpeg", "dvdnav", "iso/dvdnav"};
    for (int n = 0; n < MP_ARRAY_SIZE(unchanged); n++) {
        f.info.name = unchanged[n];
        assert_no_refresh(&f);
    }
    f.info.name = "iso/bluray";
    f.demuxer.partially_seekable = false;
    assert_no_refresh(&f);
    f.demuxer.partially_seekable = true;
    f.demuxer.seekable = false;
    assert_no_refresh(&f);
    f.demuxer.seekable = true;
    f.track.is_external = true;
    assert_no_refresh(&f);
    f.track.is_external = false;
    f.track.selected = false;
    assert_no_refresh(&f);
    finish(&f);
}

static void check_virtual_subtitles_and_startup(void)
{
    struct fixture f;
    init(&f, STREAM_SUB);
    f.codec.codec = "eia_608";
    assert_no_refresh(&f);
    f.codec.codec = "text";
    f.codec.codec_profile = "whisper";
    assert_no_refresh(&f);
    f.codec.codec_profile = "translated";
    assert_no_refresh(&f);
    f.codec.codec = "hdmv_pgs_subtitle";
    f.codec.codec_profile = NULL;
    f.player.playback_initialized = false;
    assert_no_refresh(&f);
    f.player.playback_initialized = true;
    f.player.playback_pts = MP_NOPTS_VALUE;
    assert_no_refresh(&f);
    finish(&f);
}

int main(void)
{
    check_audio_refresh();
    check_subtitle_refresh();
    check_queued_seek_is_retained();
    check_in_progress_seek_is_retained();
    check_navigation_boundaries();
#if HAVE_LIBBLURAY
    check_plain_title_navigation_snapshot();
#endif
    check_source_boundaries();
    check_virtual_subtitles_and_startup();
    return 0;
}
