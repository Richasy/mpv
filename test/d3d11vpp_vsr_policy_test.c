#include <assert.h>
#include <string.h>

#include "video/filter/d3d11vpp_vsr_policy.h"

int main(void)
{
    assert(mp_d3d11_vsr_initial_status(false, true) ==
           MP_D3D11_VSR_DISABLED);
    assert(mp_d3d11_vsr_initial_status(true, true) ==
           MP_D3D11_VSR_PENDING);
    assert(mp_d3d11_vsr_initial_status(true, false) ==
           MP_D3D11_VSR_UNSUPPORTED_FORMAT);

    assert(mp_d3d11_vsr_extension_result(MP_D3D11_VSR_PENDING, true) ==
           MP_D3D11_VSR_ACTIVE);
    assert(mp_d3d11_vsr_extension_result(MP_D3D11_VSR_PENDING, false) ==
           MP_D3D11_VSR_EXTENSION_FAILED);
    assert(mp_d3d11_vsr_extension_result(
               MP_D3D11_VSR_UNSUPPORTED_FORMAT, true) ==
           MP_D3D11_VSR_UNSUPPORTED_FORMAT);

    assert(!mp_d3d11_vsr_should_retry_blt(MP_D3D11_VSR_ACTIVE, true));
    assert(mp_d3d11_vsr_should_retry_blt(MP_D3D11_VSR_ACTIVE, false));
    assert(!mp_d3d11_vsr_should_retry_blt(
        MP_D3D11_VSR_UNSUPPORTED_FORMAT, false));
    assert(mp_d3d11_vsr_blt_result(MP_D3D11_VSR_ACTIVE, false) ==
           MP_D3D11_VSR_PROCESSING_FAILED);

    assert(strcmp(mp_d3d11_vsr_status_name(MP_D3D11_VSR_ACTIVE),
                  "active") == 0);
    assert(strcmp(mp_d3d11_vsr_status_name(
                      MP_D3D11_VSR_UNSUPPORTED_FORMAT),
                  "unsupported-format") == 0);
    assert(strcmp(mp_d3d11_vsr_status_name(
                      MP_D3D11_VSR_EXTENSION_FAILED),
                  "extension-failed") == 0);
    assert(strcmp(mp_d3d11_vsr_status_name(
                      MP_D3D11_VSR_PROCESSING_FAILED),
                  "processing-failed") == 0);
    return 0;
}
