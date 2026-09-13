#ifndef MPLAYER_DEC_SUB_H
#define MPLAYER_DEC_SUB_H

#include <stdbool.h>
#include <stdint.h>

#include "player/core.h"
#include "stream/stream.h"
#include "osd.h"

struct sh_stream;
struct mpv_global;
struct demux_packet;
struct mp_recorder_sink;
struct dec_sub;
struct sd;

enum sd_ctrl {
    SD_CTRL_SUB_STEP,
    SD_CTRL_SET_ANIMATED_CHECK,
    SD_CTRL_SET_VIDEO_PARAMS,
    SD_CTRL_SET_VIDEO_DEF_FPS,
    SD_CTRL_RESET_SOFT,
    SD_CTRL_UPDATE_OPTS,
    SD_CTRL_APPLY_DVDNAV,   // const struct stream_nav_state *
};

enum sd_text_type {
    SD_TEXT_TYPE_PLAIN,
    SD_TEXT_TYPE_ASS,
    SD_TEXT_TYPE_ASS_FULL,
};

struct sd_times {
    double start;
    double end;
};

struct attachment_list {
    struct demux_attachment *entries;
    int num_entries;
};

struct sub_line {
    char *text;
    double start;
    double end;
};

struct sub_lines {
    struct sub_line *entries;
    int num_entries;
};

struct sub_text_cue {
    uint64_t id;
    double start;
    double duration;
    const char *text; // valid only for the duration of the callback
};

typedef void (*sub_text_cue_fn)(void *ctx,
                                const struct sub_text_cue *cue);

struct dec_sub *sub_create(struct mpv_global *global, struct track *track,
                           struct attachment_list *attachments, int order);
void sub_destroy(struct dec_sub *sub);

bool sub_can_preload(struct dec_sub *sub);
void sub_preload(struct dec_sub *sub);
void sub_redecode_cached_packets(struct dec_sub *sub);
void sub_read_packets(struct dec_sub *sub, double video_pts, bool force,
                      bool *packets_read, bool *sub_updated);
struct sub_bitmaps *sub_get_bitmaps(struct dec_sub *sub, struct mp_osd_res dim,
                                    int format, double pts);
char *sub_get_text(struct dec_sub *sub, double pts, enum sd_text_type type);
char *sub_ass_get_extradata(struct dec_sub *sub);
struct sd_times sub_get_times(struct dec_sub *sub, double pts);
// Return subtitle lines in memory. Call talloc_free() on the return value.
struct sub_lines *sub_get_lines(struct dec_sub *sub);
bool sub_set_text_cue_callback(struct dec_sub *sub,
                               sub_text_cue_fn callback, void *callback_ctx);
// Re-emit decoded text cues overlapping [start, end]. This is a decoder tap,
// not an OSD text snapshot, and can include overlapping or future cues.
bool sub_emit_text_cues(struct dec_sub *sub, double start, double end);
bool sub_map_player_cue_to_subtitle(struct dec_sub *sub,
                                    double player_start,
                                    double player_duration,
                                    double *subtitle_start,
                                    double *subtitle_duration);

struct sub_packet_timing_probe {
    bool visible;
    bool sub_updated;
    int cached_packet_index;
    double read_until;
};

// Internal deterministic probe for the packet visibility/read-ahead decisions
// used by sub_read_packets(). This is intentionally not part of libmpv's API.
void sub_test_packet_timing(const char *codec_profile,
                            double secondary_delay,
                            double video_pts,
                            bool force,
                            struct sub_packet_timing_probe *out);

void sub_reset(struct dec_sub *sub);
void sub_select(struct dec_sub *sub, bool selected);
void sub_set_recorder_sink(struct dec_sub *sub, struct mp_recorder_sink *sink);
void sub_set_play_dir(struct dec_sub *sub, int dir);
bool sub_is_primary_visible(struct dec_sub *sub);
bool sub_is_secondary_visible(struct dec_sub *sub);

int sub_control(struct dec_sub *sub, enum sd_ctrl cmd, void *arg);

#endif
