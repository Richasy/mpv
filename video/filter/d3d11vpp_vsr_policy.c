#include "d3d11vpp_vsr_policy.h"

enum mp_d3d11_vsr_status mp_d3d11_vsr_initial_status(bool requested,
                                                      bool nv12_input)
{
    if (!requested)
        return MP_D3D11_VSR_DISABLED;
    return nv12_input
        ? MP_D3D11_VSR_PENDING
        : MP_D3D11_VSR_UNSUPPORTED_FORMAT;
}

enum mp_d3d11_vsr_status mp_d3d11_vsr_extension_result(
    enum mp_d3d11_vsr_status status, bool succeeded)
{
    if (status != MP_D3D11_VSR_PENDING)
        return status;
    return succeeded ? MP_D3D11_VSR_ACTIVE
                     : MP_D3D11_VSR_EXTENSION_FAILED;
}

bool mp_d3d11_vsr_should_retry_blt(enum mp_d3d11_vsr_status status,
                                   bool succeeded)
{
    return status == MP_D3D11_VSR_ACTIVE && !succeeded;
}

enum mp_d3d11_vsr_status mp_d3d11_vsr_blt_result(
    enum mp_d3d11_vsr_status status, bool succeeded)
{
    return mp_d3d11_vsr_should_retry_blt(status, succeeded)
        ? MP_D3D11_VSR_PROCESSING_FAILED
        : status;
}

const char *mp_d3d11_vsr_status_name(enum mp_d3d11_vsr_status status)
{
    switch (status) {
    case MP_D3D11_VSR_DISABLED:
        return "disabled";
    case MP_D3D11_VSR_PENDING:
        return "pending";
    case MP_D3D11_VSR_ACTIVE:
        return "active";
    case MP_D3D11_VSR_UNSUPPORTED_FORMAT:
        return "unsupported-format";
    case MP_D3D11_VSR_EXTENSION_FAILED:
        return "extension-failed";
    case MP_D3D11_VSR_PROCESSING_FAILED:
        return "processing-failed";
    }
    return "unknown";
}
