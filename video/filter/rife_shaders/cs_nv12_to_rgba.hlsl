// SPDX-License-Identifier: LGPL-2.1-or-later
//
// cs_nv12_to_rgba.hlsl
//
// Convert a D3D11 NV12 hardware frame (imported into D3D12 as two planar
// SRVs) into an R8G8B8A8_UNORM UAV that the rest of the RIFE-DML pipeline
// already consumes (zc_pack_in_tex_prev/cur). One dispatch fills one full
// orig_w x orig_h RGBA texture; the caller dispatches twice per frame
// (prev + cur) and each time also rebinds the two NV12 plane SRVs to the
// corresponding ring slot.
//
// The YUV->RGB matrix and offsets are passed in as 12 floats so the same
// shader handles BT.601 / BT.709 / BT.2020 in either limited or full range:
//   m00 m01 m02   off0
//   m10 m11 m12   off1
//   m20 m21 m22   off2
// rgb = M * (yuv - off)
//
// Chroma siting: NV12 conventionally uses MPEG-2 left-aligned chroma (chroma
// samples sit at the same column as the even-row luma samples, vertically
// centered between the two luma rows). We approximate this by sampling the
// UV plane with a half-pixel vertical offset (i.e. chroma_y = luma_y / 2)
// using bilinear filtering on the integer indices we manually compute. For
// SDR 1080p / 4K material this matches what swscale's default produces
// closely enough; if visible artefacts appear we can refine by passing a
// chroma-loc enum and switching the sample positions.
//
// Root signature:
//   0: DescriptorTable(SRV t0)  - Texture2D<float>  Y  (R8_UNORM)
//   1: DescriptorTable(SRV t1)  - Texture2D<float2> UV (R8G8_UNORM, half-res)
//   2: DescriptorTable(UAV u0)  - RWTexture2D<unorm float4> RGBA dst
//   3: RootConstants(b0, num32BitConstants=20)

Texture2D<float>   y_tex  : register(t0);
Texture2D<float2>  uv_tex : register(t1);
RWTexture2D<unorm float4> dst : register(u0);

cbuffer Params : register(b0) {
    uint  width;       // orig_w
    uint  height;      // orig_h
    uint  chroma_w;    // width  / 2 (UV plane logical dim)
    uint  chroma_h;    // height / 2
    float m00; float m01; float m02; float pad0;
    float m10; float m11; float m12; float pad1;
    float m20; float m21; float m22; float pad2;
    float off0; float off1; float off2; float pad3;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;

    int xy_x = (int)tid.x;
    int xy_y = (int)tid.y;

    float Y = y_tex.Load(int3(xy_x, xy_y, 0));

    // Chroma at half resolution. MPEG-2 / "left" siting:
    //   chroma_x lines up with even luma_x (no horizontal phase shift)
    //   chroma_y sits between luma_y and luma_y+1 (half-pel down).
    // Map: cx_f = (luma_x + 0.0) / 2 - 0.0 ; cy_f = (luma_y + 0.5) / 2 - 0.5
    // Implement bilinear by hand on the chroma integer grid.
    float cx_f = (float)xy_x * 0.5f;
    float cy_f = ((float)xy_y - 0.5f) * 0.5f + 0.25f; // = (luma_y - 0.5)/2 + 0.25
    // The +0.25 puts the sample point at the same vertical position as the
    // chroma row centroid; bilinear weights below interpolate between the
    // two adjacent chroma rows.

    int cx0 = (int)floor(cx_f);
    int cy0 = (int)floor(cy_f);
    float wx = cx_f - (float)cx0;
    float wy = cy_f - (float)cy0;

    int cx1 = cx0 + 1;
    int cy1 = cy0 + 1;
    int cmax_x = (int)chroma_w - 1;
    int cmax_y = (int)chroma_h - 1;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx0 > cmax_x) cx0 = cmax_x;
    if (cy0 > cmax_y) cy0 = cmax_y;
    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx1 > cmax_x) cx1 = cmax_x;
    if (cy1 > cmax_y) cy1 = cmax_y;

    float2 uv00 = uv_tex.Load(int3(cx0, cy0, 0));
    float2 uv10 = uv_tex.Load(int3(cx1, cy0, 0));
    float2 uv01 = uv_tex.Load(int3(cx0, cy1, 0));
    float2 uv11 = uv_tex.Load(int3(cx1, cy1, 0));
    float wx0 = 1.0f - wx;
    float wy0 = 1.0f - wy;
    float2 UV = (uv00 * wx0 + uv10 * wx) * wy0 +
                (uv01 * wx0 + uv11 * wx) * wy;
    float U = UV.x;
    float V = UV.y;

    float yo = Y - off0;
    float uo = U - off1;
    float vo = V - off2;
    float r = m00 * yo + m01 * uo + m02 * vo;
    float g = m10 * yo + m11 * uo + m12 * vo;
    float b = m20 * yo + m21 * uo + m22 * vo;

    r = saturate(r);
    g = saturate(g);
    b = saturate(b);

    dst[int2(xy_x, xy_y)] = float4(r, g, b, 1.0f);
}
