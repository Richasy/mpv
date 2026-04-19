// vf_rife step 5c.2a: pack a single RGB0 (R8G8B8A8_UNORM) frame into three
// FP32 planar channels of the input tensor buffer.
//
// Behaviour MUST match the CPU pack_rgb0() in vf_rife.c exactly so that the
// shader path can be parity-checked against the existing CPU path.
//
//   * Output buffer layout: planar [channels, pad_h, pad_w] FP32.
//     Plane c starts at byte (c * pad_h * pad_w * 4).
//   * pad coords [proc..pad-1] are filled by edge replication of (proc-1).
//   * When inv_scale > 1 (down-sampling) source coord uses nearest-neighbour
//     rounding: src = floor(pad * inv_scale + 0.5) clamped to [0, orig-1].
//   * Pixels are written as src_byte / 255.0 (no linearisation, no color
//     matrix - the input is already nonlinear R'G'B' from autoconvert).

Texture2D<float4>   g_in  : register(t0);
RWByteAddressBuffer g_out : register(u0);

cbuffer Params : register(b0) {
    uint  pad_w;
    uint  pad_h;
    uint  proc_w;
    uint  proc_h;
    uint  orig_w;
    uint  orig_h;
    uint  base_ch;     // 0 for prev frame (CH_R0), 3 for cur frame (CH_R1)
    float inv_scale;   // 1.0 for scale=1.0, 2.0 for scale=0.5
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pad_w || tid.y >= pad_h)
        return;

    int xp = (tid.x < proc_w) ? (int)tid.x : (int)proc_w - 1;
    int yp = (tid.y < proc_h) ? (int)tid.y : (int)proc_h - 1;

    int sx = (int)((float)xp * inv_scale + 0.5f);
    int sy = (int)((float)yp * inv_scale + 0.5f);
    sx = clamp(sx, 0, (int)orig_w - 1);
    sy = clamp(sy, 0, (int)orig_h - 1);

    float4 rgba = g_in.Load(int3(sx, sy, 0));

    uint plane_stride = pad_w * pad_h * 4u;
    uint pix_off      = (tid.y * pad_w + tid.x) * 4u;

    g_out.Store((base_ch + 0u) * plane_stride + pix_off, asuint(rgba.r));
    g_out.Store((base_ch + 1u) * plane_stride + pix_off, asuint(rgba.g));
    g_out.Store((base_ch + 2u) * plane_stride + pix_off, asuint(rgba.b));
}
