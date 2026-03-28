/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libplacebo/d3d11.h>

#include "common/msg.h"
#include "mpv/render_d3d11.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"
#include "video/d3d.h"
#include "video/hwdec.h"
#include "video/img_format.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

// ── NVIDIA NGX VSR via runtime loading ──
//
// The NGX SDK ships as an MSVC static library (nvsdk_ngx_d.lib) that embeds
// /DEFAULTLIB directives for msvcprt/MSVCRT/OLDNAMES, making it incompatible
// with MinGW cross-compilation. Instead, we define the minimal types and
// function signatures ourselves and load _nvngx.dll at runtime via
// LoadLibrary/GetProcAddress.
#if HAVE_NGX_VSR

#include <nvsdk_ngx_defs.h>

// Opaque types from the NGX SDK
typedef struct NVSDK_NGX_Handle NVSDK_NGX_Handle;

// ── NVSDK_NGX_Parameter vtable (MSVC C++ ABI) ──
//
// The NGX SDK defines NVSDK_NGX_Parameter as a C++ pure-virtual interface.
// The Parameter_SetUI / Parameter_GetI / Parameter_SetD3d11Resource helper
// functions are implemented in the MSVC static library (nvsdk_ngx_d.lib),
// which is incompatible with MinGW. They are NOT exported from _nvngx.dll.
//
// Instead, we mirror the vtable layout and call the virtual methods directly
// through the vptr. The MSVC x64 ABI places the vptr as the first member,
// and virtual methods use __thiscall (== first arg is `this` on x64).
//
// IMPORTANT: MSVC lays out overloaded virtual methods in REVERSE declaration
// order within each overload group. The header declares Set(ull), Set(float),
// Set(double), Set(uint), ... but the vtable stores them reversed:
// Set(void*), Set(D3D12Resource*), Set(ID3D11Resource*), Set(int), ...
// This was confirmed by disassembling the Parameter_Set*/Get* wrappers in
// nvsdk_ngx_d.lib.

// On x64, there is only one calling convention; __thiscall is x86-only.
// The `this` pointer is simply the first argument passed via rcx.
typedef void (*PFN_NGX_Param_SetUI)(
    void *thisptr, const char *name, unsigned int value);
typedef void (*PFN_NGX_Param_SetD3d11Resource)(
    void *thisptr, const char *name, ID3D11Resource *value);
typedef NVSDK_NGX_Result (*PFN_NGX_Param_GetI)(
    const void *thisptr, const char *name, int *out);

typedef struct NVSDK_NGX_Parameter {
    void **vtbl;
} NVSDK_NGX_Parameter;

// vtable slot indices
// NOTE: MSVC lays out overloaded virtual methods in REVERSE declaration order
// within each overload group. The actual order (confirmed by disassembling
// nvsdk_ngx_d.lib's Parameter_Set*/Get* wrappers) is:
//   0: Set(void*)            [+0x00]   8: Get(void**)            [+0x40]
//   1: Set(ID3D12Resource*)  [+0x08]   9: Get(ID3D12Resource**)  [+0x48]
//   2: Set(ID3D11Resource*)  [+0x10]  10: Get(ID3D11Resource**)  [+0x50]
//   3: Set(int)              [+0x18]  11: Get(int*)              [+0x58]
//   4: Set(unsigned int)     [+0x20]  12: Get(unsigned int*)     [+0x60]
//   5: Set(double)           [+0x28]  13: Get(double*)           [+0x68]
//   6: Set(float)            [+0x30]  14: Get(float*)            [+0x70]
//   7: Set(unsigned long long)[+0x38] 15: Get(unsigned long long*)[+0x78]
//  16: Reset()               [+0x80]
#define NGX_VTBL_SET_UI             4
#define NGX_VTBL_SET_D3D11RESOURCE  2
#define NGX_VTBL_GET_I             11

static inline void ngx_param_set_ui(NVSDK_NGX_Parameter *p,
                                     const char *name, unsigned int val)
{
    ((PFN_NGX_Param_SetUI)p->vtbl[NGX_VTBL_SET_UI])(p, name, val);
}

static inline void ngx_param_set_d3d11_resource(NVSDK_NGX_Parameter *p,
                                                  const char *name,
                                                  ID3D11Resource *res)
{
    ((PFN_NGX_Param_SetD3d11Resource)p->vtbl[NGX_VTBL_SET_D3D11RESOURCE])(
        p, name, res);
}

static inline NVSDK_NGX_Result ngx_param_get_i(NVSDK_NGX_Parameter *p,
                                                 const char *name, int *out)
{
    return ((PFN_NGX_Param_GetI)p->vtbl[NGX_VTBL_GET_I])(p, name, out);
}

// Function pointer types for the NGX D3D11 API
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Init)(
    unsigned long long InApplicationId, const wchar_t *InApplicationDataPath,
    ID3D11Device *InDevice, const void *InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Shutdown1)(ID3D11Device *InDevice);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_GetCapabilityParameters)(
    NVSDK_NGX_Parameter **OutParameters);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_DestroyParameters)(
    NVSDK_NGX_Parameter *InParameters);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_CreateFeature)(
    ID3D11DeviceContext *InDevCtx, NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter *InParameters, NVSDK_NGX_Handle **OutHandle);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_ReleaseFeature)(
    NVSDK_NGX_Handle *InHandle);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_EvaluateFeature)(
    ID3D11DeviceContext *InDevCtx, const NVSDK_NGX_Handle *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters, void *InCallback);

// Runtime-loaded function pointers (DLL exports only)
static struct {
    HMODULE dll;
    PFN_NGX_D3D11_Init D3D11_Init;
    PFN_NGX_D3D11_Shutdown1 D3D11_Shutdown1;
    PFN_NGX_D3D11_GetCapabilityParameters D3D11_GetCapabilityParameters;
    PFN_NGX_D3D11_DestroyParameters D3D11_DestroyParameters;
    PFN_NGX_D3D11_CreateFeature D3D11_CreateFeature;
    PFN_NGX_D3D11_ReleaseFeature D3D11_ReleaseFeature;
    PFN_NGX_D3D11_EvaluateFeature D3D11_EvaluateFeature;
} ngx_fn;

static HMODULE ngx_load_dll_from_registry(struct libmpv_gpu_next_context *ctx)
{
    HKEY key = NULL;
    LONG ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        0, KEY_READ, &key);
    if (ret != ERROR_SUCCESS) {
        MP_VERBOSE(ctx, "NGX VSR: NGXCore registry key not found.\n");
        return NULL;
    }

    wchar_t path[MAX_PATH] = {0};
    DWORD size = sizeof(path) - sizeof(wchar_t); // leave room for null
    DWORD type = 0;
    ret = RegQueryValueExW(key, L"FullPath", NULL, &type, (BYTE *)path, &size);
    RegCloseKey(key);

    if (ret != ERROR_SUCCESS || type != REG_SZ || path[0] == 0) {
        MP_VERBOSE(ctx, "NGX VSR: NGXCore FullPath registry value not found.\n");
        return NULL;
    }

    // Append \_nvngx.dll to the directory path
    size_t len = wcslen(path);
    if (len + 14 >= MAX_PATH) { // 14 = wcslen(L"\\_nvngx.dll") + 1
        MP_WARN(ctx, "NGX VSR: NGXCore path too long.\n");
        return NULL;
    }
    wcscat(path, L"\\_nvngx.dll");

    MP_VERBOSE(ctx, "NGX VSR: Loading from registry path: %ls\n", path);
    HMODULE dll = LoadLibraryW(path);
    if (!dll)
        MP_VERBOSE(ctx, "NGX VSR: LoadLibrary failed for registry path.\n");
    return dll;
}

static bool ngx_load_dll(struct libmpv_gpu_next_context *ctx)
{
    if (ngx_fn.dll)
        return true;

    // Try standard search path first, then registry
    HMODULE dll = LoadLibraryW(L"_nvngx.dll");
    if (!dll)
        dll = ngx_load_dll_from_registry(ctx);
    if (!dll) {
        MP_WARN(ctx, "NGX VSR: _nvngx.dll not found, VSR unavailable.\n");
        return false;
    }

#define NGX_LOAD(name, sym) do {                                             \
    ngx_fn.name = (void *)GetProcAddress(dll, sym);                          \
    if (!ngx_fn.name) {                                                      \
        MP_WARN(ctx, "NGX VSR: Missing symbol '%s' in _nvngx.dll.\n", sym);  \
        FreeLibrary(dll);                                                    \
        memset(&ngx_fn, 0, sizeof(ngx_fn));                                 \
        return false;                                                        \
    }                                                                        \
} while (0)

    NGX_LOAD(D3D11_Init, "NVSDK_NGX_D3D11_Init");
    NGX_LOAD(D3D11_Shutdown1, "NVSDK_NGX_D3D11_Shutdown1");
    NGX_LOAD(D3D11_GetCapabilityParameters,
             "NVSDK_NGX_D3D11_GetCapabilityParameters");
    NGX_LOAD(D3D11_DestroyParameters, "NVSDK_NGX_D3D11_DestroyParameters");
    NGX_LOAD(D3D11_CreateFeature, "NVSDK_NGX_D3D11_CreateFeature");
    NGX_LOAD(D3D11_ReleaseFeature, "NVSDK_NGX_D3D11_ReleaseFeature");
    NGX_LOAD(D3D11_EvaluateFeature, "NVSDK_NGX_D3D11_EvaluateFeature");
#undef NGX_LOAD

    ngx_fn.dll = dll;
    MP_VERBOSE(ctx, "NGX VSR: _nvngx.dll loaded successfully.\n");
    return true;
}

// NVSDK_NGX_Feature_VSR = NVSDK_NGX_Feature_Reserved16 = 16
#define MPV_NGX_FEATURE_VSR ((NVSDK_NGX_Feature)16)

// String parameter keys
#define MPV_NGX_PARAM_VSR_AVAILABLE   "VSR.Available"

// Parameter keys from nvsdk_ngx_defs.h
#define MPV_NGX_PARAM_INPUT1     "Input1"
#define MPV_NGX_PARAM_OUTPUT     "Output"
#define MPV_NGX_PARAM_RECT_X    "Rect.X"
#define MPV_NGX_PARAM_RECT_Y    "Rect.Y"
#define MPV_NGX_PARAM_RECT_W    "Rect.W"
#define MPV_NGX_PARAM_RECT_H    "Rect.H"
#define MPV_NGX_PARAM_OUTRECT_X "OutRect.X"
#define MPV_NGX_PARAM_OUTRECT_Y "OutRect.Y"
#define MPV_NGX_PARAM_OUTRECT_W "OutRect.W"
#define MPV_NGX_PARAM_OUTRECT_H "OutRect.H"
#define MPV_NGX_PARAM_VSR_QUALITY "VSR.QualityLevel"

// NVSDK_NGX_Feature_TrueHDR = 14
#define MPV_NGX_FEATURE_TRUEHDR ((NVSDK_NGX_Feature)14)

// TrueHDR parameter keys
#define MPV_NGX_PARAM_TRUEHDR_AVAILABLE   "TrueHDR.Available"
#define MPV_NGX_PARAM_TRUEHDR_IN_LEFT     "TrueHDR.InLeft"
#define MPV_NGX_PARAM_TRUEHDR_IN_TOP      "TrueHDR.InTop"
#define MPV_NGX_PARAM_TRUEHDR_IN_RIGHT    "TrueHDR.InRight"
#define MPV_NGX_PARAM_TRUEHDR_IN_BOTTOM   "TrueHDR.InBottom"
#define MPV_NGX_PARAM_TRUEHDR_OUT_LEFT    "TrueHDR.OutLeft"
#define MPV_NGX_PARAM_TRUEHDR_OUT_TOP     "TrueHDR.OutTop"
#define MPV_NGX_PARAM_TRUEHDR_OUT_RIGHT   "TrueHDR.OutRight"
#define MPV_NGX_PARAM_TRUEHDR_OUT_BOTTOM  "TrueHDR.OutBottom"
#define MPV_NGX_PARAM_TRUEHDR_CONTRAST    "TrueHDR.Contrast"
#define MPV_NGX_PARAM_TRUEHDR_SATURATION  "TrueHDR.Saturation"
#define MPV_NGX_PARAM_TRUEHDR_MIDDLEGRAY  "TrueHDR.MiddleGray"
#define MPV_NGX_PARAM_TRUEHDR_MAXLUMINANCE "TrueHDR.MaxLuminance"

#define MPV_NGX_FAILED(value) (((value) & 0xFFF00000) == 0xBAD00000)

// App ID for NGX (can be any unique value for the application)
#define MPV_NGX_APP_ID 0x524F44454C // "RODEL" in hex

#endif // HAVE_NGX_VSR

// ── AMD FidelityFX Super Resolution 1.0 ──
//
// FSR 1.0 is an open-source spatial upscaling algorithm (MIT license).
// It consists of two compute shader passes:
//   EASU (Edge Adaptive Spatial Upsampling) — the core upscaler
//   RCAS (Robust Contrast Adaptive Sharpening) — optional sharpening
// Unlike NVIDIA NGX, FSR works on any D3D11 GPU (no vendor lock).
// Shaders are precompiled to DXBC bytecode (in fsr_bytecode.h) — no runtime
// D3DCompile or d3dcompiler dependency needed.

// CPU-side constants computation from ffx_fsr1.h
// These are the FsrEasuCon / FsrRcasCon functions adapted for C.
#define A_CPU 1
#include "video/out/d3d11/fsr/ffx_a.h"
#include "video/out/d3d11/fsr/ffx_fsr1.h"

// Precompiled DXBC bytecode for EASU and RCAS compute shaders
#include "video/out/d3d11/fsr/fsr_bytecode.h"

struct priv {
    pl_d3d11 d3d11;
    pl_tex wrapped_tex;

    // D3D11VA hwdec device context for zero-copy hardware decoding
    struct mp_hwdec_ctx hwctx;

#if HAVE_NGX_VSR
    // NGX VSR state
    ID3D11Device *d3d_device;
    ID3D11DeviceContext *d3d_ctx;
    ID3D10Multithread *multithread;
    bool ngx_initialized;
    bool ngx_vsr_available;
    NVSDK_NGX_Parameter *ngx_params;
    NVSDK_NGX_Handle *ngx_vsr_handle;

    // Cached textures for VSR
    ID3D11Texture2D *vsr_intermediate_tex;
    int vsr_intermediate_w, vsr_intermediate_h;
    ID3D11Texture2D *vsr_output_tex;
    int vsr_output_w, vsr_output_h;

    // NGX TrueHDR state
    bool ngx_truehdr_available;
    NVSDK_NGX_Handle *ngx_truehdr_handle;
#endif

    // AMD FSR 1.0 state
    bool fsr_initialized;
    ID3D11Device *fsr_device;
    ID3D11DeviceContext *fsr_ctx;
    ID3D11ComputeShader *fsr_easu_cs;
    ID3D11ComputeShader *fsr_rcas_cs;
    ID3D11Buffer *fsr_cb;
    ID3D11SamplerState *fsr_sampler;

#if HAVE_NVOFA
    // NvOFFRUC frame interpolation state
    void *fruc_handle;              // NvOFFRUCHandle (opaque pointer)
    bool fruc_available;
    bool fruc_session_active;
    int fruc_width, fruc_height;
    ID3D11Device *fruc_device;
    ID3D11DeviceContext *fruc_ctx;

    // 2 render textures (double-buffered input) + 1 interpolate texture
    ID3D11Texture2D *fruc_render_tex[2];
    ID3D11Texture2D *fruc_interp_tex;
    int fruc_render_idx;            // ping-pong index for render textures

    // ID3D11Fence for CUDA-DX synchronization
    ID3D11Fence *fruc_fence;
    ID3D11DeviceContext4 *fruc_ctx4;
    uint64_t fruc_fence_value;

    bool fruc_has_prev_frame;
    double fruc_last_pts_ms;
#endif
};

// Convert DXGI_COLOR_SPACE_TYPE to pl_color_space for the render target.
// Mirrors the logic in d3d11_helpers.c:d3d11_get_mp_csp() which is static.
static struct pl_color_space dxgi_csp_to_pl(int dxgi_csp)
{
    switch (dxgi_csp) {
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return (struct pl_color_space) {
            .transfer  = PL_COLOR_TRC_LINEAR,
            .primaries = PL_COLOR_PRIM_UNKNOWN,
        };
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return (struct pl_color_space) {
            .transfer  = PL_COLOR_TRC_PQ,
            .primaries = PL_COLOR_PRIM_BT_2020,
        };
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return (struct pl_color_space) {
            .transfer  = PL_COLOR_TRC_UNKNOWN,
            .primaries = PL_COLOR_PRIM_BT_2020,
        };
    default: // including DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 (0)
        return pl_color_space_srgb;
    }
}

// ── NGX VSR implementation ──

#if HAVE_NGX_VSR

static void ngx_vsr_init(struct libmpv_gpu_next_context *ctx, ID3D11Device *device)
{
    struct priv *p = ctx->priv;
    NVSDK_NGX_Result result;

    MP_VERBOSE(ctx, "NGX VSR: Initializing NVIDIA NGX SDK...\n");

    if (!ngx_load_dll(ctx))
        return;

    // Keep a reference to the device
    p->d3d_device = device;
    ID3D11Device_AddRef(p->d3d_device);

    // Initialize NGX
    result = ngx_fn.D3D11_Init(MPV_NGX_APP_ID, L".", device, NULL,
                                NVSDK_NGX_Version_API);
    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX VSR: NVSDK_NGX_D3D11_Init failed (0x%x). "
                "NVIDIA RTX VSR will not be available.\n", (unsigned)result);
        return;
    }
    p->ngx_initialized = true;
    MP_VERBOSE(ctx, "NGX VSR: SDK initialized successfully.\n");

    // Get capability parameters
    result = ngx_fn.D3D11_GetCapabilityParameters(&p->ngx_params);
    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX VSR: GetCapabilityParameters failed (0x%x).\n",
                (unsigned)result);
        return;
    }
    MP_VERBOSE(ctx, "NGX VSR: Got capability parameters.\n");

    // Check if VSR is available
    int vsr_available = 0;
    ngx_param_get_i(p->ngx_params, MPV_NGX_PARAM_VSR_AVAILABLE,
                     &vsr_available);
    MP_VERBOSE(ctx, "NGX VSR: params=%p, vtbl=%p, VSR.Available=%d\n",
               (void *)p->ngx_params, (void *)p->ngx_params->vtbl,
               vsr_available);
    if (!vsr_available) {
        MP_WARN(ctx, "NGX VSR: VSR feature is not available on this system. "
                "Requires NVIDIA RTX GPU with compatible driver.\n");
        return;
    }
    MP_VERBOSE(ctx, "NGX VSR: VSR feature is available.\n");

    // Get device context and set up multithreading
    ID3D11Device_GetImmediateContext(p->d3d_device, &p->d3d_ctx);

    HRESULT hr = ID3D11DeviceContext_QueryInterface(
        p->d3d_ctx, &IID_ID3D10Multithread, (void **)&p->multithread);
    if (SUCCEEDED(hr) && p->multithread) {
        ID3D10Multithread_SetMultithreadProtected(p->multithread, TRUE);
        MP_VERBOSE(ctx, "NGX VSR: Multithread protection enabled.\n");
    }

    // Create VSR feature
    if (p->multithread)
        ID3D10Multithread_Enter(p->multithread);

    result = ngx_fn.D3D11_CreateFeature(p->d3d_ctx, MPV_NGX_FEATURE_VSR,
                                         p->ngx_params, &p->ngx_vsr_handle);

    if (p->multithread)
        ID3D10Multithread_Leave(p->multithread);

    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX VSR: CreateFeature failed (0x%x).\n",
                (unsigned)result);
        return;
    }

    p->ngx_vsr_available = true;
    MP_INFO(ctx, "NGX VSR: Feature created successfully. "
            "NVIDIA RTX Video Super Resolution is ready.\n");

    // ── TrueHDR detection and creation (shares NGX Init and params) ──

    int truehdr_available = 0;
    ngx_param_get_i(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_AVAILABLE,
                     &truehdr_available);
    MP_VERBOSE(ctx, "NGX TrueHDR: TrueHDR.Available=%d\n", truehdr_available);
    if (!truehdr_available) {
        MP_VERBOSE(ctx, "NGX TrueHDR: TrueHDR feature is not available on "
                   "this system.\n");
        return;
    }

    if (p->multithread)
        ID3D10Multithread_Enter(p->multithread);

    result = ngx_fn.D3D11_CreateFeature(p->d3d_ctx, MPV_NGX_FEATURE_TRUEHDR,
                                         p->ngx_params, &p->ngx_truehdr_handle);

    if (p->multithread)
        ID3D10Multithread_Leave(p->multithread);

    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX TrueHDR: CreateFeature failed (0x%x).\n",
                (unsigned)result);
        return;
    }

    p->ngx_truehdr_available = true;
    MP_INFO(ctx, "NGX TrueHDR: Feature created successfully. "
            "NVIDIA RTX TrueHDR is ready.\n");
}

static void ngx_vsr_cleanup(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    if (p->vsr_intermediate_tex) {
        ID3D11Texture2D_Release(p->vsr_intermediate_tex);
        p->vsr_intermediate_tex = NULL;
    }
    if (p->vsr_output_tex) {
        ID3D11Texture2D_Release(p->vsr_output_tex);
        p->vsr_output_tex = NULL;
    }

    if (p->ngx_vsr_handle) {
        MP_VERBOSE(ctx, "NGX VSR: Releasing VSR feature...\n");
        ngx_fn.D3D11_ReleaseFeature(p->ngx_vsr_handle);
        p->ngx_vsr_handle = NULL;
    }

    if (p->ngx_truehdr_handle) {
        MP_VERBOSE(ctx, "NGX TrueHDR: Releasing TrueHDR feature...\n");
        ngx_fn.D3D11_ReleaseFeature(p->ngx_truehdr_handle);
        p->ngx_truehdr_handle = NULL;
    }
    p->ngx_truehdr_available = false;

    if (p->ngx_initialized) {
        MP_VERBOSE(ctx, "NGX VSR: Shutting down NGX SDK...\n");
        ngx_fn.D3D11_Shutdown1(p->d3d_device);
        p->ngx_initialized = false;
    }

    if (p->ngx_params) {
        ngx_fn.D3D11_DestroyParameters(p->ngx_params);
        p->ngx_params = NULL;
    }

    if (p->multithread) {
        ID3D10Multithread_Release(p->multithread);
        p->multithread = NULL;
    }
    if (p->d3d_ctx) {
        ID3D11DeviceContext_Release(p->d3d_ctx);
        p->d3d_ctx = NULL;
    }
    if (p->d3d_device) {
        ID3D11Device_Release(p->d3d_device);
        p->d3d_device = NULL;
    }

    p->ngx_vsr_available = false;
}

static bool ngx_vsr_available_fn(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    return p->ngx_vsr_available;
}

static ID3D11Texture2D *ngx_create_texture_fn(
    struct libmpv_gpu_next_context *ctx, int w, int h)
{
    struct priv *p = ctx->priv;
    if (!p->d3d_device)
        return NULL;

    D3D11_TEXTURE2D_DESC desc = {
        .Width = w,
        .Height = h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     D3D11_BIND_UNORDERED_ACCESS,
        .CPUAccessFlags = 0,
        .MiscFlags = 0,
    };

    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(p->d3d_device, &desc, NULL, &tex);
    if (FAILED(hr)) {
        MP_ERR(ctx, "NGX VSR: Failed to create %dx%d RGBA8 texture (hr=0x%x).\n",
               w, h, (unsigned)hr);
        return NULL;
    }

    MP_DBG(ctx, "NGX VSR: Created %dx%d RGBA8 texture (UAV).\n", w, h);
    return tex;
}

static bool ngx_vsr_process_fn(struct libmpv_gpu_next_context *ctx,
                                ID3D11Texture2D *input_tex,
                                int in_w, int in_h,
                                ID3D11Texture2D *output_tex,
                                int out_w, int out_h,
                                int quality)
{
    struct priv *p = ctx->priv;
    if (!p->ngx_vsr_available || !p->ngx_vsr_handle || !p->ngx_params)
        return false;

    // Clamp quality to valid range [0, 4]
    if (quality < 0) quality = 0;
    if (quality > 4) quality = 4;

    MP_DBG(ctx, "NGX VSR: Evaluate %dx%d -> %dx%d, quality=%d\n",
           in_w, in_h, out_w, out_h, quality);

    // Set parameters for VSR evaluation (reuse same params as CreateFeature,
    // matching the SDK sample CDx11NGXVSR which uses m_ngxParameters for both)
    ngx_param_set_d3d11_resource(p->ngx_params, MPV_NGX_PARAM_INPUT1,
                                  (ID3D11Resource *)input_tex);
    ngx_param_set_d3d11_resource(p->ngx_params, MPV_NGX_PARAM_OUTPUT,
                                  (ID3D11Resource *)output_tex);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_RECT_X, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_RECT_Y, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_RECT_W, in_w);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_RECT_H, in_h);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_OUTRECT_X, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_OUTRECT_Y, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_OUTRECT_W, out_w);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_OUTRECT_H, out_h);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_VSR_QUALITY, quality);

    if (p->multithread)
        ID3D10Multithread_Enter(p->multithread);

    NVSDK_NGX_Result result = ngx_fn.D3D11_EvaluateFeature(
        p->d3d_ctx, p->ngx_vsr_handle, p->ngx_params, NULL);

    if (p->multithread)
        ID3D10Multithread_Leave(p->multithread);

    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX VSR: EvaluateFeature failed (0x%x).\n",
                (unsigned)result);
        return false;
    }

    return true;
}

static bool ngx_truehdr_available_fn(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    return p->ngx_truehdr_available;
}

static ID3D11Texture2D *ngx_create_hdr_texture_fn(
    struct libmpv_gpu_next_context *ctx, int w, int h)
{
    struct priv *p = ctx->priv;
    if (!p->d3d_device)
        return NULL;

    // Use R16G16B16A16_FLOAT so the NGX TrueHDR SDK outputs scRGB linear
    // values, matching the FBO format used in HDR mode. When the output
    // texture is FP16, the SDK auto-selects scRGB linear encoding instead
    // of PQ, which avoids any color space mismatch with the swap chain.
    D3D11_TEXTURE2D_DESC desc = {
        .Width = w,
        .Height = h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     D3D11_BIND_UNORDERED_ACCESS,
        .CPUAccessFlags = 0,
        .MiscFlags = 0,
    };

    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(p->d3d_device, &desc, NULL, &tex);
    if (FAILED(hr)) {
        MP_ERR(ctx, "NGX TrueHDR: Failed to create %dx%d FP16 texture "
               "(hr=0x%x).\n", w, h, (unsigned)hr);
        return NULL;
    }

    MP_DBG(ctx, "NGX TrueHDR: Created %dx%d FP16 HDR texture (UAV).\n",
           w, h);
    return tex;
}

// Map preset to TrueHDR parameters
static void truehdr_get_preset_params(int preset, unsigned int *contrast,
                                       unsigned int *saturation,
                                       unsigned int *middlegray)
{
    switch (preset) {
    case 1: // natural
        *contrast = 90;
        *saturation = 90;
        *middlegray = 50;
        break;
    case 3: // vivid
        *contrast = 120;
        *saturation = 130;
        *middlegray = 45;
        break;
    case 2: // standard (default)
    default:
        *contrast = 100;
        *saturation = 100;
        *middlegray = 50;
        break;
    }
}

static bool ngx_truehdr_process_fn(struct libmpv_gpu_next_context *ctx,
                                     ID3D11Texture2D *input_tex,
                                     int in_w, int in_h,
                                     ID3D11Texture2D *output_tex,
                                     int out_w, int out_h,
                                     int preset, unsigned int max_luminance)
{
    struct priv *p = ctx->priv;
    if (!p->ngx_truehdr_available || !p->ngx_truehdr_handle || !p->ngx_params)
        return false;

    unsigned int contrast, saturation, middlegray;
    truehdr_get_preset_params(preset, &contrast, &saturation, &middlegray);

    if (max_luminance == 0)
        max_luminance = 1000; // SDK default

    MP_DBG(ctx, "NGX TrueHDR: Evaluate %dx%d -> %dx%d, preset=%d "
           "(contrast=%u, saturation=%u, middlegray=%u, maxlum=%u)\n",
           in_w, in_h, out_w, out_h, preset,
           contrast, saturation, middlegray, max_luminance);

    // Set input/output textures
    ngx_param_set_d3d11_resource(p->ngx_params, MPV_NGX_PARAM_INPUT1,
                                  (ID3D11Resource *)input_tex);
    ngx_param_set_d3d11_resource(p->ngx_params, MPV_NGX_PARAM_OUTPUT,
                                  (ID3D11Resource *)output_tex);

    // Set input region
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_IN_LEFT, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_IN_TOP, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_IN_RIGHT, in_w);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_IN_BOTTOM, in_h);

    // Set output region
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_OUT_LEFT, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_OUT_TOP, 0);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_OUT_RIGHT, out_w);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_OUT_BOTTOM, out_h);

    // Set TrueHDR quality parameters
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_CONTRAST, contrast);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_SATURATION, saturation);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_MIDDLEGRAY, middlegray);
    ngx_param_set_ui(p->ngx_params, MPV_NGX_PARAM_TRUEHDR_MAXLUMINANCE,
                     max_luminance);

    if (p->multithread)
        ID3D10Multithread_Enter(p->multithread);

    NVSDK_NGX_Result result = ngx_fn.D3D11_EvaluateFeature(
        p->d3d_ctx, p->ngx_truehdr_handle, p->ngx_params, NULL);

    if (p->multithread)
        ID3D10Multithread_Leave(p->multithread);

    if (MPV_NGX_FAILED(result)) {
        MP_WARN(ctx, "NGX TrueHDR: EvaluateFeature failed (0x%x).\n",
                (unsigned)result);
        return false;
    }

    return true;
}

#endif // HAVE_NGX_VSR

// ── AMD FSR 1.0 implementation ──

static void fsr_init(struct libmpv_gpu_next_context *ctx, ID3D11Device *device)
{
    struct priv *p = ctx->priv;

    MP_VERBOSE(ctx, "FSR: Initializing AMD FidelityFX Super Resolution 1.0...\n");

    // Create EASU compute shader from precompiled bytecode
    HRESULT hr = ID3D11Device_CreateComputeShader(
        device, fsr_easu_cs_bytecode, sizeof(fsr_easu_cs_bytecode),
        NULL, &p->fsr_easu_cs);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: CreateComputeShader(EASU) failed: hr=0x%x\n",
               (unsigned)hr);
        return;
    }

    // Create RCAS compute shader from precompiled bytecode
    hr = ID3D11Device_CreateComputeShader(
        device, fsr_rcas_cs_bytecode, sizeof(fsr_rcas_cs_bytecode),
        NULL, &p->fsr_rcas_cs);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: CreateComputeShader(RCAS) failed: hr=0x%x\n",
               (unsigned)hr);
        ID3D11ComputeShader_Release(p->fsr_easu_cs);
        p->fsr_easu_cs = NULL;
        return;
    }

    // Create constant buffer (large enough for 4x uint4 = 64 bytes)
    D3D11_BUFFER_DESC cb_desc = {
        .ByteWidth = 64,  // 4 * sizeof(uint4)
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    hr = ID3D11Device_CreateBuffer(device, &cb_desc, NULL, &p->fsr_cb);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: Failed to create constant buffer (hr=0x%x).\n",
               (unsigned)hr);
        ID3D11ComputeShader_Release(p->fsr_easu_cs);
        ID3D11ComputeShader_Release(p->fsr_rcas_cs);
        p->fsr_easu_cs = NULL;
        p->fsr_rcas_cs = NULL;
        return;
    }

    // Create linear clamp sampler
    D3D11_SAMPLER_DESC sampler_desc = {
        .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    hr = ID3D11Device_CreateSamplerState(device, &sampler_desc, &p->fsr_sampler);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: Failed to create sampler (hr=0x%x).\n", (unsigned)hr);
        ID3D11ComputeShader_Release(p->fsr_easu_cs);
        ID3D11ComputeShader_Release(p->fsr_rcas_cs);
        ID3D11Buffer_Release(p->fsr_cb);
        p->fsr_easu_cs = NULL;
        p->fsr_rcas_cs = NULL;
        p->fsr_cb = NULL;
        return;
    }

    p->fsr_device = device;
    ID3D11Device_AddRef(p->fsr_device);
    ID3D11Device_GetImmediateContext(p->fsr_device, &p->fsr_ctx);

    p->fsr_initialized = true;
    MP_INFO(ctx, "FSR: AMD FidelityFX Super Resolution 1.0 is ready.\n");
}

static void fsr_cleanup(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    if (p->fsr_easu_cs) {
        ID3D11ComputeShader_Release(p->fsr_easu_cs);
        p->fsr_easu_cs = NULL;
    }
    if (p->fsr_rcas_cs) {
        ID3D11ComputeShader_Release(p->fsr_rcas_cs);
        p->fsr_rcas_cs = NULL;
    }
    if (p->fsr_cb) {
        ID3D11Buffer_Release(p->fsr_cb);
        p->fsr_cb = NULL;
    }
    if (p->fsr_sampler) {
        ID3D11SamplerState_Release(p->fsr_sampler);
        p->fsr_sampler = NULL;
    }
    if (p->fsr_ctx) {
        ID3D11DeviceContext_Release(p->fsr_ctx);
        p->fsr_ctx = NULL;
    }
    if (p->fsr_device) {
        ID3D11Device_Release(p->fsr_device);
        p->fsr_device = NULL;
    }
    p->fsr_initialized = false;
}

static bool fsr_available_fn(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    return p->fsr_initialized;
}

static ID3D11Texture2D *fsr_create_texture_fn(
    struct libmpv_gpu_next_context *ctx, int w, int h)
{
    struct priv *p = ctx->priv;
    if (!p->fsr_device)
        return NULL;

    D3D11_TEXTURE2D_DESC desc = {
        .Width = w,
        .Height = h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                     D3D11_BIND_UNORDERED_ACCESS,
    };

    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(p->fsr_device, &desc, NULL, &tex);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: Failed to create %dx%d RGBA8 texture (hr=0x%x).\n",
               w, h, (unsigned)hr);
        return NULL;
    }
    MP_DBG(ctx, "FSR: Created %dx%d RGBA8 texture (UAV).\n", w, h);
    return tex;
}

static bool fsr_update_cb(struct libmpv_gpu_next_context *ctx,
                            const void *data, UINT size)
{
    struct priv *p = ctx->priv;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(
        p->fsr_ctx, (ID3D11Resource *)p->fsr_cb, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return false;
    memcpy(mapped.pData, data, size);
    ID3D11DeviceContext_Unmap(p->fsr_ctx, (ID3D11Resource *)p->fsr_cb, 0);
    return true;
}

static bool fsr_process_fn(struct libmpv_gpu_next_context *ctx,
                            ID3D11Texture2D *input_tex,
                            int in_w, int in_h,
                            ID3D11Texture2D *output_tex,
                            int out_w, int out_h,
                            int mode)
{
    struct priv *p = ctx->priv;
    if (!p->fsr_initialized)
        return false;

    MP_DBG(ctx, "FSR: Process %dx%d -> %dx%d (mode=%d)\n",
           in_w, in_h, out_w, out_h, mode);

    HRESULT hr;
    ID3D11ShaderResourceView *input_srv = NULL;
    ID3D11UnorderedAccessView *output_uav = NULL;

    // Create SRV for input texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipLevels = 1 },
    };
    hr = ID3D11Device_CreateShaderResourceView(
        p->fsr_device, (ID3D11Resource *)input_tex, &srv_desc, &input_srv);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: Failed to create input SRV (hr=0x%x).\n", (unsigned)hr);
        return false;
    }

    // Create UAV for output texture
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
    };
    hr = ID3D11Device_CreateUnorderedAccessView(
        p->fsr_device, (ID3D11Resource *)output_tex, &uav_desc, &output_uav);
    if (FAILED(hr)) {
        MP_ERR(ctx, "FSR: Failed to create output UAV (hr=0x%x).\n", (unsigned)hr);
        ID3D11ShaderResourceView_Release(input_srv);
        return false;
    }

    // --- EASU pass ---
    // Compute EASU constants on CPU
    AU1 easu_con[4][4];
    FsrEasuCon(easu_con + 0, easu_con + 1, easu_con + 2, easu_con + 3,
               (AF1)in_w, (AF1)in_h,   // input viewport
               (AF1)in_w, (AF1)in_h,   // input size (same as viewport)
               (AF1)out_w, (AF1)out_h); // output size

    if (!fsr_update_cb(ctx, easu_con, sizeof(easu_con))) {
        MP_ERR(ctx, "FSR: Failed to update EASU constant buffer.\n");
        goto cleanup;
    }

    ID3D11DeviceContext_CSSetShader(p->fsr_ctx, p->fsr_easu_cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(p->fsr_ctx, 0, 1, &p->fsr_cb);
    ID3D11DeviceContext_CSSetSamplers(p->fsr_ctx, 0, 1, &p->fsr_sampler);
    ID3D11DeviceContext_CSSetShaderResources(p->fsr_ctx, 0, 1, &input_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        p->fsr_ctx, 0, 1, &output_uav, NULL);

    // Each threadgroup processes 16x16 pixels
    UINT groups_x = (out_w + 15) / 16;
    UINT groups_y = (out_h + 15) / 16;
    ID3D11DeviceContext_Dispatch(p->fsr_ctx, groups_x, groups_y, 1);

    // Unbind resources between passes
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11UnorderedAccessView *null_uav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(p->fsr_ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(
        p->fsr_ctx, 0, 1, &null_uav, NULL);

    // --- RCAS pass (optional) ---
    if (mode >= 2 && p->fsr_rcas_cs) {
        // RCAS works in-place on the output texture (now becomes both input and output).
        // We need a separate SRV for reading and UAV for writing, but for in-place we
        // read from the EASU output and write back to it.
        // Actually RCAS needs a separate intermediate: read from output, write to output
        // is not allowed. For simplicity, we do RCAS reading from the EASU output
        // via Load (not Gather), writing to a new view of the same texture.
        // D3D11 allows same texture as SRV+UAV if they don't overlap — but for a
        // full-screen pass they do overlap. We need a temp texture.
        //
        // For now, skip RCAS if we can't do it without an extra allocation.
        // TODO: Add intermediate texture for RCAS pass.
        MP_DBG(ctx, "FSR: RCAS pass requested but not yet implemented "
               "(requires intermediate texture). Using EASU only.\n");
    }

    ID3D11ShaderResourceView_Release(input_srv);
    ID3D11UnorderedAccessView_Release(output_uav);
    return true;

cleanup:
    if (input_srv)
        ID3D11ShaderResourceView_Release(input_srv);
    if (output_uav)
        ID3D11UnorderedAccessView_Release(output_uav);
    return false;
}

// ── NVIDIA Optical Flow Frame Interpolation (NvOFFRUC) ──
//
// NvOFFRUC is a high-level API from the NVIDIA Optical Flow SDK that provides
// hardware-accelerated frame interpolation using the dedicated optical flow
// engine on RTX GPUs. It internally handles optical flow estimation, frame
// warping, and blending — no custom compute shaders needed.
//
// Like NGX, we load NvOFFRUC.dll at runtime and define the C-compatible types
// ourselves (the SDK header is C++).

#if HAVE_NVOFA

// NvOFFRUC type definitions (from NvOFFRUC.h, adapted for C)
#define NVOFA_MAX_RESOURCE 10
#define NVOFA_MIN_RESOURCE 3

typedef void *NvOFFRUCHandle_t;

typedef enum {
    NvOFFRUC_SUCCESS = 0,
    NvOFFRUC_ERR_NOT_SUPPORTED,
    NvOFFRUC_ERR_INVALID_PTR,
    NvOFFRUC_ERR_INVALID_PARAM,
    NvOFFRUC_ERR_INVALID_HANDLE,
    NvOFFRUC_ERR_OUT_OF_SYSTEM_MEMORY,
    NvOFFRUC_ERR_OUT_OF_VIDEO_MEMORY,
    NvOFFRUC_ERR_OPENCV_NOT_AVAILABLE,
    NvOFFRUC_ERR_UNIMPLEMENTED,
    NvOFFRUC_ERR_OF_FAILURE,
    NvOFFRUC_ERR_DUPLICATE_RESOURCE,
    NvOFFRUC_ERR_UNREGISTERED_RESOURCE,
    NvOFFRUC_ERR_INCORRECT_API_SEQUENCE,
    NvOFFRUC_ERR_WRITE_TODISK_FAILED,
    NvOFFRUC_ERR_PIPELINE_EXECUTION_FAILURE,
    NvOFFRUC_ERR_SYNC_WRITE_FAILED,
    NvOFFRUC_ERR_GENERIC,
} NvOFFRUC_STATUS_t;

typedef enum {
    NvOFFRUC_RESOURCE_CUDA = 0,
    NvOFFRUC_RESOURCE_DX11 = 1,
} NvOFFRUC_ResourceType_t;

typedef enum {
    NvOFFRUC_SURFACE_NV12 = 0,
    NvOFFRUC_SURFACE_ARGB = 1,
} NvOFFRUC_SurfaceFormat_t;

typedef enum {
    NvOFFRUC_CUDA_UNDEFINED = -1,
    NvOFFRUC_CUDA_CU_DEVICE_PTR = 0,
    NvOFFRUC_CUDA_CU_ARRAY = 1,
} NvOFFRUC_CUDAResourceType_t;

typedef union {
    struct {
        uint64_t uiFenceValueToWaitOn;
    } FenceWaitValue;
    struct {
        uint64_t uiKeyForRenderTextureAcquire;
        uint64_t uiKeyForInterpTextureAcquire;
    } MutexAcquireKey;
} NvOFFRUC_SyncWait_t;

typedef union {
    struct {
        uint64_t uiFenceValueToSignalOn;
    } FenceSignalValue;
    struct {
        uint64_t uiKeyForRenderTextureRelease;
        uint64_t uiKeyForInterpolateRelease;
    } MutexReleaseKey;
} NvOFFRUC_SyncSignal_t;

typedef struct {
    uint32_t uiWidth;
    uint32_t uiHeight;
    void *pDevice;
    NvOFFRUC_ResourceType_t eResourceType;
    NvOFFRUC_SurfaceFormat_t eSurfaceFormat;
    NvOFFRUC_CUDAResourceType_t eCUDAResourceType;
    uint32_t uiReserved[32];
} NvOFFRUC_CreateParam_t;

typedef struct {
    void *pFrame;
    double nTimeStamp;
    size_t nCuSurfacePitch;
    bool *bHasFrameRepetitionOccurred;
    uint32_t uiReserved[32];
} NvOFFRUC_FrameData_t;

typedef struct {
    NvOFFRUC_FrameData_t stFrameDataInput;
    uint32_t bSkipWarp : 1;
    NvOFFRUC_SyncWait_t uSyncWait;
    uint32_t uiReserved[32];
} NvOFFRUC_ProcessInParams_t;

typedef struct {
    NvOFFRUC_FrameData_t stFrameDataOutput;
    NvOFFRUC_SyncSignal_t uSyncSignal;
    uint32_t uiReserved[32];
} NvOFFRUC_ProcessOutParams_t;

typedef struct {
    void *pArrResource[NVOFA_MAX_RESOURCE];
    void *pD3D11FenceObj;
    uint32_t uiCount;
} NvOFFRUC_RegisterResourceParam_t;

typedef struct {
    void *pArrResource[NVOFA_MAX_RESOURCE];
    uint32_t uiCount;
} NvOFFRUC_UnregisterResourceParam_t;

// Function pointer types (CALLBACK = __stdcall on Windows)
typedef NvOFFRUC_STATUS_t (CALLBACK *PFN_NvOFFRUCCreate)(
    const NvOFFRUC_CreateParam_t *, NvOFFRUCHandle_t *);
typedef NvOFFRUC_STATUS_t (CALLBACK *PFN_NvOFFRUCRegisterResource)(
    NvOFFRUCHandle_t, const NvOFFRUC_RegisterResourceParam_t *);
typedef NvOFFRUC_STATUS_t (CALLBACK *PFN_NvOFFRUCUnregisterResource)(
    NvOFFRUCHandle_t, const NvOFFRUC_UnregisterResourceParam_t *);
typedef NvOFFRUC_STATUS_t (CALLBACK *PFN_NvOFFRUCProcess)(
    NvOFFRUCHandle_t, const NvOFFRUC_ProcessInParams_t *,
    const NvOFFRUC_ProcessOutParams_t *);
typedef NvOFFRUC_STATUS_t (CALLBACK *PFN_NvOFFRUCDestroy)(NvOFFRUCHandle_t);

// Runtime-loaded function pointers
static struct {
    HMODULE dll;
    PFN_NvOFFRUCCreate Create;
    PFN_NvOFFRUCRegisterResource RegisterResource;
    PFN_NvOFFRUCUnregisterResource UnregisterResource;
    PFN_NvOFFRUCProcess Process;
    PFN_NvOFFRUCDestroy Destroy;
} nvofa_fn;

static bool nvofa_load_dll(struct libmpv_gpu_next_context *ctx)
{
    if (nvofa_fn.dll)
        return true;

    // Try standard search path first
    HMODULE dll = LoadLibraryW(L"NvOFFRUC.dll");

    // Fall back: load from the same directory as libmpv-2.dll
    if (!dll) {
        HMODULE self = NULL;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)nvofa_load_dll, &self) && self)
        {
            wchar_t path[MAX_PATH] = {0};
            DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                // Strip filename, keep directory
                wchar_t *slash = wcsrchr(path, L'\\');
                if (slash) {
                    wchar_t dir[MAX_PATH] = {0};
                    wcsncpy(dir, path, slash - path + 1);

                    // Add this directory to DLL search path so that
                    // NvOFFRUC.dll's own dependencies (cudart64_110.dll)
                    // are also found from the same directory.
                    AddDllDirectory(dir);

                    *(slash + 1) = 0;
                    wcscat(path, L"NvOFFRUC.dll");
                    MP_VERBOSE(ctx, "NVOFA FRUC: Trying %ls\n", path);
                    dll = LoadLibraryExW(path, NULL,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }
        }
    }

    if (!dll) {
        MP_VERBOSE(ctx, "NVOFA FRUC: NvOFFRUC.dll not found.\n");
        return false;
    }

#define NVOFA_LOAD(field, name) do {                                        \
    nvofa_fn.field = (void *)GetProcAddress(dll, name);                     \
    if (!nvofa_fn.field) {                                                  \
        MP_WARN(ctx, "NVOFA FRUC: Missing symbol '%s'.\n", name);           \
        FreeLibrary(dll);                                                   \
        memset(&nvofa_fn, 0, sizeof(nvofa_fn));                             \
        return false;                                                       \
    }                                                                       \
} while (0)

    NVOFA_LOAD(Create, "NvOFFRUCCreate");
    NVOFA_LOAD(RegisterResource, "NvOFFRUCRegisterResource");
    NVOFA_LOAD(UnregisterResource, "NvOFFRUCUnregisterResource");
    NVOFA_LOAD(Process, "NvOFFRUCProcess");
    NVOFA_LOAD(Destroy, "NvOFFRUCDestroy");
#undef NVOFA_LOAD

    nvofa_fn.dll = dll;
    MP_VERBOSE(ctx, "NVOFA FRUC: NvOFFRUC.dll loaded successfully.\n");
    return true;
}

static bool nvofa_fruc_available_fn(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    return p->fruc_available;
}

static void nvofa_fruc_destroy_fn(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    if (p->fruc_session_active && p->fruc_handle) {
        NvOFFRUC_UnregisterResourceParam_t unreg = {0};
        unreg.uiCount = 3;
        unreg.pArrResource[0] = p->fruc_interp_tex;
        unreg.pArrResource[1] = p->fruc_render_tex[0];
        unreg.pArrResource[2] = p->fruc_render_tex[1];
        nvofa_fn.UnregisterResource(p->fruc_handle, &unreg);

        nvofa_fn.Destroy(p->fruc_handle);
        p->fruc_handle = NULL;
        p->fruc_session_active = false;
    }

    for (int i = 0; i < 2; i++) {
        if (p->fruc_render_tex[i]) {
            ID3D11Texture2D_Release(p->fruc_render_tex[i]);
            p->fruc_render_tex[i] = NULL;
        }
    }
    if (p->fruc_interp_tex) {
        ID3D11Texture2D_Release(p->fruc_interp_tex);
        p->fruc_interp_tex = NULL;
    }
    if (p->fruc_fence) {
        ID3D11Fence_Release(p->fruc_fence);
        p->fruc_fence = NULL;
    }
    if (p->fruc_ctx4) {
        ID3D11DeviceContext4_Release(p->fruc_ctx4);
        p->fruc_ctx4 = NULL;
    }
    p->fruc_fence_value = 0;
    p->fruc_render_idx = 0;
    p->fruc_has_prev_frame = false;
    p->fruc_last_pts_ms = 0;
    p->fruc_width = 0;
    p->fruc_height = 0;

    if (p->fruc_ctx) {
        ID3D11DeviceContext_Release(p->fruc_ctx);
        p->fruc_ctx = NULL;
    }
    if (p->fruc_device) {
        ID3D11Device_Release(p->fruc_device);
        p->fruc_device = NULL;
    }
}

static bool nvofa_fruc_init_session_fn(struct libmpv_gpu_next_context *ctx,
                                        int width, int height)
{
    struct priv *p = ctx->priv;

    if (p->fruc_session_active)
        nvofa_fruc_destroy_fn(ctx);

    if (!nvofa_fn.dll || !p->fruc_device)
        return false;

    if (!p->fruc_ctx)
        ID3D11Device_GetImmediateContext(p->fruc_device, &p->fruc_ctx);

    // Get ID3D11DeviceContext4 for Fence Signal/Wait
    HRESULT hr = ID3D11DeviceContext_QueryInterface(p->fruc_ctx,
        &IID_ID3D11DeviceContext4, (void **)&p->fruc_ctx4);
    if (FAILED(hr)) {
        MP_ERR(ctx, "NVOFA FRUC: QueryInterface ID3D11DeviceContext4 failed.\n");
        return false;
    }

    // Create ID3D11Fence for CUDA-DX synchronization
    ID3D11Device5 *dev5 = NULL;
    hr = ID3D11Device_QueryInterface(p->fruc_device,
        &IID_ID3D11Device5, (void **)&dev5);
    if (FAILED(hr) || !dev5) {
        MP_ERR(ctx, "NVOFA FRUC: QueryInterface ID3D11Device5 failed.\n");
        return false;
    }
    hr = ID3D11Device5_CreateFence(dev5, 0, D3D11_FENCE_FLAG_SHARED,
        &IID_ID3D11Fence, (void **)&p->fruc_fence);
    ID3D11Device5_Release(dev5);
    if (FAILED(hr)) {
        MP_ERR(ctx, "NVOFA FRUC: CreateFence failed (0x%x).\n", (unsigned)hr);
        return false;
    }
    p->fruc_fence_value = 0;

    // SHARED + SHARED_NTHANDLE textures (Fence path, not KeyedMutex)
    D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = 0,
        .CPUAccessFlags = 0,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };

    for (int i = 0; i < 2; i++) {
        hr = ID3D11Device_CreateTexture2D(
            p->fruc_device, &desc, NULL, &p->fruc_render_tex[i]);
        if (FAILED(hr)) {
            MP_ERR(ctx, "NVOFA FRUC: CreateTexture2D render[%d] failed (0x%x).\n",
                   i, (unsigned)hr);
            nvofa_fruc_destroy_fn(ctx);
            return false;
        }
    }

    hr = ID3D11Device_CreateTexture2D(
        p->fruc_device, &desc, NULL, &p->fruc_interp_tex);
    if (FAILED(hr)) {
        MP_ERR(ctx, "NVOFA FRUC: CreateTexture2D interp failed (0x%x).\n",
               (unsigned)hr);
        nvofa_fruc_destroy_fn(ctx);
        return false;
    }

    // Create NvOFFRUC instance
    NvOFFRUC_CreateParam_t create_params = {0};
    create_params.uiWidth = width;
    create_params.uiHeight = height;
    create_params.pDevice = p->fruc_device;
    create_params.eResourceType = NvOFFRUC_RESOURCE_DX11;
    create_params.eSurfaceFormat = NvOFFRUC_SURFACE_ARGB;
    create_params.eCUDAResourceType = NvOFFRUC_CUDA_UNDEFINED;

    NvOFFRUC_STATUS_t status = nvofa_fn.Create(&create_params, &p->fruc_handle);
    if (status != NvOFFRUC_SUCCESS) {
        MP_ERR(ctx, "NVOFA FRUC: NvOFFRUCCreate failed (status=%d).\n", status);
        nvofa_fruc_destroy_fn(ctx);
        return false;
    }

    // Register: interp first, then renders; pass Fence for sync
    NvOFFRUC_RegisterResourceParam_t reg = {0};
    reg.uiCount = 3;
    reg.pD3D11FenceObj = p->fruc_fence;  // non-NULL → Fence sync path
    reg.pArrResource[0] = p->fruc_interp_tex;
    reg.pArrResource[1] = p->fruc_render_tex[0];
    reg.pArrResource[2] = p->fruc_render_tex[1];

    status = nvofa_fn.RegisterResource(p->fruc_handle, &reg);
    if (status != NvOFFRUC_SUCCESS) {
        MP_ERR(ctx, "NVOFA FRUC: RegisterResource failed (status=%d).\n", status);
        nvofa_fn.Destroy(p->fruc_handle);
        p->fruc_handle = NULL;
        nvofa_fruc_destroy_fn(ctx);
        return false;
    }

    p->fruc_session_active = true;
    p->fruc_width = width;
    p->fruc_height = height;
    p->fruc_has_prev_frame = false;
    p->fruc_last_pts_ms = 0;
    p->fruc_render_idx = 0;

    MP_INFO(ctx, "NVOFA FRUC: Session created (%dx%d, Fence sync). "
            "NVIDIA Optical Flow frame interpolation is ready.\n",
            width, height);
    return true;
}

static bool nvofa_fruc_feed_frame_fn(struct libmpv_gpu_next_context *ctx,
                                      ID3D11Texture2D *input_tex,
                                      double pts_ms)
{
    struct priv *p = ctx->priv;
    if (!p->fruc_session_active || !p->fruc_handle)
        return false;

    int idx = p->fruc_render_idx;

    // Copy input frame to registered render texture
    ID3D11DeviceContext_CopyResource(p->fruc_ctx,
        (ID3D11Resource *)p->fruc_render_tex[idx], (ID3D11Resource *)input_tex);

    // Signal fence after DX copy completes — CUDA will wait on this
    p->fruc_fence_value++;
    ID3D11DeviceContext4_Signal(p->fruc_ctx4, p->fruc_fence, p->fruc_fence_value);

    bool frame_repeated = false;
    NvOFFRUC_ProcessInParams_t in_params = {0};
    in_params.stFrameDataInput.pFrame = p->fruc_render_tex[idx];
    in_params.stFrameDataInput.nTimeStamp = pts_ms;
    in_params.bSkipWarp = 0;
    // Fence value CUDA should wait on (DX signals this after copy)
    in_params.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = p->fruc_fence_value;

    NvOFFRUC_ProcessOutParams_t out_params = {0};
    out_params.stFrameDataOutput.pFrame = p->fruc_interp_tex;
    out_params.stFrameDataOutput.nTimeStamp = pts_ms;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &frame_repeated;
    // Fence value CUDA signals after interp is done
    p->fruc_fence_value++;
    out_params.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = p->fruc_fence_value;

    NvOFFRUC_STATUS_t status = nvofa_fn.Process(
        p->fruc_handle, &in_params, &out_params);

    if (status != NvOFFRUC_SUCCESS) {
        MP_WARN(ctx, "NVOFA FRUC: Process failed (status=%d).\n", status);
        return false;
    }

    // Advance ping-pong index
    p->fruc_render_idx = (idx + 1) % 2;
    p->fruc_last_pts_ms = pts_ms;
    p->fruc_has_prev_frame = true;

    MP_DBG(ctx, "NVOFA FRUC: Processed frame at %.1f ms "
               "(repeated=%d, fence=%llu).\n",
               pts_ms, frame_repeated,
               (unsigned long long)p->fruc_fence_value);
    return !frame_repeated;
}

static bool nvofa_fruc_interpolate_fn(struct libmpv_gpu_next_context *ctx,
                                       ID3D11Texture2D *output_tex,
                                       double target_pts_ms)
{
    struct priv *p = ctx->priv;
    if (!p->fruc_session_active || !p->fruc_handle || !p->fruc_has_prev_frame)
        return false;

    // Wait for CUDA to finish writing the interpolated frame
    ID3D11DeviceContext4_Wait(p->fruc_ctx4, p->fruc_fence, p->fruc_fence_value);

    // Copy interpolated result to caller's output texture
    ID3D11DeviceContext_CopyResource(p->fruc_ctx,
        (ID3D11Resource *)output_tex, (ID3D11Resource *)p->fruc_interp_tex);
    ID3D11DeviceContext_Flush(p->fruc_ctx);

    MP_DBG(ctx, "NVOFA FRUC: Read interpolated frame at %.1f ms.\n",
               target_pts_ms);
    return true;
}

static void nvofa_fruc_init(struct libmpv_gpu_next_context *ctx,
                            ID3D11Device *device)
{
    struct priv *p = ctx->priv;
    p->fruc_available = nvofa_load_dll(ctx);
    if (p->fruc_available) {
        p->fruc_device = device;
        ID3D11Device_AddRef(p->fruc_device);
        MP_VERBOSE(ctx, "NVOFA FRUC: DLL loaded, FRUC available.\n");
    }
}

static void nvofa_fruc_cleanup(struct libmpv_gpu_next_context *ctx)
{
    nvofa_fruc_destroy_fn(ctx);
}

#endif // HAVE_NVOFA

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_d3d11_init_params *d3d_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!d3d_params || !d3d_params->device) {
        MP_FATAL(ctx, "Missing D3D11 device in init params.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog) {
        MP_FATAL(ctx, "Failed to create libplacebo log.\n");
        return MPV_ERROR_GENERIC;
    }

    p->d3d11 = pl_d3d11_create(ctx->pllog, pl_d3d11_params(
        .device = (ID3D11Device *)d3d_params->device,
    ));
    if (!p->d3d11) {
        MP_FATAL(ctx, "Failed to create libplacebo D3D11 context.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->d3d11->gpu;

    // Register D3D11 device for zero-copy hardware decoding (D3D11VA)
    if (ctx->hwdec_devs) {
        static const int subfmts[] = {
            IMGFMT_NV12,
            IMGFMT_P010,
            IMGFMT_BGRA,
            0
        };
        p->hwctx = (struct mp_hwdec_ctx){
            .driver_name = "d3d11va",
            .av_device_ref = d3d11_wrap_device_ref(
                                (ID3D11Device *)d3d_params->device),
            .supported_formats = subfmts,
            .hw_imgfmt = IMGFMT_D3D11,
        };
        if (p->hwctx.av_device_ref) {
            hwdec_devices_add(ctx->hwdec_devs, &p->hwctx);
            MP_VERBOSE(ctx, "Registered D3D11 device for hwdec.\n");
        } else {
            MP_WARN(ctx, "Failed to create hwdec device context.\n");
        }
    }

#if HAVE_NGX_VSR
    ngx_vsr_init(ctx, (ID3D11Device *)d3d_params->device);
#endif

#if HAVE_NVOFA
    nvofa_fruc_init(ctx, (ID3D11Device *)d3d_params->device);
#endif

    fsr_init(ctx, (ID3D11Device *)d3d_params->device);

    return 0;
}

static int wrap_fbo(struct libmpv_gpu_next_context *ctx, mpv_render_param *params,
                    pl_tex *out, int *w, int *h, struct pl_color_space *out_csp)
{
    struct priv *p = ctx->priv;

    mpv_d3d11_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    if (!fbo || !fbo->texture) {
        MP_FATAL(ctx, "Missing D3D11 FBO in render params.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Release previous wrapped texture if any
    if (p->wrapped_tex)
        pl_tex_destroy(ctx->gpu, &p->wrapped_tex);

    p->wrapped_tex = pl_d3d11_wrap(ctx->gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *)fbo->texture,
        .w = fbo->w,
        .h = fbo->h,
    ));
    if (!p->wrapped_tex) {
        MP_FATAL(ctx, "Failed to wrap D3D11 texture with libplacebo.\n");
        return MPV_ERROR_GENERIC;
    }

    *out = p->wrapped_tex;
    *w = fbo->w;
    *h = fbo->h;
    *out_csp = dxgi_csp_to_pl(fbo->color_space);
    return 0;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool ds)
{
    // No-op: the caller manages Present/composition timing.
    (void)ctx;
    (void)ds;
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    // Unregister hwdec device
    if (p->hwctx.av_device_ref) {
        if (ctx->hwdec_devs)
            hwdec_devices_remove(ctx->hwdec_devs, &p->hwctx);
        av_buffer_unref(&p->hwctx.av_device_ref);
    }

#if HAVE_NGX_VSR
    ngx_vsr_cleanup(ctx);
#endif

#if HAVE_NVOFA
    nvofa_fruc_cleanup(ctx);
#endif

    fsr_cleanup(ctx);

    if (p->wrapped_tex)
        pl_tex_destroy(ctx->gpu, &p->wrapped_tex);

    if (p->d3d11)
        pl_d3d11_destroy(&p->d3d11);

    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_D3D11,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
#if HAVE_NGX_VSR
    .ngx_vsr_available = ngx_vsr_available_fn,
    .ngx_create_texture = ngx_create_texture_fn,
    .ngx_vsr_process = ngx_vsr_process_fn,
    .ngx_truehdr_available = ngx_truehdr_available_fn,
    .ngx_create_hdr_texture = ngx_create_hdr_texture_fn,
    .ngx_truehdr_process = ngx_truehdr_process_fn,
#endif
    .fsr_available = fsr_available_fn,
    .fsr_create_texture = fsr_create_texture_fn,
    .fsr_process = fsr_process_fn,
#if HAVE_NVOFA
    .nvofa_fruc_available = nvofa_fruc_available_fn,
    .nvofa_fruc_init_session = nvofa_fruc_init_session_fn,
    .nvofa_fruc_feed_frame = nvofa_fruc_feed_frame_fn,
    .nvofa_fruc_interpolate = nvofa_fruc_interpolate_fn,
    .nvofa_fruc_destroy = nvofa_fruc_destroy_fn,
#endif
};
