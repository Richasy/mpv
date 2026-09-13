#include <assert.h>
#include <string.h>

#include "player/sub_translate.h"

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
    return 0;
}
