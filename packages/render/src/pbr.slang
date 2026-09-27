// slangc: -fp-mode precise
//
// The render package's physically based material, and the sky drawn behind
// the scene. Compiled ahead of time to pbr.spv by tools/shaders/shaders.py
// and embedded in lighting.cone. It imports the noise package's module, whose
// functions agree bit for bit with the CPU's only when no multiply and add
// are fused, hence '-fp-mode precise' above (noise's README, "Determinism").
//
// The material, layered from the bottom up (the OpenPBR and Filament
// shape):
// - the base: a base colour, metallic and roughness (glTF 2.0's
//   metallic-roughness model): Lambert diffuse for a dielectric, and a GGX
//   specular lobe (Smith's height-correlated visibility, Schlick's Fresnel
//   from 0.04, or the base colour for a metal), with the energy a single
//   scattering loses at high roughness given back (Fdez-Aguera's, as
//   Filament does);
// - a clear coat over it: a second, smoother GGX lobe of a dielectric of
//   index 1.5 (Kelemen's visibility), the base under it dimmed by what the
//   coat reflects;
// - a thin film over the coat (Belcour and Barla), which replaces the coat's
//   Fresnel with the film's interference colours; its thickness in
//   nanometres varies over the surface as domain-warped fractal noise of the
//   model's own coordinates, drifting with time, so the colours swirl.
//
// Lit by the one directional light, and by the sky through image-based
// lighting: a diffuse cube (irradiance), a specular cube prefiltered by
// roughness, one level a roughness step, and the split sum's table.
//
// Group 0 is the frame's, group 1 the material's (its parameters, a texture
// that multiplies the base colour, a sampler), group 2 the environment's
// (the sky's parameters, the cubes, the table and their sampler). The
// immediates are the draw's: its model matrix and a tint.

import brdf;
import sky;
import noise;

struct Frame {
    float4x4 viewProjection;       // 0
    float4 cameraPosition;         // 64: xyz
    float4 lightDirection;         // 80: xyz towards the light, unit
    float4 lightColor;             // 96: rgb
    float4 ambient;                // 112: rgb (the Blinn-Phong material's)
    float4x4 inverseViewProjection;   // 128
    float4 viewport;               // 192: width, height in pixels; time in seconds; the environment's strength
};

struct PbrParams {
    float4 baseColor;              // 0: rgba, linear
    float4 surface;                // 16: metallic, roughness, clear coat (0 to 1), the coat's roughness
    float4 film;                   // 32: the film's least and greatest thickness (nm), its index, 1 if there is a film
    float4 filmNoise;              // 48: the noise's scale (a unit of model space), drift a second, warp, seed
};

struct Immediates {
    float4x4 model;                // 0-63
    float4 tint;                   // 64-79
};

[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;
[[vk::binding(0, 1)]] ConstantBuffer<PbrParams> material;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;
[[vk::binding(0, 2)]] ConstantBuffer<SkyParams> skyParams;
[[vk::binding(1, 2)]] TextureCube irradianceCube;
[[vk::binding(2, 2)]] TextureCube specularCube;
[[vk::binding(3, 2)]] Texture2D brdfTable;
[[vk::binding(4, 2)]] SamplerState environmentSampler;
[[vk::push_constant]] ConstantBuffer<Immediates> draw;

// The specular cube's levels: roughness 0 at level 0, 1 at the last
static const float SPECULAR_LEVELS = 6.0;
static const float COAT_IOR = 1.5;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float3 local : TEXCOORD1;      // the model's own coordinates, where the film's noise is read
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 world = mul(draw.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    // The normal by the cofactor matrix of the model's 3 x 3, as lit.slang
    float3 a0 = float3(draw.model[0][0], draw.model[1][0], draw.model[2][0]);
    float3 a1 = float3(draw.model[0][1], draw.model[1][1], draw.model[2][1]);
    float3 a2 = float3(draw.model[0][2], draw.model[1][2], draw.model[2][2]);
    output.normal = cross(a1, a2) * input.normal.x + cross(a2, a0) * input.normal.y
        + cross(a0, a1) * input.normal.z;
    output.uv = input.uv;
    output.local = input.position;
    return output;
}

// The specular light the environment reflects: the prefiltered cube at the
// roughness's level times the split sum's scale and bias to 'f0'
float3 environmentSpecular(float3 r, float roughness, float3 f0, float2 ab) {
    float3 prefiltered = specularCube.SampleLevel(environmentSampler, r, roughness * (SPECULAR_LEVELS - 1.0)).rgb;
    return prefiltered * (f0 * ab.x + ab.y);
}

// The film's thickness at model point 'p', at time 't'
float filmThickness(float3 p, float t) {
    float3 q = p * material.filmNoise.x + float3(0.31, 1.0, 0.17) * (t * material.filmNoise.y);
    float w = warp3(q, uint(material.filmNoise.w), 2, material.filmNoise.z);
    float k = saturate(0.5 + 0.8 * w);
    return lerp(material.film.x, material.film.y, k);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 n = normalize(input.normal);
    float3 v = normalize(frame.cameraPosition.xyz - input.world);
    // A normal facing away (at a silhouette, interpolated) is turned to face
    float nov = dot(n, v);
    if (nov < 0.0) {
        n = normalize(n - v * (nov * 1.01));
        nov = dot(n, v);
    }
    nov = clamp(nov, 1e-4, 1.0);
    float3 l = frame.lightDirection.xyz;
    float3 h = normalize(l + v);
    float nol = saturate(dot(n, l));
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float3 r = reflect(-v, n);

    float4 base = material.baseColor * baseTexture.Sample(baseSampler, input.uv) * draw.tint;
    float metallic = material.surface.x;
    float roughness = clamp(material.surface.y, 0.045, 1.0);
    float a = roughness * roughness;
    float coat = material.surface.z;
    float coatRoughness = clamp(material.surface.w, 0.045, 1.0);
    float coatA = coatRoughness * coatRoughness;
    float3 diffuseColor = base.rgb * (1.0 - metallic);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
    float2 ab = brdfTable.SampleLevel(environmentSampler, float2(nov, roughness), 0.0).xy;
    // Multiple scattering's energy, given back to the single-scattering lobe
    float3 energy = 1.0 + f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0);

    // The coat's Fresnel: the film's colours, or the plain interface's
    float coatF0 = iorToF0(COAT_IOR);
    bool film = material.film.w > 0.5;
    float thickness = 0.0;
    if (film) {
        thickness = filmThickness(input.local, frame.viewport.z);
    }
    float3 coatFv = film ? thinFilmReflectance(nov, material.film.z, COAT_IOR, thickness)
        : float3(1.0, 1.0, 1.0) * fresnelSchlick1(coatF0, nov);
    float3 coatFl = film ? thinFilmReflectance(loh, material.film.z, COAT_IOR, thickness)
        : float3(1.0, 1.0, 1.0) * fresnelSchlick1(coatF0, loh);
    coatFv = coatFv * coat;
    coatFl = coatFl * coat;

    // The sun
    float3 direct = float3(0.0, 0.0, 0.0);
    if (nol > 0.0) {
        float3 f = fresnelSchlick(f0, loh);
        float3 specular = distributionGGX(noh, a) * visibilitySmithGGX(nov, nol, a) * f * energy;
        float3 diffuse = diffuseColor * (1.0 / PI);
        float3 baseLobe = (diffuse + specular) * (1.0 - coatFl);
        float3 coatLobe = distributionGGX(noh, coatA) * visibilityKelemen(loh) * coatFl;
        direct = (baseLobe + coatLobe) * frame.lightColor.rgb * nol;
    }

    // The sky
    float strength = frame.viewport.w;
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb;
    float3 envSpecular = environmentSpecular(r, roughness, f0, ab) * energy;
    float3 envBase = (diffuseColor * irradiance + envSpecular) * (1.0 - coatFv);
    float2 coatAb = brdfTable.SampleLevel(environmentSampler, float2(nov, coatRoughness), 0.0).xy;
    float3 coatPrefiltered = specularCube.SampleLevel(environmentSampler, r,
        coatRoughness * (SPECULAR_LEVELS - 1.0)).rgb;
    // The coat's own Fresnel already holds its angle: the table's scale and
    // bias add only the lobe's energy
    float3 envCoat = coatPrefiltered * coatFv * (coatAb.x + coatAb.y);
    float3 ambient = (envBase + envCoat) * strength;

    return float4(direct + ambient, base.a);
}

// ---- The sky behind the scene ----

struct SkyOutput {
    float4 position : SV_Position;
};

// One triangle over the target, at the far plane (depth 1)
[shader("vertex")]
SkyOutput backgroundVertex(uint id : SV_VulkanVertexID) {
    SkyOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 1.0, 1.0);
    return output;
}

[shader("fragment")]
float4 backgroundFragment(SkyOutput input) : SV_Target {
    // The pixel's direction from the camera: its point on the far plane
    // through the inverse view-projection (clip space's y is up, rows down)
    float2 ndc = float2(input.position.x / frame.viewport.x * 2.0 - 1.0,
        1.0 - input.position.y / frame.viewport.y * 2.0);
    float4 far = mul(frame.inverseViewProjection, float4(ndc, 1.0, 1.0));
    float3 dir = far.xyz / far.w - frame.cameraPosition.xyz;
    return float4(skyRadiance(dir, skyParams, true), 1.0);
}
