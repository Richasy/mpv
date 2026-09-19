#include <assert.h>
#include <stdint.h>

#include "video/filter/rife_model.h"

int main(void)
{
    assert(mp_rife_valid_padding(32));
    assert(mp_rife_valid_padding(64));
    assert(mp_rife_valid_padding(128));
    assert(!mp_rife_valid_padding(16));
    assert(!mp_rife_valid_padding(96));

    assert(mp_rife_pad_dimension(640, 128) == 640);
    assert(mp_rife_pad_dimension(650, 128) == 768);
    assert(mp_rife_pad_dimension(1264, 128) == 1280);
    assert(mp_rife_pad_dimension(1080, 32) == 1088);

    const int64_t dynamic_input[] = {1, 11, -1, -1};
    const int64_t fixed_input[] = {1, 11, 768, 1280};
    const int64_t wrong_channels[] = {1, 6, -1, -1};
    const int64_t wrong_geometry[] = {1, 11, 736, 1280};

    assert(mp_rife_tensor_shape_matches(dynamic_input, 4, 11, 768, 1280));
    assert(mp_rife_tensor_shape_matches(fixed_input, 4, 11, 768, 1280));
    assert(!mp_rife_tensor_shape_matches(wrong_channels, 4, 11, 768, 1280));
    assert(!mp_rife_tensor_shape_matches(wrong_geometry, 4, 11, 768, 1280));
    assert(!mp_rife_tensor_shape_matches(dynamic_input, 3, 11, 768, 1280));
    return 0;
}
