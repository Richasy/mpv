#include <assert.h>
#include <string.h>

#include "player/sub_translate.h"

static void check_spans(const char *source, const char **expected, int count)
{
    struct sub_translate_span spans[16] = {0};
    assert(sub_translate_ass_spans(source, NULL, 0) == count);
    assert(sub_translate_ass_spans(source, spans, 16) == count);
    size_t end = 0;
    for (int n = 0; n < count; n++) {
        assert(spans[n].start >= end);
        assert(spans[n].length == strlen(expected[n]));
        assert(!strncmp(source + spans[n].start, expected[n], spans[n].length));
        end = spans[n].start + spans[n].length;
    }
    spans[1].start = 12345;
    assert(sub_translate_ass_spans(source, spans, 1) == count);
    assert(spans[1].start == 12345);
}

static struct sub_translate_ass_sample sample(const char *text, double start)
{
    return (struct sub_translate_ass_sample){
        .text = text, .start = start, .duration = 2,
        .font_size = 18, .normal_layout = true,
    };
}

static void check_primary_blocks(void)
{
    const char *paired = "Upper one\\NUpper two\\N{\\fs12\\Arial\\i1}Lower text";
    struct sub_translate_ass_sample samples[5];
    for (int n = 0; n < 5; n++)
        samples[n] = sample(paired, n * 3);
    size_t boundary = strstr(paired, "\\N{") - paired;
    assert(sub_translate_ass_primary_end(samples, 2, &samples[0]) == 0);
    assert(sub_translate_ass_primary_end(samples, 3, &samples[0]) == boundary);
    assert(sub_translate_ass_primary_end(samples, 5, &samples[0]) == boundary);

    struct sub_translate_ass_sample cue = sample(
        "This entire upper block can wrap naturally at the video width"
        "\\N{\\fs12\\Arial\\i1}Lower text\\NLower continuation{\\i0}", 20);
    size_t end = sub_translate_ass_primary_end(samples, 5, &cue);
    assert(end == (size_t)(strstr(cue.text, "\\N{") - cue.text));
    assert(!strcmp(cue.text + end,
        "\\N{\\fs12\\Arial\\i1}Lower text\\NLower continuation{\\i0}"));
    cue = sample("Upper\\N{\\i1}emphasized continuation{\\i0}"
                 "\\N{\\fs12\\Arial\\i1}Lower text", 20);
    assert(sub_translate_ass_primary_end(samples, 5, &cue) ==
           (size_t)(strstr(cue.text, "\\N{\\fs12") - cue.text));

    const char *whole_cues[] = {
        "Ordinary wrapped\\Nmonolingual dialogue",
        "Ordinary\\N{\\i1}emphasis{\\i0}",
        "Ordinary\\N{\\fs12}small print",
        "{\\i1}Already italic\\N{\\fs12\\Arial\\i1}still italic",
        "Upper\\N{\\fs17\\Arial\\i1}insufficient size change",
        "Upper\\N{\\fs12\\Arial\\i1}lower{\\fs18\\i0}upper again",
        "{\\pos(120,40)}Speaker\\N{\\fs12\\Arial\\i1}aside",
        "{\\an8}Sign\\N{\\fs12\\Arial\\i1}annotation",
        "Upper\\N{\\t(0,100,\\fs12)\\i1}animated",
        "Upper\\N{\\fs12\\Arial\\i1}",
        "Upper\\N{\\fs12\\Arial\\i1",
    };
    for (int n = 0; n < (int)(sizeof(whole_cues) / sizeof(whole_cues[0])); n++) {
        cue = sample(whole_cues[n], 20);
        assert(sub_translate_ass_primary_end(samples, 5, &cue) == 0);
    }

    // Four matching cues out of five meet 80%; three out of four do not.
    samples[3].text = "Monolingual\\N{\\i1}emphasis";
    assert(sub_translate_ass_primary_end(samples, 4, &samples[0]) == 0);
    assert(sub_translate_ass_primary_end(samples, 5, &samples[0]) == boundary);
    samples[4].text = "Other ordinary dialogue";
    assert(sub_translate_ass_primary_end(samples, 5, &samples[0]) == 0);

    for (int n = 0; n < 3; n++)
        samples[n] = sample("Upper\\N{\\fs+12\\i1}Lower", n * 3);
    assert(sub_translate_ass_primary_end(samples, 3, &samples[0]) == 0);
    for (int n = 0; n < 3; n++)
        samples[n] = sample("Upper\\N{\\fs +12\\i1}Lower", n * 3);
    assert(sub_translate_ass_primary_end(samples, 3, &samples[0]) == 0);

    // Coincident events are not repeated independent evidence. File order does
    // not determine the selected block, and distinct events are never paired.
    for (int n = 0; n < 3; n++)
        samples[n] = sample(paired, 0);
    assert(sub_translate_ass_primary_end(samples, 3, &samples[0]) == 0);
    for (int n = 0; n < 3; n++)
        samples[n] = sample(paired, (2 - n) * 3);
    assert(sub_translate_ass_primary_end(samples, 3, &samples[0]) == boundary);
    samples[0] = sample("{\\pos(160,160)}Lower speaker", 0);
    samples[1] = sample("{\\pos(160,40)}Upper speaker", 0);
    samples[2] = sample("{\\an8}Independent sign", 0);
    for (int n = 0; n < 3; n++)
        assert(sub_translate_ass_primary_end(samples, 3, &samples[n]) == 0);

    cue = sample(paired, 20);
    cue.normal_layout = false;
    assert(sub_translate_ass_primary_end(samples, 3, &cue) == 0);
}

int main(void)
{
    check_primary_blocks();
    assert(sub_translate_classify_source("whisper", true) ==
           SUB_TRANSLATE_SOURCE_GENERATED);
    assert(sub_translate_classify_source("translated", true) ==
           SUB_TRANSLATE_SOURCE_GENERATED);
    assert(sub_translate_classify_source(NULL, false) ==
           SUB_TRANSLATE_SOURCE_BITMAP);
    assert(sub_translate_classify_source(NULL, true) ==
           SUB_TRANSLATE_SOURCE_TEXT);

    const char *source = "line {tag}\\value\r\nnext";
    const char *expected = "line \\{tag}\\\xe2\x81\xa0value\\Nnext";
    size_t required =
        sub_translate_escape_ass_buffer(NULL, 0, source);
    assert(required == strlen(expected));
    char output[128];
    assert(required < sizeof(output));
    assert(sub_translate_escape_ass_buffer(
               output, sizeof(output), source) == required);
    assert(strcmp(output, expected) == 0);
    check_spans("{\\i1} Hello \\N world {\\i0}",
                (const char *[]){"Hello", "world"}, 2);
    check_spans("{\\pos(23,45)\\t(0,500,\\fscx125)\\clip(0,0,640,360)}"
                "{\\k20}你{\\kf30}好 世界\\h{\\rSign}again",
                (const char *[]){"你", "好 世界", "again"}, 3);
    check_spans("{\\p1}m 0 0 l 100 0 100 100{\\rSign}m 1 1 l 2 2"
                "{\\pbo2\\pos(10,20)}m 3 3 l 4 4{\\p0} caption ",
                (const char *[]){"caption"}, 1);
    check_spans("{\\p+1}m 0 0{\\p-1}Text{\\p\t2}m 1 1{\\p}End",
                (const char *[]){"Text", "End"}, 2);
    check_spans("{\\p1}m 0 0 l 10 10", NULL, 0);
    check_spans("{\\p(1)}m 0 0 l 10 10{\\p0}Caption",
                (const char *[]){"Caption"}, 1);
    check_spans("{\\ p(1)}m 0 0 l 10 10{\\ p(0)}Caption",
                (const char *[]){"Caption"}, 1);
    check_spans("{\\p0(1)}m 0 0 l 10 10", NULL, -1);
    check_spans("{\\p1\\t(0,100,\\p0)}m 0 0 l 10 10", NULL, -1);
    check_spans(" \\N\\n\\h \t{comment}", NULL, 0);
    check_spans("{\\i1", NULL, -1);
    check_spans("\\{literal\\}\\NUTF-8：你好",
                (const char *[]){"literal", "UTF-8：你好"}, 2);
    assert(sub_translate_escape_ass_buffer(
        output, sizeof(output), "{\\p1}\\N\n你好") > 0);
    assert(strcmp(output,
        "\\{\\\xe2\x81\xa0p1}\\\xe2\x81\xa0N\\N你好") == 0);
    return 0;
}
