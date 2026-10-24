// The render package's depth of field: a bokeh-style gather of the frame
// blurred by each pixel's circle of confusion, from a camera with a focus
// distance and an aperture. Full-screen passes, compiled ahead of time to
// dof.spv by tools/shaders/shaders.py and embedded in dof.cone.
//
// THE METHODS:
// - The CIRCLE OF CONFUSION of a thin lens (Potmesil and Chakravarty 1981;
//   the formula of GPU Gems, ch. 23): a point at distance d, the lens of focal
//   length f and f-number N (aperture diameter f / N) focused at S, images as
//   a disc of diameter  (f / N) f |d - S| / (d (S - f))  on the sensor; over
//   the sensor's height, in pixels of the frame. Negative in front of the
//   focus (the near field), positive behind it.
// - The BLUR is a gather over a Vogel disc of taps (a golden-angle spiral),
//   turned by interleaved gradient noise at each pixel, at half resolution
//   (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
//   Warfare", SIGGRAPH 2014, which gathers near and far fields apart; and
//   Gustafsson's "Bokeh depth of field in a single pass", 2018, whose
//   scatter-as-gather test is used here): a tap's colour counts for the pixel
//   at the centre if the tap's own circle reaches it (smoothly over a
//   pixel), and a tap BEHIND the centre has its circle clamped to the
//   centre's own, so that a blurred background does not bleed over a sharp
//   thing in front of it, while a blurred thing in front does spread over
//   what is behind it. Each tap's weight is its circle's coverage over its
//   circle's area (energy conserving), the taps weighted as a tone mapper
//   will show them (1 / (1 + luma), Karis) so that one bright tap does not
//   dominate: that shapes the bright discs of bokeh.
// - The COMPOSITE at full resolution mixes the sharp frame and the blurred
//   half resolution one by how large the circle that covers the pixel is.
//
// Group 0: the frame uniform (0), a linear clamped sampler (1), the frame (2),
// the depth (3), and two half-resolution textures (4 and 5: the prepared
// frame the gather reads, the blurred one the composite reads). The
// immediates: dof.cone's DofImmediates.

import fxcommon;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] SamplerState linearClamp;
[[vk::binding(2, 0)]] Texture2D<float4> sceneFrame;
[[vk::binding(3, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(4, 0)]] Texture2D<float4> halfPrepared;
[[vk::binding(5, 0)]] Texture2D<float4> halfBlurred;

struct DofImmediates {
    float focusDistance;           // 0: metres
    float apertureDiameter;        // 4: focal length / f-number, metres
    float focalLength;             // 8: metres
    float cocScale;                // 12: pixels per metre of circle on the sensor (the frame's height / the sensor's)
    float maxCoc;                  // 16: the largest circle's diameter, in full-resolution pixels
    float strength;                // 20
    uint taps;                     // 24
    float noise;                   // 28
};

[[vk::push_constant]] ConstantBuffer<DofImmediates> dof;

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y);
    return output;
}

float3 sane(float3 c) {
    return min(max(c, float3(0.0, 0.0, 0.0)), float3(65000.0, 65000.0, 65000.0));
}

// The circle of confusion at the full-resolution pixel p, signed, in pixels
float cocAt(int2 p) {
    int2 size = int2(fx.screen.xy);
    float z = sceneDepth.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
    if (z >= 1.0) {
        z = 0.999999;
    }
    float d = viewDepth(fx, z);
    float s = dof.focusDistance;
    float c = dof.apertureDiameter * dof.focalLength * (d - s) / (d * max(s - dof.focalLength, 1e-3));
    return clamp(c * dof.cocScale * dof.strength, -dof.maxCoc, dof.maxCoc);
}

// The frame brought down to half resolution: the colour of the 2 x 2 (as it
// will be seen, by Karis's weights), and the circle of confusion of the one
// of the four with the largest, with its sign
[shader("fragment")]
float4 prepareMain(FullscreenOutput input) : SV_Target {
    int2 hp = int2(input.position.xy);
    int2 size = int2(fx.screen.xy);
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    float coc = 0.0;
    float nearest = 2.0;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            int2 q = clamp(hp * 2 + int2(x, y), int2(0, 0), size - 1);
            float3 c = sane(sceneFrame.Load(int3(q, 0)).rgb);
            float w = 1.0 / (1.0 + luma(c));
            sum += c * w;
            total += w;
            // The nearest of the four decides the circle: a sharp thing in
            // front of a blurred background stays sharp at its edge
            float z = sceneDepth.Load(int3(q, 0));
            if (z < nearest) {
                nearest = z;
                coc = cocAt(q);
            }
        }
    }
    return float4(sum / total, coc);
}

// The gather: this half-resolution pixel's colour from the taps whose
// circles reach it, and, in alpha, the largest such circle (what the
// composite mixes by).
//
// SCATTER AS GATHER, NORMALIZED: each pixel throws its colour over its
// circle, spread evenly; this pixel's colour is what the pixels whose
// circles reach it throw, each weighted by how much of it reaches (softly
// over a pixel) over its circle's area, and the sum divided by the weights'
// total. The division makes the estimate exact where everything round has one
// circle (a blurred wall comes out its own colour, whatever share of the taps
// reach), and keeps a blurred background beside a sharp edge its own colour
// rather than darker or lighter for the taps the edge took; a small bright
// circle among large dim ones still outweighs them, which shapes the bokeh.
// (Not normalized, the sum was a Monte Carlo estimate over the whole disc
// searched: a small circle was seen by a few taps, which few turning with
// the per-pixel noise, and the noise's diagonal pattern showed as stripes over
// a softly blurred wall.) Each tap reads a whole pixel, never a filtered
// blend, so that a circle is never averaged with its neighbour's: a sharp
// lamp's small circle and the blurred black's large one would average to a
// middling one that carried the lamp's light out over the black.
[shader("fragment")]
float4 gatherMain(FullscreenOutput input) : SV_Target {
    int2 hp = int2(input.position.xy);
    int2 halfSize = int2(max(fx.screen.xy * 0.5, float2(1.0, 1.0)));
    float4 centre = halfPrepared.Load(int3(hp, 0));
    float centreCoc = centre.a;
    float centreSize = abs(centreCoc);
    // The radius of the disc searched, in half-resolution pixels
    float searchRadius = max(dof.maxCoc * 0.25, 1.0);
    // The pixel itself, over its own circle (a radius in half-resolution pixels: a quarter of the diameter)
    float centreRadius = centreSize * 0.25;
    float centreWeight = 1.0 / max(1.0, centreRadius * centreRadius * 3.14159265);
    float3 sum = min(centre.rgb, float3(64.0, 64.0, 64.0)) * centreWeight;
    float total = centreWeight;
    float covering = centreSize;
    float turn = 6.2831853 * frac(interleavedGradientNoise(float2(hp)) + dof.noise);
    for (uint i = 0u; i < dof.taps; i++) {
        float r = sqrt((float(i) + 0.5) / float(dof.taps)) * searchRadius;
        float a = float(i) * 2.39996323 + turn;
        float2 offset = float2(cos(a), sin(a)) * r;
        int2 q = clamp(hp + int2(round(offset)), int2(0, 0), halfSize - 1);
        // A tap that lands on the pixel itself is the pixel's own term above
        if (q.x == hp.x && q.y == hp.y) {
            continue;
        }
        float4 s = halfPrepared.Load(int3(q, 0));
        // The tap's circle's radius in half-resolution pixels: a tap behind
        // the centre is held to the centre's own size
        float sampleRadius = abs(s.a) * 0.25;
        if (s.a > centreCoc) {
            sampleRadius = min(sampleRadius, centreRadius);
        }
        // Whether its circle reaches the centre, softly over a pixel
        float reach = saturate(sampleRadius - length(float2(q - hp)) + 0.5);
        if (reach > 0.0) {
            float w = reach / max(1.0, sampleRadius * sampleRadius * 3.14159265);
            covering = max(covering, sampleRadius * 4.0);
            sum += min(s.rgb, float3(64.0, 64.0, 64.0)) * w;
            total += w;
        }
    }
    return float4(sum / total, covering);
}

// The composite: the sharp frame mixed with the blurred one by the size of the circle
// that covers the pixel, the larger of its own and the half resolution's
[shader("fragment")]
float4 compositeMain(FullscreenOutput input) : SV_Target {
    int2 p = int2(input.position.xy);
    float3 sharp = sane(sceneFrame.Load(int3(p, 0)).rgb);
    float4 blurred = halfBlurred.SampleLevel(linearClamp, input.uv, 0.0);
    float own = abs(cocAt(p));
    float size = max(own, blurred.a);
    float t = saturate((size - 0.5) / 1.5);
    return float4(lerp(sharp, sane(blurred.rgb), t), 1.0);
}
