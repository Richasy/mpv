#include "d3d11_adapter.h"

#include <stdio.h>
#include <string.h>

static int ascii_tolower(int value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static bool ascii_starts_with_case(const char *value, const char *prefix,
                                   size_t prefix_len)
{
    if (strlen(value) < prefix_len)
        return false;

    for (size_t i = 0; i < prefix_len; i++) {
        if (ascii_tolower((unsigned char)value[i]) !=
            ascii_tolower((unsigned char)prefix[i]))
            return false;
    }

    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = ascii_tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool mp_d3d11_adapter_selector_parse(
    const char *request,
    struct mp_d3d11_adapter_selector *selector)
{
    *selector = (struct mp_d3d11_adapter_selector){
        .kind = MP_D3D11_ADAPTER_INVALID,
    };
    if (!request || !request[0]) {
        selector->kind = MP_D3D11_ADAPTER_AUTO;
        return true;
    }

    static const char prefix[] = "luid:";
    if (!ascii_starts_with_case(request, prefix, sizeof(prefix) - 1)) {
        selector->kind = MP_D3D11_ADAPTER_NAME_PREFIX;
        selector->name = request;
        selector->name_len = strlen(request);
        return true;
    }

    const char *hex = request + sizeof(prefix) - 1;
    if (strlen(hex) != 16)
        return false;

    uint64_t packed = 0;
    for (int i = 0; i < 16; i++) {
        int digit = hex_value(hex[i]);
        if (digit < 0)
            return false;
        packed = packed << 4 | (unsigned)digit;
    }

    selector->kind = MP_D3D11_ADAPTER_LUID;
    selector->luid_high = packed >> 32;
    selector->luid_low = packed;
    return true;
}

bool mp_d3d11_adapter_selector_matches(
    const struct mp_d3d11_adapter_selector *selector,
    const char *description,
    uint32_t luid_low,
    uint32_t luid_high)
{
    switch (selector->kind) {
    case MP_D3D11_ADAPTER_NAME_PREFIX:
        return ascii_starts_with_case(description, selector->name,
                                      selector->name_len);
    case MP_D3D11_ADAPTER_LUID:
        return selector->luid_low == luid_low &&
               selector->luid_high == luid_high;
    default:
        return false;
    }
}

bool mp_d3d11_adapter_selector_matches_candidate(
    const struct mp_d3d11_adapter_selector *selector,
    const char *description,
    uint32_t luid_low,
    uint32_t luid_high,
    bool software,
    int raw_ordinal,
    int requested_raw_ordinal)
{
    if (software)
        return false;
    return selector
        ? mp_d3d11_adapter_selector_matches(
              selector, description, luid_low, luid_high)
        : raw_ordinal == requested_raw_ordinal;
}

void mp_d3d11_adapter_format_luid(
    char output[MP_D3D11_ADAPTER_LUID_STRING_SIZE],
    uint32_t luid_low,
    uint32_t luid_high)
{
    snprintf(output, MP_D3D11_ADAPTER_LUID_STRING_SIZE,
             "luid:%08x%08x", luid_high, luid_low);
}
