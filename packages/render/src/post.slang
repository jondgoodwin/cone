// The render package's post-processing, from the HDR frame to the display:
// bloom, then tone mapping. Full-screen passes, compiled ahead of time to
// post.spv by tools/shaders/shaders.py and embedded in post.cone.
//
// BLOOM (Jimenez's, from Call of Duty: Advanced Warfare, SIGGRAPH 2014): the
// frame is filtered down a chain of levels, each half the one before, by a
// 13-tap filter (the first level's taps weighted by Karis's average,
// 1 / (1 + luma), so a lone bright pixel does not flicker); then back up,
// each level the one below's tent-filtered upsample added to its own
// downsample. The last is the mean of every level's blur, a wide glow with
// the energy of the frame, and the tone-mapping pass mixes a small fraction
// of it in: no threshold, so every bright thing glows in proportion.
//
// TONE MAPPING turns scene light into display values: the frame (times the
// exposure) through one of
// - AgX (Troy Sobotka's, Blender 4's default), by Benjamin Wrensch's
//   polynomial fit ("Minimal AgX Implementation", 2023): into AgX's inset
//   working space, a log2 encoding over 16.5 stops, a sigmoid, and out;
//   bright colours desaturate towards white rather than skew in hue;
// - ACES, as Krzysztof Narkowicz's fit of its filmic curve (2016);
// - Reinhard's x / (1 + x), per channel;
// - none: clamped.
// The result is linear display light, which the sRGB target encodes.
//
// THE FINISH (image quality, fx.cone), all off by default, so a frame draws
// as it did before they existed:
// - the exposure may come from a buffer (the auto-exposure's, binding 3)
//   instead of the immediates;
// - a VIGNETTE darkens the frame towards its corners before it is tone
//   mapped, as a lens does (cos^4-like: a smooth fall from 'vignetteRadius'
//   to the corner);
// - COLOUR GRADING after the tone map, in display-encoded (sRGB) values: a
//   3-D lookup table (binding 4; ordinary grading LUTs are made in display
//   space), then lift / gamma / gain (the colourist's three controls: lift
//   raises the blacks, gain scales the whites, gamma bends the middle) and a
//   saturation about the luma;
// - FILM GRAIN last: a triangular-distributed noise of the pixel and the
//   frame number, strongest in the middle tones, in display-encoded values.
//
// Group 0: two textures (what a pass reads; the second only by the upsample
// and the tone map) and a linear, clamped sampler, then the exposure buffer
// and the grading table. The immediates: the source's texel size, the mode,
// the bloom's strength, the exposure and the tone mapper, then the finish.

struct PostImmediates {
    float2 texel;                  // 0: the size of a texel of 'first', in uv
    uint mode;                     // 8: 0 plain downsample, 1 the first (Karis's average)
    float strength;                // 12: the bloom's share of the tone-mapped frame
    float exposure;                // 16
    uint toneMapper;               // 20: 0 none, 1 AgX, 2 ACES, 3 Reinhard
    float bloomScale;              // 24: what makes the chain's top its levels' mean (1 / the levels)
    uint flags;                    // 28: 1 exposure from the buffer, 2 grading, 4 vignette, 8 grain
    float4 lift;                   // 32: rgb lift, w the saturation
    float4 gamma;                  // 48: rgb gamma, w the table's share (0: none)
    float4 gain;                   // 64: rgb gain, w the table's scale (N - 1) / N
    float4 vignette;               // 80: x strength, y radius (0 to 1 of the way to the corner), z softness, w grain strength
    float4 extra;                  // 96: x the table's offset 0.5 / N, y the frame number
};

static const uint FINISH_EXPOSURE = 1;
static const uint FINISH_GRADE = 2;
static const uint FINISH_VIGNETTE = 4;
static const uint FINISH_GRAIN = 8;

[[vk::binding(0, 0)]] Texture2D first;
[[vk::binding(1, 0)]] Texture2D second;
[[vk::binding(2, 0)]] SamplerState linearClamp;
[[vk::binding(3, 0)]] StructuredBuffer<float> exposureBuffer;
[[vk::binding(4, 0)]] Texture3D gradingTable;
[[vk::push_constant]] ConstantBuffer<PostImmediates> post;

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;          // 0 to 1, v down the rows
};

[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    // Clip space's y is up and rows run down
    output.uv = float2(p.x, 1.0 - p.y);
    return output;
}

float3 tap(Texture2D t, float2 uv) {
    return t.SampleLevel(linearClamp, uv, 0.0).rgb;
}

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Karis's average of four taps
float3 karis(float3 a, float3 b, float3 c, float3 d) {
    float wa = 1.0 / (1.0 + luma(a));
    float wb = 1.0 / (1.0 + luma(b));
    float wc = 1.0 / (1.0 + luma(c));
    float wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// The 13-tap downsample: five overlapping 2 x 2 boxes, the middle one
// weighted 0.5 and the corners 0.125 each
[shader("fragment")]
float4 downsampleMain(FullscreenOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 t = post.texel;
    float3 a = tap(first, uv + t * float2(-2.0, -2.0));
    float3 b = tap(first, uv + t * float2(0.0, -2.0));
    float3 c = tap(first, uv + t * float2(2.0, -2.0));
    float3 d = tap(first, uv + t * float2(-1.0, -1.0));
    float3 e = tap(first, uv + t * float2(1.0, -1.0));
    float3 f = tap(first, uv + t * float2(-2.0, 0.0));
    float3 g = tap(first, uv);
    float3 h = tap(first, uv + t * float2(2.0, 0.0));
    float3 i = tap(first, uv + t * float2(-1.0, 1.0));
    float3 j = tap(first, uv + t * float2(1.0, 1.0));
    float3 k = tap(first, uv + t * float2(-2.0, 2.0));
    float3 l = tap(first, uv + t * float2(0.0, 2.0));
    float3 m = tap(first, uv + t * float2(2.0, 2.0));
    float3 result;
    if (post.mode == 1) {
        result = karis(d, e, i, j) * 0.5 + karis(a, b, f, g) * 0.125 + karis(b, c, g, h) * 0.125
            + karis(f, g, k, l) * 0.125 + karis(g, h, l, m) * 0.125;
    } else {
        result = (d + e + i + j) * 0.125 + (a + b + f + g) * 0.03125 + (b + c + g + h) * 0.03125
            + (f + g + k + l) * 0.03125 + (g + h + l + m) * 0.03125;
    }
    // A pixel gone to infinity or NaN would spread over the whole chain
    result = min(max(result, float3(0.0, 0.0, 0.0)), float3(65000.0, 65000.0, 65000.0));
    return float4(result, 1.0);
}

// The upsample: this level's own downsample ('first') plus the level below
// ('second', its texel 'texel') tent-filtered up
[shader("fragment")]
float4 upsampleMain(FullscreenOutput input) : SV_Target {
    float2 uv = input.uv;
    float2 t = post.texel;
    float3 s = tap(second, uv + t * float2(-1.0, -1.0)) + tap(second, uv + t * float2(1.0, -1.0))
        + tap(second, uv + t * float2(-1.0, 1.0)) + tap(second, uv + t * float2(1.0, 1.0));
    float3 edges = tap(second, uv + t * float2(0.0, -1.0)) + tap(second, uv + t * float2(-1.0, 0.0))
        + tap(second, uv + t * float2(1.0, 0.0)) + tap(second, uv + t * float2(0.0, 1.0));
    float3 up = (s + edges * 2.0 + tap(second, uv) * 4.0) * (1.0 / 16.0);
    return float4(tap(first, uv) + up, 1.0);
}

// ---- Tone mapping ----

float3 agx(float3 c) {
    // Into AgX's inset space
    float3 x = float3(
        0.842479062253094 * c.r + 0.0784335999999992 * c.g + 0.0792237451477643 * c.b,
        0.0423282422610123 * c.r + 0.878468636469772 * c.g + 0.0791661274605434 * c.b,
        0.0423756549057051 * c.r + 0.0784336 * c.g + 0.879142973793104 * c.b);
    // A log2 encoding from -12.47 to +4.03 stops about middle grey
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    x = clamp(log2(max(x, float3(1e-10, 1e-10, 1e-10))), minEv, maxEv);
    x = (x - minEv) / (maxEv - minEv);
    // The sigmoid, as Wrensch's sixth-order polynomial
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    // Out of the inset space
    float3 y = float3(
        1.19687900512017 * x.r - 0.0980208811401368 * x.g - 0.0990297440797205 * x.b,
        -0.0528968517574562 * x.r + 1.15190312990417 * x.g - 0.0989611768448433 * x.b,
        -0.0529716355144438 * x.r - 0.0980434501171241 * x.g + 1.15107367264116 * x.b);
    // AgX's output is display-encoded (a 2.2 power): made linear again for
    // the sRGB target to encode
    return pow(saturate(y), float3(2.2, 2.2, 2.2));
}

float3 acesFitted(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// sRGB's encoding and its inverse (IEC 61966-2-1), what a display-referred
// table or a grain is applied in
float3 toSrgb(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, float3(1e-6, 1e-6, 1e-6)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return lerp(lo, hi, step(float3(0.0031308, 0.0031308, 0.0031308), c));
}

float3 fromSrgb(float3 c) {
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return lerp(lo, hi, step(float3(0.04045, 0.04045, 0.04045), c));
}

// A hash of a pixel and a frame, 0 to 1 (Jarzynski and Olano, "Hash
// Functions for GPU Rendering", 2020: PCG)
float hash01(uint2 p, uint frameNumber) {
    uint v = p.x * 1664525u + p.y * 1013904223u + frameNumber * 747796405u + 2891336453u;
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967295.0);
}

[shader("fragment")]
float4 toneMapMain(FullscreenOutput input) : SV_Target {
    float3 scene = tap(first, input.uv);
    float3 bloom = tap(second, input.uv) * post.bloomScale;
    float exposure = post.exposure;
    if ((post.flags & FINISH_EXPOSURE) != 0) {
        exposure = exposureBuffer[0];
    }
    float3 c = lerp(scene, bloom, post.strength) * exposure;
    if ((post.flags & FINISH_VIGNETTE) != 0) {
        // The distance to the middle, 1 at the corner; the fall begins at the
        // radius and is smoothest over 'softness' of the way to the corner
        float2 q = (input.uv - 0.5) * 2.0;
        float r = length(q) * 0.70710678;
        float t = saturate((r - post.vignette.y) / max(post.vignette.z, 1e-4));
        c = c * (1.0 - post.vignette.x * t * t * (3.0 - 2.0 * t));
    }
    if (post.toneMapper == 1) {
        c = agx(c);
    } else if (post.toneMapper == 2) {
        c = acesFitted(c);
    } else if (post.toneMapper == 3) {
        c = c / (1.0 + c);
    } else {
        c = saturate(c);
    }
    if ((post.flags & (FINISH_GRADE | FINISH_GRAIN)) != 0) {
        float3 v = toSrgb(c);
        if ((post.flags & FINISH_GRADE) != 0) {
            if (post.gamma.w > 0.0) {
                float3 looked = gradingTable.SampleLevel(linearClamp, v * post.gain.w + post.extra.x, 0.0).rgb;
                v = lerp(v, looked, post.gamma.w);
            }
            // Lift raises the blacks (towards 1 as it grows), gain scales,
            // gamma bends the middle (above 1 brightens it)
            v = saturate(post.gain.rgb * (v + post.lift.rgb * (1.0 - v)));
            v = pow(v, float3(1.0, 1.0, 1.0) / max(post.gamma.rgb, float3(1e-3, 1e-3, 1e-3)));
            float y = dot(v, float3(0.2126, 0.7152, 0.0722));
            v = saturate(lerp(float3(y, y, y), v, post.lift.w));
        }
        if ((post.flags & FINISH_GRAIN) != 0) {
            uint2 pixel = uint2(input.position.xy);
            uint frameNumber = uint(post.extra.y);
            // Triangular noise, -1 to 1, strongest at the middle tones
            float n = hash01(pixel, frameNumber) + hash01(pixel + uint2(7u, 13u), frameNumber) - 1.0;
            float mid = 4.0 * v.g * (1.0 - v.g);
            v = saturate(v + n * post.vignette.w * (0.35 + 0.65 * mid));
        }
        c = fromSrgb(v);
    }
    return float4(c, 1.0);
}
