#include <assert.h>
#include <string.h>

#include "video/out/gpu/d3d11_adapter.h"

static void test_legacy_prefix(void)
{
    struct mp_d3d11_adapter_selector selector;
    assert(mp_d3d11_adapter_selector_parse("nViDiA GeForce", &selector));
    assert(selector.kind == MP_D3D11_ADAPTER_NAME_PREFIX);
    assert(mp_d3d11_adapter_selector_matches(
        &selector, "NVIDIA GeForce RTX 4090", 1, 2));
    assert(!mp_d3d11_adapter_selector_matches(
        &selector, "Intel Arc", 1, 2));

    assert(mp_d3d11_adapter_selector_parse("NVIDIA GeForce RTX 4090",
                                           &selector));
    assert(mp_d3d11_adapter_selector_matches(
        &selector, "NVIDIA GeForce RTX 4090", 10, 20));
    assert(mp_d3d11_adapter_selector_matches(
        &selector, "NVIDIA GeForce RTX 4090", 30, 40));
}

static void test_exact_luid(void)
{
    struct mp_d3d11_adapter_selector selector;
    assert(mp_d3d11_adapter_selector_parse(
        "luid:89abcdef01234567", &selector));
    assert(selector.kind == MP_D3D11_ADAPTER_LUID);
    assert(mp_d3d11_adapter_selector_matches(
        &selector, "duplicate", 0x01234567, 0x89abcdef));
    assert(!mp_d3d11_adapter_selector_matches(
        &selector, "duplicate", 0x01234568, 0x89abcdef));

    char formatted[MP_D3D11_ADAPTER_LUID_STRING_SIZE];
    mp_d3d11_adapter_format_luid(formatted, 0x01234567, 0x89abcdef);
    assert(strcmp(formatted, "luid:89abcdef01234567") == 0);
}

static void test_invalid_luid(void)
{
    struct mp_d3d11_adapter_selector selector;
    assert(!mp_d3d11_adapter_selector_parse("luid:1234", &selector));
    assert(selector.kind == MP_D3D11_ADAPTER_INVALID);
    assert(!mp_d3d11_adapter_selector_parse(
        "luid:89abcdef0123456z", &selector));
    assert(selector.kind == MP_D3D11_ADAPTER_INVALID);
}

static void test_auto(void)
{
    struct mp_d3d11_adapter_selector selector;
    assert(mp_d3d11_adapter_selector_parse("", &selector));
    assert(selector.kind == MP_D3D11_ADAPTER_AUTO);
    assert(!mp_d3d11_adapter_selector_matches(
        &selector, "anything", 0, 0));
}

static void test_hardware_ordinal_skips_software_entries(void)
{
    assert(!mp_d3d11_adapter_selector_matches_hardware(
        NULL, "software", 1, 2, true, 0, 0));
    assert(mp_d3d11_adapter_selector_matches_hardware(
        NULL, "physical", 3, 4, false, 0, 0));
    assert(!mp_d3d11_adapter_selector_matches_hardware(
        NULL, "second physical", 5, 6, false, 1, 0));

    struct mp_d3d11_adapter_selector selector;
    assert(mp_d3d11_adapter_selector_parse(
        "luid:0000000400000003", &selector));
    assert(mp_d3d11_adapter_selector_matches_hardware(
        &selector, "physical", 3, 4, false, 7, 0));
}

int main(void)
{
    test_legacy_prefix();
    test_exact_luid();
    test_invalid_luid();
    test_auto();
    test_hardware_ordinal_skips_software_entries();
    return 0;
}
