/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct ID3D10Effect;
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include "shaders.h"

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob **, ID3DBlob **);

int main(void)
{
    _Static_assert(sizeof(struct dlssnr_shader_config) == 32 * 4,
                   "Shader root-constant layout");
    HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", NULL,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return 1;
    FARPROC proc = GetProcAddress(module, "D3DCompile");
    compile_fn compile;
    memcpy(&compile, &proc, sizeof(compile));
    if (!compile) {
        FreeLibrary(module);
        return 1;
    }
    static const char *const entries[] = {
        "convert_source", "reduce_horizontal", "reduce_vertical",
        "prepare_residual", "compose_residual",
    };
    int result = 0;
    for (size_t n = 0; n < sizeof(entries) / sizeof(entries[0]); n++) {
        ID3DBlob *code = NULL, *error = NULL;
        HRESULT hr = compile(dlssnr_shader_source, strlen(dlssnr_shader_source),
            "mpv-original-dlssnr", NULL, NULL, entries[n], "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            0, &code, &error);
        if (FAILED(hr)) {
            fprintf(stderr, "%s: 0x%08lx: %s\n", entries[n], (unsigned long)hr,
                error ? (char *)ID3D10Blob_GetBufferPointer(error) : "");
            result = 1;
        } else {
            printf("%s: passed\n", entries[n]);
        }
        if (code)
            ID3D10Blob_Release(code);
        if (error)
            ID3D10Blob_Release(error);
    }
    FreeLibrary(module);
    return result;
}
