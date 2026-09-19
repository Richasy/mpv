#include <assert.h>
#include <stdint.h>

#include "video/filter/rife_sync.h"

int main(void)
{
    assert(mp_rife_slot_ready(0, 0, 0));
    assert(!mp_rife_slot_ready(1, 0, UINT64_MAX));
    assert(!mp_rife_slot_ready(0, 42, 41));
    assert(mp_rife_slot_ready(0, 42, 42));
    assert(mp_rife_slot_ready(0, 42, 43));
    return 0;
}
