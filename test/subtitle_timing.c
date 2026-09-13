#include <assert.h>

#include "common/common.h"
#include "sub/dec_sub.h"

int main(void)
{
    struct sub_packet_timing_probe translated;
    sub_test_packet_timing("translated", 10.0, 0.5, false, &translated);
    assert(translated.visible);
    assert(!translated.sub_updated);
    assert(translated.cached_packet_index == 0);

    sub_test_packet_timing("translated", -10.0, 0.5, false, &translated);
    assert(translated.read_until == MP_NOPTS_VALUE);

    struct sub_packet_timing_probe ordinary;
    sub_test_packet_timing(NULL, 10.0, 0.5, false, &ordinary);
    assert(!ordinary.visible);
    assert(ordinary.sub_updated);
    assert(ordinary.cached_packet_index == 1);

    sub_test_packet_timing(NULL, -10.0, 0.5, false, &ordinary);
    assert(ordinary.read_until == 0.5);
    return 0;
}
