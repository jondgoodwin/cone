// The render package's image-based lighting, made from the sky by
// full-screen passes (the gpu package has no compute): each pass draws one
// triangle over one face of one level of a cube map, or over the BRDF table,
// and its fragment shader works out that texel. Compiled ahead of time to
// ibl.spv by tools/shaders/shaders.py and embedded in lighting.cone.
//
// - skyCubeMain: the sky (no sun disc) into a face of the environment cube.
// - downsampleMain: a level of that cube from the one above it: the texel's
//   direction sampled bilinearly in the level above, where it falls on the
//   corner of four texels, so the average of the four.
// - prefilterMain: a level of the specular cube, for the roughness the level
//   stands for: the environment convolved with GGX by importance sampling,
//   Karis's split sum with n = v = r, each sample read at the level of the
//   environment whose texels are the size of its share of the sphere
//   (Krivanek and Colbert's filtered importance sampling), so a few samples
//   give no noise.
// - irradianceMain: the diffuse cube, the environment's cosine-weighted mean
//   radiance (irradiance over pi), so a Lambertian surface reflects its
//   albedo times it.
// - brdfTableMain: the split sum's table, over (n.v, roughness): the scale
//   and bias to the reflectance at normal incidence (Karis's A and B).
//
// Group 0: the sky's parameters, the cube a pass reads, and a linear sampler
// with linear mip filtering. The immediates: which face, the roughness (or
// the level read), the size of the level drawn and the number of samples.
//
// Sources: Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013; Krivanek
// and Colbert, "Real-time Shading with Filtered Importance Sampling", EGSR
// 2008; Lagarde and de Rousiers, "Moving Frostbite to Physically Based
// Rendering", SIGGRAPH 2014.
//
// The sky module imports noise (for its clouds), hence precise arithmetic:
// slangc: -fp-mode precise

import brdf;
import sky;

struct BakeImmediates {
    uint face;                     // 0: the cube face drawn (+x, -x, +y, -y, +z, -z)
    float roughness;               // 4: prefilterMain's roughness; irradianceMain's level read
    float size;                    // 8: the size of the level drawn into, in texels a side
    uint samples;                  // 12
};

[[vk::binding(0, 0)]] ConstantBuffer<SkyParams> skyParams;
[[vk::binding(1, 0)]] TextureCube source;
[[vk::binding(2, 0)]] SamplerState linearSampler;
[[vk::push_constant]] ConstantBuffer<BakeImmediates> bake;

struct FullscreenOutput {
    float4 position : SV_Position;
};

// One triangle covering the target
[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// The fragment's texel as uv, 0 to 1 (u across, v down the rows): its
// position over the target's size, which is the size of what is drawn
float2 texelUv(float4 position, float2 size) {
    return position.xy / size;
}

// The size of the level drawn into (square)
float2 targetSize() {
    return float2(bake.size, bake.size);
}

[shader("fragment")]
float4 skyCubeMain(FullscreenOutput input) : SV_Target {
    float3 dir = cubeDirection(bake.face, texelUv(input.position, targetSize()));
    return float4(skyRadiance(dir, skyParams, false), 1.0);
}

// 'source' is a cube view of the level above only
[shader("fragment")]
float4 downsampleMain(FullscreenOutput input) : SV_Target {
    float3 dir = cubeDirection(bake.face, texelUv(input.position, targetSize()));
    return float4(source.SampleLevel(linearSampler, dir, 0.0).rgb, 1.0);
}

// The environment cube's level 0, in texels a side
static const float ENVIRONMENT_SIZE = 256.0;

[shader("fragment")]
float4 prefilterMain(FullscreenOutput input) : SV_Target {
    float3 n = cubeDirection(bake.face, texelUv(input.position, targetSize()));
    float roughness = bake.roughness;
    // The mirror level: the environment read at the texel's own size
    float mirrorLevel = max(log2(ENVIRONMENT_SIZE / bake.size), 0.0);
    if (roughness <= 0.0) {
        return float4(source.SampleLevel(linearSampler, n, mirrorLevel).rgb, 1.0);
    }
    float a = roughness * roughness;
    float texelSolidAngle = 4.0 * PI / (6.0 * ENVIRONMENT_SIZE * ENVIRONMENT_SIZE);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    uint count = bake.samples;
    for (uint i = 0; i < count; i++) {
        float3 h = sampleGGX(hammersley(i, count), a, n);
        float noh = saturate(dot(n, h));
        float3 l = 2.0 * noh * h - n;
        float nol = dot(n, l);
        if (nol > 0.0) {
            // With v = n, the pdf of l is D / 4
            float pdf = distributionGGX(noh, a) * 0.25;
            float sampleSolidAngle = 1.0 / (float(count) * pdf + 1e-4);
            float level = max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, mirrorLevel);
            sum += source.SampleLevel(linearSampler, l, level).rgb * nol;
            weight += nol;
        }
    }
    return float4(sum / max(weight, 1e-4), 1.0);
}

[shader("fragment")]
float4 irradianceMain(FullscreenOutput input) : SV_Target {
    float3 n = cubeDirection(bake.face, texelUv(input.position, targetSize()));
    float3 sum = float3(0.0, 0.0, 0.0);
    uint count = bake.samples;
    for (uint i = 0; i < count; i++) {
        float3 l = sampleCosine(hammersley(i, count), n);
        sum += source.SampleLevel(linearSampler, l, bake.roughness).rgb;
    }
    return float4(sum / float(count), 1.0);
}

[shader("fragment")]
float4 brdfTableMain(FullscreenOutput input) : SV_Target {
    float2 uv = texelUv(input.position, targetSize());
    float nov = max(uv.x, 1e-3);
    float roughness = uv.y;
    float a = max(roughness * roughness, 1e-4);
    float3 v = float3(sqrt(1.0 - nov * nov), 0.0, nov);
    float3 n = float3(0.0, 0.0, 1.0);
    float sa = 0.0;
    float sb = 0.0;
    uint count = bake.samples;
    for (uint i = 0; i < count; i++) {
        float3 h = sampleGGX(hammersley(i, count), a, n);
        float voh = saturate(dot(v, h));
        float3 l = 2.0 * voh * h - v;
        float nol = saturate(l.z);
        float noh = saturate(h.z);
        if (nol > 0.0) {
            // G times v.h over (n.h n.v), from the visibility V = G / (4 n.l n.v)
            float g = visibilitySmithGGX(nov, nol, a) * 4.0 * nol * voh / max(noh, 1e-4);
            float fc = pow(1.0 - voh, 5.0);
            sa += (1.0 - fc) * g;
            sb += fc * g;
        }
    }
    return float4(sa / float(count), sb / float(count), 0.0, 1.0);
}
