// Softsplat forward warping compute shader for GMFSS frame interpolation.
//
// Implements bilinear splatting with exponential metric (importance) weighting.
// Each source pixel scatters to up to 4 destination pixels using bilinear weights.
// Uses atomic float adds via InterlockedAdd on a UINT buffer (reinterpret cast).
//
// Two passes:
//   Pass 0 (SPLAT):  Clear output, then scatter-accumulate weighted values + weights
//   Pass 1 (NORMALIZE): Divide accumulated values by accumulated weights
//
// Constant buffer layout:
//   uint width, height, channels, pass;
//   (pass: 0 = splat, 1 = normalize)

cbuffer SoftsplatCB : register(b0)
{
    uint g_width;
    uint g_height;
    uint g_channels;
    uint g_pass;       // 0 = clear+splat, 1 = normalize
};

// Input tensor: [C, H, W] in row-major NCHW (N=1 assumed)
// Stored as flat float array: index = c * H * W + y * W + x
Buffer<float> g_input : register(t0);

// Flow tensor: [2, H, W] — channel 0 = dx, channel 1 = dy (pixel units)
Buffer<float> g_flow : register(t1);

// Metric tensor: [1, H, W] — importance/confidence score
Buffer<float> g_metric : register(t2);

// Output accumulation buffer: [(C+1), H, W] as RWBuffer<uint>
// Last channel (C) stores accumulated weight sums.
// We use uint reinterpretation for atomic float add.
RWBuffer<uint> g_output : register(u0);

// Atomic float add via uint reinterpretation.
// This is a well-known pattern: reinterpret float as uint, use InterlockedAdd
// with a compare-exchange loop for correctness.
void AtomicAddFloat(uint addr, float value)
{
    uint expected, original;
    float current;
    g_output.InterlockedCompareExchange(addr, 0, 0, original);
    [allow_uav_condition] do {
        expected = original;
        current = asfloat(expected) + value;
        g_output.InterlockedCompareExchange(addr, expected, asuint(current), original);
    } while (original != expected);
}

// --- Pass 0: Splat ---
// Dispatch: ceil(W/16) x ceil(H/16) x 1, threadgroup [16, 16, 1]
// Each thread processes one source pixel.

[numthreads(16, 16, 1)]
void CSSoftsplatSplat(uint3 dtid : SV_DispatchThreadID)
{
    uint x = dtid.x;
    uint y = dtid.y;
    if (x >= g_width || y >= g_height)
        return;

    uint HW = g_height * g_width;
    uint idx_xy = y * g_width + x;

    // Read flow at (x, y)
    float dx = g_flow[idx_xy];           // flow channel 0
    float dy = g_flow[HW + idx_xy];      // flow channel 1

    // Destination position
    float dst_x = (float)x + dx;
    float dst_y = (float)y + dy;

    // Read metric and compute exp(metric) for "soft" mode
    float m = g_metric[idx_xy];
    float weight = exp(m);

    // Bilinear splat: 4 neighbors
    int ix0 = (int)floor(dst_x);
    int iy0 = (int)floor(dst_y);
    int ix1 = ix0 + 1;
    int iy1 = iy0 + 1;

    float fx = dst_x - (float)ix0;
    float fy = dst_y - (float)iy0;

    // Bilinear weights
    float w00 = (1.0f - fx) * (1.0f - fy);  // top-left
    float w10 = fx * (1.0f - fy);            // top-right
    float w01 = (1.0f - fx) * fy;            // bottom-left
    float w11 = fx * fy;                     // bottom-right

    uint C = g_channels;
    uint out_stride = (C + 1) * HW;  // total output elements (C+1 channels)

    // For each channel, scatter to 4 neighbors
    // Scatter helper: adds weighted input value and weight accumulator
    // Output layout: channel c at offset c * HW + oy * W + ox
    // Weight channel at offset C * HW + oy * W + ox

    // Helper struct for the 4 corners
    struct Corner {
        int px, py;
        float w;
    };

    Corner corners[4];
    corners[0].px = ix0; corners[0].py = iy0; corners[0].w = w00;
    corners[1].px = ix1; corners[1].py = iy0; corners[1].w = w10;
    corners[2].px = ix0; corners[2].py = iy1; corners[2].w = w01;
    corners[3].px = ix1; corners[3].py = iy1; corners[3].w = w11;

    [unroll]
    for (uint ci = 0; ci < 4; ci++)
    {
        int px = corners[ci].px;
        int py = corners[ci].py;
        float bw = corners[ci].w;

        if (px < 0 || px >= (int)g_width || py < 0 || py >= (int)g_height)
            continue;

        uint out_xy = (uint)py * g_width + (uint)px;
        float combined_w = weight * bw;

        // Accumulate weighted values for each channel
        for (uint c = 0; c < C; c++)
        {
            float val = g_input[c * HW + idx_xy];
            AtomicAddFloat(c * HW + out_xy, val * combined_w);
        }

        // Accumulate weight in the extra channel
        AtomicAddFloat(C * HW + out_xy, combined_w);
    }
}

// --- Pass 1: Normalize ---
// Output buffer has (C+1) channels. Divide first C channels by the weight channel.
// Write result back into same buffer (in-place).

[numthreads(16, 16, 1)]
void CSSoftsplatNormalize(uint3 dtid : SV_DispatchThreadID)
{
    uint x = dtid.x;
    uint y = dtid.y;
    if (x >= g_width || y >= g_height)
        return;

    uint HW = g_height * g_width;
    uint idx = y * g_width + x;
    uint C = g_channels;

    // Read accumulated weight
    float w = asfloat(g_output[C * HW + idx]);

    // Avoid division by zero
    if (w < 1e-7f)
        w = 1e-7f;

    // Normalize each channel
    for (uint c = 0; c < C; c++)
    {
        uint addr = c * HW + idx;
        float val = asfloat(g_output[addr]);
        g_output[addr] = asuint(val / w);
    }
}
