#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "sub/ocr_policy.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static struct mp_ocr_policy_line line(const char *text, int x, int y,
                                      int w, int region)
{
    return (struct mp_ocr_policy_line){
        .text = text,
        .x = x,
        .y = y,
        .w = w,
        .h = 20,
        .region = region,
        .confidence = 0.95,
    };
}

static struct mp_ocr_policy_input input(
    const struct mp_ocr_policy_line *lines, size_t count, uint64_t cue_id,
    enum mp_ocr_selection_mode mode, const char *source, const char *target)
{
    return (struct mp_ocr_policy_input){
        .lines = lines,
        .num_lines = count,
        .cue_id = cue_id,
        .revision = 1,
        .start = (double)cue_id * 3.0,
        .duration = 2.0,
        .mode = mode,
        .source_lang = source,
        .target_lang = target,
    };
}

static struct mp_ocr_policy_result select_text(
    struct mp_ocr_policy_context *context,
    const struct mp_ocr_policy_input *policy_input,
    char *output, size_t output_size, uint8_t *flags)
{
    struct mp_ocr_policy_result result;
    assert(mp_ocr_policy_select(context, policy_input, output, output_size,
                                flags, policy_input->num_lines, &result));
    return result;
}

static void establish(struct mp_ocr_policy_context *context,
                      const struct mp_ocr_policy_line *lines, size_t count,
                      const char *source, const char *target,
                      const char *expected)
{
    char output[256];
    uint8_t flags[MP_OCR_POLICY_MAX_LINES];
    for (uint64_t cue = 1; cue <= 3; cue++) {
        struct mp_ocr_policy_input policy_input =
            input(lines, count, cue, MP_OCR_SELECT_AUTO, source, target);
        struct mp_ocr_policy_result result =
            select_text(context, &policy_input, output, sizeof(output), flags);
        if (cue < 3) {
            assert(result.reason == MP_OCR_REASON_LAYOUT_UNCONFIRMED);
            assert(result.ambiguous && !result.already_target);
            assert(result.observed);
        } else {
            assert(result.reason == MP_OCR_REASON_BILINGUAL_TARGET_REUSED);
            assert(!result.ambiguous && result.already_target);
            assert(result.observed);
            assert(strcmp(output, expected) == 0);
        }
    }

}

static void check_both_orders(void)
{
    struct mp_ocr_policy_line source_first[] = {
        line("A quiet morning", 120, 100, 240, 1),
        line("安静的早晨", 150, 124, 180, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, source_first, ARRAY_SIZE(source_first),
              "en-US", "zh-CN", "安静的早晨");

    struct mp_ocr_policy_line target_first[] = {
        line("安静的早晨", 150, 100, 180, 3),
        line("A quiet morning", 120, 124, 240, 4),
    };
    mp_ocr_policy_reset(&context);
    establish(&context, target_first, ARRAY_SIZE(target_first),
              "eng", "cmn", "安静的早晨");
}

static void check_wrapped_source(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("The train leaves", 120, 80, 240, 1),
        line("after sunrise", 145, 102, 190, 1),
        line("列车日出后出发", 135, 128, 210, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, lines, ARRAY_SIZE(lines), "en", "zh",
              "列车日出后出发");

    char output[256];
    uint8_t flags[ARRAY_SIZE(lines)];
    struct mp_ocr_policy_input policy_input =
        input(lines, ARRAY_SIZE(lines), 9, MP_OCR_SELECT_SOURCE, "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(strcmp(output, "The train leaves\nafter sunrise") == 0);
    assert(result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);
    assert(result.selected_lines == 2 && !result.already_target);
}

static void check_japanese_english_targeting_chinese(void)
{
    struct mp_ocr_policy_line source_first[] = {
        line("朝の電車です", 130, 80, 220, 1),
        line("Morning train.", 140, 104, 200, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[3];
    for (uint64_t cue = 1; cue <= 3; cue++) {
        struct mp_ocr_policy_input policy_input =
            input(source_first, ARRAY_SIZE(source_first), cue,
                  MP_OCR_SELECT_AUTO, "ja", "zh");
        struct mp_ocr_policy_result result =
            select_text(&context, &policy_input, output, sizeof(output), flags);
        if (cue < 3) {
            assert(strcmp(output, "朝の電車です\nMorning train.") == 0);
            assert(result.reason == MP_OCR_REASON_LAYOUT_UNCONFIRMED);
        } else {
            assert(strcmp(output, "朝の電車です") == 0);
            assert(result.reason ==
                   MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED);
            assert(!result.already_target && !result.ambiguous);
        }
    }

    struct mp_ocr_policy_input manual =
        input(source_first, ARRAY_SIZE(source_first), 4,
              MP_OCR_SELECT_SOURCE, "ja", "zh");
    struct mp_ocr_policy_result manual_result =
        select_text(&context, &manual, output, sizeof(output), flags);
    assert(strcmp(output, "朝の電車です") == 0);
    assert(manual_result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);

    struct mp_ocr_policy_line other_first[] = {
        line("Morning train.", 140, 80, 200, 2),
        line("朝の電車です", 130, 104, 220, 1),
    };
    mp_ocr_policy_reset(&context);
    for (uint64_t cue = 1; cue <= 3; cue++) {
        struct mp_ocr_policy_input policy_input =
            input(other_first, ARRAY_SIZE(other_first), cue,
                  MP_OCR_SELECT_AUTO, "ja", "zh");
        struct mp_ocr_policy_result result =
            select_text(&context, &policy_input, output, sizeof(output), flags);
        if (cue == 3) {
            assert(strcmp(output, "朝の電車です") == 0);
            assert(result.reason ==
                   MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED);
        }
    }

    struct mp_ocr_policy_line wrapped[] = {
        line("朝の電車です", 130, 80, 220, 1),
        line("次の駅へ行きます", 120, 102, 240, 1),
        line("Morning train.", 140, 128, 200, 2),
    };
    mp_ocr_policy_reset(&context);
    for (uint64_t cue = 1; cue <= 3; cue++) {
        struct mp_ocr_policy_input policy_input =
            input(wrapped, ARRAY_SIZE(wrapped), cue,
                  MP_OCR_SELECT_AUTO, "ja", "zh");
        struct mp_ocr_policy_result result =
            select_text(&context, &policy_input, output, sizeof(output), flags);
        if (cue == 3) {
            assert(strcmp(output, "朝の電車です\n次の駅へ行きます") == 0);
            assert(result.reason ==
                   MP_OCR_REASON_BILINGUAL_SOURCE_SELECTED);
        }
    }
}

static void check_ascii_punctuation(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Morning train: 8:30 - platform 2/4, 50% full! Ready?",
             50, 80, 380, 1),
        line("早班列车即将到站", 135, 104, 210, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, lines, ARRAY_SIZE(lines), "en", "zh",
              "早班列车即将到站");

    struct mp_ocr_policy_line short_name[] = {
        line("Bo!", 200, 80, 80, 1),
        line("欢迎回来", 170, 104, 140, 2),
    };
    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_input policy_input =
        input(short_name, ARRAY_SIZE(short_name), 10,
              MP_OCR_SELECT_AUTO, "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(strcmp(output, "Bo!\n欢迎回来") == 0);
    assert(result.reason == MP_OCR_REASON_AMBIGUOUS_LANGUAGE);

    struct mp_ocr_policy_line unknown_script[] = {
        line("صباح الخير.", 160, 80, 160, 1),
        line("欢迎回来", 170, 104, 140, 2),
    };
    policy_input = input(unknown_script, ARRAY_SIZE(unknown_script), 11,
                         MP_OCR_SELECT_AUTO, "en", "zh");
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "صباح الخير.\n欢迎回来") == 0);
    assert(result.reason == MP_OCR_REASON_AMBIGUOUS_LANGUAGE);
}

static void check_han_and_numeric_ambiguity(void)
{
    struct mp_ocr_policy_line han[] = {
        line("春風", 180, 80, 120, 1),
        line("春风", 180, 104, 120, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_input policy_input =
        input(han, ARRAY_SIZE(han), 1, MP_OCR_SELECT_AUTO, "ja", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_AMBIGUOUS_LANGUAGE);
    assert(strcmp(output, "春風\n春风") == 0);

    struct mp_ocr_policy_line numeric[] = {
        line("2048", 200, 80, 80, 1),
        line("2048", 200, 104, 80, 2),
    };
    policy_input = input(numeric, ARRAY_SIZE(numeric), 2,
                         MP_OCR_SELECT_AUTO, "en", "zh");
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_AMBIGUOUS_LANGUAGE);
    assert(strcmp(output, "2048\n2048") == 0);
}

static void check_unrelated_sign(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Platform 7", 20, 20, 120, 1),
        line("请在这里等候", 140, 180, 220, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];
    for (uint64_t cue = 1; cue <= 4; cue++) {
        struct mp_ocr_policy_input policy_input =
            input(lines, ARRAY_SIZE(lines), cue, MP_OCR_SELECT_AUTO,
                  "en", "zh");
        struct mp_ocr_policy_result result =
            select_text(&context, &policy_input, output,
                        sizeof(output), flags);
        assert(result.reason == MP_OCR_REASON_SPATIAL_AMBIGUITY);
        assert(result.ambiguous && !result.already_target);
        assert(strcmp(output, "Platform 7\n请在这里等候") == 0);
    }
}

static void check_repeated_cue_and_reset(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Open the window", 130, 80, 220, 1),
        line("打开窗户", 170, 104, 140, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];
    for (int n = 0; n < 3; n++) {
        struct mp_ocr_policy_input policy_input =
            input(lines, ARRAY_SIZE(lines), 42, MP_OCR_SELECT_AUTO,
                  "en", "zh");
        policy_input.revision = (uint64_t)n + 1;
        struct mp_ocr_policy_result result =
            select_text(&context, &policy_input, output,
                        sizeof(output), flags);
        assert(result.reason == (n == 0 ? MP_OCR_REASON_LAYOUT_UNCONFIRMED
                                       : MP_OCR_REASON_REPEATED_CUE));
        assert(strcmp(output, "Open the window\n打开窗户") == 0);
    }
    assert(context.convention_count == 1);

    struct mp_ocr_policy_line changed[] = {
        line("打开窗户", 170, 80, 140, 1),
        line("Open the window", 130, 104, 220, 2),
    };
    struct mp_ocr_policy_input changed_input =
        input(changed, ARRAY_SIZE(changed), 43, MP_OCR_SELECT_AUTO,
              "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &changed_input, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_LAYOUT_UNCONFIRMED);
    assert(context.convention_count == 1);

    mp_ocr_policy_reset(&context);
    assert(context.convention_count == 0 && context.num_seen_cues == 0);
    changed_input.cue_id = 1;
    result = select_text(&context, &changed_input, output,
                         sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_LAYOUT_UNCONFIRMED);
}

static void check_timing_and_old_cues(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Open the window.", 130, 80, 220, 1),
        line("打开窗户", 170, 104, 140, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];

    struct mp_ocr_policy_input first =
        input(lines, ARRAY_SIZE(lines), 1, MP_OCR_SELECT_AUTO, "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &first, output, sizeof(output), flags);
    assert(result.observed && context.convention_count == 1);

    struct mp_ocr_policy_input overlap =
        input(lines, ARRAY_SIZE(lines), 2, MP_OCR_SELECT_AUTO, "en", "zh");
    overlap.start = first.start + 1.0;
    result = select_text(&context, &overlap, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_INVALID_TIMING);
    assert(!result.observed && context.convention_count == 0);

    mp_ocr_policy_reset(&context);
    for (uint64_t cue = 1; cue <= MP_OCR_POLICY_SEEN_CUES + 2; cue++) {
        struct mp_ocr_policy_input current =
            input(lines, ARRAY_SIZE(lines), cue, MP_OCR_SELECT_AUTO,
                  "en", "zh");
        result = select_text(&context, &current, output, sizeof(output), flags);
        assert(result.observed);
    }

    struct mp_ocr_policy_input old =
        input(lines, ARRAY_SIZE(lines), 1, MP_OCR_SELECT_AUTO, "en", "zh");
    old.start = 1000.0;
    result = select_text(&context, &old, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_REPEATED_CUE);
    assert(!result.observed);

    struct mp_ocr_policy_input invalid =
        input(lines, ARRAY_SIZE(lines), 100, MP_OCR_SELECT_AUTO, "en", "zh");
    invalid.duration = -1.0;
    result = select_text(&context, &invalid, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_INVALID_TIMING);
    assert(!result.observed);
}

static void check_geometry_limits(void)
{
    struct mp_ocr_policy_line negative[] = {
        line("Left edge caption.", -120, 80, 220, 1),
        line("左侧边缘字幕", -80, 104, 140, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, negative, ARRAY_SIZE(negative), "en", "zh",
              "左侧边缘字幕");

    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_line extreme_width[] = {
        line("Wide coordinate.", INT_MIN, 80, 1, 1),
        line("极端坐标文本", 0, 104, INT_MAX, 2),
    };
    struct mp_ocr_policy_input policy_input =
        input(extreme_width, ARRAY_SIZE(extreme_width), 10,
              MP_OCR_SELECT_AUTO, "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_SPATIAL_AMBIGUITY);
    assert(result.ambiguous);

    struct mp_ocr_policy_line extreme_height[] = {
        line("Tall coordinate.", 130, 80, 220, 1),
        line("极端高度文本", 150, 104, 180, 2),
    };
    extreme_height[0].h = INT_MAX;
    policy_input = input(extreme_height, ARRAY_SIZE(extreme_height), 11,
                         MP_OCR_SELECT_AUTO, "en", "zh");
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_SPATIAL_AMBIGUITY);
    assert(result.ambiguous);
}

static void check_manual_and_full_modes(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Fresh bread", 160, 80, 160, 1),
        line("新鲜面包", 170, 104, 140, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_input policy_input =
        input(lines, ARRAY_SIZE(lines), 1, MP_OCR_SELECT_SOURCE,
              "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(strcmp(output, "Fresh bread") == 0);
    assert(result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);
    assert(flags[0] && !flags[1]);

    policy_input.mode = MP_OCR_SELECT_FULL;
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "Fresh bread\n新鲜面包") == 0);
    assert(result.reason == MP_OCR_REASON_FULL_REQUESTED);
    assert(!result.ambiguous);
    assert(flags[0] && flags[1]);

    policy_input.mode = MP_OCR_SELECT_SOURCE;
    policy_input.source_lang = "und";
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "Fresh bread\n新鲜面包") == 0);
    assert(result.reason == MP_OCR_REASON_UNKNOWN_LANGUAGE);

    policy_input.source_lang = "fr";
    policy_input.track_lang = "en";
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "Fresh bread\n新鲜面包") == 0);
    assert(result.reason == MP_OCR_REASON_UNKNOWN_LANGUAGE);

    policy_input.source_lang = NULL;
    policy_input.track_lang = "en-GB";
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "Fresh bread") == 0);
    assert(result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);

    policy_input.source_lang = "auto";
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "Fresh bread") == 0);
    assert(result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);

    struct mp_ocr_policy_line source_only[] = {
        line("First wrapped line", 140, 80, 200, 1),
        line("Second wrapped line", 130, 104, 220, 1),
    };
    policy_input = input(source_only, ARRAY_SIZE(source_only), 2,
                         MP_OCR_SELECT_SOURCE, "en", "zh");
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "First wrapped line\nSecond wrapped line") == 0);
    assert(result.reason == MP_OCR_REASON_MANUAL_SOURCE_SELECTED);

    struct mp_ocr_policy_line target_only[] = {
        line("只有目标文本", 160, 80, 160, 1),
    };
    policy_input = input(target_only, ARRAY_SIZE(target_only), 3,
                         MP_OCR_SELECT_SOURCE, "en", "zh");
    result = select_text(&context, &policy_input, output,
                         sizeof(output), flags);
    assert(strcmp(output, "只有目标文本") == 0);
    assert(result.reason == MP_OCR_REASON_SOURCE_NOT_FOUND);
}

static void check_invalid_utf8_and_capacity(void)
{
    const char broken[] = {'B', 'a', 'd', ' ', (char)0xc3, '(', '\0'};
    struct mp_ocr_policy_line lines[] = {
        line(broken, 160, 80, 160, 1),
        line("完整文本", 170, 104, 140, 2),
    };
    struct mp_ocr_policy_context context = {0};
    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_input policy_input =
        input(lines, ARRAY_SIZE(lines), 1, MP_OCR_SELECT_AUTO,
              "en", "zh");
    struct mp_ocr_policy_result result =
        select_text(&context, &policy_input, output, sizeof(output), flags);
    assert(result.reason == MP_OCR_REASON_INVALID_UTF8);
    assert(memcmp(output, broken, sizeof(broken) - 1) == 0);
    assert(output[sizeof(broken) - 1] == '\n');

    char small[4] = "xxx";
    assert(!mp_ocr_policy_select(&context, &policy_input,
                                 small, sizeof(small), flags,
                                 ARRAY_SIZE(flags), &result));
    assert(result.reason == MP_OCR_REASON_OUTPUT_TOO_SMALL);
    assert(result.required_capacity > sizeof(small));
    assert(small[0] == '\0');

    assert(!mp_ocr_policy_select(&context, &policy_input,
                                 output, sizeof(output), flags, 1, &result));
    assert(result.reason == MP_OCR_REASON_INVALID_INPUT);
}

static void check_cached_reselection(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Open the window.", 130, 80, 220, 1),
        line("打开窗户。", 160, 104, 160, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, lines, ARRAY_SIZE(lines), "en", "zh", "打开窗户。");
    struct mp_ocr_policy_context before = context;
    char output[256];
    uint8_t flags[2];
    struct mp_ocr_policy_input cached =
        input(lines, ARRAY_SIZE(lines), 1, MP_OCR_SELECT_AUTO, "en", "zh");
    cached.reevaluate = true;
    struct mp_ocr_policy_result result =
        select_text(&context, &cached, output, sizeof(output), flags);
    assert(result.already_target && !result.observed);
    assert(!strcmp(output, "打开窗户。"));
    assert(!memcmp(&context, &before, sizeof(context)));
    lines[0].x = 0;
    lines[0].y = 0;
    result = select_text(&context, &cached, output, sizeof(output), flags);
    assert(result.ambiguous && !result.already_target && !result.observed);
    assert(strstr(output, "Open the window."));
    assert(!memcmp(&context, &before, sizeof(context)));
}

static void check_cjk_punctuation(void)
{
    struct mp_ocr_policy_line lines[] = {
        line("Wait -- did you hear that?", 100, 80, 300, 1),
        line("等等——你听见了吗？", 130, 104, 240, 2),
    };
    struct mp_ocr_policy_context context = {0};
    establish(&context, lines, ARRAY_SIZE(lines), "en", "zh",
              "等等——你听见了吗？");
}

int main(void)
{
    check_both_orders();
    check_wrapped_source();
    check_japanese_english_targeting_chinese();
    check_ascii_punctuation();
    check_han_and_numeric_ambiguity();
    check_unrelated_sign();
    check_repeated_cue_and_reset();
    check_timing_and_old_cues();
    check_geometry_limits();
    check_manual_and_full_modes();
    check_invalid_utf8_and_capacity();
    check_cached_reselection();
    check_cjk_punctuation();
    assert(strcmp(mp_ocr_policy_reason_string(
                      MP_OCR_REASON_LAYOUT_UNCONFIRMED),
                  "layout-unconfirmed") == 0);
    puts("ocr_policy: all tests passed");
    return 0;
}
