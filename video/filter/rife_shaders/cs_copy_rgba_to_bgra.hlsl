// SPDX-License-Identifier: LGPL-2.1-or-later
//
// cs_copy_rgba_to_bgra.hlsl
//
// Channel-swizzling copy from a R8G8B8A8_UNORM texture (the pack-shader
// input texture used for ML inference) into a B8G8R8A8_UNORM UAV (a slot
// of the D3D12-shared D3D11 ring used as zc-out).
//
// Used by the 5c.3 zc-out ML mode to project the *cur* frame onto the
// same shared D3D11 device as the interpolated *mid* frame, so the
// downstream filter chain sees a uniform IMGFMT_D3D11/B8G8R8A8 stream
// from a single D3D11 device. Without this, mid (BGRA on our shared
// device) and cur (NV12 on the upstream decoder device) alternate and
// the autoconvert pipeline trips an "AVFrame/imgfmt hardware context
// mismatch".
//
// Root signature layout (shared with cs_unpack_bgra):
//   [0] DescriptorTable: SRV t0 (Texture2D R8G8B8A8 source)
//   [1] DescriptorTable: UAV u0 (RWTexture2D B8G8R8A8 dst slot)
//   [2] 32bit constants b0 (8 ints; only width/height are used)
//
// Threadgroup: 8x8x1.

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint pad2;
    uint pad3;
    uint pad4;
    uint pad5;
    uint pad6;
    uint pad7;
};

Texture2D<float4>   src : register(t0);
RWTexture2D<float4> dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;

    // src.x = R, src.y = G, src.z = B, src.w = A.
    float4 c = src.Load(int3((int)tid.x, (int)tid.y, 0));

    // dst is a B8G8R8A8_UNORM UAV, but DXGI presents channels to the shader
    // in RGBA order regardless of the format's memory layout (the hardware
    // takes care of the byte swap on store). So just write the source RGB
    // values straight through. Force alpha=1 since RGB0 sources only carry
    // meaningful colour in RGB and downstream libplacebo expects opaque.
    dst[tid.xy] = float4(c.x, c.y, c.z, 1.0);
}
