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

#include "ocr_policy.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum language {
    LANG_UNKNOWN = 0,
    LANG_ENGLISH,
    LANG_CHINESE,
    LANG_JAPANESE,
    LANG_KOREAN,
};

enum line_role {
    ROLE_AMBIGUOUS = 0,
    ROLE_SOURCE,
    ROLE_TARGET,
    ROLE_OTHER,
};

enum script_bits {
    SCRIPT_COMMON = 1 << 0,
    SCRIPT_LATIN = 1 << 1,
    SCRIPT_HAN = 1 << 2,
    SCRIPT_KANA = 1 << 3,
    SCRIPT_HANGUL = 1 << 4,
    SCRIPT_OTHER = 1 << 5,
};

struct candidate {
    enum line_role roles[MP_OCR_POLICY_MAX_LINES];
    size_t order[MP_OCR_POLICY_MAX_LINES];
    size_t source_lines;
    size_t target_lines;
    size_t other_lines;
    unsigned int signature;
};

static bool ascii_equal_ci(char a, char b)
{
    if (a >= 'A' && a <= 'Z')
        a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
        b += 'a' - 'A';
    return a == b;
}

static bool language_tag(const char *tag, const char *name)
{
    if (!tag || !name)
        return false;
    while (*name && *tag && *tag != '-' && *tag != '_') {
        if (!ascii_equal_ci(*tag++, *name++))
            return false;
    }
    return !*name && (!*tag || *tag == '-' || *tag == '_');
}

static enum language parse_language(const char *tag)
{
    if (language_tag(tag, "en") || language_tag(tag, "eng"))
        return LANG_ENGLISH;
    if (language_tag(tag, "zh") || language_tag(tag, "zho") ||
        language_tag(tag, "chi") || language_tag(tag, "cmn"))
        return LANG_CHINESE;
    if (language_tag(tag, "ja") || language_tag(tag, "jpn"))
        return LANG_JAPANESE;
    if (language_tag(tag, "ko") || language_tag(tag, "kor"))
        return LANG_KOREAN;
    return LANG_UNKNOWN;
}

static bool decode_utf8(const unsigned char **cursor, uint32_t *codepoint)
{
    const unsigned char *p = *cursor;
    uint32_t value;
    int continuation;
    if (*p < 0x80) {
        *codepoint = *p;
        *cursor = p + 1;
        return true;
    } else if (*p >= 0xc2 && *p <= 0xdf) {
        value = *p & 0x1f;
        continuation = 1;
    } else if (*p >= 0xe0 && *p <= 0xef) {
        value = *p & 0x0f;
        continuation = 2;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
        value = *p & 0x07;
        continuation = 3;
    } else {
        return false;
    }
    for (int n = 1; n <= continuation; n++) {
        if ((p[n] & 0xc0) != 0x80)
            return false;
        value = (value << 6) | (p[n] & 0x3f);
    }
    if ((continuation == 2 &&
         (value < 0x800 || (value >= 0xd800 && value <= 0xdfff))) ||
        (continuation == 3 && (value < 0x10000 || value > 0x10ffff)))
        return false;
    *codepoint = value;
    *cursor = p + continuation + 1;
    return true;
}

static int text_scripts(const char *text, size_t *script_characters)
{
    int scripts = 0;
    *script_characters = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t cp;
        if (!decode_utf8(&cursor, &cp))
            return -1;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= 0x00c0 && cp <= 0x024f)) {
            scripts |= SCRIPT_LATIN;
            (*script_characters)++;
        } else if ((cp >= 0x3040 && cp <= 0x30ff) ||
                   (cp >= 0x31f0 && cp <= 0x31ff)) {
            scripts |= SCRIPT_KANA;
            (*script_characters)++;
        } else if ((cp >= 0x3400 && cp <= 0x4dbf) ||
                   (cp >= 0x4e00 && cp <= 0x9fff) ||
                   (cp >= 0xf900 && cp <= 0xfaff)) {
            scripts |= SCRIPT_HAN;
            (*script_characters)++;
        } else if ((cp >= 0xac00 && cp <= 0xd7af) ||
                   (cp >= 0x1100 && cp <= 0x11ff)) {
            scripts |= SCRIPT_HANGUL;
            (*script_characters)++;
        } else if (cp < 0x80 || (cp >= '0' && cp <= '9') ||
                 (cp >= 0x2000 && cp <= 0x206f) ||
                 (cp >= 0x3000 && cp <= 0x303f) ||
                 (cp >= 0xff01 && cp <= 0xff20) ||
                 (cp >= 0xff3b && cp <= 0xff40) ||
                 (cp >= 0xff5b && cp <= 0xff65)) {
            scripts |= SCRIPT_COMMON;
        } else {
            scripts |= SCRIPT_OTHER;
            (*script_characters)++;
        }
    }
    return scripts;
}

static bool script_matches(int scripts, enum language language,
                           enum language counterpart)
{
    int meaningful = scripts & ~SCRIPT_COMMON;
    if (!meaningful || (scripts & SCRIPT_OTHER))
        return false;
    switch (language) {
    case LANG_ENGLISH:
        // Latin is not detected as English. It is only role-compatible when
        // the explicit counterpart uses a disjoint supported script.
        return meaningful == SCRIPT_LATIN &&
               counterpart != LANG_UNKNOWN &&
               counterpart != LANG_ENGLISH;
    case LANG_CHINESE:
        // Han alone cannot distinguish Chinese from Japanese.
        return meaningful == SCRIPT_HAN && counterpart != LANG_JAPANESE;
    case LANG_JAPANESE:
        return (meaningful & SCRIPT_KANA) &&
               !(meaningful & ~(SCRIPT_KANA | SCRIPT_HAN | SCRIPT_LATIN));
    case LANG_KOREAN:
        return (meaningful & SCRIPT_HANGUL) &&
               !(meaningful & ~(SCRIPT_HANGUL | SCRIPT_HAN | SCRIPT_LATIN));
    case LANG_UNKNOWN:
        return false;
    }
    return false;
}

static bool script_is_clearly_other(int scripts, enum language source,
                                    enum language target)
{
    int meaningful = scripts & ~SCRIPT_COMMON;
    if (scripts & SCRIPT_OTHER)
        return false;
    if (meaningful == SCRIPT_LATIN)
        return source != LANG_ENGLISH && target != LANG_ENGLISH;
    if ((meaningful & SCRIPT_KANA) &&
        !(meaningful & ~(SCRIPT_KANA | SCRIPT_HAN | SCRIPT_LATIN)))
        return source != LANG_JAPANESE && target != LANG_JAPANESE;
    if ((meaningful & SCRIPT_HANGUL) &&
        !(meaningful & ~(SCRIPT_HANGUL | SCRIPT_HAN | SCRIPT_LATIN)))
        return source != LANG_KOREAN && target != LANG_KOREAN;
    return false;
}

static enum line_role classify_line(const struct mp_ocr_policy_line *line,
                                    enum language source,
                                    enum language target,
                                    bool *invalid_utf8)
{
    size_t script_characters;
    int scripts = text_scripts(line->text, &script_characters);
    if (scripts < 0) {
        *invalid_utf8 = true;
        return ROLE_AMBIGUOUS;
    }
    if (!(line->confidence >= 0.80 && line->confidence <= 1.0) ||
        script_characters < 3)
        return ROLE_AMBIGUOUS;
    bool source_match = script_matches(scripts, source, target);
    bool target_match = script_matches(scripts, target, source);
    if (source_match != target_match)
        return source_match ? ROLE_SOURCE : ROLE_TARGET;
    return !source_match && script_is_clearly_other(scripts, source, target)
        ? ROLE_OTHER : ROLE_AMBIGUOUS;
}

static int compare_lines(const struct mp_ocr_policy_line *a,
                         const struct mp_ocr_policy_line *b)
{
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    return 0;
}

static void reading_order(const struct mp_ocr_policy_input *input,
                          size_t *order)
{
    for (size_t n = 0; n < input->num_lines; n++) {
        size_t pos = n;
        while (pos > 0 &&
               compare_lines(&input->lines[n],
                             &input->lines[order[pos - 1]]) < 0) {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = n;
    }
}

static bool geometry_is_coherent(const struct mp_ocr_policy_input *input,
                                 const size_t *order)
{
    int regions[3] = {0};
    size_t num_regions = 0;
    int min_x = INT_MAX;
    int max_right = INT_MIN;

    for (size_t n = 0; n < input->num_lines; n++) {
        const struct mp_ocr_policy_line *line = &input->lines[order[n]];
        if (line->w <= 0 || line->h <= 0 ||
            line->x > INT_MAX - line->w ||
            line->y > INT_MAX - line->h)
            return false;
        bool found = false;
        for (size_t r = 0; r < num_regions; r++)
            found |= regions[r] == line->region;
        if (!found) {
            if (num_regions == 2)
                return false;
            regions[num_regions++] = line->region;
        }
        if (line->x < min_x)
            min_x = line->x;
        if (line->x + line->w > max_right)
            max_right = line->x + line->w;
        if (n > 0) {
            const struct mp_ocr_policy_line *previous =
                &input->lines[order[n - 1]];
            long long overlap =
                (long long)previous->y + previous->h - line->y;
            int max_height = previous->h > line->h ? previous->h : line->h;
            if (max_height > INT_MAX / 3 ||
                overlap > max_height / 3 ||
                (long long)line->y -
                    ((long long)previous->y + previous->h) >
                    (long long)max_height * 3)
                return false;
        }
    }
    long long center = (long long)min_x + max_right;
    long long extent = (long long)max_right - min_x;
    if (extent > INT_MAX)
        return false;
    long long tolerance = extent / 5;
    if (tolerance < 12)
        tolerance = 12;
    for (size_t n = 0; n < input->num_lines; n++) {
        const struct mp_ocr_policy_line *line = &input->lines[order[n]];
        long long line_center = (long long)line->x * 2 + line->w;
        long long delta = line_center - center;
        if (delta < 0)
            delta = -delta;
        if (delta > tolerance * 2)
            return false;
    }
    return true;
}

static bool make_candidate(const struct mp_ocr_policy_input *input,
                           enum language source, enum language target,
                           struct candidate *candidate,
                           enum mp_ocr_policy_reason *failure)
{
    reading_order(input, candidate->order);
    bool invalid_utf8 = false;
    enum line_role previous = ROLE_AMBIGUOUS;
    int runs = 0;
    for (size_t n = 0; n < input->num_lines; n++) {
        size_t index = candidate->order[n];
        enum line_role role =
            classify_line(&input->lines[index], source, target, &invalid_utf8);
        candidate->roles[index] = role;
        if (role == ROLE_AMBIGUOUS) {
            *failure = invalid_utf8 ? MP_OCR_REASON_INVALID_UTF8
                                    : MP_OCR_REASON_AMBIGUOUS_LANGUAGE;
            return false;
        }
        candidate->source_lines += role == ROLE_SOURCE;
        candidate->target_lines += role == ROLE_TARGET;
        candidate->other_lines += role == ROLE_OTHER;
        if (role != previous) {
            runs++;
            previous = role;
        }
    }
    bool target_pair = candidate->target_lines && !candidate->other_lines;
    bool other_pair = candidate->other_lines && !candidate->target_lines;
    if (!candidate->source_lines || (!target_pair && !other_pair) ||
        runs != 2) {
        *failure = MP_OCR_REASON_NO_BILINGUAL_GROUP;
        return false;
    }
    if (!geometry_is_coherent(input, candidate->order)) {
        *failure = MP_OCR_REASON_SPATIAL_AMBIGUITY;
        return false;
    }
    bool source_first =
        candidate->roles[candidate->order[0]] == ROLE_SOURCE;
    candidate->signature = (source_first ? 1U : 0U) |
        ((unsigned int)candidate->source_lines << 1) |
        ((unsigned int)(candidate->target_lines +
                        candidate->other_lines) << 8) |
        (other_pair ? 1U << 15 : 0);
    return true;
}

static bool cue_was_seen(const struct mp_ocr_policy_context *context,
                         uint64_t cue_id)
{
    for (size_t n = 0; n < context->num_seen_cues; n++) {
        if (context->seen_cue_ids[n] == cue_id)
            return true;
    }
    return false;
}

static void remember_cue(struct mp_ocr_policy_context *context,
                         uint64_t cue_id)
{
    if (context->num_seen_cues < MP_OCR_POLICY_SEEN_CUES) {
        context->seen_cue_ids[context->num_seen_cues++] = cue_id;
    } else {
        context->seen_cue_ids[context->next_seen_cue] = cue_id;
        context->next_seen_cue =
            (context->next_seen_cue + 1) % MP_OCR_POLICY_SEEN_CUES;
    }
}

enum observation_state {
    OBSERVATION_NEW,
    OBSERVATION_REPEATED,
    OBSERVATION_INVALID_TIMING,
};

static enum observation_state begin_observation(
    struct mp_ocr_policy_context *context,
    const struct mp_ocr_policy_input *input)
{
    double end = input->start + input->duration;
    if (!isfinite(input->start) || !isfinite(input->duration) ||
        input->duration <= 0 || !isfinite(end))
        return OBSERVATION_INVALID_TIMING;
    if (cue_was_seen(context, input->cue_id) ||
        (context->has_observation &&
         input->cue_id <= context->last_observed_cue_id))
        return OBSERVATION_REPEATED;
    if (context->has_observation &&
        input->start < context->last_observed_end)
        return OBSERVATION_INVALID_TIMING;

    remember_cue(context, input->cue_id);
    context->last_revision = input->revision;
    context->last_observed_cue_id = input->cue_id;
    context->last_observed_start = input->start;
    context->last_observed_end = end;
    context->has_observation = true;
    return OBSERVATION_NEW;
}

static void observe_candidate(struct mp_ocr_policy_context *context,
                              const struct candidate *candidate)
{
    if (!context->convention_valid ||
        context->convention_signature != candidate->signature) {
        context->convention_valid = true;
        context->convention_signature = candidate->signature;
        context->convention_count = 1;
    } else if (context->convention_count < UINT_MAX) {
        context->convention_count++;
    }
}

static void observe_mismatch(struct mp_ocr_policy_context *context)
{
    context->convention_valid = false;
    context->convention_signature = 0;
    context->convention_count = 0;
}

static bool select_manual_source(const struct mp_ocr_policy_input *input,
                                 enum language source, enum language target,
                                 uint8_t *flags,
                                 enum mp_ocr_policy_reason *reason)
{
    bool found_source = false;
    bool invalid_utf8 = false;
    for (size_t n = 0; n < input->num_lines; n++) {
        enum line_role role =
            classify_line(&input->lines[n], source, target, &invalid_utf8);
        if (role == ROLE_AMBIGUOUS) {
            *reason = invalid_utf8 ? MP_OCR_REASON_INVALID_UTF8
                                   : MP_OCR_REASON_AMBIGUOUS_LANGUAGE;
            return false;
        }
        flags[n] = role == ROLE_SOURCE;
        found_source |= flags[n] != 0;
    }
    if (!found_source) {
        *reason = MP_OCR_REASON_SOURCE_NOT_FOUND;
        return false;
    }
    *reason = MP_OCR_REASON_MANUAL_SOURCE_SELECTED;
    return true;
}

static void select_all(const struct mp_ocr_policy_input *input,
                       uint8_t *flags)
{
    for (size_t n = 0; n < input->num_lines; n++)
        flags[n] = 1;
}

static size_t joined_size(const struct mp_ocr_policy_input *input,
                          const size_t *order, const uint8_t *flags,
                          size_t *selected_lines)
{
    size_t length = 0;
    *selected_lines = 0;
    for (size_t n = 0; n < input->num_lines; n++) {
        size_t index = order[n];
        if (!flags[index])
            continue;
        size_t text_length = strlen(input->lines[index].text);
        if (text_length > SIZE_MAX - length - (*selected_lines != 0))
            return SIZE_MAX;
        length += text_length + (*selected_lines != 0);
        (*selected_lines)++;
    }
    return length == SIZE_MAX ? SIZE_MAX : length + 1;
}

static void join_lines(const struct mp_ocr_policy_input *input,
                       const size_t *order, const uint8_t *flags,
                       char *output)
{
    size_t length = 0;
    bool first = true;
    for (size_t n = 0; n < input->num_lines; n++) {
        size_t index = order[n];
        if (!flags[index])
            continue;
        if (!first)
            output[length++] = '\n';
        size_t text_length = strlen(input->lines[index].text);
        memcpy(output + length, input->lines[index].text, text_length);
        length += text_length;
        first = false;
    }
    output[length] = '\0';
}

static enum language effective_source(const struct mp_ocr_policy_input *input)
{
    if (input->source_lang && input->source_lang[0] &&
        !language_tag(input->source_lang, "auto"))
        return parse_language(input->source_lang);
    return parse_language(input->track_lang);
}

bool mp_ocr_policy_select(struct mp_ocr_policy_context *context,
                          const struct mp_ocr_policy_input *input,
                          char *output, size_t output_capacity,
                          uint8_t *selected_flags,
                          size_t selected_flags_capacity,
                          struct mp_ocr_policy_result *result)
{
    if (!result)
        return false;
    *result = (struct mp_ocr_policy_result){
        .ambiguous = true,
        .reason = MP_OCR_REASON_INVALID_INPUT,
    };
    if (!context || !input || !input->lines || !input->num_lines ||
        input->num_lines > MP_OCR_POLICY_MAX_LINES ||
        input->mode < MP_OCR_SELECT_AUTO ||
        input->mode > MP_OCR_SELECT_FULL ||
        (selected_flags &&
         selected_flags_capacity < input->num_lines)) {
        return false;
    }
    for (size_t n = 0; n < input->num_lines; n++) {
        if (!input->lines[n].text)
            return false;
    }

    uint8_t local_flags[MP_OCR_POLICY_MAX_LINES] = {0};
    size_t order[MP_OCR_POLICY_MAX_LINES];
    uint8_t *flags = selected_flags ? selected_flags : local_flags;
    memset(flags, 0, input->num_lines);
    reading_order(input, order);

    enum language source = effective_source(input);
    enum language target = parse_language(input->target_lang);
    enum mp_ocr_policy_reason reason = MP_OCR_REASON_UNKNOWN_LANGUAGE;
    bool already_target = false;

    if (input->mode == MP_OCR_SELECT_FULL) {
        select_all(input, flags);
        reason = MP_OCR_REASON_FULL_REQUESTED;
        result->ambiguous = false;
    } else if (source == LANG_UNKNOWN ||
               (input->mode == MP_OCR_SELECT_AUTO &&
                target == LANG_UNKNOWN)) {
        select_all(input, flags);
    } else if (input->mode == MP_OCR_SELECT_SOURCE) {
        if (select_manual_source(input, source, target, flags, &reason)) {
            result->ambiguous = false;
        } else {
            memset(flags, 0, input->num_lines);
            select_all(input, flags);
        }
    } else {
        enum observation_state observation =
            input->reevaluate ? OBSERVATION_REPEATED
                              : begin_observation(context, input);
        if (observation == OBSERVATION_REPEATED && !input->reevaluate) {
            select_all(input, flags);
            reason = MP_OCR_REASON_REPEATED_CUE;
            goto finish;
        }
        if (observation == OBSERVATION_INVALID_TIMING) {
            select_all(input, flags);
            observe_mismatch(context);
            reason = MP_OCR_REASON_INVALID_TIMING;
            goto finish;
        }
        result->observed = observation == OBSERVATION_NEW;
        struct candidate candidate = {0};
        if (!make_candidate(input, source, target,
                            &candidate, &reason)) {
            select_all(input, flags);
            if (result->observed)
                observe_mismatch(context);
        } else {
            if (result->observed)
                observe_candidate(context, &candidate);
            if (context->convention_count >=
                    MP_OCR_POLICY_REQUIRED_OBSERVATIONS &&
                context->convention_signature == candidate.signature) {
                bool reuse_target = candidate.target_lines != 0;
                for (size_t n = 0; n < input->num_lines; n++) {
                    flags[n] = candidate.roles[n] ==
                        (reuse_target ? ROLE_TARGET : ROLE_SOURCE);
                }
                already_target = reuse_target;
                reason = reuse_target
                    ? MP_OCR_REASON_BILINGUAL_TARGET_REUSED
                    : MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED;
                result->ambiguous = false;
            } else {
                select_all(input, flags);
                reason = input->reevaluate ? MP_OCR_REASON_REPEATED_CUE
                                           : MP_OCR_REASON_LAYOUT_UNCONFIRMED;
            }
        }
    }

finish:
    result->required_capacity =
        joined_size(input, order, flags, &result->selected_lines);
    result->already_target = already_target;
    result->reason = reason;
    if (result->required_capacity == SIZE_MAX ||
        !output || output_capacity < result->required_capacity) {
        if (output && output_capacity)
            output[0] = '\0';
        result->already_target = false;
        result->ambiguous = true;
        result->reason = MP_OCR_REASON_OUTPUT_TOO_SMALL;
        return false;
    }
    join_lines(input, order, flags, output);
    return true;
}

void mp_ocr_policy_reset(struct mp_ocr_policy_context *context)
{
    if (context)
        *context = (struct mp_ocr_policy_context){0};
}

const char *mp_ocr_policy_reason_string(enum mp_ocr_policy_reason reason)
{
    static const char *const names[] = {
        [MP_OCR_REASON_BILINGUAL_TARGET_REUSED] = "bilingual-target-reused",
        [MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED] =
            "bilingual-source-selected",
        [MP_OCR_REASON_MANUAL_SOURCE_SELECTED] = "manual-source-selected",
        [MP_OCR_REASON_FULL_REQUESTED] = "full-requested",
        [MP_OCR_REASON_LAYOUT_UNCONFIRMED] = "layout-unconfirmed",
        [MP_OCR_REASON_REPEATED_CUE] = "repeated-cue",
        [MP_OCR_REASON_UNKNOWN_LANGUAGE] = "unknown-language",
        [MP_OCR_REASON_AMBIGUOUS_LANGUAGE] = "ambiguous-language",
        [MP_OCR_REASON_NO_BILINGUAL_GROUP] = "no-bilingual-group",
        [MP_OCR_REASON_SPATIAL_AMBIGUITY] = "spatial-ambiguity",
        [MP_OCR_REASON_SOURCE_NOT_FOUND] = "source-not-found",
        [MP_OCR_REASON_INVALID_TIMING] = "invalid-timing",
        [MP_OCR_REASON_INVALID_UTF8] = "invalid-utf8",
        [MP_OCR_REASON_INVALID_INPUT] = "invalid-input",
        [MP_OCR_REASON_OUTPUT_TOO_SMALL] = "output-too-small",
    };
    if (reason < 0 || (size_t)reason >= sizeof(names) / sizeof(names[0]) ||
        !names[reason])
        return "invalid-reason";
    return names[reason];
}
