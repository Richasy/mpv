// RIFE RGBA8→NCHW compute shader
// Reads an RGBA8 Texture2D and writes padded NCHW float data to a buffer.
// Dispatched once per frame (prev=channel 0-2, curr=channel 3-5).

Texture2D<float4> g_input : register(t0);
RWBuffer<float> g_output  : register(u0);  // [1,6,pH,pW] NCHW float

cbuffer Constants : register(b0) {
    uint src_w, src_h;       // original texture dimensions
    uint pad_w, pad_h;       // padded dimensions (64-aligned)
    uint channel_offset;     // 0 for img0 (channels 0-2), 3 for img1 (channels 3-5)
    uint _pad0, _pad1, _pad2; // padding to 32 bytes
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pad_w || tid.y >= pad_h)
        return;

    // Replicate-border padding: clamp source coords
    uint sx = min(tid.x, src_w - 1);
    uint sy = min(tid.y, src_h - 1);

    float4 rgba = g_input.Load(int3(sx, sy, 0));

    uint plane_size = pad_w * pad_h;
    uint base = channel_offset * plane_size;
    uint idx = tid.y * pad_w + tid.x;

    g_output[base + 0 * plane_size + idx] = rgba.r;
    g_output[base + 1 * plane_size + idx] = rgba.g;
    g_output[base + 2 * plane_size + idx] = rgba.b;
}
