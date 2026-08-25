#ifndef MP_D3D11_ADAPTER_H_
#define MP_D3D11_ADAPTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_D3D11_ADAPTER_LUID_STRING_SIZE 22

enum mp_d3d11_adapter_selector_kind {
    MP_D3D11_ADAPTER_AUTO,
    MP_D3D11_ADAPTER_NAME_PREFIX,
    MP_D3D11_ADAPTER_LUID,
    MP_D3D11_ADAPTER_INVALID,
};

struct mp_d3d11_adapter_selector {
    enum mp_d3d11_adapter_selector_kind kind;
    const char *name;
    size_t name_len;
    uint32_t luid_low;
    uint32_t luid_high;
};

bool mp_d3d11_adapter_selector_parse(
    const char *request,
    struct mp_d3d11_adapter_selector *selector);

bool mp_d3d11_adapter_selector_matches(
    const struct mp_d3d11_adapter_selector *selector,
    const char *description,
    uint32_t luid_low,
    uint32_t luid_high);

bool mp_d3d11_adapter_selector_matches_hardware(
    const struct mp_d3d11_adapter_selector *selector,
    const char *description,
    uint32_t luid_low,
    uint32_t luid_high,
    bool software,
    int hardware_ordinal,
    int requested_hardware_ordinal);

void mp_d3d11_adapter_format_luid(
    char output[MP_D3D11_ADAPTER_LUID_STRING_SIZE],
    uint32_t luid_low,
    uint32_t luid_high);

#endif
