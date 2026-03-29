// RIFE NCHW→RGBA8 compute shader
// Reads padded NCHW float buffer, crops to original size, writes RGBA8 texture.

Buffer<float> g_input          : register(t0);  // [1,3,pH,pW] NCHW float
RWTexture2D<float4> g_output   : register(u0);  // output RGBA8 texture

cbuffer Constants : register(b0) {
    uint src_w, src_h;       // original (output) dimensions
    uint pad_w, pad_h;       // padded dimensions
    uint _pad0, _pad1, _pad2, _pad3; // padding to 32 bytes
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= src_w || tid.y >= src_h)
        return;

    uint plane_size = pad_w * pad_h;
    uint idx = tid.y * pad_w + tid.x;

    float r = saturate(g_input[0 * plane_size + idx]);
    float g = saturate(g_input[1 * plane_size + idx]);
    float b = saturate(g_input[2 * plane_size + idx]);

    g_output[tid.xy] = float4(r, g, b, 1.0);
}
