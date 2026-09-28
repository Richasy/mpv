/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpv_talloc.h"
#include "sub/ass_mp.h"
#include "sub/dec_sub.h"

static uint64_t image_hash(ASS_Image *image)
{
    uint64_t hash = 1;
    assert(image);
    for (; image; image = image->next) {
        hash = hash * 31 + image->dst_x;
        hash = hash * 31 + image->dst_y;
        hash = hash * 31 + image->color;
        for (int y = 0; y < image->h; y++) {
            for (int x = 0; x < image->w; x++)
                hash = hash * 31 + image->bitmap[y * image->stride + x];
        }
    }
    return hash;
}

static ASS_Track *new_cue_track(ASS_Library *library)
{
    static const char header[] =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,"
        "OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,"
        "ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "\n"
        "[Events]\n"
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
    ASS_Track *track = ass_new_track(library);
    assert(track);
    ass_process_codec_private(track, header, (int)sizeof(header) - 1);
    assert(track->n_styles == 1 && track->n_events == 0);
    return track;
}

static void add_cue(ASS_Track *track, const char *profile, int order,
                    long long start, long long duration, const char *text)
{
    char packet[256];
    int length = snprintf(packet, sizeof(packet),
                          "%d,0,Default,,0,0,0,,%s", order, text);
    assert(length > 0 && (size_t)length < sizeof(packet));
    int first = track->n_events;
    ass_process_chunk(track, packet, length, start, duration);
    assert(track->n_events == first + 1);
    mp_ass_clip_whisper_cues(track, first, profile);
}

static void check_whisper_cue_timing(ASS_Library *library, ASS_Renderer *renderer)
{
    ASS_Track *track = new_cue_track(library);
    add_cue(track, "whisper", 11, 2196795, 8200, "earlier");
    add_cue(track, "whisper", 14, 2201023, 500, "next");
    assert(track->events[0].Duration == 4228);
    assert(track->events[1].Duration == 500);

    add_cue(track, "whisper", 18, 2208423, 10000,
            "{\\fs72}translated\\N{\\fs48}original");
    assert(track->events[1].Duration == 500);
    add_cue(track, "whisper", 20, 2211039, 1000, "second speaker");
    assert(track->events[2].Duration == 2616);
    assert(track->events[3].Duration == 1000);
    assert(!strcmp(track->events[2].Text,
                   "{\\fs72}translated\\N{\\fs48}original"));

    ASS_Track *expected = new_cue_track(library);
    add_cue(expected, NULL, 20, 2211039, 1000, "second speaker");
    int changed;
    assert(image_hash(ass_render_frame(renderer, track, 2211500, &changed)) ==
           image_hash(ass_render_frame(renderer, expected, 2211500, &changed)));
    ass_free_track(expected);
    ass_free_track(track);

    track = new_cue_track(library);
    add_cue(track, "whisper", 37, 2249087, 2000, "later");
    add_cue(track, "whisper", 35, 2240071, 10000, "earlier");
    assert(track->events[1].Duration == 9016);
    add_cue(track, "whisper", 38, 2241087, 2000, "middle");
    assert(track->events[1].Duration == 1016);
    assert(track->events[2].Duration == 2000);
    assert(track->events[0].Duration == 2000);
    expected = new_cue_track(library);
    add_cue(expected, NULL, 38, 2241087, 2000, "middle");
    assert(image_hash(ass_render_frame(renderer, track, 2241500, &changed)) ==
           image_hash(ass_render_frame(renderer, expected, 2241500, &changed)));
    ass_free_track(expected);
    ass_free_track(track);

    track = new_cue_track(library);
    add_cue(track, NULL, 18, 2208423, 10000, "ordinary ASS");
    add_cue(track, NULL, 20, 2211039, 1000, "later ASS");
    assert(track->events[0].Duration == 10000);
    ass_free_track(track);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    char *script = talloc_size(NULL, size + 1);
    assert(fread(script, 1, size, file) == (size_t)size);
    script[size] = 0;
    fclose(file);

    ASS_Library *library = ass_library_init();
    assert(library);
    ASS_Track *track = ass_read_memory(library, script, size, NULL);
    assert(track && track->n_events == 9 && track->PlayResX == 640 &&
           track->PlayResY == 360 && track->WrapStyle == 2);
    ASS_Renderer *renderer = ass_renderer_init(library);
    assert(renderer);
    ass_set_frame_size(renderer, 640, 360);
    ass_set_storage_size(renderer, 640, 360);
    ass_set_fonts(renderer, NULL, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    int changed;
    uint64_t source_image = image_hash(ass_render_frame(renderer, track, 1200,
                                                       &changed));
    ASS_Event *events = talloc_memdup(NULL, track->events,
                                    track->n_events * sizeof(*events));
    ASS_Style *styles = talloc_memdup(NULL, track->styles,
                                    track->n_styles * sizeof(*styles));
    struct sub_text_replacement replacement = {
        .id = mp_ass_event_id(&track->events[0]),
        .source = talloc_strdup(NULL, track->events[0].Text),
        .text = "{\\i1}A substantially longer translated line\\Nsecond line{\\i0}",
    };
    uint64_t translated_image = image_hash(mp_ass_render_replacements(
        renderer, track, 1200, &changed, &replacement, 1, -1));
    assert(translated_image != source_image);
    for (int n = 0; n < track->n_events; n++) {
        assert(track->events[n].Text == events[n].Text);
        // libass owns collision bookkeeping; every script field stays intact.
        events[n].render_priv = track->events[n].render_priv;
        assert(!memcmp(&events[n], &track->events[n], sizeof(*events)));
    }
    assert(!memcmp(styles, track->styles, track->n_styles * sizeof(*styles)));
    assert(track->PlayResX == 640 && track->PlayResY == 360 &&
           track->WrapStyle == 2);
    assert(mp_ass_event_id(&track->events[0]) == replacement.id);
    assert(!strcmp(track->events[0].Text, replacement.source));
    assert(image_hash(mp_ass_render_replacements(
        renderer, track, 1200, &changed, NULL, 0, -1)) == source_image);

    // Event storage may move between frames. No retained event pointer is used.
    for (int n = 0; n < 128; n++) {
        int index = ass_alloc_event(track);
        assert(index >= 0);
        track->events[index].Start = 10000;
        track->events[index].Duration = 1000;
        track->events[index].Style = track->default_style;
        track->events[index].Text = strdup("future");
    }
    assert(image_hash(mp_ass_render_replacements(
        renderer, track, 1200, &changed, &replacement, 1, -1)) ==
        translated_image);
    check_whisper_cue_timing(library, renderer);
#if LIBASS_VERSION >= 0x01703010
    // Auto-pruning must never free a borrowed translated Text pointer.
    ass_configure_prune(track, 0);
    mp_ass_render_replacements(renderer, track, 4000, &changed,
                              &replacement, 1, 0);
    assert(track->n_events == 128);
    for (int n = 0; n < track->n_events; n++)
        assert(!strcmp(track->events[n].Text, "future"));
#endif
    ass_renderer_done(renderer);
    ass_free_track(track);
    ass_library_done(library);
    talloc_free((char *)replacement.source);
    talloc_free(events);
    talloc_free(styles);
    talloc_free(script);
    return 0;
}
