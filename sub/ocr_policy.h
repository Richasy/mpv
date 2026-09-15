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

#ifndef MP_OCR_POLICY_H
#define MP_OCR_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_OCR_POLICY_MAX_LINES 64
#define MP_OCR_POLICY_SEEN_CUES 16
#define MP_OCR_POLICY_REQUIRED_OBSERVATIONS 3

enum mp_ocr_selection_mode {
    MP_OCR_SELECT_AUTO = 0,
    MP_OCR_SELECT_SOURCE,
    MP_OCR_SELECT_FULL,
};

enum mp_ocr_policy_reason {
    MP_OCR_REASON_BILINGUAL_TARGET_REUSED = 0,
    MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED,
    MP_OCR_REASON_MANUAL_SOURCE_SELECTED,
    MP_OCR_REASON_FULL_REQUESTED,
    MP_OCR_REASON_LAYOUT_UNCONFIRMED,
    MP_OCR_REASON_REPEATED_CUE,
    MP_OCR_REASON_UNKNOWN_LANGUAGE,
    MP_OCR_REASON_AMBIGUOUS_LANGUAGE,
    MP_OCR_REASON_NO_BILINGUAL_GROUP,
    MP_OCR_REASON_SPATIAL_AMBIGUITY,
    MP_OCR_REASON_SOURCE_NOT_FOUND,
    MP_OCR_REASON_INVALID_TIMING,
    MP_OCR_REASON_INVALID_UTF8,
    MP_OCR_REASON_INVALID_INPUT,
    MP_OCR_REASON_OUTPUT_TOO_SMALL,
};

struct mp_ocr_policy_line {
    const char *text;
    int x;
    int y;
    int w;
    int h;
    int region;
    double confidence;
};

struct mp_ocr_policy_input {
    const struct mp_ocr_policy_line *lines;
    size_t num_lines;
    uint64_t cue_id;
    uint64_t revision;
    double start;
    double duration;
    bool reevaluate; // apply established context without observing this cue again
    enum mp_ocr_selection_mode mode;
    const char *source_lang;
    const char *target_lang;
    const char *track_lang;
};

struct mp_ocr_policy_context {
    uint64_t seen_cue_ids[MP_OCR_POLICY_SEEN_CUES];
    size_t num_seen_cues;
    size_t next_seen_cue;
    uint64_t last_revision;
    uint64_t last_observed_cue_id;
    double last_observed_start;
    double last_observed_end;
    unsigned int convention_count;
    unsigned int convention_signature;
    bool convention_valid;
    bool has_observation;
};

struct mp_ocr_policy_result {
    size_t required_capacity;
    size_t selected_lines;
    bool already_target;
    bool ambiguous;
    bool observed;
    enum mp_ocr_policy_reason reason;
};

/*
 * Select OCR text without allocation or external dependencies.
 *
 * output is newline-joined in geometric reading order. selected_flags, when
 * non-NULL, is indexed like input->lines. On success required_capacity includes
 * the terminating NUL. A false return means invalid input or insufficient
 * output/flag capacity; result still describes the failure.
 *
 * AUTO acts only after three distinct, time-ordered, nonoverlapping cue IDs
 * establish the same mutually center-aligned two-script layout. It reuses a
 * matching target block, or selects the known source block when the companion
 * block is clearly disjoint from both source and target scripts. This is
 * evidence of an authoring convention, never language detection or proof of
 * translation/semantic equivalence. Short script runs, Han-only Chinese versus
 * Japanese, initial layout, extra objects, malformed UTF-8, invalid timing, and
 * unknown scripts preserve the full OCR text. SOURCE filters only when every
 * line is confidently source, target, or a clearly disjoint script. FULL always
 * retains every line. A NULL, empty, or literal "auto" source_lang uses
 * track_lang; any other unsupported explicit tag remains unknown. Call reset on
 * seek, track change, or language/config change. observed reports whether this
 * call consumed a new context observation. reevaluate applies the current
 * convention to cached cues without changing context or counting them again.
 */
bool mp_ocr_policy_select(struct mp_ocr_policy_context *context,
                          const struct mp_ocr_policy_input *input,
                          char *output, size_t output_capacity,
                          uint8_t *selected_flags,
                          size_t selected_flags_capacity,
                          struct mp_ocr_policy_result *result);

void mp_ocr_policy_reset(struct mp_ocr_policy_context *context);

const char *mp_ocr_policy_reason_string(enum mp_ocr_policy_reason reason);

#endif
