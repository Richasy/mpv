/*
 * RIFE frame interpolation filter (ONNX Runtime + DirectML).
 *
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
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This is step 3 of 3:
 *   - Loads onnxruntime.dll dynamically and queries DirectML EP.
 *   - Creates the ORT session lazily on the first usable frame.
 *   - For every adjacent input pair (prev, cur) runs the t=0.5 midpoint
 *     inference, packs the float output into an RGB0 mp_image at the
 *     original (unpadded) size, sets its PTS to (prev.pts + cur.pts) / 2,
 *     and splices it between prev and cur in the output stream.
 *   - This realises 2x frame multiplication (24→48 fps etc.). Higher
 *     multipliers are announced via the `multiplier` option but the
 *     MVP clamps to 2x.
 *
 * Expected input format: IMGFMT_RGB0 (chain `vf=format=rgb0,rife=...`).
 * Anything else falls back to passthrough with a one-shot warning.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>

// mpv's build globally defines INITGUID, so d3d12/dxgi GUIDs are emitted
// locally when their headers are first seen in this TU. IID_IDMLDevice is
// declared `extern` by DirectML.h and not provided by any import library, so
// we define it ourselves as MPV_IID_IDMLDevice further down.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>

#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include <d3d12.h>
#include <dxgi1_4.h>

// We do not include <DirectML.h> because it ships only in the
// microsoft.ai.directml NuGet package, which is not part of the mpv build
// include path (only the ORT-DirectML headers are). Provide the minimal
// forward declarations needed to call DMLCreateDevice and pass IDMLDevice*
// through to ORT's OrtDmlApi. dml_provider_factory.h also needs this type.
typedef struct IDMLDevice IDMLDevice;
typedef enum DML_CREATE_DEVICE_FLAGS {
    DML_CREATE_DEVICE_FLAG_NONE = 0,
} DML_CREATE_DEVICE_FLAGS;

#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>

// IDMLDevice GUID, manually defined (no import lib exposes it).
DEFINE_GUID(MPV_IID_IDMLDevice, 0x6dbd6437, 0x96fd, 0x423f,
            0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f);

#include "common/common.h"
#include "common/msg.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "common/tags.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "osdep/timer.h"
#include "osdep/windows_utils.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#include "video/filter/rife_shaders/cs_pack_rgb0.dxil.h"
#include "video/filter/rife_shaders/cs_fill_meta.dxil.h"
#include "video/filter/rife_shaders/cs_unpack_bgra.dxil.h"
#include "video/filter/rife_shaders/cs_copy_rgba_to_bgra.dxil.h"
#include "video/filter/rife_shaders/cs_nv12_to_rgba.dxil.h"
#include "video/filter/rife_shaders/cs_frame_diff.dxil.h"

// ---------------------------------------------------------------------------
// Runtime-loaded ORT API (shared across all instances).
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(
    IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid, void **ppDevice);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(
    UINT Flags, REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_DMLCreateDevice)(
    ID3D12Device *d3d12Device, DML_CREATE_DEVICE_FLAGS flags,
    REFIID riid, void **ppv);
typedef HRESULT (WINAPI *PFN_D3D12SerializeRootSignature)(
    const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob);

static struct {
    HMODULE       dll;
    HMODULE       d3d12_dll;
    HMODULE       dxgi_dll;
    HMODULE       dml_dll;
    const OrtApi *api;
    const OrtDmlApi *dml_api;
    OrtStatus *(*append_dml)(OrtSessionOptions *, int);
    PFN_D3D12CreateDevice            d3d12_create_device;
    PFN_CreateDXGIFactory2           create_dxgi_factory2;
    PFN_DMLCreateDevice              dml_create_device;
    PFN_D3D12SerializeRootSignature  d3d12_serialize_root_signature;
    wchar_t       resolved_path[MAX_PATH];
} g_ort = {0};

static bool ort_load(struct mp_log *log)
{
    if (g_ort.dll)
        return g_ort.api != NULL;

    HMODULE dll = LoadLibraryW(L"onnxruntime.dll");
    if (!dll) {
        // Fallback: try the directory holding the loaded copy of mpv/libmpv.
        HMODULE self = NULL;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ort_load, &self) && self)
        {
            wchar_t path[MAX_PATH] = {0};
            DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                wchar_t *slash = wcsrchr(path, L'\\');
                if (slash) {
                    wchar_t dir[MAX_PATH] = {0};
                    wcsncpy(dir, path, slash - path + 1);
                    // Add directory so transitive deps (DirectML.dll,
                    // onnxruntime_providers_shared.dll) load too.
                    AddDllDirectory(dir);
                    *(slash + 1) = 0;
                    wcscat(path, L"onnxruntime.dll");
                    mp_verbose(log, "trying %ls\n", path);
                    dll = LoadLibraryExW(path, NULL,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                }
            }
        }
    }
    if (!dll) {
        mp_warn(log, "onnxruntime.dll not found; RIFE disabled.\n");
        return false;
    }

    typedef const OrtApiBase *(*PFN_GetApiBase)(void);
    PFN_GetApiBase get_api_base =
        (PFN_GetApiBase)GetProcAddress(dll, "OrtGetApiBase");
    if (!get_api_base) {
        mp_warn(log, "missing OrtGetApiBase in onnxruntime.dll\n");
        FreeLibrary(dll);
        return false;
    }
    const OrtApiBase *base = get_api_base();
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api)
        api = base->GetApi(1);
    if (!api) {
        mp_warn(log, "ORT GetApi returned NULL\n");
        FreeLibrary(dll);
        return false;
    }

    g_ort.append_dml = (OrtStatus *(*)(OrtSessionOptions *, int))
        GetProcAddress(dll, "OrtSessionOptionsAppendExecutionProvider_DML");

    // Resolve OrtDmlApi (needed for DML1 EP + D3D12 device tensors).
    const void *dml_api_v = NULL;
    OrtStatus *st = api->GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                 &dml_api_v);
    if (st) {
        api->ReleaseStatus(st);
        mp_warn(log, "ORT GetExecutionProviderApi(\"DML\") failed; "
                     "zerocopy DML1 path will be unavailable.\n");
    } else {
        g_ort.dml_api = (const OrtDmlApi *)dml_api_v;
    }

    // Capture the actual loaded path so we can log it (rubber-duck note:
    // wrong onnxruntime.dll on PATH is a real footgun).
    GetModuleFileNameW(dll, g_ort.resolved_path, MAX_PATH);

    g_ort.dll = dll;
    g_ort.api = api;
    mp_verbose(log, "onnxruntime.dll loaded (API v%d): %ls\n",
               ORT_API_VERSION, g_ort.resolved_path);
    if (g_ort.dml_api)
        mp_verbose(log, "OrtDmlApi available (DML1 EP supported)\n");

    // Lazily resolve d3d12.dll / dxgi.dll / DirectML.dll. Failure here is
    // non-fatal: callers fall back to the legacy DML(gpu_id) path.
    g_ort.d3d12_dll = LoadLibraryW(L"d3d12.dll");
    g_ort.dxgi_dll  = LoadLibraryW(L"dxgi.dll");
    g_ort.dml_dll   = LoadLibraryW(L"DirectML.dll");
    if (g_ort.d3d12_dll) {
        g_ort.d3d12_create_device = (PFN_D3D12CreateDevice)
            GetProcAddress(g_ort.d3d12_dll, "D3D12CreateDevice");
        g_ort.d3d12_serialize_root_signature = (PFN_D3D12SerializeRootSignature)
            GetProcAddress(g_ort.d3d12_dll, "D3D12SerializeRootSignature");
    }
    if (g_ort.dxgi_dll) {
        g_ort.create_dxgi_factory2 = (PFN_CreateDXGIFactory2)
            GetProcAddress(g_ort.dxgi_dll, "CreateDXGIFactory2");
    }
    if (g_ort.dml_dll) {
        g_ort.dml_create_device = (PFN_DMLCreateDevice)
            GetProcAddress(g_ort.dml_dll, "DMLCreateDevice");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Filter state
// ---------------------------------------------------------------------------

struct rife_opts {
    bool  enabled;
    char *model_path;
    int   multiplier;
    int   gpu_id;
    float scale;
    float scene_threshold;
    float static_threshold;
    int   max_width;
    int   max_height;
    bool  hdr_passthrough;
    bool  zerocopy_smoke;  // Step 5a debug: replace D3D11 input with cyan
                           // BGRA D3D11 texture from our own pool, to prove
                           // the IMGFMT_D3D11+BGRA output plumbing works.
    bool  zerocopy;        // Step 5b: route inference through D3D12-backed
                           // OrtValues + DML1 EP + IoBinding instead of the
                           // legacy CPU-tensor + DML(gpu_id) path.
    bool  zerocopy_output; // Step 5c.1: allocate output BGRA textures as
                           // D3D11/D3D12 shared resources synchronised by a
                           // shared fence. Per-frame work on the D3D12 side is
                           // still placeholder (ClearRTV in dark blue) so we
                           // can validate cross-API plumbing in isolation.
    bool  pack_shader;       // Step 5c.2a: pack the input tensor on the GPU via
                             // a D3D12 compute shader instead of CPU pack +
                             // upload-heap memcpy + CopyBufferRegion.
    bool  pack_shader_debug; // Step 5c.2a: also run the CPU pack path each
                             // frame, read back the GPU-packed tensor, and
                             // log the max abs diff per channel. Slow.
    bool  unpack_shader;     // Step 5c.3: when zc-out is active, write the ML
                             // output tensor directly into a ring-slot BGRA
                             // texture via a D3D12 compute shader and emit
                             // IMGFMT_D3D11 mp_images instead of CPU RGB0.
    bool  nv12_input;        // Step 5c.4: when an upstream d3d11 hwframe with
                             // NV12 hw_subfmt is delivered, skip the CPU
                             // HW-download + swscale path and import the NV12
                             // texture directly into D3D12 via a small shared
                             // staging ring on the upstream d3d11 device, then
                             // run cs_nv12_to_rgba on the D3D12 side to fill
                             // the existing zc_pack_in_tex_* RGBA inputs.
};

struct rife_zc_slot;

struct priv {
    struct rife_opts *opts;

    // Internal autoconvert sub-filter so that callers may write `vf=rife=...`
    // without having to remember `format=rgb0`. The conv subfilter consumes
    // arbitrary input and emits IMGFMT_RGB0.
    struct mp_autoconvert *conv;

    bool tried_init;     // session creation has been attempted
    bool init_failed;    // permanent passthrough mode if true
    bool warned_format;  // one-shot bad-format warning
    bool warned_ring_exhaust;  // one-shot zc-out ring fully starved
    bool warned_ring_tight;    // one-shot zc-out ring couldn't satisfy N
    int  ring_short_count;     // consecutive frames with degraded N

    // ORT session state (NULL when not initialised)
    OrtEnv             *env;
    OrtSessionOptions  *session_opts;
    OrtSession         *session;
    OrtMemoryInfo      *mem_info;

    // Cached input/output names allocated by ORT.
    char  *input_name;
    char  *output_name;
    OrtAllocator *allocator;

    // Tensor geometry. The network actually consumes proc_w x proc_h pixels
    // padded out to (pad_w, pad_h) which is a multiple of 32. When opts->scale
    // < 1, proc dims are smaller than orig dims; pack_rgb0 nearest-down-samples
    // and unpack_rgb0 bilinear-upsamples back to (orig_w, orig_h).
    int orig_w, orig_h;
    int proc_w, proc_h;
    int pad_w,  pad_h;
    float inv_scale;     // == 1.0f / opts->scale (cached)
    int channels;        // 11 for vsmlrt RIFE v4.x ext_proc layout

    // Pre-allocated host-side buffers and tensors.
    float     *in_buf;   // [1 * 11 * pad_h * pad_w]
    float     *out_buf;  // [1 *  3 * pad_h * pad_w]
    OrtValue  *in_tensor;
    OrtValue  *out_tensor;

    // ----- Step 5b: D3D12 + DML1 IoBinding path (active iff opts->zerocopy) -----
    bool                       use_zc;            // computed at session init
    ID3D12Device              *zc_d3d12;
    ID3D12CommandQueue        *zc_queue;
    ID3D12CommandAllocator    *zc_cmd_alloc;
    ID3D12GraphicsCommandList *zc_cmd_list;
    ID3D12Fence               *zc_fence;
    HANDLE                     zc_fence_event;
    UINT64                     zc_fence_value;
    IDMLDevice                *zc_dml;

    ID3D12Resource            *zc_in_default;     // UAV, default heap
    ID3D12Resource            *zc_in_upload;      // upload heap, persistent map
    void                      *zc_in_upload_ptr;
    ID3D12Resource            *zc_out_default;    // UAV, default heap
    ID3D12Resource            *zc_out_readback;   // readback heap
    void                      *zc_in_alloc;       // OrtDmlApi opaque alloc
    void                      *zc_out_alloc;
    OrtMemoryInfo             *zc_dml_mem_info;
    OrtValue                  *zc_in_tensor;
    OrtValue                  *zc_out_tensor;
    OrtIoBinding              *zc_io_binding;
    size_t                     zc_in_bytes;
    size_t                     zc_out_bytes;

    // ----- Step 5c.1: cross-API output ring (active iff opts->zerocopy_output) -----
    // We allocate our OWN BGRA textures on the D3D12 side with HEAP_FLAG_SHARED,
    // open them as ID3D11Texture2D on mpv's D3D11 device, synchronise via a
    // single shared D3D12 fence opened on D3D11. Each emitted mp_image holds
    // exactly one slot through a custom AVBufferRef; the destructor returns the
    // slot to the free list so it can be recycled for a future frame.
    bool                       use_zc_out;
    LUID                       zc_d3d12_luid;
    LUID                       zc_d3d11_luid;
    ID3D12Fence               *zc_xfence_d3d12;   // SHARED fence
    ID3D11Fence               *zc_xfence_d3d11;
    HANDLE                     zc_xfence_handle;  // closed after both opens
    _Atomic uint64_t           zc_xfence_value;   // monotonic frame counter
    ID3D11Device5             *zc_d3d11_dev5;     // for OpenSharedFence/-Resource1
    ID3D11DeviceContext4      *zc_d3d11_ctx4;     // for Wait()
    ID3D12DescriptorHeap      *zc_rtv_heap;
    UINT                       zc_rtv_stride;
    int                        zc_ring_n;
    int                        zc_ring_w, zc_ring_h;
    struct rife_zc_slot       *zc_ring;

    // ----- Step 5c.2a: GPU input-tensor pack via compute shader (active iff
    // opts->pack_shader). Replaces the CPU pack_rgb0 + memcpy + CopyBufferRegion
    // path with a 3-dispatch GPU pack. zc_in_default must remain in
    // UNORDERED_ACCESS state for the shader's lifetime.
    bool                       use_pack_shader;
    ID3D12RootSignature       *zc_pack_root_sig;
    ID3D12PipelineState       *zc_pack_pso;     // cs_pack_rgb0
    ID3D12PipelineState       *zc_meta_pso;     // cs_fill_meta
    ID3D12DescriptorHeap      *zc_pack_heap;    // shader-visible CBV/SRV/UAV
    UINT                       zc_pack_heap_stride;
    // Per-pair input textures (R8G8B8A8_UNORM, orig_w x orig_h, default heap).
    // Initial state is COPY_DEST; transitioned to NON_PIXEL_SHADER_RESOURCE
    // for the dispatch and back to COPY_DEST at the end of each frame.
    ID3D12Resource            *zc_pack_in_tex_prev;
    ID3D12Resource            *zc_pack_in_tex_cur;
    // Persistently-mapped UPLOAD textures used to stage host RGB0 into the
    // default-heap input textures. row pitch is D3D12 placement-aligned.
    ID3D12Resource            *zc_pack_upload_prev;
    ID3D12Resource            *zc_pack_upload_cur;
    void                      *zc_pack_upload_prev_ptr;
    void                      *zc_pack_upload_cur_ptr;
    UINT64                     zc_pack_upload_row_pitch;  // aligned, in bytes
    UINT64                     zc_pack_upload_offset;     // placement offset
    int                        zc_pack_in_tex_w, zc_pack_in_tex_h;
    // Optional readback buffer used by parity-debug mode.
    ID3D12Resource            *zc_pack_in_readback;
    int                        zc_pack_dbg_frames; // counter for log throttling

    // ---- Step 5c.3: GPU output unpack (FP32 tensor -> BGRA8 ring slot) ----
    // Compute pipeline that reads zc_out_default (3-channel planar FP32 at
    // pad_w x pad_h, valid in proc_w x proc_h sub-area) and writes a BGRA8
    // ring slot texture (orig_w x orig_h) with bilinear upsample matching
    // the CPU unpack_rgb0 reference. Activated only when zc-out mode is on.
    ID3D12RootSignature       *zc_unpack_root_sig;
    ID3D12PipelineState       *zc_unpack_pso;
    // Descriptor heap layout: [0] = SRV ByteAddressBuffer over zc_out_default
    //                          [1..N] = UAV per ring slot (B8G8R8A8_UNORM)
    ID3D12DescriptorHeap      *zc_unpack_heap;
    UINT                       zc_unpack_heap_stride;
    bool                       use_unpack_shader;

    // ---- Step 5c.3 cur-copy compute pipeline (R8G8B8A8 -> B8G8R8A8) ----
    // Reuses zc_unpack_root_sig (same descriptor table layout). Heap layout:
    //   [0]      = SRV Texture2D R8G8B8A8 (zc_pack_in_tex_cur)
    //   [1..N]   = UAV per ring slot (B8G8R8A8_UNORM)
    // Used to project the *cur* frame onto the same shared D3D11 device as
    // the interpolated *mid* frame so the downstream filter chain sees a
    // uniform IMGFMT_D3D11 stream from a single device.
    ID3D12PipelineState       *zc_copy_pso;
    ID3D12DescriptorHeap      *zc_copy_heap;
    UINT                       zc_copy_heap_stride;
    bool                       use_copy_shader;

    // ---- Step 5c.4: D3D11 NV12 direct-import input path ----
    // When the upstream filter delivers IMGFMT_D3D11/NV12 hwframes, we copy
    // them into a tiny ring of "shareable" NV12 textures we allocate on the
    // upstream d3d11 device (BIND_SHADER_RESOURCE | MISC_SHARED_NTHANDLE),
    // import those into D3D12 via OpenSharedResource1, sync via the existing
    // zc_xfence pair, and run cs_nv12_to_rgba on D3D12 to fill the existing
    // zc_pack_in_tex_prev/cur (R8G8B8A8) — replacing the HW-download +
    // swscale + UPLOAD-buffer path on these frames.
    bool                       use_nv12_input;
    int                        nv12_w, nv12_h;
    int                        nv12_subfmt; // IMGFMT_NV12 or IMGFMT_P010
    ID3D11Texture2D           *nv12_stage_d11[2]; // [0]=prev, [1]=cur
    HANDLE                     nv12_share_handle[2];
    ID3D12Resource            *nv12_stage_d12[2];
    ID3D12RootSignature       *zc_n2r_root_sig;
    ID3D12PipelineState       *zc_n2r_pso;
    // Heap layout (3 contiguous descriptors per slot):
    //   slot i base+0 = SRV Texture2D<float>  Y plane (R8_UNORM for NV12,
    //                                          R16_UNORM for P010), PlaneSlice 0
    //   slot i base+1 = SRV Texture2D<float2> UV plane (R8G8_UNORM for NV12,
    //                                          R16G16_UNORM for P010), PlaneSlice 1
    //   slot i base+2 = UAV Texture2D unorm float4 (zc_pack_in_tex_prev/cur)
    ID3D12DescriptorHeap      *zc_n2r_heap;
    UINT                       zc_n2r_heap_stride;
    UINT                       zc_n2r_slot_descs; // 3

    // ---- Frame-diff (scene/static detection) ----
    // Reads pack_in_tex_prev/cur (RGBA8) on the same cmd list as pack_shader,
    // writes a 16-byte UAV buffer (uint64 SAD-fp16, uint64 sumLuma-fp16) that
    // is copied to a CPU-readable readback buffer. After fence wait we
    // compute ratio = SAD/sumLuma and decide whether to skip RIFE.
    bool                       use_frame_diff;
    ID3D12RootSignature       *zc_diff_root_sig;
    ID3D12PipelineState       *zc_diff_pso;
    ID3D12DescriptorHeap      *zc_diff_heap;     // 3 descriptors: SRV prev, SRV cur, UAV buf
    UINT                       zc_diff_heap_stride;
    ID3D12Resource            *zc_diff_uav_buf;  // default heap, 16 bytes
    ID3D12Resource            *zc_diff_readback; // readback heap, 16 bytes
    void                      *zc_diff_readback_ptr; // persistent map
    ID3D12Fence               *zc_diff_fence;
    HANDLE                     zc_diff_event;
    uint64_t                   zc_diff_fence_value;
    // Last decision (for logging / hysteresis if needed).
    int                        diff_last_kind;   // 0=normal 1=static 2=scene

    // Stats (exposed via vf-metadata, also used by stats.lua).
    double                     stats_last_infer_ms;
    double                     stats_last_diff_ratio;
    uint64_t                   stats_total_pairs;
    uint64_t                   stats_skipped_static;
    uint64_t                   stats_skipped_scene;

    // Sliding-window FPS tracking. Tick once per source pair. source_fps =
    // pairs/sec, output_fps = source_fps * multiplier (every pair emits
    // `multiplier` frames whether RIFE inference runs or is skipped).
    double                     fps_window_start;
    uint64_t                   fps_window_count;
    double                     fps_source_pps;

    // ---- Step 5c.3 raw-input FIFO ----
    // Pre-autoconvert raw mp_image refs queued in arrival order. When zc-out
    // mode is active we pop from here whenever autoconvert delivers an RGB0
    // result, so we can emit the original D3D11 frame downstream instead of
    // the converted RGB0 copy. Sized for typical autoconvert latency.
#define RIFE_RAW_FIFO_N 8
    struct mp_image           *raw_in_fifo[RIFE_RAW_FIFO_N];
    int                        raw_in_head;  // pop index
    int                        raw_in_tail;  // push index
    bool                       use_zc_out_ml;  // sticky stream-mode flag
    bool                       zc_out_ml_decided;
    // Last raw D3D11 ref of the previous original frame, used to emit "cur"
    // as a passthrough D3D11 ref alongside the ring-slot midpoint.
    struct mp_image           *raw_pending;

    // Last input frame kept for forming the (prev, cur) pair.
    struct mp_image *prev;

    // Pending output frame queue. For Nx multiplier we emit the first
    // midpoint immediately and queue mid_2..mid_{N-1} + cur here, draining
    // one per process() turn before consuming new input.
#define RIFE_PENDING_Q_N 8
    struct mp_image *pending_q[RIFE_PENDING_Q_N];
    int              pending_q_head;  // pop index
    int              pending_q_tail;  // push index
    int              pending_q_count;

    bool warned_multiplier;

    // ---- Step 5a: zero-copy output plumbing prove-out ----
    // These are populated lazily on the first IMGFMT_D3D11 input frame when
    // opts->zerocopy_smoke is enabled. The smoke test allocates BGRA D3D11
    // textures from a private hw_pool and emits them in place of the input,
    // filled with a recognisable solid colour, so we can validate that
    // vo_gpu_next/libplacebo will accept and render our self-allocated
    // IMGFMT_D3D11 output without writing any shaders or ORT D3D12 code yet.
    AVBufferRef         *av_device_ref;  // shared D3D11 device (FFmpeg ref)
    AVBufferRef         *hw_pool;        // BGRA hw_frames pool
    ID3D11Device        *d3d11_dev;      // borrowed; AddRef'd
    ID3D11DeviceContext *d3d11_ctx;      // immediate context
    int                  zc_w, zc_h;     // pool dimensions
    bool                 zc_init_tried;
    bool                 zc_init_failed;
};

struct rife_zc_slot {
    int                idx;
    _Atomic int        in_flight;        // 1 = held by an mp_image, 0 = free
    ID3D12Resource    *d3d12_tex;        // BGRA, SHARED, RT|UAV
    ID3D11Texture2D   *d3d11_tex;        // opened wrapper on mpv's D3D11
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu;
    struct priv       *owner;            // for destructor logging only
};

#define CH_R0   0
#define CH_G0   1
#define CH_B0   2
#define CH_R1   3
#define CH_G1   4
#define CH_B1   5
#define CH_T    6
#define CH_HG   7
#define CH_VG   8
#define CH_HSCL 9
#define CH_VSCL 10

static bool ort_check(struct mp_filter *vf, OrtStatus *st, const char *op)
{
    if (!st)
        return true;
    const char *msg = g_ort.api->GetErrorMessage(st);
    MP_ERR(vf, "ORT %s failed: %s\n", op, msg ? msg : "(unknown)");
    g_ort.api->ReleaseStatus(st);
    return false;
}

// Pack one RGB0 plane (R,G,B,X) into channels [base..base+2] of the input
// tensor as float [0,1], writing into the padded buffer. The padded region
// beyond (proc_w, proc_h) is filled by edge replication. When inv_scale > 1
// (i.e. opts->scale < 1) the source is nearest-neighbour down-sampled.
static void pack_rgb0(struct priv *p, const uint8_t *src, ptrdiff_t stride,
                      int base_ch)
{
    const int W = p->pad_w, H = p->pad_h;
    const int pW = p->proc_w, pH = p->proc_h;
    const int oW = p->orig_w, oH = p->orig_h;
    const float inv_s = p->inv_scale;
    float *dr = p->in_buf + (size_t)(base_ch + 0) * H * W;
    float *dg = p->in_buf + (size_t)(base_ch + 1) * H * W;
    float *db = p->in_buf + (size_t)(base_ch + 2) * H * W;
    const float inv = 1.0f / 255.0f;

    for (int y = 0; y < H; y++) {
        int yp = y < pH ? y : pH - 1;
        int sy = (int)(yp * inv_s + 0.5f);
        if (sy < 0) sy = 0;
        if (sy >= oH) sy = oH - 1;
        const uint8_t *row = src + sy * stride;
        for (int x = 0; x < W; x++) {
            int xp = x < pW ? x : pW - 1;
            int sx = (int)(xp * inv_s + 0.5f);
            if (sx < 0) sx = 0;
            if (sx >= oW) sx = oW - 1;
            const uint8_t *px = row + sx * 4;
            size_t off = (size_t)y * W + x;
            dr[off] = px[0] * inv;
            dg[off] = px[1] * inv;
            db[off] = px[2] * inv;
        }
    }
}

// Fill the four "metadata" channels: timestep, hgrid, vgrid, hscale, vscale.
// The grid uses padded dimensions to match the network's spatial tensor.
static void fill_meta_channels(struct priv *p, float t)
{
    const int W = p->pad_w, H = p->pad_h;
    float *t_ch    = p->in_buf + (size_t)CH_T    * H * W;
    float *hg_ch   = p->in_buf + (size_t)CH_HG   * H * W;
    float *vg_ch   = p->in_buf + (size_t)CH_VG   * H * W;
    float *hscl_ch = p->in_buf + (size_t)CH_HSCL * H * W;
    float *vscl_ch = p->in_buf + (size_t)CH_VSCL * H * W;
    const float hscl = 2.0f / (W - 1);
    const float vscl = 2.0f / (H - 1);

    for (int y = 0; y < H; y++) {
        float vg = 2.0f * y / (H - 1) - 1.0f;
        for (int x = 0; x < W; x++) {
            size_t off = (size_t)y * W + x;
            t_ch[off]    = t;
            hg_ch[off]   = 2.0f * x / (W - 1) - 1.0f;
            vg_ch[off]   = vg;
            hscl_ch[off] = hscl;
            vscl_ch[off] = vscl;
        }
    }
}

// ---------------------------------------------------------------------------
// Step 5b: D3D12 + DML1 IoBinding helpers
// ---------------------------------------------------------------------------

static void pack_shader_release(struct priv *p);
static void unpack_shader_release(struct priv *p);
static void copy_shader_release(struct priv *p);
static void nv12_input_release(struct priv *p);
static void frame_diff_release(struct priv *p);

static void d3d12_release(struct priv *p)
{
    const OrtApi *api = g_ort.api;
    const OrtDmlApi *dml = g_ort.dml_api;

    // Drain the GPU before tearing down any resources. Otherwise shared
    // NV12/P010 staging textures (and other in-flight resources) may still
    // be referenced by pending command lists, which on some drivers (e.g.
    // NVIDIA at 4K P010) corrupts internal state and crashes the next
    // CreateTexture2D / OpenSharedHandle on the same device.
    if (p->zc_queue && p->zc_fence && p->zc_fence_event) {
        uint64_t v = ++p->zc_fence_value;
        if (SUCCEEDED(ID3D12CommandQueue_Signal(p->zc_queue, p->zc_fence, v))) {
            if (ID3D12Fence_GetCompletedValue(p->zc_fence) < v) {
                if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(
                        p->zc_fence, v, p->zc_fence_event)))
                {
                    WaitForSingleObject(p->zc_fence_event, 2000);
                }
            }
        }
    }

    pack_shader_release(p);
    unpack_shader_release(p);
    copy_shader_release(p);
    // Flush the upstream D3D11 immediate context too, so any queued
    // CopySubresourceRegion onto our shared NV12/P010 staging textures
    // completes before we release them.
    if (p->d3d11_ctx)
        ID3D11DeviceContext_Flush(p->d3d11_ctx);
    nv12_input_release(p);
    frame_diff_release(p);

    if (p->zc_io_binding && api) {
        api->ReleaseIoBinding(p->zc_io_binding);
        p->zc_io_binding = NULL;
    }
    if (p->zc_in_tensor && api) {
        api->ReleaseValue(p->zc_in_tensor);
        p->zc_in_tensor = NULL;
    }
    if (p->zc_out_tensor && api) {
        api->ReleaseValue(p->zc_out_tensor);
        p->zc_out_tensor = NULL;
    }
    if (p->zc_dml_mem_info && api) {
        api->ReleaseMemoryInfo(p->zc_dml_mem_info);
        p->zc_dml_mem_info = NULL;
    }
    if (p->zc_in_alloc && dml) {
        dml->FreeGPUAllocation(p->zc_in_alloc);
        p->zc_in_alloc = NULL;
    }
    if (p->zc_out_alloc && dml) {
        dml->FreeGPUAllocation(p->zc_out_alloc);
        p->zc_out_alloc = NULL;
    }
    if (p->zc_in_upload && p->zc_in_upload_ptr) {
        ID3D12Resource_Unmap(p->zc_in_upload, 0, NULL);
        p->zc_in_upload_ptr = NULL;
    }
    SAFE_RELEASE(p->zc_in_default);
    SAFE_RELEASE(p->zc_in_upload);
    SAFE_RELEASE(p->zc_out_default);
    SAFE_RELEASE(p->zc_out_readback);
    if (p->zc_dml) {
        IUnknown_Release((IUnknown *)p->zc_dml);
        p->zc_dml = NULL;
    }
    SAFE_RELEASE(p->zc_fence);
    if (p->zc_fence_event) {
        CloseHandle(p->zc_fence_event);
        p->zc_fence_event = NULL;
    }
    SAFE_RELEASE(p->zc_cmd_list);
    SAFE_RELEASE(p->zc_cmd_alloc);
    SAFE_RELEASE(p->zc_queue);
    SAFE_RELEASE(p->zc_d3d12);
    p->zc_fence_value = 0;
    p->use_zc = false;
}

static bool d3d12_init(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (!g_ort.dml_api || !g_ort.d3d12_create_device ||
        !g_ort.create_dxgi_factory2 || !g_ort.dml_create_device)
    {
        MP_WARN(vf, "RIFE zerocopy: prerequisites missing "
                    "(dml_api=%p d3d12=%p dxgi=%p dml=%p)\n",
                (void *)g_ort.dml_api,
                (void *)g_ort.d3d12_create_device,
                (void *)g_ort.create_dxgi_factory2,
                (void *)g_ort.dml_create_device);
        return false;
    }

    HRESULT hr;
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;

    hr = g_ort.create_dxgi_factory2(0, &IID_IDXGIFactory4, (void **)&factory);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: CreateDXGIFactory2 hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }

    int gpu_id = p->opts->gpu_id;
    int hw_idx = 0;
    for (UINT i = 0; ; i++) {
        IDXGIAdapter1 *cand = NULL;
        if (FAILED(IDXGIFactory4_EnumAdapters1(factory, i, &cand)))
            break;
        DXGI_ADAPTER_DESC1 desc = {0};
        IDXGIAdapter1_GetDesc1(cand, &desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            SAFE_RELEASE(cand);
            continue;
        }
        if (hw_idx == gpu_id) {
            adapter = cand;
            MP_VERBOSE(vf, "RIFE zc: selected adapter %d: %ls "
                           "(vendor=0x%04x device=0x%04x)\n",
                       hw_idx, desc.Description,
                       desc.VendorId, desc.DeviceId);
            break;
        }
        SAFE_RELEASE(cand);
        hw_idx++;
    }
    if (!adapter) {
        MP_ERR(vf, "RIFE zc: gpu_id=%d not found among hw adapters\n", gpu_id);
        SAFE_RELEASE(factory);
        return false;
    }

    hr = g_ort.d3d12_create_device((IUnknown *)adapter,
                                   D3D_FEATURE_LEVEL_11_0,
                                   &IID_ID3D12Device,
                                   (void **)&p->zc_d3d12);
    SAFE_RELEASE(adapter);
    SAFE_RELEASE(factory);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: D3D12CreateDevice hr=0x%08lx\n", (unsigned long)hr);
        goto fail;
    }

    D3D12_COMMAND_QUEUE_DESC qdesc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
    };
    hr = ID3D12Device_CreateCommandQueue(p->zc_d3d12, &qdesc,
            &IID_ID3D12CommandQueue, (void **)&p->zc_queue);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: CreateCommandQueue hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }

    hr = ID3D12Device_CreateCommandAllocator(p->zc_d3d12,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&p->zc_cmd_alloc);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: CreateCommandAllocator hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }

    hr = ID3D12Device_CreateCommandList(p->zc_d3d12, 0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, p->zc_cmd_alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&p->zc_cmd_list);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: CreateCommandList hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    ID3D12GraphicsCommandList_Close(p->zc_cmd_list);

    hr = ID3D12Device_CreateFence(p->zc_d3d12, 0,
            D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
            (void **)&p->zc_fence);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: CreateFence hr=0x%08lx\n", (unsigned long)hr);
        goto fail;
    }
    p->zc_fence_value = 0;
    p->zc_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->zc_fence_event) {
        MP_ERR(vf, "RIFE zc: CreateEventW failed\n");
        goto fail;
    }

    hr = g_ort.dml_create_device(p->zc_d3d12, DML_CREATE_DEVICE_FLAG_NONE,
            &MPV_IID_IDMLDevice, (void **)&p->zc_dml);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: DMLCreateDevice hr=0x%08lx\n", (unsigned long)hr);
        goto fail;
    }

    MP_INFO(vf, "RIFE zc: D3D12 device + DML device ready (gpu=%d)\n", gpu_id);
    return true;

fail:
    d3d12_release(p);
    return false;
}

static bool d3d12_alloc_tensors(struct mp_filter *vf,
                                size_t in_bytes, size_t out_bytes)
{
    struct priv *p = vf->priv;
    HRESULT hr;
    OrtStatus *st;

    p->zc_in_bytes = in_bytes;
    p->zc_out_bytes = out_bytes;

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES upload_heap  = { .Type = D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES rb_heap      = { .Type = D3D12_HEAP_TYPE_READBACK };

    D3D12_RESOURCE_DESC rd = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };

    rd.Width = in_bytes;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &default_heap,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&p->zc_in_default);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: create in_default hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }

    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &upload_heap,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void **)&p->zc_in_upload);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: create in_upload hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    D3D12_RANGE no_read = {0, 0};
    hr = ID3D12Resource_Map(p->zc_in_upload, 0, &no_read,
                            &p->zc_in_upload_ptr);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: map in_upload hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }

    rd.Width = out_bytes;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &default_heap,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&p->zc_out_default);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: create out_default hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }

    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &rb_heap,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&p->zc_out_readback);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc: create out_readback hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }

    st = g_ort.dml_api->CreateGPUAllocationFromD3DResource(p->zc_in_default,
            &p->zc_in_alloc);
    if (!ort_check(vf, st, "CreateGPUAllocationFromD3DResource(in)"))
        return false;
    st = g_ort.dml_api->CreateGPUAllocationFromD3DResource(p->zc_out_default,
            &p->zc_out_alloc);
    if (!ort_check(vf, st, "CreateGPUAllocationFromD3DResource(out)"))
        return false;

    st = g_ort.api->CreateMemoryInfo("DML", OrtDeviceAllocator,
            p->opts->gpu_id, OrtMemTypeDefault, &p->zc_dml_mem_info);
    if (!ort_check(vf, st, "CreateMemoryInfo(DML)"))
        return false;

    int64_t in_shape[]  = {1, p->channels, p->pad_h, p->pad_w};
    int64_t out_shape[] = {1,           3, p->pad_h, p->pad_w};
    st = g_ort.api->CreateTensorWithDataAsOrtValue(p->zc_dml_mem_info,
            p->zc_in_alloc, in_bytes, in_shape, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &p->zc_in_tensor);
    if (!ort_check(vf, st, "CreateTensorWithDataAsOrtValue(zc in)"))
        return false;
    st = g_ort.api->CreateTensorWithDataAsOrtValue(p->zc_dml_mem_info,
            p->zc_out_alloc, out_bytes, out_shape, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &p->zc_out_tensor);
    if (!ort_check(vf, st, "CreateTensorWithDataAsOrtValue(zc out)"))
        return false;

    MP_INFO(vf, "RIFE zc: D3D12 buffers + tensors ready "
                "(in=%zu B, out=%zu B)\n", in_bytes, out_bytes);
    return true;
}

// ---------------------------------------------------------------------------
// Step 5c.2a: GPU input-tensor pack via D3D12 compute shaders.
// Replaces pack_rgb0() + fill_meta_channels() + memcpy + CopyBufferRegion
// with: 2x CopyTextureRegion (UPLOAD buffer -> input tex) + 2x cs_pack_rgb0
// dispatch + 1x cs_fill_meta dispatch. zc_in_default is held in
// UNORDERED_ACCESS state for the lifetime of the shader path.
// ---------------------------------------------------------------------------

#define D3D12_PITCH_ALIGN  256u
#define D3D12_PLACE_ALIGN  512u

static void pack_shader_release(struct priv *p)
{
    if (p->zc_pack_upload_prev && p->zc_pack_upload_prev_ptr) {
        ID3D12Resource_Unmap(p->zc_pack_upload_prev, 0, NULL);
        p->zc_pack_upload_prev_ptr = NULL;
    }
    if (p->zc_pack_upload_cur && p->zc_pack_upload_cur_ptr) {
        ID3D12Resource_Unmap(p->zc_pack_upload_cur, 0, NULL);
        p->zc_pack_upload_cur_ptr = NULL;
    }
    SAFE_RELEASE(p->zc_pack_upload_prev);
    SAFE_RELEASE(p->zc_pack_upload_cur);
    SAFE_RELEASE(p->zc_pack_in_tex_prev);
    SAFE_RELEASE(p->zc_pack_in_tex_cur);
    SAFE_RELEASE(p->zc_pack_in_readback);
    SAFE_RELEASE(p->zc_pack_heap);
    SAFE_RELEASE(p->zc_pack_pso);
    SAFE_RELEASE(p->zc_meta_pso);
    SAFE_RELEASE(p->zc_pack_root_sig);
    p->zc_pack_heap_stride = 0;
    p->zc_pack_in_tex_w = p->zc_pack_in_tex_h = 0;
    p->zc_pack_upload_row_pitch = 0;
    p->use_pack_shader = false;
}

// Build a shared root signature: descriptor table SRV(t0), descriptor table
// UAV(u0), 32-bit constants b0 (8 dwords). Both PSOs use it; cs_fill_meta
// just leaves the SRV slot unused.
static bool pack_shader_make_root_sig(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    if (!g_ort.d3d12_serialize_root_signature) {
        MP_WARN(vf, "RIFE pack-shader: D3D12SerializeRootSignature unavailable\n");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srv_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE uav_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_ROOT_PARAMETER params[3] = {
        [0] = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = { 1, &srv_range },
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        [1] = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = { 1, &uav_range },
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        [2] = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                .Constants = { .ShaderRegister = 0, .RegisterSpace = 0,
                               .Num32BitValues = 8 },
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = 3, .pParameters = params,
        .NumStaticSamplers = 0, .pStaticSamplers = NULL,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };

    ID3DBlob *blob = NULL, *err = NULL;
    hr = g_ort.d3d12_serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            MP_ERR(vf, "RIFE pack-shader: serialize root sig hr=0x%08lx: %.*s\n",
                   (unsigned long)hr,
                   (int)err->lpVtbl->GetBufferSize(err),
                   (const char *)err->lpVtbl->GetBufferPointer(err));
            err->lpVtbl->Release(err);
        } else {
            MP_ERR(vf, "RIFE pack-shader: serialize root sig hr=0x%08lx\n",
                   (unsigned long)hr);
        }
        return false;
    }

    hr = ID3D12Device_CreateRootSignature(p->zc_d3d12, 0,
            blob->lpVtbl->GetBufferPointer(blob),
            blob->lpVtbl->GetBufferSize(blob),
            &IID_ID3D12RootSignature,
            (void **)&p->zc_pack_root_sig);
    blob->lpVtbl->Release(blob);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE pack-shader: CreateRootSignature hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    return true;
}

static bool pack_shader_make_pso(struct mp_filter *vf,
                                 const void *dxil, size_t dxil_size,
                                 ID3D12PipelineState **out)
{
    struct priv *p = vf->priv;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {
        .pRootSignature = p->zc_pack_root_sig,
        .CS = { .pShaderBytecode = dxil, .BytecodeLength = dxil_size },
        .NodeMask = 0,
        .CachedPSO = { 0 },
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
    HRESULT hr = ID3D12Device_CreateComputePipelineState(p->zc_d3d12, &desc,
            &IID_ID3D12PipelineState, (void **)out);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE pack-shader: CreateComputePipelineState hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    return true;
}

static bool pack_shader_init(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    if (!p->zc_d3d12 || !p->zc_in_default) {
        MP_WARN(vf, "RIFE pack-shader: prereqs missing (D3D12 zc not ready)\n");
        return false;
    }

    if (!pack_shader_make_root_sig(vf))
        goto fail;

    if (!pack_shader_make_pso(vf, cs_pack_rgb0_dxil, cs_pack_rgb0_dxil_size,
                              &p->zc_pack_pso))
        goto fail;
    if (!pack_shader_make_pso(vf, cs_fill_meta_dxil, cs_fill_meta_dxil_size,
                              &p->zc_meta_pso))
        goto fail;

    // Shader-visible heap with 3 entries: SRV[0]=prev, SRV[1]=cur, UAV[2]=tensor.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 3,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_pack_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE pack-shader: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    p->zc_pack_heap_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Allocate per-pair input textures (R8G8B8A8_UNORM, orig_w x orig_h).
    int W = p->orig_w, H = p->orig_h;
    p->zc_pack_in_tex_w = W;
    p->zc_pack_in_tex_h = H;

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES upload_heap  = { .Type = D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC tex_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = W, .Height = H, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        // ALLOW_UAV is required so the Step 5c.4 NV12-input compute path
        // (cs_nv12_to_rgba) can write into these textures directly without
        // going through the CPU upload buffer.
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    for (int i = 0; i < 2; i++) {
        ID3D12Resource **dst = (i == 0) ? &p->zc_pack_in_tex_prev
                                        : &p->zc_pack_in_tex_cur;
        hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &default_heap,
                D3D12_HEAP_FLAG_NONE, &tex_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void **)dst);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE pack-shader: create in_tex[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
    }

    // Upload buffer sized to one full RGBA8 frame at D3D12 placement pitch.
    UINT64 row_pitch = ((UINT64)W * 4u + D3D12_PITCH_ALIGN - 1u) &
                       ~(UINT64)(D3D12_PITCH_ALIGN - 1u);
    UINT64 upload_size = row_pitch * (UINT64)H;
    p->zc_pack_upload_row_pitch = row_pitch;
    p->zc_pack_upload_offset = 0;

    D3D12_RESOURCE_DESC buf_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = upload_size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    for (int i = 0; i < 2; i++) {
        ID3D12Resource **dst = (i == 0) ? &p->zc_pack_upload_prev
                                        : &p->zc_pack_upload_cur;
        hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &upload_heap,
                D3D12_HEAP_FLAG_NONE, &buf_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void **)dst);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE pack-shader: create upload[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
        D3D12_RANGE no_read = { 0, 0 };
        void **ptr = (i == 0) ? &p->zc_pack_upload_prev_ptr
                              : &p->zc_pack_upload_cur_ptr;
        hr = ID3D12Resource_Map(*dst, 0, &no_read, ptr);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE pack-shader: map upload[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
    }

    // Populate the descriptor heap.
    D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu;
    p->zc_pack_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_pack_heap, &heap_cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1,
                       .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_cpu;
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12,
            p->zc_pack_in_tex_prev, &srv_desc, h);
    h.ptr += p->zc_pack_heap_stride;
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12,
            p->zc_pack_in_tex_cur, &srv_desc, h);
    h.ptr += p->zc_pack_heap_stride;

    UINT in_floats = (UINT)(p->zc_in_bytes / sizeof(float));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = { .FirstElement = 0,
                    .NumElements = in_floats,
                    .StructureByteStride = 0,
                    .CounterOffsetInBytes = 0,
                    .Flags = D3D12_BUFFER_UAV_FLAG_RAW },
    };
    ID3D12Device_CreateUnorderedAccessView(p->zc_d3d12,
            p->zc_in_default, NULL, &uav_desc, h);

    // Optional readback buffer for parity-debug. Mirrors zc_in_default size.
    if (p->opts->pack_shader_debug) {
        D3D12_HEAP_PROPERTIES rb_heap = { .Type = D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rb_desc = buf_desc;
        rb_desc.Width = p->zc_in_bytes;
        hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &rb_heap,
                D3D12_HEAP_FLAG_NONE, &rb_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void **)&p->zc_pack_in_readback);
        if (FAILED(hr)) {
            MP_WARN(vf, "RIFE pack-shader: in_readback hr=0x%08lx (no debug)\n",
                    (unsigned long)hr);
            SAFE_RELEASE(p->zc_pack_in_readback);
        }
    }

    p->use_pack_shader = true;
    MP_INFO(vf, "RIFE pack-shader: ready (%dx%d RGBA8, upload pitch=%llu B)\n",
            W, H, (unsigned long long)row_pitch);
    return true;

fail:
    pack_shader_release(p);
    return false;
}

// Stage one RGB0 host plane into a persistently-mapped UPLOAD buffer using
// D3D12 placement pitch.
static void pack_shader_stage(uint8_t *dst, UINT64 dst_pitch,
                              const uint8_t *src, ptrdiff_t src_stride,
                              int W, int H)
{
    size_t copy_bytes = (size_t)W * 4u;
    for (int y = 0; y < H; y++) {
        memcpy(dst + (size_t)y * dst_pitch, src + (ptrdiff_t)y * src_stride,
               copy_bytes);
    }
}

// Pack one frame: record the CopyTextureRegion + barrier + dispatch
// sequence. Caller has already populated upload buffer and reset cmdlist.
static void pack_shader_record_one(struct priv *p,
                                   ID3D12Resource *in_tex,
                                   ID3D12Resource *upload,
                                   D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu,
                                   UINT base_ch)
{
    if (upload) {
        D3D12_TEXTURE_COPY_LOCATION src = {
            .pResource = upload,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = {
                .Offset = 0,
                .Footprint = {
                    .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                    .Width = p->orig_w, .Height = p->orig_h, .Depth = 1,
                    .RowPitch = (UINT)p->zc_pack_upload_row_pitch,
                },
            },
        };
        D3D12_TEXTURE_COPY_LOCATION dst = {
            .pResource = in_tex,
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };
        ID3D12GraphicsCommandList_CopyTextureRegion(p->zc_cmd_list, &dst,
                0, 0, 0, &src, NULL);
    }

    D3D12_RESOURCE_BARRIER b = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = in_tex,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);

    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
            p->zc_cmd_list, 0, srv_gpu);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
            p->zc_cmd_list, 1, uav_gpu);

    UINT params[8];
    params[0] = (UINT)p->pad_w;
    params[1] = (UINT)p->pad_h;
    params[2] = (UINT)p->proc_w;
    params[3] = (UINT)p->proc_h;
    params[4] = (UINT)p->orig_w;
    params[5] = (UINT)p->orig_h;
    params[6] = base_ch;
    float inv_s = p->inv_scale;
    memcpy(&params[7], &inv_s, sizeof(float));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(
            p->zc_cmd_list, 2, 8, params, 0);

    UINT gx = ((UINT)p->pad_w + 7u) / 8u;
    UINT gy = ((UINT)p->pad_h + 7u) / 8u;
    ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);
}

// ---------------------------------------------------------------------------
// Step 5c.3: GPU output unpack compute pipeline (FP32 tensor -> BGRA8 ring
// slot texture). Built lazily by zc_out_init when opts->unpack_shader is on.
// Heap layout: [0] SRV ByteAddressBuffer over zc_out_default
//              [1..ZC_RING_N] UAV B8G8R8A8_UNORM per ring slot
// ---------------------------------------------------------------------------

static void unpack_shader_release(struct priv *p)
{
    if (p->zc_unpack_pso)      { ID3D12PipelineState_Release(p->zc_unpack_pso); p->zc_unpack_pso = NULL; }
    if (p->zc_unpack_root_sig) { ID3D12RootSignature_Release(p->zc_unpack_root_sig); p->zc_unpack_root_sig = NULL; }
    if (p->zc_unpack_heap)     { ID3D12DescriptorHeap_Release(p->zc_unpack_heap); p->zc_unpack_heap = NULL; }
    p->use_unpack_shader = false;
}

static bool unpack_shader_make_root_sig(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!g_ort.d3d12_serialize_root_signature) {
        MP_WARN(vf, "RIFE unpack-shader: D3D12SerializeRootSignature missing\n");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE srv_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE uav_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_ROOT_PARAMETER params[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &srv_range },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &uav_range },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
          .Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 8 },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = 3, .pParameters = params,
        .NumStaticSamplers = 0, .pStaticSamplers = NULL,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    ID3DBlob *sig = NULL, *err = NULL;
    HRESULT hr = g_ort.d3d12_serialize_root_signature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1_0, &sig, &err);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE unpack-shader: SerializeRootSignature hr=0x%08lx %s\n",
               (unsigned long)hr,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return false;
    }
    hr = ID3D12Device_CreateRootSignature(p->zc_d3d12, 0,
            ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&p->zc_unpack_root_sig);
    ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE unpack-shader: CreateRootSignature hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    return true;
}

static bool unpack_shader_init(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!p->zc_d3d12 || !p->zc_out_default || !p->zc_ring) {
        MP_WARN(vf, "RIFE unpack-shader: zc/D3D12/ring not ready\n");
        return false;
    }
    if (!unpack_shader_make_root_sig(vf))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {
        .pRootSignature = p->zc_unpack_root_sig,
        .CS = { .pShaderBytecode = cs_unpack_bgra_dxil,
                .BytecodeLength  = cs_unpack_bgra_dxil_size },
    };
    HRESULT hr = ID3D12Device_CreateComputePipelineState(p->zc_d3d12, &psd,
            &IID_ID3D12PipelineState, (void **)&p->zc_unpack_pso);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE unpack-shader: CreateComputePipelineState hr=0x%08lx\n",
               (unsigned long)hr);
        unpack_shader_release(p);
        return false;
    }

    // Heap: [0] SRV over zc_out_default; [1..N] UAV per slot.
    UINT n_descs = 1u + (UINT)p->zc_ring_n;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = n_descs,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_unpack_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE unpack-shader: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        unpack_shader_release(p);
        return false;
    }
    p->zc_unpack_heap_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    p->zc_unpack_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_unpack_heap, &cpu_base);

    // [0] SRV ByteAddressBuffer over zc_out_default.
    UINT64 num_elems = p->zc_out_bytes / 4u;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {
            .FirstElement = 0,
            .NumElements = (UINT)num_elems,
            .StructureByteStride = 0,
            .Flags = D3D12_BUFFER_SRV_FLAG_RAW,
        },
    };
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12, p->zc_out_default,
            &sv, cpu_base);

    // [1..N] UAV per slot (DXGI_FORMAT_B8G8R8A8_UNORM Texture2D).
    for (int i = 0; i < p->zc_ring_n; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpu_base;
        cpu.ptr += (size_t)(i + 1) * p->zc_unpack_heap_stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
        };
        ID3D12Device_CreateUnorderedAccessView(p->zc_d3d12,
                p->zc_ring[i].d3d12_tex, NULL, &uv, cpu);
    }

    p->use_unpack_shader = true;
    MP_INFO(vf, "RIFE unpack-shader: ready (out %dx%d BGRA, ring=%d)\n",
            p->zc_ring_w, p->zc_ring_h, p->zc_ring_n);
    return true;
}

// Append unpack-shader work to the OPEN command list. Caller must:
//   - Ensure zc_out_default is in UAV (or COMMON) state and writes have
//     finished (we issue a UAV barrier here as a defensive ordering point).
//   - Have set the descriptor heap + root signature for the slot UAV.
// Transitions zc_out_default to NON_PIXEL_SHADER_RESOURCE for the dispatch
// and back to UAV; transitions slot tex from COMMON to UAV and back to
// COMMON. Caller closes/executes the list and signals the cross-fence.
static void unpack_shader_record(struct priv *p, struct rife_zc_slot *s)
{
    // zc_out_default UAV->NON_PIXEL_SHADER_RESOURCE; slot COMMON->UAV.
    D3D12_RESOURCE_BARRIER bs[2] = {
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_out_default,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = s->d3d12_tex,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                          .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);

    ID3D12DescriptorHeap *heaps[] = { p->zc_unpack_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(p->zc_cmd_list, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(p->zc_cmd_list,
            p->zc_unpack_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list,
            p->zc_unpack_pso);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    p->zc_unpack_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
            p->zc_unpack_heap, &gpu_base);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu_base;
    uav_gpu.ptr += (UINT64)p->zc_unpack_heap_stride * (UINT64)(s->idx + 1);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list,
            0, srv_gpu);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list,
            1, uav_gpu);

    UINT params[8] = {0};
    params[0] = (UINT)p->pad_w;
    params[1] = (UINT)p->pad_h;
    params[2] = (UINT)p->proc_w;
    params[3] = (UINT)p->proc_h;
    params[4] = (UINT)p->orig_w;
    params[5] = (UINT)p->orig_h;
    float fxs = (p->orig_w > 1) ? (float)(p->proc_w - 1) / (float)(p->orig_w - 1) : 0.0f;
    float fys = (p->orig_h > 1) ? (float)(p->proc_h - 1) / (float)(p->orig_h - 1) : 0.0f;
    memcpy(&params[6], &fxs, sizeof(float));
    memcpy(&params[7], &fys, sizeof(float));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(p->zc_cmd_list,
            2, 8, params, 0);

    UINT gx = ((UINT)p->orig_w + 7u) / 8u;
    UINT gy = ((UINT)p->orig_h + 7u) / 8u;
    ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);

    // Restore states for next frame.
    bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);
}

// ---------------------------------------------------------------------------
// Step 5c.3: GPU cur-copy compute pipeline (R8G8B8A8 zc_pack_in_tex_cur ->
// B8G8R8A8 ring slot). Built lazily when zc-out ML mode comes up. Reuses
// the unpack root signature (same descriptor table layout).
// ---------------------------------------------------------------------------

static void copy_shader_release(struct priv *p)
{
    if (p->zc_copy_pso)  { ID3D12PipelineState_Release(p->zc_copy_pso); p->zc_copy_pso = NULL; }
    if (p->zc_copy_heap) { ID3D12DescriptorHeap_Release(p->zc_copy_heap); p->zc_copy_heap = NULL; }
    p->use_copy_shader = false;
}

static bool copy_shader_init(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!p->zc_d3d12 || !p->zc_unpack_root_sig || !p->zc_ring ||
        !p->zc_pack_in_tex_cur)
    {
        MP_WARN(vf, "RIFE copy-shader: prerequisites missing\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {
        .pRootSignature = p->zc_unpack_root_sig,
        .CS = { .pShaderBytecode = cs_copy_rgba_to_bgra_dxil,
                .BytecodeLength  = cs_copy_rgba_to_bgra_dxil_size },
    };
    HRESULT hr = ID3D12Device_CreateComputePipelineState(p->zc_d3d12, &psd,
            &IID_ID3D12PipelineState, (void **)&p->zc_copy_pso);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE copy-shader: CreateComputePipelineState hr=0x%08lx\n",
               (unsigned long)hr);
        copy_shader_release(p);
        return false;
    }

    UINT n_descs = 1u + (UINT)p->zc_ring_n;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = n_descs,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_copy_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE copy-shader: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        copy_shader_release(p);
        return false;
    }
    p->zc_copy_heap_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    p->zc_copy_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_copy_heap, &cpu_base);

    // [0] SRV Texture2D over zc_pack_in_tex_cur (R8G8B8A8_UNORM).
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1,
                       .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12,
            p->zc_pack_in_tex_cur, &sv, cpu_base);

    // [1..N] UAV per slot (B8G8R8A8_UNORM Texture2D).
    for (int i = 0; i < p->zc_ring_n; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpu_base;
        cpu.ptr += (size_t)(i + 1) * p->zc_copy_heap_stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
        };
        ID3D12Device_CreateUnorderedAccessView(p->zc_d3d12,
                p->zc_ring[i].d3d12_tex, NULL, &uv, cpu);
    }

    p->use_copy_shader = true;
    MP_INFO(vf, "RIFE copy-shader: ready (cur-source %dx%d RGBA -> BGRA, "
                "ring=%d)\n", p->orig_w, p->orig_h, p->zc_ring_n);
    return true;
}

// Append a cur-copy dispatch to the OPEN command list. tex_cur must be in
// COPY_DEST state on entry (which is the post-pack-shader state). Slot
// must be in COMMON. We transition both, dispatch, restore.
static void copy_shader_record(struct priv *p, struct rife_zc_slot *s)
{
    D3D12_RESOURCE_BARRIER bs[2] = {
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_pack_in_tex_cur,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = s->d3d12_tex,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                          .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);

    ID3D12DescriptorHeap *heaps[] = { p->zc_copy_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(p->zc_cmd_list, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(p->zc_cmd_list,
            p->zc_unpack_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list,
            p->zc_copy_pso);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    p->zc_copy_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
            p->zc_copy_heap, &gpu_base);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu_base;
    uav_gpu.ptr += (UINT64)p->zc_copy_heap_stride * (UINT64)(s->idx + 1);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list,
            0, srv_gpu);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list,
            1, uav_gpu);

    UINT params[8] = {0};
    params[0] = (UINT)p->orig_w;
    params[1] = (UINT)p->orig_h;
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(p->zc_cmd_list,
            2, 8, params, 0);

    UINT gx = ((UINT)p->orig_w + 7u) / 8u;
    UINT gy = ((UINT)p->orig_h + 7u) / 8u;
    ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);

    // Restore: tex_cur back to COPY_DEST, slot back to COMMON.
    bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    bs[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);
}

// ---------------------------------------------------------------------------
// Step 5c.4: NV12 direct-import input pipeline. Replaces HW-download +
// swscale + UPLOAD-buffer staging with a per-frame D3D11 CopySubresource
// onto a shared NV12 staging texture, a cross-API fence sync, and a
// D3D12 NV12->RGBA compute dispatch that fills the existing
// zc_pack_in_tex_prev/cur. The downstream cs_pack_rgb0 / cs_copy_rgba_to_bgra
// then reuse those textures unchanged.
// ---------------------------------------------------------------------------

static void nv12_input_release(struct priv *p)
{
    for (int i = 0; i < 2; i++) {
        SAFE_RELEASE(p->nv12_stage_d12[i]);
        if (p->nv12_stage_d11[i]) {
            ID3D11Texture2D_Release(p->nv12_stage_d11[i]);
            p->nv12_stage_d11[i] = NULL;
        }
        if (p->nv12_share_handle[i]) {
            CloseHandle(p->nv12_share_handle[i]);
            p->nv12_share_handle[i] = NULL;
        }
    }
    SAFE_RELEASE(p->zc_n2r_pso);
    SAFE_RELEASE(p->zc_n2r_root_sig);
    SAFE_RELEASE(p->zc_n2r_heap);
    p->zc_n2r_heap_stride = 0;
    p->zc_n2r_slot_descs = 0;
    p->use_nv12_input = false;
    p->nv12_w = p->nv12_h = 0;
    p->nv12_subfmt = 0;
}

// Build the cs_nv12_to_rgba root signature: t0 SRV table, t1 SRV table,
// u0 UAV table, b0 root constants (20 dwords).
static bool nv12_input_make_root_sig(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!g_ort.d3d12_serialize_root_signature) {
        MP_WARN(vf, "RIFE NV12-input: D3D12SerializeRootSignature missing\n");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE r_y = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE r_uv = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 1, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE r_uav = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_ROOT_PARAMETER params[4] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_y },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_uv },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_uav },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
          .Constants = { .ShaderRegister = 0, .RegisterSpace = 0,
                         .Num32BitValues = 20 },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC desc = { .NumParameters = 4, .pParameters = params };

    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = g_ort.d3d12_serialize_root_signature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE NV12-input: SerializeRootSignature hr=0x%08lx %s\n",
               (unsigned long)hr,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return false;
    }
    if (err) ID3D10Blob_Release(err);

    hr = ID3D12Device_CreateRootSignature(p->zc_d3d12, 0,
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&p->zc_n2r_root_sig);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE NV12-input: CreateRootSignature hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    return true;
}

static bool nv12_input_init(struct mp_filter *vf, int W, int H, int subfmt)
{
    struct priv *p = vf->priv;
    if (subfmt != IMGFMT_NV12 && subfmt != IMGFMT_P010) {
        MP_WARN(vf, "RIFE NV12-input: unsupported subfmt %s\n",
                mp_imgfmt_to_name(subfmt));
        return false;
    }
    if (p->use_nv12_input && p->nv12_w == W && p->nv12_h == H &&
        p->nv12_subfmt == subfmt)
        return true;
    if (p->use_nv12_input) {
        // size or subfmt change — rebuild
        nv12_input_release(p);
    }
    if (!p->zc_d3d12 || !p->d3d11_dev || !p->zc_d3d11_dev5 ||
        !p->zc_pack_in_tex_prev || !p->zc_pack_in_tex_cur)
    {
        MP_WARN(vf, "RIFE NV12-input: prerequisites missing\n");
        return false;
    }
    if ((W & 1) || (H & 1)) {
        MP_WARN(vf, "RIFE NV12-input: NV12/P010 needs even dims (got %dx%d)\n",
                W, H);
        return false;
    }

    HRESULT hr;
    DXGI_FORMAT tex_fmt = (subfmt == IMGFMT_P010) ? DXGI_FORMAT_P010
                                                   : DXGI_FORMAT_NV12;
    DXGI_FORMAT y_fmt   = (subfmt == IMGFMT_P010) ? DXGI_FORMAT_R16_UNORM
                                                   : DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT uv_fmt  = (subfmt == IMGFMT_P010) ? DXGI_FORMAT_R16G16_UNORM
                                                   : DXGI_FORMAT_R8G8_UNORM;
    const char *fmt_name = (subfmt == IMGFMT_P010) ? "P010" : "NV12";

    MP_INFO(vf, "RIFE NV12-input: init begin %dx%d %s\n", W, H, fmt_name);

    // 1. Allocate two shareable NV12/P010 staging textures on the upstream
    //    d3d11 device. SHADER_RESOURCE bind so D3D12 SRV creation succeeds
    //    after OpenSharedResource1; SHARED + SHARED_NTHANDLE for cross-API
    //    import.
    D3D11_TEXTURE2D_DESC td = {
        .Width = (UINT)W, .Height = (UINT)H,
        .MipLevels = 1, .ArraySize = 1,
        .Format = tex_fmt,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = 0,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                     D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };
    for (int i = 0; i < 2; i++) {
        MP_INFO(vf, "RIFE NV12-input: [%d] CreateTexture2D...\n", i);
        hr = ID3D11Device_CreateTexture2D(p->d3d11_dev, &td, NULL,
                                          &p->nv12_stage_d11[i]);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE NV12-input: CreateTexture2D[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
        MP_INFO(vf, "RIFE NV12-input: [%d] QI IDXGIResource1...\n", i);
        IDXGIResource1 *dxr = NULL;
        hr = ID3D11Texture2D_QueryInterface(p->nv12_stage_d11[i],
                &IID_IDXGIResource1, (void **)&dxr);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE NV12-input: QI IDXGIResource1[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
        MP_INFO(vf, "RIFE NV12-input: [%d] CreateSharedHandle...\n", i);
        hr = dxr->lpVtbl->CreateSharedHandle(dxr, NULL,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                NULL, &p->nv12_share_handle[i]);
        IDXGIResource1_Release(dxr);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE NV12-input: CreateSharedHandle[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
        MP_INFO(vf, "RIFE NV12-input: [%d] OpenSharedHandle (D3D12)...\n", i);
        hr = ID3D12Device_OpenSharedHandle(p->zc_d3d12, p->nv12_share_handle[i],
                &IID_ID3D12Resource, (void **)&p->nv12_stage_d12[i]);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE NV12-input: OpenSharedHandle[%d] hr=0x%08lx\n",
                   i, (unsigned long)hr);
            goto fail;
        }
        MP_INFO(vf, "RIFE NV12-input: [%d] OK\n", i);
    }

    MP_INFO(vf, "RIFE NV12-input: make_root_sig...\n");
    // 2. Build root sig + PSO + descriptor heap.
    if (!nv12_input_make_root_sig(vf))
        goto fail;
    MP_INFO(vf, "RIFE NV12-input: CreateComputePipelineState...\n");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {
        .pRootSignature = p->zc_n2r_root_sig,
        .CS = { .pShaderBytecode = cs_nv12_to_rgba_dxil,
                .BytecodeLength  = cs_nv12_to_rgba_dxil_size },
    };
    hr = ID3D12Device_CreateComputePipelineState(p->zc_d3d12, &psd,
            &IID_ID3D12PipelineState, (void **)&p->zc_n2r_pso);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE NV12-input: CreateComputePipelineState hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    MP_INFO(vf, "RIFE NV12-input: PSO ok; CreateDescriptorHeap...\n");

    // Heap: 2 slots × 3 descriptors each (Y SRV, UV SRV, RGBA UAV).
    p->zc_n2r_slot_descs = 3;
    UINT n_descs = (UINT)p->zc_n2r_slot_descs * 2u;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = n_descs,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_n2r_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE NV12-input: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    p->zc_n2r_heap_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    p->zc_n2r_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_n2r_heap, &cpu_base);

    for (int i = 0; i < 2; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_base;
        h.ptr += (size_t)i * p->zc_n2r_slot_descs * p->zc_n2r_heap_stride;

        D3D12_SHADER_RESOURCE_VIEW_DESC y_srv = {
            .Format = y_fmt,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1,
                           .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
        };
        ID3D12Device_CreateShaderResourceView(p->zc_d3d12,
                p->nv12_stage_d12[i], &y_srv, h);
        h.ptr += p->zc_n2r_heap_stride;

        D3D12_SHADER_RESOURCE_VIEW_DESC uv_srv = {
            .Format = uv_fmt,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1,
                           .PlaneSlice = 1, .ResourceMinLODClamp = 0.0f },
        };
        ID3D12Device_CreateShaderResourceView(p->zc_d3d12,
                p->nv12_stage_d12[i], &uv_srv, h);
        h.ptr += p->zc_n2r_heap_stride;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uv_uav = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
        };
        ID3D12Resource *uav_tex = (i == 0) ? p->zc_pack_in_tex_prev
                                           : p->zc_pack_in_tex_cur;
        ID3D12Device_CreateUnorderedAccessView(p->zc_d3d12, uav_tex,
                NULL, &uv_uav, h);
    }

    p->use_nv12_input = true;
    p->nv12_w = W;
    p->nv12_h = H;
    p->nv12_subfmt = subfmt;
    MP_INFO(vf, "RIFE NV12-input: ready (%dx%d shared %s staging x2 "
                "+ cs_nv12_to_rgba)\n", W, H, fmt_name);
    return true;

fail:
    nv12_input_release(p);
    return false;
}

// Compute YUV->RGB matrix + offsets for the given mp_image params. Falls
// back to BT.709 limited range if the params are unset/unknown.
static void nv12_compute_yuv_matrix(struct mp_image *src, float m[12])
{
    // Default: BT.709 limited (most modern HD content).
    enum pl_color_system sys = src->params.repr.sys;
    enum pl_color_levels lvl = src->params.repr.levels;
    if (sys == PL_COLOR_SYSTEM_UNKNOWN)
        sys = PL_COLOR_SYSTEM_BT_709;
    if (lvl == PL_COLOR_LEVELS_UNKNOWN)
        lvl = PL_COLOR_LEVELS_LIMITED;

    // Coefficients (Kr, Kb).
    float Kr, Kb;
    switch (sys) {
    case PL_COLOR_SYSTEM_BT_601:
        Kr = 0.299f; Kb = 0.114f; break;
    case PL_COLOR_SYSTEM_BT_2020_NC:
    case PL_COLOR_SYSTEM_BT_2020_C:
        Kr = 0.2627f; Kb = 0.0593f; break;
    case PL_COLOR_SYSTEM_BT_709:
    default:
        Kr = 0.2126f; Kb = 0.0722f; break;
    }
    float Kg = 1.0f - Kr - Kb;

    // Range scaling. Limited: Y in [16/255, 235/255], C in [16/255, 240/255],
    // both with chroma centered on 128/255. Full: Y in [0, 1], C in [0, 1]
    // with chroma centered on 128/255 (NV12 stores UV as unsigned).
    float yo, co, ys, cs;
    if (lvl == PL_COLOR_LEVELS_FULL) {
        yo = 0.0f;     ys = 1.0f;
        co = 128.0f / 255.0f; cs = 1.0f;
    } else {
        yo = 16.0f / 255.0f;
        ys = 255.0f / 219.0f;
        co = 128.0f / 255.0f;
        cs = 255.0f / 224.0f;
    }

    // Standard YUV->RGB:
    //   R = ys*(Y - yo) + 2*(1-Kr)*cs*(V - co)
    //   G = ys*(Y - yo) - 2*Kb*(1-Kb)/Kg*cs*(U - co)
    //                   - 2*Kr*(1-Kr)/Kg*cs*(V - co)
    //   B = ys*(Y - yo) + 2*(1-Kb)*cs*(U - co)
    float r_v = 2.0f * (1.0f - Kr);
    float b_u = 2.0f * (1.0f - Kb);
    float g_u = -2.0f * Kb * (1.0f - Kb) / Kg;
    float g_v = -2.0f * Kr * (1.0f - Kr) / Kg;

    // Shader expects: rgb = M * (yuv - off). Our yuv is (Y, U, V).
    m[0] = ys;        m[1] = 0.0f;      m[2] = r_v * cs;  m[3] = 0.0f; // r row
    m[4] = ys;        m[5] = g_u * cs;  m[6] = g_v * cs;  m[7] = 0.0f; // g row
    m[8] = ys;        m[9] = b_u * cs;  m[10]= 0.0f;      m[11]= 0.0f; // b row
    // off: written separately below by caller
    (void)yo; (void)co;
}

// Append nv12_to_rgba dispatches for both prev (slot 0) and cur (slot 1)
// to the OPEN command list. Caller must have already set the cmd list to
// WRITE state; we don't open or close it. zc_pack_in_tex_* are assumed to
// be in COPY_DEST state on entry (the post-pack-shader steady-state) and
// we restore them to COPY_DEST on exit so the existing pack_shader path
// (with upload=NULL) works without further state churn.
static void nv12_input_record_dispatch(struct priv *p, struct mp_image *cur)
{
    D3D12_RESOURCE_BARRIER bs_in[2] = {
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->nv12_stage_d12[0],
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->nv12_stage_d12[1],
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs_in);

    D3D12_RESOURCE_BARRIER bs_out[2] = {
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_pack_in_tex_prev,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                          .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_pack_in_tex_cur,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                          .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs_out);

    ID3D12DescriptorHeap *heaps[] = { p->zc_n2r_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(p->zc_cmd_list, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(p->zc_cmd_list,
            p->zc_n2r_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list, p->zc_n2r_pso);

    float m[12];
    nv12_compute_yuv_matrix(cur, m);

    UINT params[20] = {0};
    params[0] = (UINT)p->orig_w;
    params[1] = (UINT)p->orig_h;
    params[2] = (UINT)(p->orig_w / 2);
    params[3] = (UINT)(p->orig_h / 2);
    memcpy(&params[4],  &m[0],  sizeof(float) * 4); // m00 m01 m02 pad0
    memcpy(&params[8],  &m[4],  sizeof(float) * 4); // m10 m11 m12 pad1
    memcpy(&params[12], &m[8],  sizeof(float) * 4); // m20 m21 m22 pad2
    enum pl_color_levels lvl = cur->params.repr.levels;
    float yo = (lvl == PL_COLOR_LEVELS_FULL) ? 0.0f : 16.0f / 255.0f;
    float co = 128.0f / 255.0f;
    float zerof = 0.0f;
    memcpy(&params[16], &yo, sizeof(float));
    memcpy(&params[17], &co, sizeof(float));
    memcpy(&params[18], &co, sizeof(float));
    memcpy(&params[19], &zerof, sizeof(float));

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    p->zc_n2r_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
            p->zc_n2r_heap, &gpu_base);

    UINT gx = ((UINT)p->orig_w + 7u) / 8u;
    UINT gy = ((UINT)p->orig_h + 7u) / 8u;

    for (int i = 0; i < 2; i++) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpu_base;
        h.ptr += (UINT64)i * p->zc_n2r_slot_descs * p->zc_n2r_heap_stride;
        D3D12_GPU_DESCRIPTOR_HANDLE y_h  = h;
        D3D12_GPU_DESCRIPTOR_HANDLE uv_h = h;
        uv_h.ptr += p->zc_n2r_heap_stride;
        D3D12_GPU_DESCRIPTOR_HANDLE u_h  = h;
        u_h.ptr  += (UINT64)2 * p->zc_n2r_heap_stride;

        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
                p->zc_cmd_list, 0, y_h);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
                p->zc_cmd_list, 1, uv_h);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
                p->zc_cmd_list, 2, u_h);
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(
                p->zc_cmd_list, 3, 20, params, 0);
        ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);
    }

    // Back to steady-state: COPY_DEST for the RGBA inputs (so the existing
    // pack_shader_record_one transitions COPY_DEST -> NPSR -> COPY_DEST work
    // unchanged with upload=NULL), and COMMON for the imported NV12 staging
    // resources (so D3D11 can re-write them next frame).
    bs_out[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs_out[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    bs_out[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bs_out[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs_out);

    bs_in[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs_in[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    bs_in[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs_in[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs_in);
}

// ---------------------------------------------------------------------------
// Frame-diff: scene-change & static-frame detection.
//
// Computes a normalised mean absolute luma difference between
// zc_pack_in_tex_prev and zc_pack_in_tex_cur via cs_frame_diff.hlsl, then
// reads the result back to the CPU. The shader writes a 16-byte UAV buffer:
//   bytes  0..7 = uint64 SAD (BT.709 luma) in 16.16 fixed point
//   bytes  8..15= uint64 sum of (Yp+Yc)/2  in 16.16 fixed point
// ratio = SAD / sumLuma is content-normalised and roughly invariant to
// overall brightness. Decision:
//   ratio < static_threshold  -> static  (skip RIFE; emit cur as mid)
//   ratio > scene_threshold   -> scene   (skip RIFE; emit cur as mid)
//   otherwise                  -> normal (run RIFE)
// ---------------------------------------------------------------------------

static void frame_diff_release(struct priv *p)
{
    if (p->zc_diff_readback && p->zc_diff_readback_ptr) {
        ID3D12Resource_Unmap(p->zc_diff_readback, 0, NULL);
        p->zc_diff_readback_ptr = NULL;
    }
    SAFE_RELEASE(p->zc_diff_readback);
    SAFE_RELEASE(p->zc_diff_uav_buf);
    SAFE_RELEASE(p->zc_diff_heap);
    SAFE_RELEASE(p->zc_diff_pso);
    SAFE_RELEASE(p->zc_diff_root_sig);
    SAFE_RELEASE(p->zc_diff_fence);
    if (p->zc_diff_event) {
        CloseHandle(p->zc_diff_event);
        p->zc_diff_event = NULL;
    }
    p->zc_diff_heap_stride = 0;
    p->zc_diff_fence_value = 0;
    p->use_frame_diff = false;
}

static bool frame_diff_make_root_sig(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!g_ort.d3d12_serialize_root_signature)
        return false;

    // t0 prev SRV, t1 cur SRV, u0 UAV byte address buffer, b0 4 dwords.
    D3D12_DESCRIPTOR_RANGE r_p = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE r_c = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1, .BaseShaderRegister = 1, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE r_u = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1, .BaseShaderRegister = 0, .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_ROOT_PARAMETER params[4] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_p },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_c },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
          .DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &r_u },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
          .Constants = { .ShaderRegister = 0, .RegisterSpace = 0,
                         .Num32BitValues = 4 },
          .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC desc = { .NumParameters = 4, .pParameters = params };

    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = g_ort.d3d12_serialize_root_signature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: SerializeRootSignature hr=0x%08lx %s\n",
               (unsigned long)hr,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return false;
    }
    if (err) ID3D10Blob_Release(err);

    hr = ID3D12Device_CreateRootSignature(p->zc_d3d12, 0,
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&p->zc_diff_root_sig);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateRootSignature hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    return true;
}

static bool frame_diff_init(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (p->use_frame_diff)
        return true;
    if (!p->zc_d3d12 || !p->zc_pack_in_tex_prev || !p->zc_pack_in_tex_cur) {
        MP_WARN(vf, "RIFE frame-diff: prerequisites missing\n");
        return false;
    }
    HRESULT hr;
    if (!frame_diff_make_root_sig(vf))
        goto fail;

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {
        .pRootSignature = p->zc_diff_root_sig,
        .CS = { .pShaderBytecode = cs_frame_diff_dxil,
                .BytecodeLength  = cs_frame_diff_dxil_size },
    };
    hr = ID3D12Device_CreateComputePipelineState(p->zc_d3d12, &psd,
            &IID_ID3D12PipelineState, (void **)&p->zc_diff_pso);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateComputePipelineState hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 3,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_diff_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    p->zc_diff_heap_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 16-byte UAV buffer (default heap, raw) and a matching readback buffer.
    D3D12_HEAP_PROPERTIES hp_def = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES hp_rb  = { .Type = D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rd = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = 16, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &hp_def,
            D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void **)&p->zc_diff_uav_buf);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateCommittedResource(UAV) hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &hp_rb,
            D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&p->zc_diff_readback);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateCommittedResource(RB) hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    D3D12_RANGE no_read = { 0, 0 };
    hr = ID3D12Resource_Map(p->zc_diff_readback, 0, &no_read,
                            &p->zc_diff_readback_ptr);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: Map readback hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }

    // Populate descriptors.
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    p->zc_diff_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(p->zc_diff_heap, &h);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1,
                       .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12, p->zc_pack_in_tex_prev,
            &srv, h);
    h.ptr += p->zc_diff_heap_stride;
    ID3D12Device_CreateShaderResourceView(p->zc_d3d12, p->zc_pack_in_tex_cur,
            &srv, h);
    h.ptr += p->zc_diff_heap_stride;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = { .FirstElement = 0, .NumElements = 4,
                    .StructureByteStride = 0, .CounterOffsetInBytes = 0,
                    .Flags = D3D12_BUFFER_UAV_FLAG_RAW },
    };
    ID3D12Device_CreateUnorderedAccessView(p->zc_d3d12, p->zc_diff_uav_buf,
            NULL, &uav, h);

    hr = ID3D12Device_CreateFence(p->zc_d3d12, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&p->zc_diff_fence);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE frame-diff: CreateFence hr=0x%08lx\n",
               (unsigned long)hr);
        goto fail;
    }
    p->zc_diff_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->zc_diff_event) {
        MP_ERR(vf, "RIFE frame-diff: CreateEvent failed\n");
        goto fail;
    }

    p->use_frame_diff = true;
    MP_INFO(vf, "RIFE frame-diff: ready (%dx%d, static_thr=%.4f scene_thr=%.4f)\n",
            p->orig_w, p->orig_h, p->opts->static_threshold,
            p->opts->scene_threshold);
    return true;

fail:
    frame_diff_release(p);
    return false;
}

// Append cs_frame_diff dispatch + UAV→readback copy onto the OPEN cmd list.
// Caller must guarantee zc_pack_in_tex_prev/cur are in COPY_DEST state on
// entry (the post-pack-shader steady-state). On exit they are still in
// COPY_DEST (we transition NPSR -> COPY_DEST).
static void frame_diff_record_dispatch(struct priv *p)
{
    // Reset UAV buffer to zero. Easiest: copy from a zero source... or just
    // use a UAV clear. ClearUnorderedAccessViewUint requires both CPU and
    // GPU descriptor handles for the same resource in a SHADER_VISIBLE heap.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h;
    p->zc_diff_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_diff_heap, &cpu_h);
    cpu_h.ptr += (size_t)2 * p->zc_diff_heap_stride;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_h;
    p->zc_diff_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
            p->zc_diff_heap, &gpu_h);
    gpu_h.ptr += (UINT64)2 * p->zc_diff_heap_stride;

    ID3D12DescriptorHeap *heaps[] = { p->zc_diff_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(p->zc_cmd_list, 1, heaps);
    UINT zero[4] = {0, 0, 0, 0};
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(p->zc_cmd_list,
            gpu_h, cpu_h, p->zc_diff_uav_buf, zero, 0, NULL);

    // Transition pack_in_tex_* COPY_DEST -> NPSR for SRV reads.
    D3D12_RESOURCE_BARRIER bs[2] = {
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_pack_in_tex_prev,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Transition = { .pResource = p->zc_pack_in_tex_cur,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                          .StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);

    // Bind & dispatch.
    ID3D12GraphicsCommandList_SetComputeRootSignature(p->zc_cmd_list,
            p->zc_diff_root_sig);
    ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list, p->zc_diff_pso);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    p->zc_diff_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
            p->zc_diff_heap, &gpu_base);
    D3D12_GPU_DESCRIPTOR_HANDLE g_p = gpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE g_c = gpu_base; g_c.ptr += p->zc_diff_heap_stride;
    D3D12_GPU_DESCRIPTOR_HANDLE g_u = gpu_base; g_u.ptr += (UINT64)2 * p->zc_diff_heap_stride;
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list, 0, g_p);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list, 1, g_c);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(p->zc_cmd_list, 2, g_u);

    UINT params[4] = { (UINT)p->orig_w, (UINT)p->orig_h, 0, 0 };
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(p->zc_cmd_list,
            3, 4, params, 0);

    UINT gx = ((UINT)p->orig_w + 15u) / 16u;
    UINT gy = ((UINT)p->orig_h + 15u) / 16u;
    ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);

    // UAV barrier so subsequent copy sees writes.
    D3D12_RESOURCE_BARRIER uavb = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
        .UAV = { .pResource = p->zc_diff_uav_buf },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &uavb);

    // UAV -> COPY_SOURCE; copy 16 bytes to readback; back to UAV.
    D3D12_RESOURCE_BARRIER bcopy = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = { .pResource = p->zc_diff_uav_buf,
                        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &bcopy);
    ID3D12GraphicsCommandList_CopyBufferRegion(p->zc_cmd_list,
            p->zc_diff_readback, 0, p->zc_diff_uav_buf, 0, 16);
    bcopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bcopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &bcopy);

    // Restore pack_in_tex_* to COPY_DEST so the rest of the pipeline (or
    // next frame's pack_shader) finds them in the expected state.
    bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    bs[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bs[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs);
}

// Issue Signal on the diff fence and wait CPU-side. Returns the SAD/sumLuma
// ratio after readback. Returns -1.0f on error (treat as "normal").
static float frame_diff_signal_and_read(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!p->use_frame_diff)
        return -1.0f;
    uint64_t v = ++p->zc_diff_fence_value;
    if (FAILED(ID3D12CommandQueue_Signal(p->zc_queue, p->zc_diff_fence, v)))
        return -1.0f;
    if (ID3D12Fence_GetCompletedValue(p->zc_diff_fence) < v) {
        if (FAILED(ID3D12Fence_SetEventOnCompletion(p->zc_diff_fence, v,
                p->zc_diff_event)))
            return -1.0f;
        if (WaitForSingleObject(p->zc_diff_event, 1000) != WAIT_OBJECT_0) {
            MP_WARN(vf, "RIFE frame-diff: fence wait timed out\n");
            return -1.0f;
        }
    }
    // Readback: 2x uint64 in 16.16 fixed point.
    const volatile uint32_t *rb = (const volatile uint32_t *)p->zc_diff_readback_ptr;
    if (!rb)
        return -1.0f;
    uint64_t sad = (uint64_t)rb[0] | ((uint64_t)rb[1] << 32);
    uint64_t sum = (uint64_t)rb[2] | ((uint64_t)rb[3] << 32);
    if (sum == 0)
        return -1.0f;
    return (double)sad / (double)sum;
}

// Copy two upstream NV12 frames into our shared staging textures and signal
// the cross-API fence so the D3D12 queue can wait on the copies before
// dispatching cs_nv12_to_rgba. Returns the fence value to wait on (0 on
// failure).
static uint64_t nv12_input_copy_and_signal(struct mp_filter *vf,
                                           struct mp_image *prev,
                                           struct mp_image *cur)
{
    struct priv *p = vf->priv;
    if (!p->use_nv12_input)
        return 0;

    ID3D11Texture2D *prev_t = (ID3D11Texture2D *)prev->planes[0];
    UINT             prev_s = (UINT)(intptr_t)prev->planes[1];
    ID3D11Texture2D *cur_t  = (ID3D11Texture2D *)cur->planes[0];
    UINT             cur_s  = (UINT)(intptr_t)cur->planes[1];
    if (!prev_t || !cur_t) {
        MP_ERR(vf, "RIFE NV12-input: missing upstream textures\n");
        return 0;
    }

    ID3D11DeviceContext_CopySubresourceRegion(p->d3d11_ctx,
            (ID3D11Resource *)p->nv12_stage_d11[0], 0, 0, 0, 0,
            (ID3D11Resource *)prev_t, prev_s, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(p->d3d11_ctx,
            (ID3D11Resource *)p->nv12_stage_d11[1], 0, 0, 0, 0,
            (ID3D11Resource *)cur_t, cur_s, NULL);

    uint64_t v = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
    HRESULT hr = ID3D11DeviceContext4_Signal(p->zc_d3d11_ctx4,
            p->zc_xfence_d3d11, v);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE NV12-input: D3D11 Signal hr=0x%08lx\n",
               (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_Flush(p->d3d11_ctx);
    return v;
}

// Stand-alone helper for the first frame (and post-seek), where we want
// to project cur into a ring slot but no ML run is happening. Resets the
// command list, uploads cur into tex_cur, dispatches copy_shader, signals
// the cross-fence and arms a D3D11 Wait so downstream sees a complete
// BGRA slot. Returns true on success.
static bool cur_only_to_slot(struct mp_filter *vf,
                             struct mp_image *cur,
                             struct rife_zc_slot *slot,
                             uint64_t *out_xfence_v)
{
    struct priv *p = vf->priv;
    if (!p->use_copy_shader || !p->zc_d3d12 || !p->zc_pack_in_tex_cur ||
        !p->zc_pack_upload_cur || !p->zc_pack_upload_cur_ptr || !slot)
    {
        return false;
    }

    pack_shader_stage(p->zc_pack_upload_cur_ptr,
                      p->zc_pack_upload_row_pitch,
                      cur->planes[0], cur->stride[0],
                      p->orig_w, p->orig_h);

    HRESULT hr = ID3D12CommandAllocator_Reset(p->zc_cmd_alloc);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE cur-only: alloc reset hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = ID3D12GraphicsCommandList_Reset(p->zc_cmd_list, p->zc_cmd_alloc, NULL);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE cur-only: cmdlist reset hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION src = {
        .pResource = p->zc_pack_upload_cur,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {
            .Offset = 0,
            .Footprint = {
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .Width = p->orig_w, .Height = p->orig_h, .Depth = 1,
                .RowPitch = (UINT)p->zc_pack_upload_row_pitch,
            },
        },
    };
    D3D12_TEXTURE_COPY_LOCATION dst = {
        .pResource = p->zc_pack_in_tex_cur,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(p->zc_cmd_list, &dst,
            0, 0, 0, &src, NULL);

    copy_shader_record(p, slot);

    hr = ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE cur-only: cmdlist close hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)p->zc_cmd_list };
    ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists);

    uint64_t v = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
    hr = ID3D12CommandQueue_Signal(p->zc_queue, p->zc_xfence_d3d12, v);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE cur-only: queue signal hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = ID3D11DeviceContext4_Wait(p->zc_d3d11_ctx4, p->zc_xfence_d3d11, v);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE cur-only: D3D11 Wait hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    if (out_xfence_v) *out_xfence_v = v;
    return true;
}

// ---------------------------------------------------------------------------
// Step 5c.1: cross-API output ring (D3D12-allocated shared BGRA textures
// opened on mpv's D3D11 device + a shared D3D12 fence).
// ---------------------------------------------------------------------------

#define ZC_RING_N 8

static void zc_out_release(struct priv *p)
{
    // The ring backs live mp_images via custom AVBufferRef destructors that
    // dereference p->zc_ring. We MUST NOT free the ring while any slot is
    // still in_flight; the filter destroy path drains pending images before
    // calling release_session, so by here all slots should be free. Assert
    // and leak gracefully if not.
    if (p->zc_ring) {
        for (int i = 0; i < p->zc_ring_n; i++) {
            struct rife_zc_slot *s = &p->zc_ring[i];
            if (atomic_load(&s->in_flight)) {
                // A downstream consumer still holds a ref. Detach the slot
                // from the ring (set owner to NULL) so the destructor knows
                // not to touch ring memory; let the OS reclaim the textures
                // when refcount drops. This prevents a UAF but does leak.
                s->owner = NULL;
                continue;
            }
            if (s->d3d11_tex) {
                ID3D11Texture2D_Release(s->d3d11_tex);
                s->d3d11_tex = NULL;
            }
            if (s->d3d12_tex) {
                ID3D12Resource_Release(s->d3d12_tex);
                s->d3d12_tex = NULL;
            }
        }
        // Only free if no slot leaked. (talloc would simplify; use plain
        // free for symmetry with the rest of the file.)
        bool any_leaked = false;
        for (int i = 0; i < p->zc_ring_n; i++)
            if (p->zc_ring[i].owner == NULL) any_leaked = true;
        if (!any_leaked) {
            free(p->zc_ring);
            p->zc_ring = NULL;
        }
    }
    p->zc_ring_n = 0;
    p->zc_ring_w = p->zc_ring_h = 0;

    SAFE_RELEASE(p->zc_rtv_heap);
    if (p->zc_d3d11_ctx4) {
        ID3D11DeviceContext4_Release(p->zc_d3d11_ctx4);
        p->zc_d3d11_ctx4 = NULL;
    }
    if (p->zc_d3d11_dev5) {
        ID3D11Device5_Release(p->zc_d3d11_dev5);
        p->zc_d3d11_dev5 = NULL;
    }
    if (p->zc_xfence_d3d11) {
        ID3D11Fence_Release(p->zc_xfence_d3d11);
        p->zc_xfence_d3d11 = NULL;
    }
    SAFE_RELEASE(p->zc_xfence_d3d12);
    if (p->zc_xfence_handle) {
        CloseHandle(p->zc_xfence_handle);
        p->zc_xfence_handle = NULL;
    }
    atomic_store(&p->zc_xfence_value, 0);
    p->use_zc_out = false;
}

// Validate that mpv's D3D11 device (provided by the d3d11va hwdec) is on the
// same physical adapter as our D3D12 device. Cross-API shared NT handles
// require an exact LUID match.
static bool zc_check_luid(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    if (!p->d3d11_dev || !p->zc_d3d12) {
        MP_WARN(vf, "RIFE zc-out: D3D11 device or D3D12 device missing\n");
        return false;
    }

    // D3D12 adapter LUID (call via vtable to avoid the WIDL aggregate-return
    // wrapper macro that doesn't expand without WIDL_C_INLINE_WRAPPERS).
    LUID l12;
    p->zc_d3d12->lpVtbl->GetAdapterLuid(p->zc_d3d12, &l12);

    // D3D11 adapter LUID via DXGI parent.
    IDXGIDevice  *dxgi_dev = NULL;
    IDXGIAdapter *dxgi_ad  = NULL;
    DXGI_ADAPTER_DESC desc = {0};
    if (FAILED(ID3D11Device_QueryInterface(p->d3d11_dev, &IID_IDXGIDevice,
                                           (void **)&dxgi_dev))) {
        MP_WARN(vf, "RIFE zc-out: D3D11 QueryInterface(IDXGIDevice) failed\n");
        return false;
    }
    if (FAILED(IDXGIDevice_GetAdapter(dxgi_dev, &dxgi_ad))) {
        IDXGIDevice_Release(dxgi_dev);
        return false;
    }
    IDXGIAdapter_GetDesc(dxgi_ad, &desc);
    IDXGIAdapter_Release(dxgi_ad);
    IDXGIDevice_Release(dxgi_dev);

    p->zc_d3d12_luid = l12;
    p->zc_d3d11_luid = desc.AdapterLuid;
    bool match = (l12.LowPart == desc.AdapterLuid.LowPart &&
                  l12.HighPart == desc.AdapterLuid.HighPart);
    MP_INFO(vf, "RIFE zc-out: LUID d3d12=%08lx:%08lx d3d11=%08lx:%08lx %s\n",
            (unsigned long)l12.HighPart, (unsigned long)l12.LowPart,
            (unsigned long)desc.AdapterLuid.HighPart,
            (unsigned long)desc.AdapterLuid.LowPart,
            match ? "[match]" : "[MISMATCH -> falling back]");
    return match;
}

static bool zc_out_init(struct mp_filter *vf, int w, int h)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    if (!p->zc_d3d12) {
        // Bring up the D3D12 device + queue + cmd list + internal fence
        // that the 5b path also uses. We don't need ML/ORT inference for
        // the 5c.1 placeholder, but we DO need ort_load() to populate the
        // d3d12.dll / dxgi.dll function pointers that d3d12_init reads
        // from g_ort.
        if (!g_ort.d3d12_create_device && !ort_load(vf->log)) {
            MP_WARN(vf, "RIFE zc-out: ort_load failed (need d3d12.dll "
                        "function pointers via g_ort)\n");
            return false;
        }
        if (!d3d12_init(vf)) {
            MP_WARN(vf, "RIFE zc-out: d3d12_init failed\n");
            return false;
        }
    }
    if (!p->d3d11_dev) {
        MP_WARN(vf, "RIFE zc-out: D3D11 device not available "
                    "(needs zerocopy-smoke=yes / hwdec init)\n");
        return false;
    }
    if (!zc_check_luid(vf))
        return false;

    // Upgrade D3D11 device + context to v5/v4 (Win10 1703+ for shared fences).
    if (FAILED(ID3D11Device_QueryInterface(p->d3d11_dev, &IID_ID3D11Device5,
                                           (void **)&p->zc_d3d11_dev5))) {
        MP_WARN(vf, "RIFE zc-out: ID3D11Device5 not available\n");
        return false;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(p->d3d11_ctx,
            &IID_ID3D11DeviceContext4, (void **)&p->zc_d3d11_ctx4))) {
        MP_WARN(vf, "RIFE zc-out: ID3D11DeviceContext4 not available\n");
        return false;
    }

    // Create shared D3D12 fence and open on D3D11.
    hr = ID3D12Device_CreateFence(p->zc_d3d12, 0, D3D12_FENCE_FLAG_SHARED,
            &IID_ID3D12Fence, (void **)&p->zc_xfence_d3d12);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: CreateFence(SHARED) hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    hr = ID3D12Device_CreateSharedHandle(p->zc_d3d12,
            (ID3D12DeviceChild *)p->zc_xfence_d3d12, NULL, GENERIC_ALL,
            NULL, &p->zc_xfence_handle);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: CreateSharedHandle(fence) hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    hr = ID3D11Device5_OpenSharedFence(p->zc_d3d11_dev5, p->zc_xfence_handle,
            &IID_ID3D11Fence, (void **)&p->zc_xfence_d3d11);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: OpenSharedFence hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    // Both APIs hold their refs now; the NT handle can be closed.
    CloseHandle(p->zc_xfence_handle);
    p->zc_xfence_handle = NULL;
    atomic_store(&p->zc_xfence_value, 0);

    // Allocate ring + a small RTV descriptor heap (one descriptor per slot).
    p->zc_ring_n = ZC_RING_N;
    p->zc_ring_w = w;
    p->zc_ring_h = h;
    p->zc_ring = calloc(p->zc_ring_n, sizeof(*p->zc_ring));
    if (!p->zc_ring)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = p->zc_ring_n,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(p->zc_d3d12, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&p->zc_rtv_heap);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: CreateDescriptorHeap hr=0x%08lx\n",
               (unsigned long)hr);
        return false;
    }
    p->zc_rtv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
            p->zc_d3d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_base;
    p->zc_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
            p->zc_rtv_heap, &rtv_base);

    D3D12_HEAP_PROPERTIES default_heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width  = w,
        .Height = h,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
    };

    for (int i = 0; i < p->zc_ring_n; i++) {
        struct rife_zc_slot *s = &p->zc_ring[i];
        s->idx = i;
        s->owner = p;
        atomic_store(&s->in_flight, 0);

        hr = ID3D12Device_CreateCommittedResource(p->zc_d3d12, &default_heap,
                D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                NULL, &IID_ID3D12Resource, (void **)&s->d3d12_tex);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE zc-out: slot %d CreateCommittedResource "
                       "hr=0x%08lx\n", i, (unsigned long)hr);
            return false;
        }

        HANDLE tex_handle = NULL;
        hr = ID3D12Device_CreateSharedHandle(p->zc_d3d12,
                (ID3D12DeviceChild *)s->d3d12_tex, NULL, GENERIC_ALL, NULL,
                &tex_handle);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE zc-out: slot %d CreateSharedHandle(tex) "
                       "hr=0x%08lx\n", i, (unsigned long)hr);
            return false;
        }
        hr = ID3D11Device5_OpenSharedResource1(p->zc_d3d11_dev5, tex_handle,
                &IID_ID3D11Texture2D, (void **)&s->d3d11_tex);
        CloseHandle(tex_handle);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE zc-out: slot %d OpenSharedResource1 "
                       "hr=0x%08lx\n", i, (unsigned long)hr);
            return false;
        }

        s->rtv_cpu.ptr = rtv_base.ptr + (size_t)i * p->zc_rtv_stride;
        D3D12_RENDER_TARGET_VIEW_DESC rvd = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
        };
        ID3D12Device_CreateRenderTargetView(p->zc_d3d12, s->d3d12_tex,
                &rvd, s->rtv_cpu);
    }

    p->use_zc_out = true;
    MP_INFO(vf, "RIFE zc-out: shared ring ready (%d slots, %dx%d BGRA)\n",
            p->zc_ring_n, w, h);
    return true;
}

static struct rife_zc_slot *zc_out_acquire(struct priv *p)
{
    if (!p->zc_ring)
        return NULL;
    for (int i = 0; i < p->zc_ring_n; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&p->zc_ring[i].in_flight,
                                           &expected, 1))
            return &p->zc_ring[i];
    }
    return NULL;  // ring exhausted; caller falls back
}

static void zc_slot_release(void *opaque)
{
    struct rife_zc_slot *s = opaque;
    if (!s)
        return;
    // owner==NULL means the filter was destroyed while we held a ref. The
    // slot's textures are still alive (no other release path); clean them up
    // ourselves and let `s` itself live until... actually `s` is part of a
    // calloc'd array that was leaked in zc_out_release if any slot was still
    // in flight, so just release the textures here.
    if (!s->owner) {
        if (s->d3d11_tex) ID3D11Texture2D_Release(s->d3d11_tex);
        if (s->d3d12_tex) ID3D12Resource_Release(s->d3d12_tex);
        s->d3d11_tex = NULL;
        s->d3d12_tex = NULL;
    }
    atomic_store(&s->in_flight, 0);
}

// Per-frame placeholder: D3D12 ClearRTV to dark blue, signal fence, D3D11
// Wait. Returns the new fence value the slot was tagged with (so the caller
// can later assert), or 0 on failure.
static uint64_t zc_out_paint_placeholder(struct mp_filter *vf,
                                         struct rife_zc_slot *s)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    ID3D12CommandAllocator_Reset(p->zc_cmd_alloc);
    ID3D12GraphicsCommandList_Reset(p->zc_cmd_list, p->zc_cmd_alloc, NULL);

    D3D12_RESOURCE_BARRIER b = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = s->d3d12_tex,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COMMON,
            .StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);

    const float dark_blue[4] = { 0.05f, 0.05f, 0.30f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(p->zc_cmd_list,
            s->rtv_cpu, dark_blue, 0, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);

    ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)p->zc_cmd_list };
    ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists);

    uint64_t v = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
    hr = ID3D12CommandQueue_Signal(p->zc_queue, p->zc_xfence_d3d12, v);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: queue signal hr=0x%08lx\n",
               (unsigned long)hr);
        return 0;
    }
    // Make the next D3D11 use of this texture wait until our signal lands.
    hr = ID3D11DeviceContext4_Wait(p->zc_d3d11_ctx4, p->zc_xfence_d3d11, v);
    if (FAILED(hr)) {
        MP_ERR(vf, "RIFE zc-out: D3D11 Wait hr=0x%08lx\n",
               (unsigned long)hr);
        return 0;
    }
    return v;
}

// Promote a freshly-filled ring slot into an emit-ready mp_image. The slot's
// shared D3D11 texture has SHARED+SHARED_NTHANDLE+SIMULTANEOUS_ACCESS misc
// flags; that combination prevents ra_d3d11 (vo_gpu_next side) from creating
// a SRV on it (E_INVALIDARG). To stay compatible with the d3d11va hwdec
// mapper, we copy the slot into a normal mp_image_pool BGRA texture (which
// has plain BindFlags=RT|SR, no shared-misc) and emit that. The ring slot is
// recycled immediately after queuing the GPU copy. The cross-fence Wait()
// issued by run_inference() guarantees D3D11 will execute the copy AFTER
// the D3D12 producer is done; the natural ring depth (8) provides slack so
// D3D12 does not reuse the slot before D3D11 has consumed it.
static struct mp_image *zc_out_make_mpi(struct mp_filter *vf,
                                        struct rife_zc_slot *s,
                                        struct mp_image *src)
{
    struct priv *p = vf->priv;

    if (!p->hw_pool) {
        zc_slot_release(s);
        return NULL;
    }

    AVFrame *avf = av_frame_alloc();
    if (!avf) {
        zc_slot_release(s);
        return NULL;
    }
    if (av_hwframe_get_buffer(p->hw_pool, avf, 0) < 0) {
        MP_ERR(vf, "RIFE zc-out: hw_pool get_buffer failed\n");
        av_frame_free(&avf);
        zc_slot_release(s);
        return NULL;
    }
    struct mp_image *out = mp_image_from_av_frame(avf);
    av_frame_free(&avf);
    if (!out) {
        zc_slot_release(s);
        return NULL;
    }
    mp_image_set_size(out, p->zc_ring_w, p->zc_ring_h);

    // Issue the GPU-side copy on the immediate context. The earlier
    // ID3D11DeviceContext4_Wait() (in run_inference's signal step) ensures
    // the slot's contents are visible before this copy executes.
    ID3D11Texture2D *dst_tex = (ID3D11Texture2D *)out->planes[0];
    UINT             dst_sub = (UINT)(intptr_t)out->planes[1];

    D3D11_BOX box = {
        .left = 0, .top = 0, .front = 0,
        .right = p->zc_ring_w, .bottom = p->zc_ring_h, .back = 1,
    };
    ID3D11DeviceContext_CopySubresourceRegion(p->d3d11_ctx,
            (ID3D11Resource *)dst_tex, dst_sub, 0, 0, 0,
            (ID3D11Resource *)s->d3d11_tex, 0, &box);

    // Slot is logically consumed (queued in D3D11 cmd stream). Free it for
    // the next round; ring depth + FIFO ordering keep this safe.
    zc_slot_release(s);

    mp_image_copy_attributes(out, src);
    out->params.hw_subfmt = IMGFMT_BGRA;
    // Output is BGRA; we don't preserve HDR through the 8-bit SDR pipeline,
    // so strip both the YUV-flavoured bits AND the HDR primaries/transfer
    // metadata. Otherwise downstream vo_gpu_next would treat our SDR-range
    // pixels as PQ-encoded HDR and apply an inverse PQ EOTF, producing
    // heavy colour shifts (notably green casts on Dolby Vision content).
    out->params.repr.sys = PL_COLOR_SYSTEM_RGB;
    out->params.repr.levels = PL_COLOR_LEVELS_FULL;
    out->params.repr.alpha = PL_ALPHA_NONE;
    out->params.chroma_location = PL_CHROMA_UNKNOWN;
    out->params.color.primaries = PL_COLOR_PRIM_BT_709;
    out->params.color.transfer = PL_COLOR_TRC_BT_1886;
    out->params.color.hdr = (struct pl_hdr_metadata){0};
    out->params.light = MP_CSP_LIGHT_DISPLAY;
    mp_image_params_guess_csp(&out->params);
    return out;
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

static void release_session(struct priv *p)
{
    if (!g_ort.api) {
        d3d12_release(p);
        return;
    }
    const OrtApi *api = g_ort.api;
    if (p->zc_io_binding) { api->ReleaseIoBinding(p->zc_io_binding); p->zc_io_binding = NULL; }
    if (p->in_tensor)    api->ReleaseValue(p->in_tensor);
    if (p->out_tensor)   api->ReleaseValue(p->out_tensor);
    if (p->zc_in_tensor) { api->ReleaseValue(p->zc_in_tensor); p->zc_in_tensor = NULL; }
    if (p->zc_out_tensor){ api->ReleaseValue(p->zc_out_tensor); p->zc_out_tensor = NULL; }
    if (p->mem_info)     api->ReleaseMemoryInfo(p->mem_info);
    if (p->zc_dml_mem_info) { api->ReleaseMemoryInfo(p->zc_dml_mem_info); p->zc_dml_mem_info = NULL; }
    // OrtDmlApi::FreeGPUAllocation must run BEFORE ReleaseSession - the DML
    // execution provider owns the allocator bookkeeping; releasing the session
    // first leaves FreeGPUAllocation walking freed memory and produces a
    // recursive Release-chain crash on shutdown. d3d12_release() below still
    // calls FreeGPUAllocation as a safety net for the d3d12_init failure path
    // (where no session was ever created); the NULL-out makes it a no-op here.
    if (g_ort.dml_api) {
        if (p->zc_in_alloc)  { g_ort.dml_api->FreeGPUAllocation(p->zc_in_alloc);  p->zc_in_alloc  = NULL; }
        if (p->zc_out_alloc) { g_ort.dml_api->FreeGPUAllocation(p->zc_out_alloc); p->zc_out_alloc = NULL; }
    }
    if (p->session)      api->ReleaseSession(p->session);
    if (p->session_opts) api->ReleaseSessionOptions(p->session_opts);
    if (p->env)          api->ReleaseEnv(p->env);
    if (p->allocator) {
        if (p->input_name)  api->AllocatorFree(p->allocator, p->input_name);
        if (p->output_name) api->AllocatorFree(p->allocator, p->output_name);
    }
    p->in_tensor = p->out_tensor = NULL;
    p->session = NULL;
    p->session_opts = NULL;
    p->env = NULL;
    p->mem_info = NULL;
    p->input_name = p->output_name = NULL;
    p->allocator = NULL;
    free(p->in_buf);  p->in_buf  = NULL;
    free(p->out_buf); p->out_buf = NULL;
    d3d12_release(p);
}

static bool init_session(struct mp_filter *vf, int orig_w, int orig_h)
{
    struct priv *p = vf->priv;
    const OrtApi *api = g_ort.api;

    p->orig_w = orig_w;
    p->orig_h = orig_h;
    float scale = p->opts->scale;
    if (scale <= 0.0f || scale > 1.0f) scale = 1.0f;
    p->proc_w = (int)(orig_w * scale + 0.5f);
    p->proc_h = (int)(orig_h * scale + 0.5f);
    if (p->proc_w < 32) p->proc_w = 32;
    if (p->proc_h < 32) p->proc_h = 32;
    p->inv_scale = (float)orig_w / (float)p->proc_w;  // also ~ 1/scale
    p->pad_w  = (p->proc_w + 31) & ~31;
    p->pad_h  = (p->proc_h + 31) & ~31;
    p->channels = 11;

    OrtStatus *st;

    st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "mpv_rife", &p->env);
    if (!ort_check(vf, st, "CreateEnv")) goto fail;

    st = api->CreateSessionOptions(&p->session_opts);
    if (!ort_check(vf, st, "CreateSessionOptions")) goto fail;

    api->SetSessionGraphOptimizationLevel(p->session_opts, ORT_ENABLE_ALL);

    bool zc_wanted = p->opts->zerocopy && g_ort.dml_api;
    if (zc_wanted) {
        if (!d3d12_init(vf)) {
            MP_WARN(vf, "RIFE zc init failed; falling back to legacy DML EP\n");
            zc_wanted = false;
        }
    }

    if (zc_wanted) {
        // DML EP requires sequential execution + memory pattern disabled.
        api->SetSessionExecutionMode(p->session_opts, ORT_SEQUENTIAL);
        api->DisableMemPattern(p->session_opts);
        st = g_ort.dml_api->SessionOptionsAppendExecutionProvider_DML1(
                p->session_opts, p->zc_dml, p->zc_queue);
        if (!ort_check(vf, st, "AppendExecutionProvider_DML1")) goto fail;
    } else {
        if (!g_ort.append_dml) {
            MP_ERR(vf, "OrtSessionOptionsAppendExecutionProvider_DML not exported"
                       " by onnxruntime.dll; this build lacks DirectML.\n");
            goto fail;
        }
        st = g_ort.append_dml(p->session_opts, p->opts->gpu_id);
        if (!ort_check(vf, st, "AppendExecutionProvider_DML")) goto fail;

        // DML EP recommends disabling memory pattern + per-session arena.
        api->DisableMemPattern(p->session_opts);
    }

    wchar_t wpath[MAX_PATH] = {0};
    if (MultiByteToWideChar(CP_UTF8, 0, p->opts->model_path, -1,
                            wpath, MAX_PATH) == 0)
    {
        MP_ERR(vf, "model-path '%s' could not be converted to UTF-16\n",
               p->opts->model_path);
        goto fail;
    }
    st = api->CreateSession(p->env, wpath, p->session_opts, &p->session);
    if (!ort_check(vf, st, "CreateSession")) goto fail;

    st = api->GetAllocatorWithDefaultOptions(&p->allocator);
    if (!ort_check(vf, st, "GetAllocatorWithDefaultOptions")) goto fail;

    st = api->SessionGetInputName(p->session, 0, p->allocator, &p->input_name);
    if (!ort_check(vf, st, "SessionGetInputName")) goto fail;
    st = api->SessionGetOutputName(p->session, 0, p->allocator, &p->output_name);
    if (!ort_check(vf, st, "SessionGetOutputName")) goto fail;

    st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                  &p->mem_info);
    if (!ort_check(vf, st, "CreateCpuMemoryInfo")) goto fail;

    size_t in_n  = (size_t)1 * p->channels * p->pad_h * p->pad_w;
    size_t out_n = (size_t)1 *           3 * p->pad_h * p->pad_w;
    p->in_buf  = calloc(in_n,  sizeof(float));
    p->out_buf = calloc(out_n, sizeof(float));
    if (!p->in_buf || !p->out_buf) {
        MP_ERR(vf, "RIFE: failed to allocate %.1f MiB host buffers\n",
               (in_n + out_n) * sizeof(float) / (1024.0 * 1024.0));
        goto fail;
    }

    if (zc_wanted) {
        if (!d3d12_alloc_tensors(vf, in_n * sizeof(float),
                                 out_n * sizeof(float)))
            goto fail;
        st = api->CreateIoBinding(p->session, &p->zc_io_binding);
        if (!ort_check(vf, st, "CreateIoBinding")) goto fail;
        st = api->BindInput(p->zc_io_binding, p->input_name, p->zc_in_tensor);
        if (!ort_check(vf, st, "BindInput")) goto fail;
        st = api->BindOutput(p->zc_io_binding, p->output_name, p->zc_out_tensor);
        if (!ort_check(vf, st, "BindOutput")) goto fail;
        p->use_zc = true;
        MP_INFO(vf, "RIFE session ready (zerocopy/DML1): src=%dx%d proc=%dx%d "
                    "(padded %dx%d) scale=%.3f, gpu=%d\n",
                orig_w, orig_h, p->proc_w, p->proc_h, p->pad_w, p->pad_h,
                (float)p->proc_w / orig_w, p->opts->gpu_id);

        if (p->opts->pack_shader) {
            if (!pack_shader_init(vf)) {
                MP_WARN(vf, "RIFE pack-shader: init failed; "
                            "falling back to CPU pack path\n");
            }
        }
        // Note: unpack_shader_init() requires the zc-out ring (created on
        // the first IMGFMT_D3D11 frame), so it's deferred to that path.
    } else {
        int64_t in_shape[]  = {1, p->channels, p->pad_h, p->pad_w};
        int64_t out_shape[] = {1,           3, p->pad_h, p->pad_w};
        st = api->CreateTensorWithDataAsOrtValue(p->mem_info,
                p->in_buf,  in_n  * sizeof(float),
                in_shape,  4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &p->in_tensor);
        if (!ort_check(vf, st, "CreateTensor(in)")) goto fail;
        st = api->CreateTensorWithDataAsOrtValue(p->mem_info,
                p->out_buf, out_n * sizeof(float),
                out_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &p->out_tensor);
        if (!ort_check(vf, st, "CreateTensor(out)")) goto fail;

        MP_INFO(vf, "RIFE session ready: src=%dx%d proc=%dx%d (padded %dx%d) "
                    "scale=%.3f, DirectML gpu=%d\n",
                orig_w, orig_h, p->proc_w, p->proc_h, p->pad_w, p->pad_h,
                (float)p->proc_w / orig_w, p->opts->gpu_id);
    }
    return true;

fail:
    release_session(p);
    return false;
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

static bool run_inference(struct mp_filter *vf,
                          struct mp_image *prev, struct mp_image *cur,
                          float t, struct rife_zc_slot *out_slot,
                          struct rife_zc_slot *cur_slot,
                          uint64_t *out_xfence_v,
                          bool force_skip)
{
    struct priv *p = vf->priv;
    const OrtApi *api = g_ort.api;

    // Step 5c.4 / E: detect upstream D3D11 NV12 or P010 input. Lazy-init the
    // direct-import path on the first such frame; if init fails we silently
    // fall back to the CPU pack path (which requires an upstream RGB0
    // conversion).
    bool nv12_path = false;
    if (p->use_zc && p->use_pack_shader && p->opts->nv12_input &&
        prev->imgfmt == IMGFMT_D3D11 && cur->imgfmt == IMGFMT_D3D11 &&
        prev->params.hw_subfmt == cur->params.hw_subfmt &&
        (cur->params.hw_subfmt == IMGFMT_NV12 ||
         cur->params.hw_subfmt == IMGFMT_P010))
    {
        if (!p->use_nv12_input ||
            p->nv12_subfmt != cur->params.hw_subfmt)
        {
            (void)nv12_input_init(vf, p->orig_w, p->orig_h,
                                  cur->params.hw_subfmt);
        }
        nv12_path = p->use_nv12_input;
    }

    bool need_cpu_pack = (!p->use_pack_shader || p->opts->pack_shader_debug)
                         && !nv12_path;
    if (need_cpu_pack) {
        pack_rgb0(p, prev->planes[0], prev->stride[0], CH_R0);
        pack_rgb0(p, cur->planes[0],  cur->stride[0],  CH_R1);
        fill_meta_channels(p, t);
    }

    int64_t t0 = mp_time_ns();

    if (p->use_zc) {
        HRESULT hr;
        ID3D12CommandAllocator_Reset(p->zc_cmd_alloc);
        ID3D12GraphicsCommandList_Reset(p->zc_cmd_list, p->zc_cmd_alloc, NULL);

        D3D12_RESOURCE_BARRIER b = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = p->zc_in_default,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            },
        };

        if (p->use_pack_shader) {
            // Stage RGB0 into both upload buffers (CPU writes, no GPU work yet).
            // Skipped for NV12 input — the GPU NV12->RGBA dispatch fills the
            // RGBA inputs directly.
            if (!nv12_path) {
                pack_shader_stage(p->zc_pack_upload_prev_ptr,
                                  p->zc_pack_upload_row_pitch,
                                  prev->planes[0], prev->stride[0],
                                  p->orig_w, p->orig_h);
                pack_shader_stage(p->zc_pack_upload_cur_ptr,
                                  p->zc_pack_upload_row_pitch,
                                  cur->planes[0], cur->stride[0],
                                  p->orig_w, p->orig_h);
            }

            // zc_in_default COMMON -> UAV (shader writes).
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);

            // For NV12 input we must run cs_nv12_to_rgba BEFORE pack_shader,
            // because pack_shader_record_one transitions in_tex_*
            // COPY_DEST -> NPSR -> COPY_DEST and reads it as SRV. Our dispatch
            // ends with in_tex_* in COPY_DEST too, matching the expected entry
            // state.
            if (nv12_path)
                nv12_input_record_dispatch(p, cur);

            // Common compute setup.
            ID3D12DescriptorHeap *heaps[] = { p->zc_pack_heap };
            ID3D12GraphicsCommandList_SetDescriptorHeaps(p->zc_cmd_list,
                    1, heaps);
            ID3D12GraphicsCommandList_SetComputeRootSignature(p->zc_cmd_list,
                    p->zc_pack_root_sig);
            ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list,
                    p->zc_pack_pso);

            D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu;
            p->zc_pack_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                    p->zc_pack_heap, &heap_gpu);
            D3D12_GPU_DESCRIPTOR_HANDLE srv_prev_gpu = heap_gpu;
            D3D12_GPU_DESCRIPTOR_HANDLE srv_cur_gpu  = heap_gpu;
            srv_cur_gpu.ptr += p->zc_pack_heap_stride;
            D3D12_GPU_DESCRIPTOR_HANDLE uav_in_gpu   = heap_gpu;
            uav_in_gpu.ptr  += (UINT64)p->zc_pack_heap_stride * 2u;

            // prev: copy upload -> in_tex, dispatch base=0.
            pack_shader_record_one(p, p->zc_pack_in_tex_prev,
                    nv12_path ? NULL : p->zc_pack_upload_prev,
                    srv_prev_gpu, uav_in_gpu, CH_R0);
            // cur: copy upload -> in_tex, dispatch base=3.
            pack_shader_record_one(p, p->zc_pack_in_tex_cur,
                    nv12_path ? NULL : p->zc_pack_upload_cur,
                    srv_cur_gpu, uav_in_gpu, CH_R1);

            // meta: dispatch only (no SRV needed; UAV table is already set).
            ID3D12GraphicsCommandList_SetPipelineState(p->zc_cmd_list,
                    p->zc_meta_pso);
            UINT meta_params[8] = {0};
            meta_params[0] = (UINT)p->pad_w;
            meta_params[1] = (UINT)p->pad_h;
            memcpy(&meta_params[2], &t, sizeof(float));
            meta_params[3] = (UINT)CH_T;
            meta_params[4] = (UINT)CH_HG;
            meta_params[5] = (UINT)CH_VG;
            meta_params[6] = (UINT)CH_HSCL;
            meta_params[7] = (UINT)CH_VSCL;
            ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(
                    p->zc_cmd_list, 2, 8, meta_params, 0);
            UINT gx = ((UINT)p->pad_w + 7u) / 8u;
            UINT gy = ((UINT)p->pad_h + 7u) / 8u;
            ID3D12GraphicsCommandList_Dispatch(p->zc_cmd_list, gx, gy, 1);

            // Make the tensor writes visible to the ML dispatch (same UAV).
            D3D12_RESOURCE_BARRIER uavb = {
                .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                .UAV = { .pResource = p->zc_in_default },
            };
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &uavb);
        } else {
            // Step 5b CPU-pack path: memcpy host buffer -> upload heap, then
            // CopyBufferRegion -> default UAV.
            memcpy(p->zc_in_upload_ptr, p->in_buf, p->zc_in_bytes);

            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);
            ID3D12GraphicsCommandList_CopyBufferRegion(p->zc_cmd_list,
                    p->zc_in_default, 0, p->zc_in_upload, 0, p->zc_in_bytes);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);
        }

        // Optional readback of the GPU-packed input tensor for parity check.
        bool dbg_active = p->use_pack_shader && p->opts->pack_shader_debug &&
                          p->zc_pack_in_readback;
        if (dbg_active) {
            D3D12_RESOURCE_BARRIER bd = {
                .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                .Transition = {
                    .pResource = p->zc_in_default,
                    .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE,
                },
            };
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &bd);
            ID3D12GraphicsCommandList_CopyBufferRegion(p->zc_cmd_list,
                    p->zc_pack_in_readback, 0, p->zc_in_default, 0,
                    p->zc_in_bytes);
            bd.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bd.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &bd);
        }

        // ---- Frame-diff (scene/static detection) ----
        // Lazy-init when at least one threshold is positive AND we have the
        // shader-pack path active (which keeps zc_pack_in_tex_prev/cur as
        // RGBA8 we can read). The CPU-pack fallback doesn't have these.
        bool diff_dispatched = false;
        if (!force_skip && p->use_pack_shader &&
            (p->opts->static_threshold > 0.0f || p->opts->scene_threshold > 0.0f))
        {
            if (!p->use_frame_diff)
                (void)frame_diff_init(vf);
            if (p->use_frame_diff) {
                frame_diff_record_dispatch(p);
                diff_dispatched = true;
            }
        }

        b.Transition.pResource   = p->zc_out_default;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b);
        ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
        ID3D12CommandList *lists1[] = { (ID3D12CommandList *)p->zc_cmd_list };

        // For NV12 input: copy NV12 from upstream textures into our shared
        // staging slots on the upstream D3D11 device, signal the cross-API
        // fence, and queue a Wait on the D3D12 queue so the dispatch sees
        // completed copies. Failure here aborts this run; the next frame
        // can retry.
        if (nv12_path) {
            uint64_t v_in = nv12_input_copy_and_signal(vf, prev, cur);
            if (!v_in)
                return false;
            hr = ID3D12CommandQueue_Wait(p->zc_queue, p->zc_xfence_d3d12, v_in);
            if (FAILED(hr)) {
                MP_ERR(vf, "RIFE NV12-input: D3D12 Wait hr=0x%08lx\n",
                       (unsigned long)hr);
                return false;
            }
        }

        ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists1);

        // Resolve scene/static decision before deciding whether to run RIFE.
        // STATIC: prev ≈ cur, mid = cur (skip ML).
        // SCENE:  hard cut, mid = cur to avoid morphing.
        // Both: requires copy_shader + a destination slot to be useful;
        // otherwise fall through to the normal RIFE path.
        int diff_kind = 0;  // 0=normal 1=static 2=scene
        if (diff_dispatched) {
            float ratio = frame_diff_signal_and_read(vf);
            if (ratio >= 0.0f) {
                p->stats_last_diff_ratio = ratio;
                if (ratio < p->opts->static_threshold)
                    diff_kind = 1;
                else if (ratio > p->opts->scene_threshold)
                    diff_kind = 2;
                if (diff_kind != p->diff_last_kind) {
                    MP_VERBOSE(vf, "RIFE diff ratio=%.4f -> %s\n", ratio,
                               diff_kind == 1 ? "STATIC" :
                               diff_kind == 2 ? "SCENE"  : "normal");
                }
                p->diff_last_kind = diff_kind;
            }
        }

        bool skip_rife = ((diff_kind != 0) || force_skip) && out_slot &&
                         p->use_unpack_shader && p->use_copy_shader;
        if (skip_rife) {
            if (diff_kind == 1) p->stats_skipped_static++;
            else                p->stats_skipped_scene++;
        }

        if (skip_rife) {
            // Phase C (no-RIFE variant): copy cur into out_slot (and cur_slot
            // if requested). Restore zc_in_default and zc_out_default to
            // COMMON. Signal cross-fence so D3D11 side waits.
            //
            // CRITICAL: cmd_list1 (pack/meta/diff) was just submitted at line
            // ExecuteCommandLists above. Resetting the command allocator
            // while that submission is still in flight on the GPU is UB per
            // D3D12 spec and triggers TDR / device hang under sustained
            // back-pressure (observed at multiplier >= 4x). The NORMAL ML
            // path is only safe because OrtRun + SynchronizeBoundOutputs
            // implicitly drains the queue before its allocator reset; the
            // STATIC/SCENE path has no such drain. Force one here.
            {
                uint64_t v_drain = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
                hr = ID3D12CommandQueue_Signal(p->zc_queue,
                                               p->zc_xfence_d3d12, v_drain);
                if (SUCCEEDED(hr) &&
                    ID3D12Fence_GetCompletedValue(p->zc_xfence_d3d12) < v_drain)
                {
                    if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(
                            p->zc_xfence_d3d12, v_drain, p->zc_fence_event)))
                    {
                        WaitForSingleObject(p->zc_fence_event, 1000);
                    }
                }
            }
            ID3D12CommandAllocator_Reset(p->zc_cmd_alloc);
            ID3D12GraphicsCommandList_Reset(p->zc_cmd_list, p->zc_cmd_alloc, NULL);

            copy_shader_record(p, out_slot);
            if (cur_slot)
                copy_shader_record(p, cur_slot);

            D3D12_RESOURCE_BARRIER bs2[2] = {
                { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                  .Transition = {
                      .pResource = p->zc_out_default,
                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                      .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      .StateAfter  = D3D12_RESOURCE_STATE_COMMON } },
                { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                  .Transition = {
                      .pResource = p->zc_in_default,
                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                      .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      .StateAfter  = D3D12_RESOURCE_STATE_COMMON } },
            };
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs2);
            ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
            ID3D12CommandList *lists2[] = { (ID3D12CommandList *)p->zc_cmd_list };
            ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists2);

            uint64_t v = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
            hr = ID3D12CommandQueue_Signal(p->zc_queue, p->zc_xfence_d3d12, v);
            if (FAILED(hr)) {
                MP_ERR(vf, "RIFE diff-skip: queue signal hr=0x%08lx\n",
                       (unsigned long)hr);
                return false;
            }
            hr = ID3D11DeviceContext4_Wait(p->zc_d3d11_ctx4,
                                           p->zc_xfence_d3d11, v);
            if (FAILED(hr)) {
                MP_ERR(vf, "RIFE diff-skip: D3D11 Wait hr=0x%08lx\n",
                       (unsigned long)hr);
                return false;
            }
            if (out_xfence_v) *out_xfence_v = v;
            goto inference_done;
        }

        OrtStatus *st;
        st = api->SynchronizeBoundInputs(p->zc_io_binding);
        if (!ort_check(vf, st, "SynchronizeBoundInputs")) return false;
        st = api->RunWithBinding(p->session, NULL, p->zc_io_binding);
        if (!ort_check(vf, st, "RunWithBinding")) return false;
        st = api->SynchronizeBoundOutputs(p->zc_io_binding);
        if (!ort_check(vf, st, "SynchronizeBoundOutputs")) return false;

        ID3D12CommandAllocator_Reset(p->zc_cmd_alloc);
        ID3D12GraphicsCommandList_Reset(p->zc_cmd_list, p->zc_cmd_alloc, NULL);

        if (out_slot && p->use_unpack_shader) {
            // Step 5c.3 path: GPU-unpack ML output into the ring slot
            // texture, then signal the cross-fence so the D3D11 side waits.
            unpack_shader_record(p, out_slot);

            // Optionally project cur (RGB0 in zc_pack_in_tex_cur) onto a
            // separate ring slot so downstream sees a uniform IMGFMT_D3D11
            // BGRA stream from the same shared device.
            if (cur_slot && p->use_copy_shader)
                copy_shader_record(p, cur_slot);

            D3D12_RESOURCE_BARRIER bs2[2] = {
                { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                  .Transition = {
                      .pResource = p->zc_out_default,
                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                      .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      .StateAfter  = D3D12_RESOURCE_STATE_COMMON } },
                { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                  .Transition = {
                      .pResource = p->zc_in_default,
                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                      .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      .StateAfter  = D3D12_RESOURCE_STATE_COMMON } },
            };
            ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 2, bs2);
            ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
            ID3D12CommandList *lists2[] = { (ID3D12CommandList *)p->zc_cmd_list };
            ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists2);

            uint64_t v = atomic_fetch_add(&p->zc_xfence_value, 1) + 1;
            hr = ID3D12CommandQueue_Signal(p->zc_queue, p->zc_xfence_d3d12, v);
            if (FAILED(hr)) {
                MP_ERR(vf, "RIFE zc-out: queue signal hr=0x%08lx\n",
                       (unsigned long)hr);
                return false;
            }
            hr = ID3D11DeviceContext4_Wait(p->zc_d3d11_ctx4,
                                           p->zc_xfence_d3d11, v);
            if (FAILED(hr)) {
                MP_ERR(vf, "RIFE zc-out: D3D11 Wait hr=0x%08lx\n",
                       (unsigned long)hr);
                return false;
            }
            if (out_xfence_v) *out_xfence_v = v;
            // No CPU readback in this path; out_buf is stale.
            goto inference_done;
        }

        D3D12_RESOURCE_BARRIER b2 = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = p->zc_out_default,
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE,
            },
        };
        ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b2);
        ID3D12GraphicsCommandList_CopyBufferRegion(p->zc_cmd_list,
                p->zc_out_readback, 0, p->zc_out_default, 0, p->zc_out_bytes);
        // Return both default-heap buffers to COMMON for the next frame.
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b2.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b2);
        b2.Transition.pResource   = p->zc_in_default;
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b2.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        ID3D12GraphicsCommandList_ResourceBarrier(p->zc_cmd_list, 1, &b2);
        ID3D12GraphicsCommandList_Close(p->zc_cmd_list);
        ID3D12CommandList *lists2[] = { (ID3D12CommandList *)p->zc_cmd_list };
        ID3D12CommandQueue_ExecuteCommandLists(p->zc_queue, 1, lists2);

        p->zc_fence_value++;
        hr = ID3D12CommandQueue_Signal(p->zc_queue, p->zc_fence,
                                       p->zc_fence_value);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE zc: queue signal hr=0x%08lx\n",
                   (unsigned long)hr);
            return false;
        }
        if (ID3D12Fence_GetCompletedValue(p->zc_fence) < p->zc_fence_value) {
            ID3D12Fence_SetEventOnCompletion(p->zc_fence, p->zc_fence_value,
                                             p->zc_fence_event);
            WaitForSingleObject(p->zc_fence_event, INFINITE);
        }

        void *rb_ptr = NULL;
        D3D12_RANGE read_all = { 0, p->zc_out_bytes };
        hr = ID3D12Resource_Map(p->zc_out_readback, 0, &read_all, &rb_ptr);
        if (FAILED(hr)) {
            MP_ERR(vf, "RIFE zc: map readback hr=0x%08lx\n",
                   (unsigned long)hr);
            return false;
        }
        memcpy(p->out_buf, rb_ptr, p->zc_out_bytes);
        D3D12_RANGE no_write = { 0, 0 };
        ID3D12Resource_Unmap(p->zc_out_readback, 0, &no_write);

        // Step 5c.2a parity-debug: compare the GPU-packed input tensor with
        // the CPU-packed reference (filled at the top of run_inference). Log
        // worst-case per-channel diff for the first few frames.
        if (p->use_pack_shader && p->opts->pack_shader_debug &&
            p->zc_pack_in_readback)
        {
            void *rin = NULL;
            D3D12_RANGE rin_range = { 0, p->zc_in_bytes };
            HRESULT hr2 = ID3D12Resource_Map(p->zc_pack_in_readback, 0,
                                             &rin_range, &rin);
            if (SUCCEEDED(hr2)) {
                size_t plane = (size_t)p->pad_h * p->pad_w;
                const float *gpu = rin;
                const float *cpu = p->in_buf;
                float worst[16] = {0};
                size_t worst_pos[16] = {0};
                int nch = p->channels;
                if (nch > 16) nch = 16;
                for (int c = 0; c < nch; c++) {
                    for (size_t i = 0; i < plane; i++) {
                        size_t off = (size_t)c * plane + i;
                        float d = fabsf(gpu[off] - cpu[off]);
                        if (d > worst[c]) {
                            worst[c] = d;
                            worst_pos[c] = i;
                        }
                    }
                }
                D3D12_RANGE no_w = { 0, 0 };
                ID3D12Resource_Unmap(p->zc_pack_in_readback, 0, &no_w);
                if (p->zc_pack_dbg_frames < 5) {
                    MP_INFO(vf, "RIFE pack-shader parity #%d: "
                                "max|gpu-cpu| per ch (R0/G0/B0 R1/G1/B1 "
                                "T HG VG HSCL VSCL) = "
                                "%.6f %.6f %.6f %.6f %.6f %.6f "
                                "%.6f %.6f %.6f %.6f %.6f\n",
                            p->zc_pack_dbg_frames,
                            worst[0], worst[1], worst[2],
                            worst[3], worst[4], worst[5],
                            worst[6], worst[7], worst[8],
                            worst[9], worst[10]);
                    p->zc_pack_dbg_frames++;
                } else {
                    float wmax = 0;
                    for (int c = 0; c < nch; c++)
                        if (worst[c] > wmax) wmax = worst[c];
                    if (wmax > 1e-5f)
                        MP_WARN(vf, "RIFE pack-shader parity: "
                                    "unexpected diff %.6f\n", wmax);
                }
            }
        }
    } else {
        const char *in_names[]  = {p->input_name};
        const char *out_names[] = {p->output_name};
        OrtValue   *inputs[]    = {p->in_tensor};
        OrtValue   *outputs[]   = {p->out_tensor};
        OrtStatus *st = api->Run(p->session, NULL,
                                 in_names,  (const OrtValue *const *)inputs,  1,
                                 out_names, 1, outputs);
        if (!ort_check(vf, st, "Run"))
            return false;
    }
inference_done:;
    int64_t dt = mp_time_ns() - t0;
    p->stats_last_infer_ms = dt / 1.0e6;
    p->stats_total_pairs++;
    MP_TRACE(vf, "RIFE infer %dx%d t=%.2f: %.2f ms%s%s%s%s\n",
               p->pad_w, p->pad_h, t, dt / 1.0e6,
               p->use_zc ? " [zc]" : "",
               p->use_pack_shader ? " [cs-pack]" : "",
               (out_slot && p->use_unpack_shader) ? " [cs-unpack]" : "",
               (cur_slot && p->use_copy_shader) ? " [cs-copy]" : "");
    return true;
}

// ---------------------------------------------------------------------------
// Filter glue
// ---------------------------------------------------------------------------

static bool can_process(struct mp_image *img)
{
    if (img->imgfmt == IMGFMT_RGB0)
        return true;
    // Step 5c.4 / E: also accept native D3D11 NV12 and P010 hwframes; the
    // per-frame dispatcher decides whether to take the direct-import path or
    // fall back to the autoconvert/RGB0 path on a per-instance basis.
    if (img->imgfmt == IMGFMT_D3D11 &&
        (img->params.hw_subfmt == IMGFMT_NV12 ||
         img->params.hw_subfmt == IMGFMT_P010))
        return true;
    return false;
}

// Unpack the network output ([1,3,pad_h,pad_w] float RGB planar) into the
// RGB0 destination mp_image (orig_w x orig_h, packed 8-bit). When proc_w/h
// is smaller than orig_w/h (i.e. opts->scale < 1), do bilinear up-sampling
// from the proc-sized region of out_buf back to (orig_w, orig_h).
static void unpack_rgb0(struct priv *p, struct mp_image *dst)
{
    const int W = p->pad_w, H = p->pad_h;
    const int pW = p->proc_w, pH = p->proc_h;
    const int oW = dst->w, oH = dst->h;
    const float *rc = p->out_buf + (size_t)0 * H * W;
    const float *gc = p->out_buf + (size_t)1 * H * W;
    const float *bc = p->out_buf + (size_t)2 * H * W;

    // Map dst coord -> proc coord. Use a "no edge stretch" mapping where
    // the four corners of dst align with the four corners of proc:
    //   src_x = dst_x * (pW - 1) / (oW - 1)
    // This preserves geometry better than scale * dst_x at small ratios.
    const float fx_step = (oW > 1) ? (float)(pW - 1) / (float)(oW - 1) : 0.0f;
    const float fy_step = (oH > 1) ? (float)(pH - 1) / (float)(oH - 1) : 0.0f;

    for (int oy = 0; oy < oH; oy++) {
        float fy = oy * fy_step;
        int   y0 = (int)fy;
        int   y1 = y0 + 1;
        if (y1 > pH - 1) y1 = pH - 1;
        if (y0 > pH - 1) y0 = pH - 1;
        float wy = fy - (float)y0;
        const float wy0 = 1.0f - wy;
        size_t row0 = (size_t)y0 * W;
        size_t row1 = (size_t)y1 * W;

        uint8_t *row = dst->planes[0] + (ptrdiff_t)oy * dst->stride[0];
        for (int ox = 0; ox < oW; ox++) {
            float fx = ox * fx_step;
            int   x0 = (int)fx;
            int   x1 = x0 + 1;
            if (x1 > pW - 1) x1 = pW - 1;
            if (x0 > pW - 1) x0 = pW - 1;
            float wx = fx - (float)x0;
            const float wx0 = 1.0f - wx;

            #define SAMPLE(C) ( \
                (C[row0 + x0] * wx0 + C[row0 + x1] * wx) * wy0 + \
                (C[row1 + x0] * wx0 + C[row1 + x1] * wx) * wy )

            float rf = SAMPLE(rc) * 255.0f + 0.5f;
            float gf = SAMPLE(gc) * 255.0f + 0.5f;
            float bf = SAMPLE(bc) * 255.0f + 0.5f;
            #undef SAMPLE

            int r = rf < 0 ? 0 : (rf > 255 ? 255 : (int)rf);
            int g = gf < 0 ? 0 : (gf > 255 ? 255 : (int)gf);
            int b = bf < 0 ? 0 : (bf > 255 ? 255 : (int)bf);
            row[ox * 4 + 0] = (uint8_t)r;
            row[ox * 4 + 1] = (uint8_t)g;
            row[ox * 4 + 2] = (uint8_t)b;
            row[ox * 4 + 3] = 0;
        }
    }
}

// Build a new RGB0 mp_image holding the interpolated midpoint frame. The
// caller owns the returned ref. Returns NULL if allocation fails; callers
// should fall back to passing `cur` through without a midpoint.
static struct mp_image *build_midpoint(struct mp_filter *vf,
                                       struct mp_image *prev,
                                       struct mp_image *cur)
{
    struct priv *p = vf->priv;

    struct mp_image *mid = mp_image_alloc(IMGFMT_RGB0, cur->w, cur->h);
    if (!mid) {
        MP_ERR(vf, "RIFE: failed to allocate midpoint mp_image\n");
        return NULL;
    }

    // Inherit color metadata / params from cur, then override PTS.
    mp_image_copy_attributes(mid, cur);
    unpack_rgb0(p, mid);

    double mid_pts = cur->pts;
    if (prev->pts != MP_NOPTS_VALUE && cur->pts != MP_NOPTS_VALUE)
        mid_pts = (prev->pts + cur->pts) * 0.5;
    mid->pts = mid_pts;

    // pkt_duration of a "half" frame is half the gap between prev and cur.
    if (prev->pts != MP_NOPTS_VALUE && cur->pts != MP_NOPTS_VALUE) {
        double half = (cur->pts - prev->pts) * 0.5;
        if (half > 0) {
            mid->pkt_duration = half;
            // Split cur's reported duration so total stream duration matches.
            // cur itself will be emitted next turn; adjust its duration too.
            if (cur->pkt_duration > 0)
                cur->pkt_duration = half;
        }
    }

    return mid;
}

// ---------------------------------------------------------------------------
// Step 5a: zero-copy output plumbing prove-out.
//
// Lazily acquires the shared D3D11 device that mpv's d3d11va hwdec already
// created, allocates a hwframe pool of IMGFMT_D3D11+IMGFMT_BGRA textures,
// and emits cyan-cleared frames through the standard mp_image surface so
// downstream `vo_gpu_next` can sample them. Steady-state RIFE work (D3D12,
// DirectML IoBinding, compute shaders) is intentionally absent: the goal of
// 5a is purely to validate that the *output* mp_image format is acceptable
// to the VO before we invest in shaders.
// ---------------------------------------------------------------------------

static void zc_release(struct priv *p)
{
    av_buffer_unref(&p->hw_pool);
    av_buffer_unref(&p->av_device_ref);
    if (p->d3d11_ctx) {
        ID3D11DeviceContext_Release(p->d3d11_ctx);
        p->d3d11_ctx = NULL;
    }
    if (p->d3d11_dev) {
        ID3D11Device_Release(p->d3d11_dev);
        p->d3d11_dev = NULL;
    }
    p->zc_w = p->zc_h = 0;
}

static bool zc_init(struct mp_filter *vf, struct mp_image *src)
{
    struct priv *p = vf->priv;

    struct mp_stream_info *info = mp_filter_find_stream_info(vf);
    if (!info || !info->hwdec_devs) {
        MP_WARN(vf, "RIFE zc: no hwdec_devs available\n");
        return false;
    }

    struct hwdec_imgfmt_request req = {
        .imgfmt  = IMGFMT_D3D11,
        .probing = false,
    };
    hwdec_devices_request_for_img_fmt(info->hwdec_devs, &req);

    struct mp_hwdec_ctx *hwctx =
        hwdec_devices_get_by_imgfmt_and_type(info->hwdec_devs, IMGFMT_D3D11,
                                             AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwctx || !hwctx->av_device_ref) {
        MP_WARN(vf, "RIFE zc: no D3D11VA device context\n");
        return false;
    }

    p->av_device_ref = av_buffer_ref(hwctx->av_device_ref);
    AVHWDeviceContext     *ahw  = (void *)p->av_device_ref->data;
    AVD3D11VADeviceContext *d3d = ahw->hwctx;

    p->d3d11_dev = d3d->device;
    ID3D11Device_AddRef(p->d3d11_dev);
    ID3D11Device_GetImmediateContext(p->d3d11_dev, &p->d3d11_ctx);

    if (!mp_update_av_hw_frames_pool(&p->hw_pool, p->av_device_ref,
                                     IMGFMT_D3D11, IMGFMT_BGRA,
                                     src->w, src->h, false))
    {
        MP_WARN(vf, "RIFE zc: hw_pool create failed (%dx%d BGRA)\n",
                src->w, src->h);
        return false;
    }

    p->zc_w = src->w;
    p->zc_h = src->h;
    MP_INFO(vf, "RIFE zc: BGRA D3D11 hw_pool ready (%dx%d)\n",
            p->zc_w, p->zc_h);
    return true;
}

static struct mp_image *zc_smoke_alloc(struct mp_filter *vf,
                                       struct mp_image *src)
{
    struct priv *p = vf->priv;

    AVFrame *avf = av_frame_alloc();
    if (!avf)
        return NULL;
    if (av_hwframe_get_buffer(p->hw_pool, avf, 0) < 0) {
        MP_ERR(vf, "RIFE zc: hwframe_get_buffer failed\n");
        av_frame_free(&avf);
        return NULL;
    }

    struct mp_image *out = mp_image_from_av_frame(avf);
    av_frame_free(&avf);
    if (!out)
        return NULL;

    mp_image_set_size(out, p->zc_w, p->zc_h);
    mp_image_copy_attributes(out, src);
    // Source frame is YUV (NV12); output is BGRA. Strip the YUV-flavoured
    // colour metadata so vo_gpu_next doesn't run a YUV->RGB matrix on our
    // already-RGB pixels.
    out->params.hw_subfmt = IMGFMT_BGRA;
    out->params.repr.sys = PL_COLOR_SYSTEM_RGB;
    out->params.repr.levels = PL_COLOR_LEVELS_FULL;
    out->params.repr.alpha = PL_ALPHA_NONE;
    out->params.chroma_location = PL_CHROMA_UNKNOWN;
    out->params.color.primaries = PL_COLOR_PRIM_BT_709;
    out->params.color.transfer = PL_COLOR_TRC_BT_1886;
    out->params.color.hdr = (struct pl_hdr_metadata){0};
    out->params.light = MP_CSP_LIGHT_DISPLAY;
    mp_image_params_guess_csp(&out->params);

    // Smoke-test placeholder: clear texture to black. Real ML output goes
    // here in step 5f. Texture is created with D3D11_BIND_RENDER_TARGET via
    // mp_update_av_hw_frames_pool().
    ID3D11Texture2D *tex = (ID3D11Texture2D *)out->planes[0];
    UINT             sub = (UINT)(intptr_t)out->planes[1];

    if (!p->warned_multiplier) {
        MP_INFO(vf, "RIFE zc: BGRA D3D11 output ready (%dx%d)\n",
                p->zc_w, p->zc_h);
        p->warned_multiplier = true;
    }

    ID3D11RenderTargetView *rtv = NULL;
    D3D11_RENDER_TARGET_VIEW_DESC rdesc = {
        .Format        = DXGI_FORMAT_B8G8R8A8_UNORM,
        .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY,
        .Texture2DArray = {
            .MipSlice        = 0,
            .FirstArraySlice = sub,
            .ArraySize       = 1,
        },
    };
    HRESULT hr = ID3D11Device_CreateRenderTargetView(p->d3d11_dev,
        (ID3D11Resource *)tex, &rdesc, &rtv);
    if (SUCCEEDED(hr)) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ID3D11DeviceContext_ClearRenderTargetView(p->d3d11_ctx, rtv, black);
        ID3D11RenderTargetView_Release(rtv);
    }

    return out;
}

// ---- Pending output queue helpers (for Nx multiplier) ----

static void pending_q_clear(struct priv *p)
{
    for (int i = 0; i < RIFE_PENDING_Q_N; i++)
        mp_image_unrefp(&p->pending_q[i]);
    p->pending_q_head = p->pending_q_tail = p->pending_q_count = 0;
}

static bool pending_q_push(struct priv *p, struct mp_image *mpi)
{
    if (!mpi)
        return false;
    if (p->pending_q_count >= RIFE_PENDING_Q_N) {
        mp_image_unrefp(&mpi);
        return false;
    }
    p->pending_q[p->pending_q_tail] = mpi;
    p->pending_q_tail = (p->pending_q_tail + 1) % RIFE_PENDING_Q_N;
    p->pending_q_count++;
    return true;
}

static struct mp_image *pending_q_pop(struct priv *p)
{
    if (p->pending_q_count <= 0)
        return NULL;
    struct mp_image *mpi = p->pending_q[p->pending_q_head];
    p->pending_q[p->pending_q_head] = NULL;
    p->pending_q_head = (p->pending_q_head + 1) % RIFE_PENDING_Q_N;
    p->pending_q_count--;
    return mpi;
}

static void vf_rife_process(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    // No autoconvert subfilter (i.e. enabled=no at construction time):
    // act as a transparent pass-through filter.
    if (!p->conv) {
        if (mp_pin_can_transfer_data(vf->ppins[1], vf->ppins[0])) {
            struct mp_frame frame = mp_pin_out_read(vf->ppins[0]);
            mp_pin_in_write(vf->ppins[1], frame);
        }
        return;
    }

    // Runtime bypass: when enabled=no, skip the entire RIFE pipeline and
    // just forward frames. Drain any leftover state so we don't emit stale
    // mids or hold a stale prev when the user toggles back on.
    if (!p->opts->enabled) {
        if (p->prev) mp_image_unrefp(&p->prev);
        if (p->pending_q_count > 0) pending_q_clear(p);
        if (p->raw_pending) mp_image_unrefp(&p->raw_pending);
        if (p->raw_in_head != p->raw_in_tail) {
            for (int i = 0; i < RIFE_RAW_FIFO_N; i++)
                mp_image_unrefp(&p->raw_in_fifo[i]);
            p->raw_in_head = p->raw_in_tail = 0;
        }
        if (mp_pin_can_transfer_data(vf->ppins[1], vf->ppins[0])) {
            struct mp_frame frame = mp_pin_out_read(vf->ppins[0]);
            mp_pin_in_write(vf->ppins[1], frame);
        }
        return;
    }

    // 1) Pump raw input frames into the autoconvert subfilter so it can
    //    deliver IMGFMT_RGB0 on its output pin. When the 5c.3 ML zc-out
    //    mode might be active we also tee the raw mp_image into a FIFO so
    //    we can later emit the original (D3D11) frame downstream alongside
    //    the ring-slot midpoint without a redundant RGB0 copy.
    if (mp_pin_can_transfer_data(p->conv->f->pins[0], vf->ppins[0])) {
        struct mp_frame frame = mp_pin_out_read(vf->ppins[0]);
        if (frame.type == MP_FRAME_VIDEO) {
            struct mp_image *raw = frame.data;
            int next = (p->raw_in_tail + 1) % RIFE_RAW_FIFO_N;
            if (next != p->raw_in_head) {
                p->raw_in_fifo[p->raw_in_tail] = mp_image_new_ref(raw);
                p->raw_in_tail = next;
            } else {
                // FIFO full: advance head (drop oldest) so we stay in sync
                // with autoconvert output. This is a defensive fallback;
                // RIFE_RAW_FIFO_N=8 should comfortably exceed converter
                // latency.
                mp_image_unrefp(&p->raw_in_fifo[p->raw_in_head]);
                p->raw_in_head = (p->raw_in_head + 1) % RIFE_RAW_FIFO_N;
                p->raw_in_fifo[p->raw_in_tail] = mp_image_new_ref(raw);
                p->raw_in_tail = next;
            }
        } else {
            // EOF / signaling: flush the raw FIFO so we don't pair stale
            // frames with later RGB0 outputs.
            for (int i = 0; i < RIFE_RAW_FIFO_N; i++)
                mp_image_unrefp(&p->raw_in_fifo[i]);
            p->raw_in_head = p->raw_in_tail = 0;
        }
        mp_pin_in_write(p->conv->f->pins[0], frame);
    }

    // 2) Drain the queued midpoint(s) / cur before consuming new input.
    if (p->pending_q_count > 0) {
        if (!mp_pin_in_needs_data(vf->ppins[1]))
            return;
        struct mp_image *q = pending_q_pop(p);
        mp_pin_in_write(vf->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, q));
        if (p->pending_q_count > 0)
            mp_filter_internal_mark_progress(vf);
        return;
    }

    // 3) Pull a converted frame out of autoconvert and run the rife pipeline.
    if (!mp_pin_can_transfer_data(vf->ppins[1], p->conv->f->pins[1]))
        return;

    struct mp_frame frame = mp_pin_out_read(p->conv->f->pins[1]);
    if (frame.type == MP_FRAME_NONE)
        return;
    if (frame.type != MP_FRAME_VIDEO) {
        // EOF / audio / signalling: drop internal state and forward.
        mp_image_unrefp(&p->prev);
        pending_q_clear(p);
        mp_image_unrefp(&p->raw_pending);
        for (int i = 0; i < RIFE_RAW_FIFO_N; i++)
            mp_image_unrefp(&p->raw_in_fifo[i]);
        p->raw_in_head = p->raw_in_tail = 0;
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    // Pop the matching raw upstream frame from the FIFO. May be NULL if
    // upstream wasn't VIDEO when we pumped (defensive); we'll fall back to
    // emitting the RGB0 conversion in that case.
    struct mp_image *raw = NULL;
    if (p->raw_in_head != p->raw_in_tail) {
        raw = p->raw_in_fifo[p->raw_in_head];
        p->raw_in_fifo[p->raw_in_head] = NULL;
        p->raw_in_head = (p->raw_in_head + 1) % RIFE_RAW_FIFO_N;
    }

    struct mp_image *cur = frame.data;

    // ---- Step 5c.1 cross-API output ----
    // When --rife=enabled=yes:zerocopy-output=yes is set AND we receive a
    // D3D11 hwdec frame, allocate a BGRA texture from our D3D12-owned shared
    // ring, paint it dark blue from the D3D12 side, signal the shared fence
    // and have mpv's D3D11 context Wait() on it. If the player shows dark
    // blue tinted frames the entire D3D11<->D3D12 plumbing (LUID match, NT
    // shared handles, fence interop, ring slot recycling) is working. The
    // ML output integrates here in steps 5c.2/5c.3.
    if (p->opts->zerocopy_output && cur->imgfmt == IMGFMT_D3D11 &&
        !(p->opts->nv12_input && p->opts->zerocopy &&
          (cur->params.hw_subfmt == IMGFMT_NV12 ||
           cur->params.hw_subfmt == IMGFMT_P010))) {
        // Note: this branch fires for the legacy 5c.1 placeholder mode where
        // autoconvert was configured to pass IMGFMT_D3D11 through. With the
        // 5c.3 ML zc-out mode (zerocopy=yes && zerocopy_output=yes) we
        // intentionally route D3D11 through autoconvert -> RGB0 instead, so
        // cur is RGB0 here and this branch does not fire.
        mp_image_unrefp(&raw);
        if (!p->use_zc_out && !p->zc_init_failed) {
            // zc_out depends on the 5b D3D12/DML path (which init_session
            // brings up via zc_init->d3d12_init when zerocopy=yes). We also
            // need the d3d11 hw_pool init from zc_init for `p->d3d11_dev`.
            if (!p->zc_init_tried) {
                p->zc_init_tried = true;
                if (!zc_init(vf, cur)) {
                    p->zc_init_failed = true;
                }
            }
            if (!p->zc_init_failed) {
                if (!zc_out_init(vf, cur->w, cur->h)) {
                    MP_WARN(vf, "RIFE zc-out: init failed; "
                                "falling back to passthrough\n");
                    p->zc_init_failed = true;
                }
            }
        }
        if (p->use_zc_out) {
            if (cur->w != p->zc_ring_w || cur->h != p->zc_ring_h) {
                MP_WARN(vf, "RIFE zc-out: input size changed %dx%d -> %dx%d, "
                            "ring rebuild not yet implemented; passthrough\n",
                        p->zc_ring_w, p->zc_ring_h, cur->w, cur->h);
            } else {
                struct rife_zc_slot *s = zc_out_acquire(p);
                if (s) {
                    if (zc_out_paint_placeholder(vf, s) != 0) {
                        struct mp_image *out = zc_out_make_mpi(vf, s, cur);
                        if (out) {
                            mp_frame_unref(&frame);
                            mp_pin_in_write(vf->ppins[1],
                                            MAKE_FRAME(MP_FRAME_VIDEO, out));
                            return;
                        }
                    } else {
                        // Paint failed; release slot and fall through.
                        zc_slot_release(s);
                    }
                } else {
                    MP_TRACE(vf, "RIFE zc-out: ring exhausted, "
                                 "passthrough this frame\n");
                }
            }
        }
        // Failed: forward the original frame unchanged.
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    // ---- Step 5a smoke test ----
    // When --rife=enabled=yes:zerocopy-smoke=yes is set AND we receive a
    // D3D11 hwdec frame, replace it with a cyan-cleared BGRA D3D11 texture
    // allocated from our own hwframe pool. If the player shows a cyan tint,
    // the IMGFMT_D3D11+BGRA output plumbing works end-to-end (vo_gpu_next
    // accepts our self-allocated D3D11 texture).
    if (p->opts->zerocopy_smoke && cur->imgfmt == IMGFMT_D3D11) {
        mp_image_unrefp(&raw);
        if (!p->zc_init_tried) {
            p->zc_init_tried = true;
            if (!zc_init(vf, cur))
                p->zc_init_failed = true;
        }
        if (!p->zc_init_failed) {
            if (cur->w != p->zc_w || cur->h != p->zc_h) {
                // Reinit on size change.
                if (!mp_update_av_hw_frames_pool(&p->hw_pool, p->av_device_ref,
                                                 IMGFMT_D3D11, IMGFMT_BGRA,
                                                 cur->w, cur->h, false))
                {
                    MP_ERR(vf, "RIFE zc: pool resize to %dx%d failed\n",
                           cur->w, cur->h);
                    p->zc_init_failed = true;
                } else {
                    p->zc_w = cur->w;
                    p->zc_h = cur->h;
                }
            }
            struct mp_image *fake = p->zc_init_failed
                ? NULL : zc_smoke_alloc(vf, cur);
            if (fake) {
                mp_frame_unref(&frame);
                mp_pin_in_write(vf->ppins[1],
                                MAKE_FRAME(MP_FRAME_VIDEO, fake));
                return;
            }
        }
        // Failed: fall through to forwarding the original frame unchanged.
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    if (p->init_failed || !p->opts->model_path || !p->opts->model_path[0]) {
        if (!p->warned_format) {
            MP_WARN(vf, "RIFE inactive (init failed or no model-path); "
                        "passing frames through.\n");
            p->warned_format = true;
        }
        mp_image_unrefp(&raw);
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    // autoconvert is configured to deliver IMGFMT_RGB0; this is a defensive
    // sanity check that should normally never fire.
    if (!can_process(cur)) {
        if (!p->warned_format) {
            MP_WARN(vf, "RIFE: autoconvert delivered %s instead of rgb0; "
                        "passthrough.\n", mp_imgfmt_to_name(cur->imgfmt));
            p->warned_format = true;
        }
        mp_image_unrefp(&raw);
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    if (!p->tried_init) {
        p->tried_init = true;
        if (!ort_load(vf->log)) {
            p->init_failed = true;
        } else if (!init_session(vf, cur->w, cur->h)) {
            p->init_failed = true;
        }
        if (p->init_failed) {
            MP_WARN(vf, "RIFE initialisation failed; permanent passthrough.\n");
            mp_image_unrefp(&raw);
            mp_pin_in_write(vf->ppins[1], frame);
            return;
        }
    }

    if (cur->w != p->orig_w || cur->h != p->orig_h) {
        MP_WARN(vf, "RIFE: input size changed from %dx%d to %dx%d; "
                    "reinit not yet supported, falling back to passthrough.\n",
                p->orig_w, p->orig_h, cur->w, cur->h);
        mp_image_unrefp(&p->prev);
        mp_image_unrefp(&raw);
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    int N = p->opts->multiplier;
    if (N < 2) N = 2;
    if (N > RIFE_PENDING_Q_N) N = RIFE_PENDING_Q_N;

    // ---- Step 5c.3: decide zc-out ML mode on first eligible frame ----
    // Sticky for the lifetime of the session. Requires the user to have set
    // both zerocopy=yes and zerocopy_output=yes, the raw input to be a
    // D3D11 surface, and the zc-out ring + unpack-shader pipeline to come
    // up cleanly. If anything fails we fall back to the legacy RGB0 output.
    if (!p->zc_out_ml_decided) {
        p->zc_out_ml_decided = true;
        if (p->opts->zerocopy && p->opts->zerocopy_output && p->use_zc &&
            p->use_pack_shader && p->opts->unpack_shader &&
            raw && raw->imgfmt == IMGFMT_D3D11)
        {
            bool ok = true;
            if (!p->zc_init_tried) {
                p->zc_init_tried = true;
                if (!zc_init(vf, raw)) {
                    p->zc_init_failed = true;
                    ok = false;
                }
            }
            if (ok && !p->use_zc_out && !p->zc_init_failed) {
                if (!zc_out_init(vf, raw->w, raw->h)) {
                    MP_WARN(vf, "RIFE zc-out (ML): zc_out_init failed; "
                                "falling back to RGB0 output\n");
                    ok = false;
                }
            }
            if (ok && !unpack_shader_init(vf)) {
                MP_WARN(vf, "RIFE unpack-shader: init failed; "
                            "falling back to RGB0 output\n");
                ok = false;
            }
            if (ok && !copy_shader_init(vf)) {
                MP_WARN(vf, "RIFE copy-shader: init failed; "
                            "falling back to RGB0 output\n");
                ok = false;
            }
            if (ok) {
                p->use_zc_out_ml = true;
                MP_INFO(vf, "RIFE zc-out ML mode active: D3D11 in -> "
                            "D3D12/DML -> D3D11 BGRA out\n");
            }
        }
    }

    // First frame: no prev yet, cannot interpolate. We still need to emit
    // cur in a uniform format compatible with what subsequent frames will
    // produce; otherwise the downstream chain reconfigures every pair and
    // the autoconvert d3d11 path trips a hardware-context mismatch.
    if (!p->prev) {
        mp_image_setrefp(&p->prev, cur);
        if (p->use_zc_out_ml && p->use_copy_shader) {
            struct rife_zc_slot *slot = zc_out_acquire(p);
            if (slot) {
                uint64_t fv = 0;
                if (cur_only_to_slot(vf, cur, slot, &fv)) {
                    struct mp_image *out = zc_out_make_mpi(vf, slot,
                                                           raw ? raw : cur);
                    if (out) {
                        out->pts = cur->pts;
                        out->pkt_duration = cur->pkt_duration;
                        mp_image_unrefp(&raw);
                        mp_frame_unref(&frame);
                        mp_pin_in_write(vf->ppins[1],
                                MAKE_FRAME(MP_FRAME_VIDEO, out));
                        return;
                    }
                    zc_slot_release(slot);
                } else {
                    zc_slot_release(slot);
                }
            }
            // cur_only_to_slot path failed; fall through to RGB0 emit
            // (single one-time format flip at startup).
        }
        mp_image_unrefp(&raw);
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }

    // Run the midpoint inference using prev and cur.
    // Tick FPS counter once per source pair (whether or not RIFE actually
    // runs - STATIC/SCENE skips still emit `multiplier` output frames).
    {
        double now = mp_time_sec();
        if (p->fps_window_start == 0.0)
            p->fps_window_start = now;
        p->fps_window_count++;
        double dt = now - p->fps_window_start;
        if (dt >= 1.0) {
            p->fps_source_pps = (double)p->fps_window_count / dt;
            p->fps_window_count = 0;
            p->fps_window_start = now;
        }
    }
    struct mp_image *mid = NULL;
    struct rife_zc_slot *mid_slot = NULL;
    struct rife_zc_slot *cur_slot = NULL;
    struct mp_image *cur_mp = NULL;  // BGRA wrapping of cur_slot, if any
    uint64_t xfv = 0;
    bool zc_emit = (p->use_zc_out_ml && p->use_copy_shader && raw &&
                    raw->imgfmt == IMGFMT_D3D11);
    bool size_ok = true;
    if (zc_emit) {
        if (raw->w != p->zc_ring_w || raw->h != p->zc_ring_h) {
            MP_WARN(vf, "RIFE zc-out: ring size mismatch (%dx%d vs %dx%d); "
                        "passthrough this frame\n",
                    raw->w, raw->h, p->zc_ring_w, p->zc_ring_h);
            size_ok = false;
        }
    }

    // ---- Acquire output slots for Nx interpolation. ----
    // We need (N-1) mid slots + 1 cur projection slot. If the ring is short,
    // degrade N down to what we can actually fit. Always require at least
    // 1 mid + 1 cur (= N=2) for the zc-out ML happy path; otherwise fall
    // through to the existing degraded paths below.
    struct rife_zc_slot *mid_slots[RIFE_PENDING_Q_N] = {0};
    int n_mid = 0;
    if (zc_emit && size_ok) {
        int want_mid = N - 1;
        for (int i = 0; i < want_mid; i++) {
            mid_slots[i] = zc_out_acquire(p);
            if (!mid_slots[i]) break;
            n_mid++;
        }
        cur_slot = (n_mid > 0) ? zc_out_acquire(p) : NULL;
        if (n_mid == 0 || !cur_slot) {
            for (int i = 0; i < n_mid; i++) {
                zc_slot_release(mid_slots[i]);
                mid_slots[i] = NULL;
            }
            n_mid = 0;
            if (cur_slot) { zc_slot_release(cur_slot); cur_slot = NULL; }
            if (!p->warned_ring_exhaust) {
                p->warned_ring_exhaust = true;
                MP_WARN(vf, "RIFE zc-out: ring fully exhausted at "
                            "multiplier=%dx (need %d slots/pair, ring=%d). "
                            "Falling back to passthrough; consider lowering "
                            "the multiplier or scale.\n",
                        N, N, p->zc_ring_n);
            }
            MP_TRACE(vf, "RIFE zc-out: ring exhausted, "
                         "passthrough this frame\n");
        } else if (n_mid < want_mid) {
            p->ring_short_count++;
            // Warn once after a sustained pattern of degradation, not after
            // every brief blip.
            if (!p->warned_ring_tight && p->ring_short_count >= 16) {
                p->warned_ring_tight = true;
                MP_WARN(vf, "RIFE zc-out: ring tight at multiplier=%dx; "
                            "sustained degradation to %dx (got %d of %d "
                            "mids). Downstream is consuming slower than "
                            "we produce.\n",
                        N, n_mid + 1, n_mid, want_mid);
            }
            MP_TRACE(vf, "RIFE zc-out: ring tight (got %d of %d mids); "
                         "this pair runs at %dx instead of %dx\n",
                     n_mid, want_mid, n_mid + 1, N);
        } else {
            p->ring_short_count = 0;
        }
    }
    // Effective per-pair multiplier honoured (1 mid+1 cur = 2x). May be < N.
    int N_eff = (n_mid > 0 && cur_slot) ? (n_mid + 1) : 2;
    // For backwards compat with the legacy degraded-path code below, expose
    // the first mid_slot as "mid_slot".
    mid_slot = (n_mid > 0) ? mid_slots[0] : NULL;

    if (zc_emit && size_ok && !cur_slot) {
        // Ring fully exhausted: try at least to project cur via the copy
        // shader using the standalone helper (one slot needed). If that
        // fails too, fall back to emitting cur as RGB0 (one-time reconfig).
        if (mid_slot) { zc_slot_release(mid_slot); mid_slot = NULL; }
        struct rife_zc_slot *only = zc_out_acquire(p);
        if (only) {
            uint64_t fv = 0;
            if (cur_only_to_slot(vf, cur, only, &fv)) {
                struct mp_image *out = zc_out_make_mpi(vf, only,
                                                       raw ? raw : cur);
                if (out) {
                    out->pts = cur->pts;
                    out->pkt_duration = cur->pkt_duration;
                    mp_image_setrefp(&p->prev, cur);
                    mp_frame_unref(&frame);
                    mp_pin_in_write(vf->ppins[1],
                            MAKE_FRAME(MP_FRAME_VIDEO, out));
                    mp_image_unrefp(&raw);
                    return;
                }
                zc_slot_release(only);
            } else {
                zc_slot_release(only);
            }
        }
        // Fall through to legacy RGB0 emit below.
        mid_slot = cur_slot = NULL;
        zc_emit = false;
    }

    // ---- Run RIFE for each midpoint, build mid mp_images, queue them. ----
    bool zc_loop_path = (n_mid > 0 && cur_slot);
    bool any_inference_failed = false;
    struct mp_image *mids[RIFE_PENDING_Q_N] = {0};
    int n_built = 0;

    if (zc_loop_path) {
        for (int i = 1; i <= n_mid; i++) {
            float ti = (float)i / (float)N_eff;
            bool last = (i == n_mid);
            // Only run frame-diff on the first iteration. Iterations 2..N-1
            // reuse that decision (force_skip if it was static/scene).
            bool force_skip = (i > 1) && (p->diff_last_kind != 0);
            if (!run_inference(vf, p->prev, cur, ti,
                               mid_slots[i - 1],
                               last ? cur_slot : NULL,
                               &xfv, force_skip))
            {
                any_inference_failed = true;
                break;
            }
            struct mp_image *mi = zc_out_make_mpi(vf, mid_slots[i - 1],
                                                  raw ? raw : cur);
            if (!mi) {
                any_inference_failed = true;
                break;
            }
            // PTS at prev.pts + i/N * (cur.pts - prev.pts).
            double mi_pts = cur->pts;
            if (p->prev->pts != MP_NOPTS_VALUE && cur->pts != MP_NOPTS_VALUE) {
                mi_pts = p->prev->pts +
                         (cur->pts - p->prev->pts) * ((double)i / N_eff);
                double step = (cur->pts - p->prev->pts) / (double)N_eff;
                if (step > 0)
                    mi->pkt_duration = step;
            }
            mi->pts = mi_pts;
            mids[n_built++] = mi;
        }
        if (any_inference_failed) {
            // Release whatever didn't get consumed by zc_out_make_mpi.
            for (int i = 0; i < n_mid; i++) {
                if (mid_slots[i] && i + 1 > n_built) {
                    zc_slot_release(mid_slots[i]);
                    mid_slots[i] = NULL;
                }
            }
            if (cur_slot) { zc_slot_release(cur_slot); cur_slot = NULL; }
            for (int i = 0; i < n_built; i++)
                mp_image_unrefp(&mids[i]);
            n_built = 0;
        } else {
            // Build cur projection mp_image.
            cur_mp = zc_out_make_mpi(vf, cur_slot, raw ? raw : cur);
            if (!cur_mp) {
                zc_slot_release(cur_slot);
                cur_slot = NULL;
                for (int i = 0; i < n_built; i++)
                    mp_image_unrefp(&mids[i]);
                n_built = 0;
            } else {
                cur_mp->pts = cur->pts;
                cur_mp->pkt_duration = cur->pkt_duration;
            }
        }
    } else {
        // Legacy degraded path (no zc-out ML or ring exhausted): single 2x
        // midpoint via run_inference + build_midpoint fallback. Always 2x
        // here, regardless of N.
        if (run_inference(vf, p->prev, cur, 0.5f,
                          mid_slot, cur_slot,
                          (mid_slot || cur_slot) ? &xfv : NULL,
                          /*force_skip=*/false))
        {
            if (cur_slot) {
                cur_mp = zc_out_make_mpi(vf, cur_slot, raw ? raw : cur);
                if (!cur_mp) {
                    zc_slot_release(cur_slot);
                    cur_slot = NULL;
                } else {
                    cur_mp->pts = cur->pts;
                    cur_mp->pkt_duration = cur->pkt_duration;
                }
            }
            if (mid_slot) {
                mid = zc_out_make_mpi(vf, mid_slot, raw ? raw : cur);
                if (!mid) {
                    zc_slot_release(mid_slot);
                    mid_slot = NULL;
                } else {
                    double mid_pts = cur->pts;
                    if (p->prev->pts != MP_NOPTS_VALUE &&
                        cur->pts  != MP_NOPTS_VALUE)
                        mid_pts = (p->prev->pts + cur->pts) * 0.5;
                    mid->pts = mid_pts;
                    if (p->prev->pts != MP_NOPTS_VALUE &&
                        cur->pts  != MP_NOPTS_VALUE)
                    {
                        double half = (cur->pts - p->prev->pts) * 0.5;
                        if (half > 0) mid->pkt_duration = half;
                    }
                }
            } else if (!cur_slot) {
                mid = build_midpoint(vf, p->prev, cur);
            }
        } else {
            if (mid_slot) zc_slot_release(mid_slot);
            if (cur_slot) zc_slot_release(cur_slot);
            mid_slot = cur_slot = NULL;
        }
    }

    // prev becomes cur for the next iteration regardless of success.
    mp_image_setrefp(&p->prev, cur);

    if (zc_loop_path && n_built > 0 && cur_mp) {
        // Nx happy path: emit mids[0] immediately, queue mids[1..] + cur.
        struct mp_image *first = mids[0];
        for (int i = 1; i < n_built; i++)
            pending_q_push(p, mids[i]);
        pending_q_push(p, cur_mp);
        cur_mp = NULL;
        mp_frame_unref(&frame);
        mp_pin_in_write(vf->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, first));
        mp_filter_internal_mark_progress(vf);
    } else if (cur_mp && mid) {
        // 2x degraded happy path.
        pending_q_push(p, cur_mp);
        cur_mp = NULL;
        mp_frame_unref(&frame);
        mp_pin_in_write(vf->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, mid));
        mp_filter_internal_mark_progress(vf);
    } else if (cur_mp && !mid) {
        // Mid alloc/inference failed but cur_slot succeeded: emit cur only
        // (skip a midpoint this turn); keeps downstream format uniform.
        mp_frame_unref(&frame);
        mp_pin_in_write(vf->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, cur_mp));
        cur_mp = NULL;
    } else if (mid) {
        // Legacy RGB0 path (zc-out ML inactive). Always 2x here.
        pending_q_push(p, mp_image_new_ref(cur));
        mp_frame_unref(&frame);
        mp_pin_in_write(vf->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, mid));
        mp_filter_internal_mark_progress(vf);
    } else {
        // Inference/alloc failed entirely: emit cur as-is.
        mp_pin_in_write(vf->ppins[1], frame);
    }
    if (cur_mp) mp_image_unrefp(&cur_mp);
    mp_image_unrefp(&raw);
}

static void vf_rife_reset(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    mp_image_unrefp(&p->prev);
    pending_q_clear(p);
    mp_image_unrefp(&p->raw_pending);
    for (int i = 0; i < RIFE_RAW_FIFO_N; i++)
        mp_image_unrefp(&p->raw_in_fifo[i]);
    p->raw_in_head = p->raw_in_tail = 0;
    p->warned_format = false;
}

static void vf_rife_destroy(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    mp_image_unrefp(&p->prev);
    pending_q_clear(p);
    mp_image_unrefp(&p->raw_pending);
    for (int i = 0; i < RIFE_RAW_FIFO_N; i++)
        mp_image_unrefp(&p->raw_in_fifo[i]);
    release_session(p);
    zc_out_release(p);
    zc_release(p);
}

static bool vf_rife_command(struct mp_filter *vf, struct mp_filter_command *cmd)
{
    struct priv *p = vf->priv;
    switch (cmd->type) {
    case MP_FILTER_COMMAND_TEXT: {
        // Runtime modification of opts via `vf-command <label> <key> <value>`.
        // Only fields that don't require re-initialising D3D12/DML/shader
        // resources are accepted. Unsupported keys silently fail.
        if (!cmd->cmd || !cmd->arg)
            return false;
        const char *key = cmd->cmd;
        const char *val = cmd->arg;

        if (strcmp(key, "enabled") == 0) {
            bool b = !(strcmp(val, "no") == 0 || strcmp(val, "false") == 0 ||
                       strcmp(val, "0") == 0);
            if (p->opts->enabled != b) {
                p->opts->enabled = b;
                // Drop any queued mids when bypassing — they belong to a
                // pair that was previously emitted as prev-frame.
                if (!b)
                    pending_q_clear(p);
                MP_INFO(vf, "RIFE: enabled -> %s\n", b ? "yes" : "no");
            }
            return true;
        }
        if (strcmp(key, "multiplier") == 0) {
            int n = atoi(val);
            if (n < 2) n = 2;
            if (n > 8) n = 8;
            if (p->opts->multiplier != n) {
                int old = p->opts->multiplier;
                p->opts->multiplier = n;
                // Shrinking: drop already-queued mids from the previous N
                // (they outnumber the new target and would create cadence
                // glitches). Growing is safe; the next pair will use the
                // larger N naturally.
                if (n < old)
                    pending_q_clear(p);
                MP_INFO(vf, "RIFE: multiplier -> %dx\n", n);
            }
            return true;
        }
        if (strcmp(key, "static-threshold") == 0) {
            float f = (float)atof(val);
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            p->opts->static_threshold = f;
            MP_INFO(vf, "RIFE: static-threshold -> %.4f\n", f);
            return true;
        }
        if (strcmp(key, "scene-threshold") == 0) {
            float f = (float)atof(val);
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            p->opts->scene_threshold = f;
            MP_INFO(vf, "RIFE: scene-threshold -> %.4f\n", f);
            return true;
        }
        if (strcmp(key, "frame-diff") == 0) {
            // Convenience: toggle frame-diff by zeroing or restoring
            // thresholds. When turning back on, use sane defaults if the
            // current values are zero.
            bool b = !(strcmp(val, "no") == 0 || strcmp(val, "false") == 0 ||
                       strcmp(val, "0") == 0);
            if (b) {
                if (p->opts->static_threshold <= 0.0f)
                    p->opts->static_threshold = 0.01f;
                if (p->opts->scene_threshold  <= 0.0f)
                    p->opts->scene_threshold  = 0.35f;
            } else {
                p->opts->static_threshold = 0.0f;
                p->opts->scene_threshold  = 0.0f;
            }
            MP_INFO(vf, "RIFE: frame-diff -> %s (static=%.4f scene=%.4f)\n",
                    b ? "yes" : "no",
                    p->opts->static_threshold, p->opts->scene_threshold);
            return true;
        }
        MP_WARN(vf, "RIFE: vf-command: unknown or non-runtime key '%s'\n", key);
        return false;
    }
    case MP_FILTER_COMMAND_GET_META: {
        struct mp_tags *t = talloc_zero(NULL, struct mp_tags);
        mp_tags_set_str(t, "enabled", p->opts->enabled ? "yes" : "no");
        mp_tags_set_str(t, "multiplier",
                        mp_tprintf(16, "%dx", p->opts->multiplier));
        mp_tags_set_str(t, "model", p->opts->model_path ? p->opts->model_path : "");
        mp_tags_set_str(t, "scale",
                        mp_tprintf(32, "%.3f", p->opts->scale > 0 ? p->opts->scale : 1.0));
        mp_tags_set_str(t, "src",
                        mp_tprintf(32, "%dx%d", p->orig_w, p->orig_h));
        mp_tags_set_str(t, "proc",
                        mp_tprintf(32, "%dx%d", p->proc_w, p->proc_h));
        mp_tags_set_str(t, "padded",
                        mp_tprintf(32, "%dx%d", p->pad_w, p->pad_h));
        mp_tags_set_str(t, "zerocopy", p->use_zc ? "yes" : "no");
        mp_tags_set_str(t, "pack-shader", p->use_pack_shader ? "yes" : "no");
        mp_tags_set_str(t, "unpack-shader", p->use_unpack_shader ? "yes" : "no");
        mp_tags_set_str(t, "copy-shader", p->use_copy_shader ? "yes" : "no");
        mp_tags_set_str(t, "nv12-input", p->use_nv12_input ? "yes" : "no");
        mp_tags_set_str(t, "frame-diff", p->use_frame_diff ? "yes" : "no");
        mp_tags_set_str(t, "static-threshold",
                        mp_tprintf(32, "%.4f", p->opts->static_threshold));
        mp_tags_set_str(t, "scene-threshold",
                        mp_tprintf(32, "%.4f", p->opts->scene_threshold));
        mp_tags_set_str(t, "last-infer-ms",
                        mp_tprintf(32, "%.2f", p->stats_last_infer_ms));
        mp_tags_set_str(t, "diff-ratio",
                        mp_tprintf(32, "%.4f", p->stats_last_diff_ratio));
        mp_tags_set_str(t, "diff-kind",
                        p->diff_last_kind == 1 ? "static" :
                        p->diff_last_kind == 2 ? "scene"  : "normal");
        mp_tags_set_str(t, "total-pairs",
                        mp_tprintf(32, "%llu",
                                   (unsigned long long)p->stats_total_pairs));
        mp_tags_set_str(t, "skipped-static",
                        mp_tprintf(32, "%llu",
                                   (unsigned long long)p->stats_skipped_static));
        mp_tags_set_str(t, "skipped-scene",
                        mp_tprintf(32, "%llu",
                                   (unsigned long long)p->stats_skipped_scene));
        mp_tags_set_str(t, "source-fps",
                        mp_tprintf(32, "%.2f", p->fps_source_pps));
        mp_tags_set_str(t, "output-fps",
                        mp_tprintf(32, "%.2f",
                                   p->fps_source_pps *
                                   (double)(p->opts->multiplier > 0
                                            ? p->opts->multiplier : 1)));
        *(struct mp_tags **)cmd->res = t;
        return true;
    }
    default:
        return false;
    }
}

static const struct mp_filter_info vf_rife_filter = {
    .name = "rife",
    .process = vf_rife_process,
    .reset = vf_rife_reset,
    .destroy = vf_rife_destroy,
    .command = vf_rife_command,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *vf_rife_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_rife_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);

    // Always allocate the autoconvert subfilter so that runtime
    // 'enabled' toggles (vf-command rife enabled yes) can take effect
    // without needing to rebuild the whole vf chain. When disabled, we
    // skip pumping conv entirely and just pass frames through.
    p->conv = mp_autoconvert_create(f);
    if (!p->conv) {
        talloc_free(f);
        return NULL;
    }
    mp_autoconvert_add_imgfmt(p->conv, IMGFMT_RGB0, 0);
    if (p->opts->nv12_input && p->opts->zerocopy &&
        p->opts->zerocopy_output)
    {
        mp_autoconvert_add_imgfmt(p->conv, IMGFMT_D3D11, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(p->conv, IMGFMT_D3D11, IMGFMT_P010);
    }
    bool d3d11_passthrough = p->opts->zerocopy_smoke ||
        (p->opts->zerocopy_output && !p->opts->zerocopy);
    if (d3d11_passthrough) {
        mp_autoconvert_add_imgfmt(p->conv, IMGFMT_D3D11, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(p->conv, IMGFMT_D3D11, IMGFMT_P010);
    }

    return f;
}

#define OPT_BASE_STRUCT struct rife_opts
static const m_option_t rife_opts_fields[] = {
    {"enabled",          OPT_BOOL(enabled)},
    {"model-path",       OPT_STRING(model_path)},
    {"multiplier",       OPT_INT(multiplier),   M_RANGE(2, 8)},
    {"gpu",              OPT_INT(gpu_id),       M_RANGE(0, 15)},
    {"scale",            OPT_FLOAT(scale),      M_RANGE(0.25, 1.0)},
    {"scene-threshold",  OPT_FLOAT(scene_threshold),  M_RANGE(0.0, 1.0)},
    {"static-threshold", OPT_FLOAT(static_threshold), M_RANGE(0.0, 1.0)},
    {"max-width",        OPT_INT(max_width),    M_RANGE(0, 16384)},
    {"max-height",       OPT_INT(max_height),   M_RANGE(0, 16384)},
    {"hdr-passthrough",  OPT_BOOL(hdr_passthrough)},
    {"zerocopy-smoke",   OPT_BOOL(zerocopy_smoke)},
    {"zerocopy",         OPT_BOOL(zerocopy)},
    {"zerocopy-output",  OPT_BOOL(zerocopy_output)},
    {"pack-shader",      OPT_BOOL(pack_shader)},
    {"pack-shader-debug",OPT_BOOL(pack_shader_debug)},
    {"unpack-shader",    OPT_BOOL(unpack_shader)},
    {"nv12-input",       OPT_BOOL(nv12_input)},
    {0}
};

const struct mp_user_filter_entry vf_rife = {
    .desc = {
        .description = "RIFE frame interpolation (DirectML)",
        .name = "rife",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT) {
            .enabled = false,
            .model_path = NULL,
            .multiplier = 2,
            .gpu_id = 0,
            .scale = 1.0f,
            .scene_threshold = 0.35f,
            .static_threshold = 0.01f,
            .max_width = 0,
            .max_height = 0,
            .hdr_passthrough = true,
            .pack_shader = true,
            .unpack_shader = true,
            .nv12_input = false,
        },
        .options = rife_opts_fields,
    },
    .create = vf_rife_create,
};
