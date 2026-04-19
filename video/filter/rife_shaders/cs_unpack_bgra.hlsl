// cs_unpack_bgra.hlsl
//
// Read a planar FP32 RGB tensor (3 channels, layout = ch * pad_w * pad_h +
// y * pad_w + x; only the top-left proc_w x proc_h area is valid) and write
// BGRA8 into an UAV-bound DXGI_FORMAT_B8G8R8A8_UNORM texture sized
// orig_w x orig_h. Bilinear upsample, corner-aligned to match the CPU
// reference unpack_rgb0 (vf_rife.c:2011) bit-for-bit.
//
// Root signature:
//   0: DescriptorTable(SRV t0)  - ByteAddressBuffer (raw FP32 tensor)
//   1: DescriptorTable(UAV u0)  - RWTexture2D<unorm float4> (BGRA8 dst)
//   2: RootConstants(b0, num32BitConstants=8)
//
// Per-thread: one output pixel. 8x8 thread groups.

ByteAddressBuffer        src : register(t0);
RWTexture2D<unorm float4> dst : register(u0);

cbuffer Params : register(b0) {
    uint  pad_w;
    uint  pad_h;
    uint  proc_w;
    uint  proc_h;
    uint  orig_w;
    uint  orig_h;
    float fx_step;   // (proc_w - 1) / (orig_w - 1)   [or 0 if orig_w == 1]
    float fy_step;   // (proc_h - 1) / (orig_h - 1)   [or 0 if orig_h == 1]
};

float load_plane(uint ch, int x, int y)
{
    uint plane = pad_w * pad_h;
    uint off = (ch * plane + (uint)y * pad_w + (uint)x) * 4u;
    return asfloat(src.Load(off));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= orig_w || tid.y >= orig_h)
        return;

    // Corner-aligned mapping (matches CPU unpack_rgb0).
    float fx = (float)tid.x * fx_step;
    float fy = (float)tid.y * fy_step;
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    int max_x = (int)proc_w - 1;
    int max_y = (int)proc_h - 1;
    if (x0 > max_x) x0 = max_x;
    if (y0 > max_y) y0 = max_y;
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;

    float wx  = fx - (float)((int)fx);
    float wy  = fy - (float)((int)fy);
    float wx0 = 1.0f - wx;
    float wy0 = 1.0f - wy;

    float r00 = load_plane(0, x0, y0);
    float r10 = load_plane(0, x1, y0);
    float r01 = load_plane(0, x0, y1);
    float r11 = load_plane(0, x1, y1);
    float g00 = load_plane(1, x0, y0);
    float g10 = load_plane(1, x1, y0);
    float g01 = load_plane(1, x0, y1);
    float g11 = load_plane(1, x1, y1);
    float b00 = load_plane(2, x0, y0);
    float b10 = load_plane(2, x1, y0);
    float b01 = load_plane(2, x0, y1);
    float b11 = load_plane(2, x1, y1);

    float r = (r00 * wx0 + r10 * wx) * wy0 + (r01 * wx0 + r11 * wx) * wy;
    float g = (g00 * wx0 + g10 * wx) * wy0 + (g01 * wx0 + g11 * wx) * wy;
    float b = (b00 * wx0 + b10 * wx) * wy0 + (b01 * wx0 + b11 * wx) * wy;

    // Saturate to [0,1] (ML output may overshoot slightly).
    r = saturate(r);
    g = saturate(g);
    b = saturate(b);

    // The UAV is bound as DXGI_FORMAT_B8G8R8A8_UNORM, but DXGI presents the
    // channels to the shader in RGBA order (hardware does the byte swap on
    // store: shader .x = R lands in the memory R slot). So write the RGB
    // values in their natural .x=R/.y=G/.z=B positions.
    dst[int2(tid.xy)] = float4(r, g, b, 1.0f);
}
