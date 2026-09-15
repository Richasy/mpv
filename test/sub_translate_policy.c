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

int main(void)
{
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
