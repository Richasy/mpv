#pragma once

#include <stdbool.h>

enum mp_d3d11_vsr_status {
    MP_D3D11_VSR_DISABLED,
    MP_D3D11_VSR_PENDING,
    MP_D3D11_VSR_ACTIVE,
    MP_D3D11_VSR_UNSUPPORTED_FORMAT,
    MP_D3D11_VSR_EXTENSION_FAILED,
    MP_D3D11_VSR_PROCESSING_FAILED,
};

enum mp_d3d11_vsr_status mp_d3d11_vsr_initial_status(bool requested,
                                                      bool nv12_input);

enum mp_d3d11_vsr_status mp_d3d11_vsr_extension_result(
    enum mp_d3d11_vsr_status status, bool succeeded);

bool mp_d3d11_vsr_should_retry_blt(enum mp_d3d11_vsr_status status,
                                   bool succeeded);

enum mp_d3d11_vsr_status mp_d3d11_vsr_blt_result(
    enum mp_d3d11_vsr_status status, bool succeeded);

const char *mp_d3d11_vsr_status_name(enum mp_d3d11_vsr_status status);
