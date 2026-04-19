// vf_rife frame-diff reduce shader: compute luma SAD and luma sum between
// two RGBA8 frames, output as two scalar UINT atomics into a small readback
// buffer. Used to drive scene-change / static-frame detection.
//
// Output buffer layout (UAV u0, RWByteAddressBuffer):
//   offset 0: uint64 fixed-point sum of |Y_prev - Y_cur|, scaled by 65536
//   offset 8: uint64 fixed-point sum of (Y_prev + Y_cur) / 2, scaled by 65536
//             (used as the normalisation factor; serves as mean-luma proxy)
//
// Caller must clear both 8-byte slots to zero before dispatch. Caller must
// also ensure dispatch covers exactly orig_w x orig_h pixels (one thread
// per pixel). Final ratio MAD/mean is computed CPU-side after readback.
//
// Design notes:
//   * BT.709 luma (0.2126 R + 0.7152 G + 0.0722 B). Approximation is fine —
//     we just need a relative metric across frames of the same content.
//   * Group-shared accumulation reduces atomic pressure: each 16x16 group
//     accumulates ~256 pixels into two LDS uints, then ONE atomic add per
//     group commits to global memory.
//   * 16-bit fixed-point precision: pixel luma is in [0, 1], multiply by
//     65536 → max per-pixel contribution ≈ 65536. For 4K (8.3M pixels) the
//     theoretical max sum is ~5.4e11, comfortably fits in uint64. We use
//     a paired uint32 atomic on (low, high) words.

Texture2D<float4> g_prev : register(t0);
Texture2D<float4> g_cur  : register(t1);
RWByteAddressBuffer g_out : register(u0);

cbuffer Params : register(b0) {
    uint orig_w;
    uint orig_h;
    uint pad0;
    uint pad1;
};

groupshared uint gs_sad_lo;
groupshared uint gs_sad_hi;
groupshared uint gs_sum_lo;
groupshared uint gs_sum_hi;

static const float3 kLumaW = float3(0.2126f, 0.7152f, 0.0722f);

void atomic_add_u64(uint base_off, uint add_lo, uint add_hi)
{
    uint orig_lo;
    g_out.InterlockedAdd(base_off + 0u, add_lo, orig_lo);
    // Detect carry from low word: if (orig_lo + add_lo) wrapped, +1 to hi.
    uint new_lo = orig_lo + add_lo;
    uint carry = (new_lo < orig_lo) ? 1u : 0u;
    if (add_hi != 0u || carry != 0u) {
        uint dummy;
        g_out.InterlockedAdd(base_off + 4u, add_hi + carry, dummy);
    }
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gtid : SV_GroupIndex)
{
    if (gtid == 0u) {
        gs_sad_lo = 0u; gs_sad_hi = 0u;
        gs_sum_lo = 0u; gs_sum_hi = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x < orig_w && tid.y < orig_h) {
        float3 p = g_prev.Load(int3((int)tid.x, (int)tid.y, 0)).rgb;
        float3 c = g_cur.Load(int3((int)tid.x, (int)tid.y, 0)).rgb;
        float yp = dot(p, kLumaW);
        float yc = dot(c, kLumaW);
        float diff = abs(yp - yc);
        float avg  = 0.5f * (yp + yc);

        // Quantise to 16.16 fixed point.
        uint qd = (uint)(diff * 65536.0f + 0.5f);
        uint qa = (uint)(avg  * 65536.0f + 0.5f);

        uint dummy;
        InterlockedAdd(gs_sad_lo, qd, dummy);
        InterlockedAdd(gs_sum_lo, qa, dummy);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gtid == 0u) {
        // Per-group totals fit in uint32 trivially (16x16=256 pixels max,
        // each contributing <= 65536 → <= 16.7M), so gs_*_hi is always 0.
        atomic_add_u64(0u, gs_sad_lo, 0u);
        atomic_add_u64(8u, gs_sum_lo, 0u);
    }
}
