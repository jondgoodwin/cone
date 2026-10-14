// The render package's scene capture and the tables the extended material
// reads, full-screen passes (the gpu package has no compute here), compiled
// ahead of time to capture.spv by tools/shaders/shaders.py and embedded in
// scenecapture.cone.
//
// THE CAPTURE of the frame drawn so far, for screen-space reflections and for
// glass:
// - copyMain: the frame's colour, texel for texel, into level 0 of the colour
//   pyramid (the frame is the half-float target, already multiplied by the
//   exposure; nothing is changed);
// - downMain: each level below from the one above, Jimenez's 13-tap
//   downsample ("Next Generation Post Processing in Call of Duty: Advanced
//   Warfare", SIGGRAPH 2014: four overlapping 2 x 2 boxes and a centre box, so
//   no texel of the upper level is missed and the filter is smooth), with
//   Karis's average on the first step down ("Brian Karis, "High Quality
//   Bloom", 2014: each tap weighted by 1 / (1 + luma)) so a lone hot pixel (the
//   sun in a reflection) does not flash through the blurrier levels;
// - depthMain: the scene's depth buffer into an r32float colour texture, for the
//   ray march to read with the depth buffer in use by the pass.
//
// THE TABLES, baked once:
// - diffusionBakeMain: the pre-integrated diffuse of a translucent surface
//   (Penner and Borshukov, "Pre-Integrated Skin Shading", GPU Pro 2, 2011). A
//   point on a curved surface receives light scattered from the lit and
//   unlit points about it on the ring of the sphere through it; for a ring of
//   radius rho, the light at angle theta is
//       D(theta, rho) = int cos+(theta + x) R(2 rho sin(x / 2)) dx / int R(2 rho sin(x / 2)) dx
//   with R the scattering profile. R is the green channel of d'Eon and
//   Luebke's sum of six Gaussians for human skin ("Advanced Techniques for
//   Realistic Real-Time Skin Rendering", GPU Gems 3, ch. 14: variances 0.0064
//   0.0484 0.187 0.567 1.99 7.41 mm2, weights 0.455 0.336 0.198 0.007
//   0.004 0), the width of the other channels and of other stuff than
//   skin being the material's scale of it. The table's u is n.l (-1 to 1)
//   as 0 to 1, its v the square root of the curvature in 1/mm up to 0.5; the
//   ring is integrated over x with the sample spacing a power of 4, so the
//   narrow Gaussians of a nearly flat surface are not missed;
// - sheenBakeMain: the Charlie sheen lobe's directional albedo, the integral
//   of D V cos over the hemisphere of light directions, over (n.v, roughness),
//   by midpoint quadrature (64 x 64 directions), so the sheen can take from
//   the layer below exactly what it reflects.
//
// Group 0 of the colour passes is the source (binding 0, a texture; 1, a
// sampler); of the depth pass, the depth (binding 2); the bakes bind nothing.

import lobes;

struct CaptureImmediates {
    float4 info;                   // 0: downMain: the source level's texel size (x, y), 1 if Karis's average is wanted (z)
};

[[vk::push_constant]] ConstantBuffer<CaptureImmediates> capture;

[[vk::binding(0, 0)]] Texture2D source;
[[vk::binding(1, 0)]] SamplerState sourceSampler;
[[vk::binding(2, 0)]] Texture2D<float> depthSource;

struct FullscreenOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 copyMain(FullscreenOutput input) : SV_Target {
    return source.Load(int3(int2(input.position.xy), 0));
}

float karis(float3 c) {
    return 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
}

float3 tap(float2 uv, float2 offset) {
    return source.SampleLevel(sourceSampler, uv + offset * capture.info.xy, 0.0).rgb;
}

[shader("fragment")]
float4 downMain(FullscreenOutput input) : SV_Target {
    // The pixel is the centre of a 2 x 2 block of the source: its uv in the source
    float2 uv = (floor(input.position.xy) * 2.0 + 1.0) * capture.info.xy;
    float3 a = tap(uv, float2(-2.0, -2.0));
    float3 b = tap(uv, float2(0.0, -2.0));
    float3 c = tap(uv, float2(2.0, -2.0));
    float3 d = tap(uv, float2(-2.0, 0.0));
    float3 e = tap(uv, float2(0.0, 0.0));
    float3 f = tap(uv, float2(2.0, 0.0));
    float3 g = tap(uv, float2(-2.0, 2.0));
    float3 h = tap(uv, float2(0.0, 2.0));
    float3 i = tap(uv, float2(2.0, 2.0));
    float3 j = tap(uv, float2(-1.0, -1.0));
    float3 k = tap(uv, float2(1.0, -1.0));
    float3 l = tap(uv, float2(-1.0, 1.0));
    float3 m = tap(uv, float2(1.0, 1.0));
    if (capture.info.z > 0.5) {
        // Karis: each of the five boxes weighted by the brightness of its own mean
        float3 g0 = (a + b + d + e) * 0.25;
        float3 g1 = (b + c + e + f) * 0.25;
        float3 g2 = (d + e + g + h) * 0.25;
        float3 g3 = (e + f + h + i) * 0.25;
        float3 g4 = (j + k + l + m) * 0.25;
        float w0 = karis(g0);
        float w1 = karis(g1);
        float w2 = karis(g2);
        float w3 = karis(g3);
        float w4 = karis(g4);
        float3 mean = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3) * 0.125 + g4 * w4 * 0.5;
        float wsum = (w0 + w1 + w2 + w3) * 0.125 + w4 * 0.5;
        return float4(mean / max(wsum, 1e-6), 1.0);
    }
    float3 mean = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    return float4(mean, 1.0);
}

[shader("fragment")]
float4 depthMain(FullscreenOutput input) : SV_Target {
    return float4(depthSource.Load(int3(int2(input.position.xy), 0)), 0.0, 0.0, 1.0);
}

// ---- The diffusion table ----

static const int GAUSSIANS = 5;
static const float GAUSS_VARIANCE[5] = { 0.0064, 0.0484, 0.187, 0.567, 1.99 };
static const float GAUSS_WEIGHT[5] = { 0.455, 0.336, 0.198, 0.007, 0.004 };

// The profile at distance 'r' (mm): the weighted sum of 2-D Gaussians, each
// with its own variance, normalised to unit volume
float scatterProfile(float r) {
    float sum = 0.0;
    for (int i = 0; i < GAUSSIANS; i++) {
        float v = GAUSS_VARIANCE[i];
        sum += GAUSS_WEIGHT[i] * exp(-r * r / (2.0 * v)) / (6.28318530718 * v);
    }
    return sum;
}

[shader("fragment")]
float4 diffusionBakeMain(FullscreenOutput input) : SV_Target {
    // The table is 128 wide and 64 tall: u is n.l as 0 to 1, v the root of the curvature
    float u = (input.position.x) / 128.0;
    float v = (input.position.y) / 64.0;
    float nol = u * 2.0 - 1.0;
    float theta = acos(clamp(nol, -1.0, 1.0));
    float perMm = v * v * 0.5;
    if (perMm < 1e-4) {
        return float4(saturate(nol), 0.0, 0.0, 1.0);
    }
    float rho = 1.0 / perMm;
    float lit = 0.0;
    float total = 0.0;
    const int SAMPLES = 192;
    for (int i = 0; i < SAMPLES; i++) {
        // x from 0 to pi, the spacing a power of 4 so the nearest rings are many
        float s0 = float(i) / float(SAMPLES);
        float s1 = float(i + 1) / float(SAMPLES);
        float x0 = 3.14159265359 * pow(s0, 4.0);
        float x1 = 3.14159265359 * pow(s1, 4.0);
        float x = 0.5 * (x0 + x1);
        float dx = x1 - x0;
        float r = 2.0 * rho * sin(0.5 * x);
        float weight = scatterProfile(r) * dx;
        // Both sides of the point: the angle theta moves by +x and -x
        lit += weight * (max(cos(theta + x), 0.0) + max(cos(theta - x), 0.0));
        total += weight * 2.0;
    }
    return float4(lit / max(total, 1e-12), 0.0, 0.0, 1.0);
}

// ---- The sheen's albedo ----

[shader("fragment")]
float4 sheenBakeMain(FullscreenOutput input) : SV_Target {
    // 32 x 32: u is n.v, v the sheen's perceptual roughness
    float nov = max((input.position.x) / 32.0, 1e-3);
    float roughness = max((input.position.y) / 32.0, 0.07);
    float alpha = roughness * roughness;
    float3 vv = float3(sqrt(1.0 - nov * nov), 0.0, nov);
    float sum = 0.0;
    const int THETA = 48;
    const int PHI = 96;
    for (int i = 0; i < THETA; i++) {
        float cosL = (float(i) + 0.5) / float(THETA);
        float sinL = sqrt(1.0 - cosL * cosL);
        for (int j = 0; j < PHI; j++) {
            float phi = (float(j) + 0.5) / float(PHI) * 6.28318530718;
            float3 l = float3(sinL * cos(phi), sinL * sin(phi), cosL);
            float3 h = normalize(vv + l);
            float noh = saturate(h.z);
            // dOmega = sin(theta) dtheta dphi = d(cos theta) dphi; the cosine's own factor is n.l
            sum += sheenDistribution(noh, alpha) * sheenVisibility(nov, cosL) * cosL;
        }
    }
    float albedo = sum * (1.0 / float(THETA)) * (6.28318530718 / float(PHI));
    return float4(saturate(albedo), 0.0, 0.0, 1.0);
}
