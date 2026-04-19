// vf_rife step 5c.2a: fill the 5 "metadata" channels of the input tensor.
// Mirrors fill_meta_channels() in vf_rife.c.

RWByteAddressBuffer g_out : register(u0);

cbuffer Params : register(b0) {
    uint  pad_w;
    uint  pad_h;
    float t;
    uint  ch_t;
    uint  ch_hg;
    uint  ch_vg;
    uint  ch_hscl;
    uint  ch_vscl;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pad_w || tid.y >= pad_h)
        return;

    uint plane_stride = pad_w * pad_h * 4u;
    uint pix_off      = (tid.y * pad_w + tid.x) * 4u;

    float fW   = (float)pad_w;
    float fH   = (float)pad_h;
    float hg   = 2.0f * (float)tid.x / (fW - 1.0f) - 1.0f;
    float vg   = 2.0f * (float)tid.y / (fH - 1.0f) - 1.0f;
    float hscl = 2.0f / (fW - 1.0f);
    float vscl = 2.0f / (fH - 1.0f);

    g_out.Store(ch_t    * plane_stride + pix_off, asuint(t));
    g_out.Store(ch_hg   * plane_stride + pix_off, asuint(hg));
    g_out.Store(ch_vg   * plane_stride + pix_off, asuint(vg));
    g_out.Store(ch_hscl * plane_stride + pix_off, asuint(hscl));
    g_out.Store(ch_vscl * plane_stride + pix_off, asuint(vscl));
}
