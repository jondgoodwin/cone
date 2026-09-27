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
// Group 0: two textures (what a pass reads; the second only by the upsample
// and the tone map) and a linear, clamped sampler. The immediates: the
// source's texel size, the mode, the bloom's strength, the exposure and the
// tone mapper.

struct PostImmediates {
    float2 texel;                  // 0: the size of a texel of 'first', in uv
    uint mode;                     // 8: 0 plain downsample, 1 the first (Karis's average)
    float strength;                // 12: the bloom's share of the tone-mapped frame
    float exposure;                // 16
    uint toneMapper;               // 20: 0 none, 1 AgX, 2 ACES, 3 Reinhard
    float bloomScale;              // 24: what makes the chain's top its levels' mean (1 / the levels)
    float pad;                     // 28
};

[[vk::binding(0, 0)]] Texture2D first;
[[vk::binding(1, 0)]] Texture2D second;
[[vk::binding(2, 0)]] SamplerState linearClamp;
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

[shader("fragment")]
float4 toneMapMain(FullscreenOutput input) : SV_Target {
    float3 scene = tap(first, input.uv);
    float3 bloom = tap(second, input.uv) * post.bloomScale;
    float3 c = lerp(scene, bloom, post.strength) * post.exposure;
    if (post.toneMapper == 1) {
        c = agx(c);
    } else if (post.toneMapper == 2) {
        c = acesFitted(c);
    } else if (post.toneMapper == 3) {
        c = c / (1.0 + c);
    } else {
        c = saturate(c);
    }
    return float4(c, 1.0);
}
