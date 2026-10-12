// The render package's auto-exposure: a luminance histogram of the frame in
// compute, and a resolve that turns it into an exposure and adapts the
// exposure over time. Compute passes, compiled ahead of time to
// autoexposure.spv by tools/shaders/shaders.py and embedded in exposure.cone.
//
// THE METHOD is the luminance-histogram one (Hable, "Automatic Exposure",
// 2011; Bruno Opsenica's and Alex Tardif's compute versions; Unity's and
// Unreal's, Godot's 4 equivalents), not a downsampled mean, so that a few
// very bright pixels (the sun, a lamp) do not dominate the average:
// 1. 'histogramMain': every 'stride'-th pixel of the frame, both ways, has
//    its luminance counted into one of 256 bins evenly spaced in log2 between
//    'minLog' and 'minLog + logRange' (bin 0 holds everything darker, which
//    is not metered), each pixel with a weight (1, or more towards the middle
//    if the metering is centre-weighted). The counts are first made in
//    workgroup memory and added to the global histogram once a workgroup.
// 2. 'resolveMain', one workgroup: the metered bins between two percentiles
//    (ignoring the darkest and brightest tails), their mean log2 luminance
//    (weighted by count, each bin at its centre), then the exposure that
//    makes that luminance 'key' (a middle grey, 0.18): key / L, times 2^compensation,
//    clamped to a range in stops. The exposure the frame is shown with
//    moves towards it in log space with a time constant, a different speed
//    when it must rise (the scene went dark: eyes adapt slowly) and fall
//    (the scene went bright: quickly): exposure' = exposure * 2^(delta ev * (1 - e^(-dt speed))).
//    The first frame snaps.
// The frame the histogram reads is pre-exposed by the renderer's own
// exposure (renderframe.slang), so a scene in physical units (lux, nits) is
// near 1 there; the exposure this finds is applied on top of that, by the tone
// map (post.slang), and a weather's exposure offset is the 'compensation'.
//
// The exposure buffer (binding 3, 4 floats): x the exposure the tone map
// uses, y the metered mean log2 luminance, z the target exposure, w 1 once
// it has been written. The histogram (binding 2, 256 words) is cleared
// before each frame by the CPU.

static const uint BINS = 256;

struct ExposureImmediates {
    uint width;                    // 0: the frame's size
    uint height;                   // 4
    uint stride;                   // 8: every stride-th pixel is metered, both ways
    float minLog;                  // 12: log2 luminance of the first metered bin's lower edge
    float logRange;                // 16: the metered range in stops
    float centerWeight;            // 20: 0 even metering, 1 all towards the middle
    float key;                     // 24: the luminance the exposure makes a middle grey (0.18)
    float compensationEv;          // 28: stops added to the exposure
    float minEv;                   // 32: the exposure's least and greatest, in stops
    float maxEv;                   // 36
    float lowPercent;              // 40: the share of the darkest metered pixels left out
    float highPercent;             // 44: and of the brightest (counted from the dark end)
    float speedUp;                 // 48: per second, when the exposure must rise
    float speedDown;               // 52: and fall
    float dt;                      // 56: seconds since the last frame
    float snap;                    // 60: 1 to jump to the target (the first frame, a cut)
};

[[vk::push_constant]] ConstantBuffer<ExposureImmediates> params;
[[vk::binding(1, 0)]] Texture2D<float4> sourceFrame;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> histogram;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> exposureOut;

groupshared uint localHistogram[BINS];

[shader("compute")]
[numthreads(16, 16, 1)]
void histogramMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    localHistogram[index] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 pixel = id.xy * params.stride;
    if (pixel.x < params.width && pixel.y < params.height) {
        float3 c = sourceFrame.Load(int3(pixel, 0)).rgb;
        float lum = dot(max(c, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
        uint bin = 0u;
        if (lum > 1e-6) {
            float t = (log2(lum) - params.minLog) / params.logRange;
            if (t >= 0.0) {
                bin = 1u + uint(min(t, 0.99999) * float(BINS - 1));
            }
        }
        float2 q = (float2(pixel) + 0.5) / float2(params.width, params.height) - 0.5;
        float g = exp(-dot(q, q) * 12.0);
        uint weight = max(1u, uint(lerp(1.0, g, params.centerWeight) * 16.0 + 0.5));
        InterlockedAdd(localHistogram[bin], weight);
    }
    GroupMemoryBarrierWithGroupSync();
    if (localHistogram[index] != 0u) {
        InterlockedAdd(histogram[index], localHistogram[index]);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void resolveMain() {
    float total = 0.0;
    for (uint b = 1u; b < BINS; b++) {
        total += float(histogram[b]);
    }
    float previous = exposureOut[0];
    float seen = exposureOut[3];
    float measured = exposureOut[1];
    if (total > 0.0) {
        float low = total * params.lowPercent;
        float high = total * params.highPercent;
        float cum = 0.0;
        float sum = 0.0;
        float taken = 0.0;
        for (uint b = 1u; b < BINS; b++) {
            float c = float(histogram[b]);
            float take = max(0.0, min(cum + c, high) - max(cum, low));
            float center = params.minLog + params.logRange * ((float(b) - 0.5) / float(BINS - 1));
            sum += take * center;
            taken += take;
            cum += c;
        }
        if (taken > 0.0) {
            measured = sum / taken;
        }
    }
    // The exposure that makes the metered luminance the key, in stops
    float targetEv = clamp(log2(params.key) - measured + params.compensationEv, params.minEv, params.maxEv);
    float ev = targetEv;
    if (seen > 0.5 && params.snap < 0.5) {
        float evNow = log2(max(previous, 1e-6));
        float speed = targetEv > evNow ? params.speedUp : params.speedDown;
        ev = evNow + (targetEv - evNow) * (1.0 - exp(-params.dt * speed));
    }
    exposureOut[0] = exp2(ev);
    exposureOut[1] = measured;
    exposureOut[2] = exp2(targetEv);
    exposureOut[3] = 1.0;
}
