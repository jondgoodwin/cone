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
//   coat reflects; everywhere, or, for a wet ground, in patches where fBm
//   of the model's coordinates rises above a level (the material's 'wet');
// - a thin film over the coat (Belcour and Barla), which replaces the coat's
//   Fresnel with the film's interference colours; its thickness in
//   nanometres varies over the surface as domain-warped fractal noise of the
//   model's own coordinates, drifting with time, so the colours swirl.
//
// - foliage, on top of any of that, each off by default: BACKLIT
//   TRANSLUCENCY, light that reaches a thin surface's far side and comes out
//   of its near side in the surface's own colour (a leaf against the sun
//   glows like stained glass); and FLUTTER, each leaf of a tree turning a few
//   degrees about its stem on its own phase, in the vertex stage.
//
// Lit by the one directional light, by the scene's point and spot lights (the
// lights of the fragment's cluster, locallights.slang: the same lobes, the
// base's and the coat's, for each, the highlight about the point of a
// bulb's sphere nearest the reflection), and by the sky through image-based
// lighting: a diffuse cube (irradiance), a specular cube prefiltered by
// roughness, one level a roughness step, and the split sum's table.
//
// The base's inputs may come from glTF 2.0's maps: the base colour, a
// metallic-roughness texture multiplying the parameters, an occlusion
// texture dimming the environment's light, and a tangent-space normal map,
// turned into the surface by tangents found from screen-space derivatives
// (the meshes carry none).
//
// It keeps the renderer's one lighting convention (renderframe.slang): the
// sun's colour is irradiance, the environment and the emissive colour are
// radiance, and what it outputs is multiplied by the frame's exposure.
//
// Its alpha is used as its alpha mode says (material.cone): not at all; to
// cut out a fragment below the cut-off; or, see-through, as premultiplied
// alpha: the diffuse light (the sun's and the environment's) times alpha,
// the reflections and the glow added whole (glass).
//
// Group 0 is the frame's (renderframe.slang), group 1 the material's (its
// parameters, a texture that multiplies the base colour, a sampler, then
// the metallic-roughness, normal and occlusion textures), group 2 the
// environment's (the sky's parameters, the cubes, the table and their
// sampler). Each drawn copy's model matrix and tint are its instance's,
// in group 0 (renderframe.slang).

import brdf;
import sky;
import noise;
import renderframe;
import skinning;
import fade;
import shadowing;
import flutter;
import locallights;
#ifdef PBR_EXTENDED
import lobes;
import scenecapture;
#endif

// The sun's light (the direct terms and the foliage's light through a leaf,
// not the environment's) is multiplied by how much of the sun reaches the
// point, from the cascaded shadow map (shadowing.slang): 1 when shadows
// are off.

struct PbrParams {
    float4 baseColor;              // 0: rgba, linear
    float4 surface;                // 16: metallic, roughness, clear coat (0 to 1), the coat's roughness
    float4 film;                   // 32: the film's least and greatest thickness (nm), its index, 1 if there is a film
    float4 filmNoise;              // 48: the noise's scale (a unit of model space), drift a second, warp, seed
    float4 wet;                    // 64: where the coat lies: fBm's scale (0: everywhere), level, softness, seed
    float4 maps;                   // 80: the normal map's scale (0: no normal map), the occlusion map's strength,
                                   //     the alpha mode (0 opaque, 1 cut out, 2 see-through), the cut-off
    float4 emissive;               // 96: rgb radiance (nits in physical units), its strength multiplied in; w the light layers (locallights.slang)
    float4 foliage;                // 112: the translucency (0: none), its lobe's exponent, the flutter's most turn (radians, 0: none),
                                   //      and w, the base's specular scale (1: as a dielectric's; a conifer's cards 0: see below)
#ifdef PBR_EXTENDED
    // The extended lobes (fragmentExtended, below), read only by pbrx.spv
    float4 sheen;                  // 128: rgb the sheen's colour (black: none), w its perceptual roughness
    float4 lobes;                  // 144: x the anisotropy (-1 to 1), y its rotation about the normal (radians),
                                   //      z the subsurface share (0 to 1), w how far wet patches darken the base (0 to 1)
    float4 subsurface;             // 160: rgb the scattering's width in each channel relative to the table's, w its scale (1: skin)
    float4 glass;                  // 176: x the share of light that passes through (0: not glass), y the index of refraction,
                                   //      z the thickness the light crosses (metres), w the dispersion (glTF's KHR_materials_dispersion)
    float4 attenuation;            // 192: rgb the colour light has after crossing 'attenuationDistance' (metres, w)
    float4 reflection;             // 208: x the screen-space reflection's strength (0: none), y the share of the sun's
                                   //      caustics it receives (0: none), zw spare
#endif
};

static const uint ALPHA_MASK = 1;
static const uint ALPHA_BLEND = 2;

[[vk::binding(0, 1)]] ConstantBuffer<PbrParams> material;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;
// glTF 2.0's other maps, each white (or, the normal map, unread) when the
// material has none: metallic-roughness (green roughness, blue metallic,
// multiplying the parameters), a tangent-space normal map (+y towards +v),
// occlusion (red)
[[vk::binding(3, 1)]] Texture2D metallicRoughnessTexture;
[[vk::binding(4, 1)]] Texture2D normalTexture;
[[vk::binding(5, 1)]] Texture2D occlusionTexture;
// Group 2's uniform buffer: the sky's parameters, then, for a reflection
// probe's group (views.cone), its box and position. boxMin.w is 1 for a
// probe and 0 for the sky's own group (all of these zero).
struct EnvParams {
    SkyParams sky;
    float4 boxMin;                 // 144: xyz the probe's box, its minimum corner; w 1: box projection on
    float4 boxMax;                 // 160: xyz the maximum corner
    float4 probePosition;          // 176: xyz where the cube was captured
};
[[vk::binding(0, 2)]] ConstantBuffer<EnvParams> envParams;

// The direction 'r' read from a probe's cube for a reflection leaving the
// world point 'p': the ray p + t r is carried to where it leaves the
// probe's box, and the cube is read towards that point from where it was
// captured (box-projected parallax correction: Lagarde, "Image-based
// Lighting approaches and parallax-corrected cubemap", SIGGRAPH 2012 course
// notes, after Bjorke's "Image-Based Lighting", GPU Gems 2000). The sky's
// own group is unprojected.
float3 probeDirection(float3 r, float3 p) {
    if (envParams.boxMin.w < 0.5) {
        return r;
    }
    float3 safe = float3(abs(r.x) < 1e-6 ? 1e-6 : r.x, abs(r.y) < 1e-6 ? 1e-6 : r.y, abs(r.z) < 1e-6 ? 1e-6 : r.z);
    float3 first = (envParams.boxMax.xyz - p) / safe;
    float3 second = (envParams.boxMin.xyz - p) / safe;
    float3 farthest = max(first, second);
    float t = min(min(farthest.x, farthest.y), farthest.z);
    float3 hit = p + r * t;
    return normalize(hit - envParams.probePosition.xyz);
}
[[vk::binding(1, 2)]] TextureCube irradianceCube;
[[vk::binding(2, 2)]] TextureCube specularCube;
[[vk::binding(3, 2)]] Texture2D brdfTable;
[[vk::binding(4, 2)]] SamplerState environmentSampler;

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
    nointerpolation float4 tint : COLOR;
    nointerpolation float fade : TEXCOORD7;   // fade.slang: a copy fading in or out (0: whole)
#ifdef PBR_EXTENDED
    float4 tangent : TEXCOORD2;    // the mesh's tangent in world coordinates (xyz) and its handedness (w); 0: none
#endif
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, input.normal);
    output.uv = input.uv;
    output.local = input.position;
    output.tint = inst.tint;
    output.fade = instanceFade(id);
#ifdef PBR_EXTENDED
    output.tangent = float4(0.0, 0.0, 0.0, 0.0);
#endif
    return output;
}

#ifdef PBR_EXTENDED
// A mesh with tangents (gpumesh.cone) drawn with the extended material: a fourth
// vertex stream, each vertex's tangent (xyz along u, w the handedness), carried
// to the fragment stage, where the anisotropic highlight follows it
struct TangentInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : COLOR;
};

[shader("vertex")]
VertexOutput vertexTangent(TangentInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, input.normal);
    output.uv = input.uv;
    output.local = input.position;
    output.tint = inst.tint;
    output.fade = instanceFade(id);
    float3 t = mul((float3x3)inst.model, input.tangent.xyz);
    output.tangent = float4(normalize(t), input.tangent.w);
    return output;
}
#endif

// The same for a skinned mesh (skinning.slang): its position and normal
// carried by the vertex's blended bones first, then placed by the copy's
// model matrix. The film's noise is read at the vertex as modelled, so it
// stays on the surface as the bones move it.
[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    float4 world = mul(inst.model, float4(skinPoint(skin, input.position), 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, skinVector(skin, input.normal));
    output.uv = input.uv;
    output.local = input.position;
    output.tint = inst.tint;
    output.fade = 0.0;
#ifdef PBR_EXTENDED
    output.tangent = float4(0.0, 0.0, 0.0, 0.0);
#endif
    return output;
}

// ---- Flutter ----
//
// A mesh drawn with a flutter material carries a fourth vertex stream
// (location 3, the mesh's colors, gpumesh.cone): each leaf's turn about its
// pivot, 'flutterLeaf' in flutter.slang, which the shadow's casters share.

struct SwayInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 sway : COLOR;
};

[shader("vertex")]
VertexOutput vertexSway(SwayInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float3 position = input.position;
    float3 normal = input.normal;
    flutterLeaf(input.sway.xyz, input.sway.w, material.foliage.z, frame.viewport.z, position, normal);
    float4 world = mul(inst.model, float4(position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, normal);
    output.uv = input.uv;
    output.local = input.position;
    output.tint = inst.tint;
    output.fade = instanceFade(id);
#ifdef PBR_EXTENDED
    output.tangent = float4(0.0, 0.0, 0.0, 0.0);
#endif
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

// The normal 'n' turned by the normal map at 'uv', in the frame of tangents
// the position's and the uv's screen-space derivatives give (Schuler,
// "Normal Mapping without Precomputed Tangents", ShaderX5, 2006, and his 2013
// follow-up): no tangents in the mesh. The frame's sign is taken from the
// parallelogram's orientation, so it holds with y up or down on the screen.
float3 mapNormal(float3 n, float3 world, float2 uv) {
    float3 tn = normalTexture.Sample(baseSampler, uv).xyz * 2.0 - 1.0;
    tn.xy = tn.xy * material.maps.x;
    float3 dp1 = ddx(world);
    float3 dp2 = ddy(world);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, n);
    float3 dp1perp = cross(n, dp1);
    float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float s = dot(dp1, dp2perp) < 0.0 ? -1.0 : 1.0;
    float m = max(dot(t, t), dot(b, b));
    if (m < 1e-30) {
        return n;
    }
    float k = s * rsqrt(m);
    return normalize(t * (k * tn.x) + b * (k * tn.y) + n * tn.z);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    fadeClip(input.fade, input.position.xy);
    float3 n = normalize(input.normal);
    if (material.maps.x > 0.0) {
        n = mapNormal(n, input.world, input.uv);
    }
    float3 v = normalize(frame.cameraPosition.xyz - input.world);
    // A normal facing away (at a silhouette, interpolated) is turned to face
    float nov = dot(n, v);
    if (nov < 0.0) {
        n = normalize(n - v * (nov * 1.01));
        nov = dot(n, v);
    }
    nov = clamp(nov, 1e-4, 1.0);
    float3 l = frame.lightDirection.xyz;
    // The geometric normal, before the normal map and the silhouette's turn
    // have moved it, is the one the shadow's offset goes along
    float visible = sunVisibility(input.world, normalize(input.normal), l, input.position.xy);
    float3 h = normalize(l + v);
    float nol = saturate(dot(n, l));
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float3 r = probeDirection(reflect(-v, n), input.world);

    float4 base =material.baseColor * baseTexture.Sample(baseSampler, input.uv) * input.tint;
    uint alphaMode = uint(material.maps.z + 0.5);
    if (alphaMode == ALPHA_MASK) {
        if (base.a < material.maps.w) {
            discard;
        }
        base.a = 1.0;
    }
    float4 metalRough = metallicRoughnessTexture.Sample(baseSampler, input.uv);
    float metallic = material.surface.x * metalRough.b;
    float roughness = clamp(material.surface.y * metalRough.g, 0.045, 1.0);
    // Occlusion dims the environment's light, not the sun's (glTF's rule)
    float occlusion = 1.0 + material.maps.y * (occlusionTexture.Sample(baseSampler, input.uv).r - 1.0);
    float a = roughness * roughness;
    float coat = material.surface.z;
    if (material.wet.x > 0.0) {
        // Wet in patches: where fBm of the model's coordinates rises above the level
        float n = fbm3(input.local * material.wet.x, uint(material.wet.w), 4, 2.0, 0.5).value;
        coat *= smoothstep(material.wet.y - material.wet.z, material.wet.y + material.wet.z, n);
    }
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
    float3 directDiffuse = float3(0.0, 0.0, 0.0);
    if (nol > 0.0) {
        float3 f = fresnelSchlick(f0, loh);
        float3 specular = distributionGGX(noh, a) * visibilitySmithGGX(nov, nol, a) * f * energy * material.foliage.w;
        float3 diffuse = diffuseColor * (1.0 / PI);
        float3 baseLobe = (diffuse + specular) * (1.0 - coatFl);
        float3 coatLobe = distributionGGX(noh, coatA) * visibilityKelemen(loh) * coatFl;
        direct = (baseLobe + coatLobe) * frame.lightColor.rgb * (nol * visible);
        directDiffuse = diffuse * (1.0 - coatFl) * frame.lightColor.rgb * (nol * visible);
    }

    // The point and spot lights of the fragment's cluster (locallights.slang),
    // each the sun's terms again with its own direction and irradiance. The
    // diffuse term is Lambert's about the direction to the light's centre;
    // the specular lobes (base and coat) are found about the direction to
    // the point of the light's sphere nearest the reflection, widened by
    // Karis's normalisation, for a source with a radius. None of them is
    // shadowed (the sun's shadow map is the sun's). The foliage's light
    // through a leaf is the sun's and the sky's alone.
    if (lightParams.grid.w > 0.5) {
        uint materialLayers = uint(material.emissive.w + 0.5);
        uint2 cluster = fragmentCluster(input.world, input.position.xy);
        bool withSpecular = lightParams.slices.z > 0.5;
        for (uint k = 0; k < cluster.y; k++) {
            LocalLight light = loadLocalLight(lightClusters[cluster.x + k]);
            if (!lightReaches(light, materialLayers)) {
                continue;
            }
            LightHit hit;
            if (!lightAt(light, input.world, hit)) {
                continue;
            }
            float lnol = saturate(dot(n, hit.toLight));
            if (light.shadowView > -0.5 && (lnol > 0.0 || withSpecular)) {
                float lightVisible = localShadow(light, input.world, normalize(input.normal), input.position.xy);
                if (lightVisible <= 0.0) {
                    continue;
                }
                hit.irradiance *= lightVisible;
            }
            if (!withSpecular) {
                // The cheap setting: Lambert alone
                float3 plain = diffuseColor * (1.0 / PI) * lnol * hit.irradiance;
                direct += plain;
                directDiffuse += plain;
                continue;
            }
            // A source's size changes the highlight only where the lobe is
            // narrow beside it: a rough surface sees a point (the
            // representative point and its normalisation are the costliest
            // part of the loop)
            float spread = light.radius / (2.0 * hit.dist);
            bool sized = spread > 0.1 * a;
            float3 ls = sized ? representativeDirection(light, input.world, hit.toLight, hit.dist, r) : hit.toLight;
            float3 lh = normalize(ls + v);
            float lnolSpec = saturate(dot(n, ls));
            if (lnol <= 0.0 && lnolSpec <= 0.0) {
                continue;
            }
            float lnoh = saturate(dot(n, lh));
            float lloh = saturate(dot(ls, lh));
            float widen = saturate(a + spread);
            float norm = sized ? (a / widen) * (a / widen) : 1.0;
            float3 lcoat = float3(0.0, 0.0, 0.0);
            float3 lcoatLobe = float3(0.0, 0.0, 0.0);
            if (coat > 0.0) {
                lcoat = (film ? thinFilmReflectance(lloh, material.film.z, COAT_IOR, thickness)
                    : float3(1.0, 1.0, 1.0) * fresnelSchlick1(coatF0, lloh)) * coat;
                float widenCoat = saturate(coatA + spread);
                float normCoat = (spread > 0.1 * coatA) ? (coatA / widenCoat) * (coatA / widenCoat) : 1.0;
                lcoatLobe = distributionGGX(lnoh, coatA) * normCoat * visibilityKelemen(lloh) * lcoat;
            }
            float3 lf = fresnelSchlick(f0, lloh);
            float3 lspecular = distributionGGX(lnoh, a) * norm * visibilitySmithGGX(nov, max(lnolSpec, 1e-4), a)
                * lf * energy * material.foliage.w;
            float3 ldiffuse = diffuseColor * (1.0 / PI) * (1.0 - lcoat);
            direct += (ldiffuse * lnol + (lspecular * (1.0 - lcoat) + lcoatLobe) * lnolSpec) * hit.irradiance;
            directDiffuse += ldiffuse * lnol * hit.irradiance;
        }
    }

    // The sky
    float strength = frame.viewport.w;
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb;
    float3 envSpecular = environmentSpecular(r, roughness, f0, ab) * energy * material.foliage.w;
    float3 envBase = (diffuseColor * irradiance + envSpecular) * (1.0 - coatFv);
    float2 coatAb = brdfTable.SampleLevel(environmentSampler, float2(nov, coatRoughness), 0.0).xy;
    float3 coatPrefiltered = specularCube.SampleLevel(environmentSampler, r,
        coatRoughness * (SPECULAR_LEVELS - 1.0)).rgb;
    // The coat's own Fresnel already holds its angle: the table's scale and
    // bias add only the lobe's energy
    float3 envCoat = coatPrefiltered * coatFv * (coatAb.x + coatAb.y);
    float3 ambient = (envBase + envCoat) * (strength * occlusion);

    // What it gives off: radiance, not lit, so not divided by pi; the
    // draw's tint multiplies it as it does the base
    float3 emitted = material.emissive.rgb * input.tint.rgb;
    float3 color = direct + ambient + emitted;
    if (material.foliage.x > 0.0) {
        // Backlit translucency, the two-sided foliage model of Barré-Brisebois
        // and Bouchard ("Approximating Translucency for a Fast, Cheap and
        // Convincing Subsurface Scattering Look", GDC 2011, as Frostbite
        // shipped it), and of Crysis's foliage (Sousa, "Vegetation
        // Procedural Animation and Shading in Crysis", GPU Gems 3, ch. 16):
        // the light reaching the far side is the diffuse of the reversed
        // normal, and the part that goes straight on to the eye is a lobe
        // about the light's direction, bent by the normal ('distortion'
        // 0.2) so it follows the surface. Both leave in the base colour,
        // the leaf's own; the sky's light on the far side comes through
        // too, as the 'ambient' term of theirs, at 0.4 of its strength (a
        // leaf lit from the sky alone should not brighten a tree seen with
        // the sun behind us). The leaf's thickness is folded into the
        // material's strength (the papers read it from a map); with no
        // shadows, a leaf behind others glows as if it were open to the
        // sun. The colour that comes through is the base colour to the power
        // 1.5: light crosses the leaf's pigment on its way in and on its way
        // out, so what leaves is more saturated than what reflects (a
        // squared colour would be the whole of it), which also keeps a
        // bright orange from tone-mapping to cream.
        float back = saturate(dot(-n, l));
        float3 bent = normalize(l + n * 0.2);
        float lobe = pow(saturate(dot(v, -bent)), material.foliage.y);
        float3 sunThrough = frame.lightColor.rgb * (back * (0.4 + 1.6 * lobe) * (1.0 / PI) * visible);
        float3 skyThrough = irradianceCube.SampleLevel(environmentSampler, -n, 0.0).rgb * (0.4 * strength * occlusion);
        float3 through = pow(max(base.rgb, 0.0), 1.5);
        color += through * (1.0 - metallic) * material.foliage.x * (sunThrough + skyThrough);
    }
    if (alphaMode == ALPHA_BLEND) {
        // Premultiplied: of the diffuse light (the sun's and the
        // environment's) only the share alpha covers; the reflections and the
        // glow whole
        float3 envDiffuse = diffuseColor * irradiance * (1.0 - coatFv) * (strength * occlusion);
        color = color - (directDiffuse + envDiffuse) * (1.0 - base.a);
    }
    return float4(color * frame.exposure.x, base.a);
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
    return float4(skyRadiance(dir, envParams.sky, true) * frame.exposure.x, 1.0);
}

#ifdef PBR_EXTENDED

// ---- The extended material: sheen, anisotropy, subsurface, glass, screen-space reflections ----
//
// fragmentExtended is fragmentMain's lighting with the lobes lobes.slang and
// the capture scenecapture.slang add; it is compiled only into pbrx.spv
// (pbrx.slang defines PBR_EXTENDED and includes this file), so pbr.spv, which
// every other material draws with, is untouched. Where a feature is off it
// is the same arithmetic as fragmentMain's (the base terms are repeated
// here, and are to be kept in step with it). What it adds, in the order
// light meets the surface:
// - WET darkening: where the wet patches' mask is on, the base colour is
//   darkened (a wet surface absorbs more of the light it scatters: by up to
//   half, 'lobes.w' of it);
// - ANISOTROPY: the base's specular lobe is Burley's anisotropic GGX with the
//   Heitz visibility, along a tangent frame found from the uv derivatives
//   (the meshes carry none) and turned about the normal;
// - SHEEN: a Charlie lobe over the base, which takes from the base what it
//   reflects (its directional albedo, a table);
// - SUBSURFACE: the base's diffuse light is the pre-integrated, curvature
//   dependent diffuse (scenecapture.slang's diffusionLookup) instead of
//   Lambert's cosine, in each channel at its own width, so the terminator of
//   a sphere of skin or wax softens and warms; curvature is the screen-space
//   change of the interpolated normal over that of the position (Penner);
// - SCREEN-SPACE REFLECTIONS: the radiance the environment gives a
//   specular lobe is replaced, by the march's confidence, by what the frame
//   shows in the direction of the reflection;
// - GLASS (a see-through material with 'glass' above 0): the light behind it,
//   refracted and absorbed (below).

struct ExtSurface {
    float3 n;
    float3 v;
    float nov;
    float3 t;                      // the tangent and bitangent (anisotropy)
    float3 b;
    float3 diffuseColor;
    float3 f0;
    float3 energy;                 // multiple scattering's energy given back to the specular lobe
    float a;                       // the base's alpha (roughness squared)
    float2 anisotropicAlpha;       // along the tangent and the bitangent
    bool anisotropic;
    float coat;
    float coatA;
    float coatF0;
    bool film;
    float thickness;               // the film's, in nanometres
    float3 sheenColor;
    float sheenAlpha;
    float sheenScale;              // what the sheen leaves of the layer beneath it
    float subsurface;
    float curvature;
    float3 scatterWidth;
    float specularScale;
};

// The cosine a channel's diffuse light follows: Lambert's, or the table's
float3 diffuseFactor(ExtSurface s, float nol) {
    float lambert = saturate(nol);
    if (s.subsurface <= 0.0) {
        return float3(lambert, lambert, lambert);
    }
    float3 table = float3(diffusionLookup(nol, s.curvature, s.scatterWidth.x),
        diffusionLookup(nol, s.curvature, s.scatterWidth.y), diffusionLookup(nol, s.curvature, s.scatterWidth.z));
    return lerp(float3(lambert, lambert, lambert), table, s.subsurface);
}

// The light one source gives: 'l' the unit direction to its centre (the
// diffuse light), 'ls' the direction its highlight is found about (a sphere
// light's representative point; the same as 'l' for the sun), 'norm' and
// 'normCoat' the lobes' normalisations for a sphere light, 'e' its
// irradiance (shadowed). 'diffuseOut' is the share of it that is diffuse.
float3 evalLight(ExtSurface s, float3 l, float3 ls, float norm, float normCoat, float3 e, out float3 diffuseOut) {
    diffuseOut = float3(0.0, 0.0, 0.0);
    float nolDiffuse = dot(s.n, l);
    float nolSpec = saturate(dot(s.n, ls));
    if (s.subsurface <= 0.0 && nolDiffuse <= 0.0 && nolSpec <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    float3 h = normalize(ls + s.v);
    float noh = saturate(dot(s.n, h));
    float loh = saturate(dot(ls, h));
    float3 coatFl = float3(0.0, 0.0, 0.0);
    if (s.coat > 0.0) {
        float3 fc = s.film ? thinFilmReflectance(loh, material.film.z, COAT_IOR, s.thickness)
            : float3(1.0, 1.0, 1.0) * fresnelSchlick1(s.coatF0, loh);
        coatFl = fc * s.coat;
    }
    float3 diffuse = s.diffuseColor * (1.0 / PI) * diffuseFactor(s, nolDiffuse) * (1.0 - coatFl) * s.sheenScale;
    float3 f = fresnelSchlick(s.f0, loh);
    float d;
    float vis;
    if (s.anisotropic) {
        d = distributionAnisotropic(s.anisotropicAlpha.x, s.anisotropicAlpha.y, dot(s.t, h), dot(s.b, h), noh);
        vis = visibilityAnisotropic(s.anisotropicAlpha.x, s.anisotropicAlpha.y, dot(s.t, s.v), dot(s.b, s.v),
            dot(s.t, ls), dot(s.b, ls), s.nov, max(nolSpec, 1e-4));
    } else {
        d = distributionGGX(noh, s.a);
        vis = visibilitySmithGGX(s.nov, max(nolSpec, 1e-4), s.a);
    }
    float3 specular = d * norm * vis * f * s.energy * s.specularScale * (1.0 - coatFl) * s.sheenScale;
    float3 coatLobe = float3(0.0, 0.0, 0.0);
    if (s.coat > 0.0) {
        coatLobe = distributionGGX(noh, s.coatA) * normCoat * visibilityKelemen(loh) * coatFl;
    }
    float3 sheen = float3(0.0, 0.0, 0.0);
    if (dot(s.sheenColor, s.sheenColor) > 0.0) {
        sheen = s.sheenColor * sheenDistribution(noh, s.sheenAlpha) * sheenVisibility(s.nov, max(nolSpec, 1e-4));
    }
    diffuseOut = diffuse * e;
    return diffuse * e + (specular + coatLobe + sheen) * (nolSpec * e);
}

// The pixel's own uv in the frame
float2 pixelUv(float2 position) {
    return position * scene.size.zw;
}

#ifdef PBR_RT
// Ray-traced glass, soap film and reflections (rtglass.slang), for a card that has ray tracing
#include "rtglass.slang"

// The record of the material being drawn, as raytrace.cone writes one for an instance of it: the glass or film it
// is, for the first face the camera sees
RtRecord rtSelfRecord(float4 base, float roughness, bool thinFilm) {
    RtRecord r;
    r.a = float4(base.rgb, 0.0);
    r.b = float4(roughness, max(material.glass.y, 1.0), thinFilm ? RT_FILM : RT_SOLID, material.glass.x);
    r.c = material.attenuation;
    r.d = material.film;
    r.e = material.filmNoise;
    r.f = float4(0.0, 0.0, 0.0, material.glass.w);
    r.geometry = uint4(0u, 0u, 0u, 0u);
    return r;
}
#endif

[shader("fragment")]
float4 fragmentExtended(VertexOutput input) : SV_Target {
    fadeClip(input.fade, input.position.xy);
    // Derivatives first, while every fragment of the quad is still here
    float curvature = min(length(fwidth(normalize(input.normal))) / max(length(fwidth(input.world)), 1e-6), 4000.0);
    float3 ng = normalize(input.normal);
    float3 n = ng;
    if (material.maps.x > 0.0) {
        n = mapNormal(n, input.world, input.uv);
    }
    float3 v = normalize(frame.cameraPosition.xyz - input.world);
    float nov = dot(n, v);
    if (nov < 0.0) {
        n = normalize(n - v * (nov * 1.01));
        nov = dot(n, v);
    }
    nov = clamp(nov, 1e-4, 1.0);
    float3 l = frame.lightDirection.xyz;
    float visible = sunVisibility(input.world, normalize(input.normal), l, input.position.xy);
    float3 h = normalize(l + v);
    float3 rPlain = reflect(-v, n);

    float4 base = material.baseColor * baseTexture.Sample(baseSampler, input.uv) * input.tint;
    uint alphaMode = uint(material.maps.z + 0.5);
    if (alphaMode == ALPHA_MASK) {
        if (base.a < material.maps.w) {
            discard;
        }
        base.a = 1.0;
    }
    float glassShare = alphaMode == ALPHA_BLEND ? saturate(material.glass.x) : 0.0;
    bool isGlass = glassShare > 0.0;
    float4 metalRough = metallicRoughnessTexture.Sample(baseSampler, input.uv);
    float metallic = isGlass ? 0.0 : material.surface.x * metalRough.b;
    float roughness = clamp(material.surface.y * metalRough.g, 0.045, 1.0);
    float occlusion = 1.0 + material.maps.y * (occlusionTexture.Sample(baseSampler, input.uv).r - 1.0);
    float a = roughness * roughness;
    float coat = material.surface.z;
    float wetMask = 1.0;
    if (material.wet.x > 0.0) {
        float wn = fbm3(input.local * material.wet.x, uint(material.wet.w), 4, 2.0, 0.5).value;
        wetMask = smoothstep(material.wet.y - material.wet.z, material.wet.y + material.wet.z, wn);
        coat *= wetMask;
    }
    // A wet surface is darker (up to half) where it is wet
    base.rgb *= lerp(1.0, 0.5, material.lobes.w * wetMask);
    float coatRoughness = clamp(material.surface.w, 0.045, 1.0);
    float coatA = coatRoughness * coatRoughness;

    ExtSurface s;
    s.n = n;
    s.v = v;
    s.nov = nov;
    // The tangent frame from the uv derivatives, turned about the normal
    {
        float3 dp1 = ddx(input.world);
        float3 dp2 = ddy(input.world);
        float2 duv1 = ddx(input.uv);
        float2 duv2 = ddy(input.uv);
        float3 dp2perp = cross(dp2, n);
        float3 dp1perp = cross(n, dp1);
        float3 tt = dp2perp * duv1.x + dp1perp * duv2.x;
        if (dot(input.tangent.xyz, input.tangent.xyz) > 0.25) {
            // The mesh's own tangents, smooth across its triangles (a mesh without them:
            // the uv derivatives', constant over each triangle)
            tt = input.tangent.xyz;
        }
        if (dot(tt, tt) < 1e-30) {
            float3 other = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            tt = cross(other, n);
        }
        tt = normalize(tt - n * dot(n, tt));
        float3 bb = cross(n, tt);
        float cr = cos(material.lobes.y);
        float sr = sin(material.lobes.y);
        s.t = tt * cr + bb * sr;
        s.b = cross(n, s.t);
    }
    s.diffuseColor = isGlass ? float3(0.0, 0.0, 0.0) : base.rgb * (1.0 - metallic);
    float dielectricF0 = isGlass ? iorToF0(material.glass.y) : 0.04;
    s.f0 = lerp(float3(dielectricF0, dielectricF0, dielectricF0), base.rgb, metallic);
    if (isGlass) {
        s.f0 = float3(dielectricF0, dielectricF0, dielectricF0);
    }
    float2 ab = brdfTable.SampleLevel(environmentSampler, float2(nov, roughness), 0.0).xy;
    s.energy = 1.0 + s.f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0);
    s.a = a;
    s.anisotropic = abs(material.lobes.x) > 0.001;
    s.anisotropicAlpha = anisotropicAlphas(a, material.lobes.x);
    s.coat = coat;
    s.coatA = coatA;
    s.coatF0 = iorToF0(COAT_IOR);
    s.film = material.film.w > 0.5;
    s.thickness = 0.0;
    if (s.film) {
        s.thickness = filmThickness(input.local, frame.viewport.z);
    }
    s.sheenColor = material.sheen.rgb;
    float sheenRoughness = clamp(material.sheen.w, 0.07, 1.0);
    s.sheenAlpha = sheenRoughness * sheenRoughness;
    bool hasSheen = dot(s.sheenColor, s.sheenColor) > 0.0;
    s.sheenScale = 1.0;
    if (hasSheen) {
        s.sheenScale = 1.0 - max(max(s.sheenColor.r, s.sheenColor.g), s.sheenColor.b) * sheenAlbedo(nov, sheenRoughness);
    }
    s.subsurface = isGlass ? 0.0 : material.lobes.z;
    s.curvature = curvature;
    s.scatterWidth = material.subsurface.rgb * material.subsurface.w;
    s.specularScale = material.foliage.w;

    // The coat's Fresnel at the view, as fragmentMain
    float3 coatFv = s.film ? thinFilmReflectance(nov, material.film.z, COAT_IOR, s.thickness)
        : float3(1.0, 1.0, 1.0) * fresnelSchlick1(s.coatF0, nov);
    coatFv = coatFv * coat;

    // The sun
    float3 direct = float3(0.0, 0.0, 0.0);
    float3 directDiffuse = float3(0.0, 0.0, 0.0);
    {
        float3 dd;
        // Under a rippled surface the sun is focused into caustics (scenecapture.slang)
        float caustic = causticLight(input.world, l, material.reflection.y);
#ifdef PBR_RT
        // The glass between the surface and the sun tints and dims its light (a shadow ray at the glass alone)
        float3 sunTint = float3(1.0, 1.0, 1.0);
        if (!isGlass && visible > 0.0 && dot(ng, l) > 0.0) {
            sunTint = rtTransmittance(input.world + ng * RT_EPSILON, l, 2u);
        }
        direct = evalLight(s, l, l, 1.0, 1.0, frame.lightColor.rgb * (sunTint * (visible * caustic)), dd);
#else
        direct = evalLight(s, l, l, 1.0, 1.0, frame.lightColor.rgb * (visible * caustic), dd);
#endif
        directDiffuse = dd;
    }

    // The point and spot lights of the fragment's cluster, as fragmentMain's loop
    if (lightParams.grid.w > 0.5) {
        uint materialLayers = uint(material.emissive.w + 0.5);
        uint2 cluster = fragmentCluster(input.world, input.position.xy);
        bool withSpecular = lightParams.slices.z > 0.5;
        for (uint k = 0; k < cluster.y; k++) {
            LocalLight light = loadLocalLight(lightClusters[cluster.x + k]);
            if (!lightReaches(light, materialLayers)) {
                continue;
            }
            LightHit hit;
            if (!lightAt(light, input.world, hit)) {
                continue;
            }
            float lnol = dot(n, hit.toLight);
            if (light.shadowView > -0.5 && (lnol > 0.0 || withSpecular || s.subsurface > 0.0)) {
                float lightVisible = localShadow(light, input.world, normalize(input.normal), input.position.xy);
                if (lightVisible <= 0.0) {
                    continue;
                }
                hit.irradiance *= lightVisible;
            }
            float3 ld;
            if (!withSpecular) {
                float3 plain = s.diffuseColor * (1.0 / PI) * diffuseFactor(s, lnol) * hit.irradiance;
                direct += plain;
                directDiffuse += plain;
                continue;
            }
            float spread = light.radius / (2.0 * hit.dist);
            bool sized = spread > 0.1 * a;
            float3 ls = sized ? representativeDirection(light, input.world, hit.toLight, hit.dist, rPlain) : hit.toLight;
            float widen = saturate(a + spread);
            float norm = sized ? (a / widen) * (a / widen) : 1.0;
            float widenCoat = saturate(coatA + spread);
            float normCoat = (spread > 0.1 * coatA) ? (coatA / widenCoat) * (coatA / widenCoat) : 1.0;
            direct += evalLight(s, hit.toLight, ls, norm, normCoat, hit.irradiance, ld);
            directDiffuse += ld;
        }
    }

    // The sky, the probe, and the screen: what a specular lobe sees
    float strength = frame.viewport.w;
    float exposure = max(frame.exposure.x, 1e-9);
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb;
    float3 rBase = s.anisotropic ? anisotropicReflection(n, s.t, s.b, v, material.lobes.x) : rPlain;
    float3 prefilteredBase = specularCube.SampleLevel(environmentSampler, probeDirection(rBase, input.world),
        roughness * (SPECULAR_LEVELS - 1.0)).rgb * strength;
    float3 prefilteredCoat = specularCube.SampleLevel(environmentSampler, probeDirection(rPlain, input.world),
        coatRoughness * (SPECULAR_LEVELS - 1.0)).rgb * strength;
    float2 coatAb = brdfTable.SampleLevel(environmentSampler, float2(nov, coatRoughness), 0.0).xy;
    float screenShare = 0.0;
#ifdef PBR_RT
    // Reflections by ray (rtglass.slang): the objects off the screen, the glass, the sky, everything a ray meets,
    // for a surface smooth enough to show them; the blurrier the surface, the more of the environment's
    // prefiltered picture takes over
    if (material.reflection.x > 0.0 && !isGlass) {
        float lobeRoughness = coat > 0.0 ? min(roughness, coatRoughness) : roughness;
        float share = saturate(material.reflection.x) * (1.0 - smoothstep(0.12, 0.5, lobeRoughness));
        if (share > 0.0 && dot(rPlain, ng) > 0.0) {
            float3 traced = rtReflection(input.world + ng * RT_EPSILON, rPlain);
            prefilteredBase = lerp(prefilteredBase, traced, share);
            if (coat > 0.0) {
                prefilteredCoat = lerp(prefilteredCoat, traced, share);
            }
        }
    }
#else
    if (material.reflection.x > 0.0) {
        float lobeRoughness = coat > 0.0 ? min(roughness, coatRoughness) : roughness;
        ScreenHit hit = traceScreen(input.world, rPlain, lobeRoughness, input.position.xy);
        screenShare = hit.confidence * saturate(material.reflection.x);
        if (screenShare > 0.0) {
            prefilteredBase = lerp(prefilteredBase, screenHitColor(hit, roughness) / exposure, screenShare);
            if (coat > 0.0) {
                prefilteredCoat = lerp(prefilteredCoat, screenHitColor(hit, coatRoughness) / exposure, screenShare);
            }
        }
    }
#endif
    float3 envSpecular = prefilteredBase * (s.f0 * ab.x + ab.y) * s.energy * s.specularScale;
    float3 envBase = (s.diffuseColor * irradiance * strength + envSpecular) * s.sheenScale * (1.0 - coatFv);
    float3 envCoat = prefilteredCoat * coatFv * (coatAb.x + coatAb.y);
    float3 envSheen = float3(0.0, 0.0, 0.0);
    if (hasSheen) {
        float3 sheenRadiance = specularCube.SampleLevel(environmentSampler, probeDirection(rPlain, input.world),
            sheenRoughness * (SPECULAR_LEVELS - 1.0)).rgb * strength;
        envSheen = s.sheenColor * sheenRadiance * sheenAlbedo(nov, sheenRoughness);
    }
    float3 ambient = (envBase + envCoat + envSheen) * occlusion;

    float3 emitted = material.emissive.rgb * input.tint.rgb;
    float3 color = direct + ambient + emitted;
    if (material.foliage.x > 0.0 && !isGlass) {
        float back = saturate(dot(-n, l));
        float3 bent = normalize(l + n * 0.2);
        float lobe = pow(saturate(dot(v, -bent)), material.foliage.y);
        float3 sunThrough = frame.lightColor.rgb * (back * (0.4 + 1.6 * lobe) * (1.0 / PI) * visible);
        float3 skyThrough = irradianceCube.SampleLevel(environmentSampler, -n, 0.0).rgb * (0.4 * strength * occlusion);
        float3 through = pow(max(base.rgb, 0.0), 1.5);
        color += through * (1.0 - metallic) * material.foliage.x * (sunThrough + skyThrough);
    }
    float outAlpha = base.a;
#ifdef PBR_RT
    // Glass smooth enough to be traced is traced: the light that reaches this point of its face from the eye's side
    // is the sum of the reflections and the transmission through the whole volume, found by rays (rtglass.slang).
    // The sun's highlight (direct) is added; the environment's reflection is not (the rays find it). Frosted
    // glass takes the raster path below, which blurs the captured frame.
    float roughGlass = roughness * saturate(max(material.glass.y, 1.0) * 2.0 - 2.0);
    bool traced = isGlass && roughGlass < 0.08;
    if (traced) {
        bool thin = material.film.w > 0.5 && material.glass.z < 0.001;
        RtRecord self = rtSelfRecord(base, roughness, thin);
        float3 gl = rtGlass(self, input.world, input.local, normalize(input.normal), -v);
        color = (direct + emitted + gl) * glassShare;
        outAlpha = glassShare;
    } else
#endif
    if (isGlass) {
        // The light behind: refracted by the surface (glTF's KHR_materials_volume and
        // _transmission, as the Khronos sample viewer draws them): the refracted ray
        // runs on through the thickness of the volume, and where it comes out is where
        // the frame is read. Three rays, one an index each, when the glass disperses
        // (KHR_materials_dispersion: the indices spread by (ior - 1) 0.025 dispersion
        // either side), so the edges of what is seen through it fringe in colour.
        float ior = max(material.glass.y, 1.0);
        float thicknessM = max(material.glass.z, 0.0);
        float spreadHalf = (ior - 1.0) * 0.025 * material.glass.w;
        float3 iors = float3(ior - spreadHalf, ior, ior + spreadHalf);
        int rays = material.glass.w > 0.0 ? 3 : 1;
        float2 ownUv = pixelUv(input.position.xy);
        // Rougher glass sees a blurrier picture: Khronos' index-dependent roughness, and
        // a blur whose radius grows as the roughness squared (the lobe's width), at 8% of the
        // frame's shorter side at roughness 1: the pyramid level whose texels are that wide
        float roughSeen = roughness * saturate(ior * 2.0 - 2.0);
        float blurPixels = roughSeen * roughSeen * 0.08 * min(scene.size.x, scene.size.y);
        float lod = log2(max(blurPixels * 2.0, 1.0));
        float3 seen = float3(0.0, 0.0, 0.0);
        for (int c = 0; c < rays; c++) {
            float iorC = c == 0 ? iors.x : (c == 1 ? iors.y : iors.z);
            float3 rd = refract(-v, n, 1.0 / iorC);
            if (dot(rd, rd) < 1e-6) {
                rd = reflect(-v, n);
            }
            rd = normalize(rd);
            float3 exitPoint = input.world + rd * thicknessM;
            float behind;
            float3 exitUv = sceneProject(exitPoint, behind);
            float3 environmentSeen = specularCube.SampleLevel(environmentSampler, probeDirection(rd, exitPoint),
                roughSeen * (SPECULAR_LEVELS - 1.0)).rgb * strength;
            float3 picture = environmentSeen;
            if (sceneCaptured()) {
                float2 uv = exitUv.xy;
                // Something nearer than the glass cannot be seen through it: read straight behind
                if (sceneDepthAt(uv) < input.position.z - 1e-5) {
                    uv = ownUv;
                }
                float2 edge = min(uv, 1.0 - uv) / 0.04;
                float onScreen = saturate(min(edge.x, edge.y)) * (1.0 - behind);
                picture = lerp(environmentSeen, sceneSample(uv, lod) / exposure, onScreen);
            }
            float3 picked = c == 0 ? float3(picture.r, 0.0, 0.0) : (c == 1 ? float3(0.0, picture.g, 0.0) : float3(0.0, 0.0, picture.b));
            if (rays == 1) {
                seen = picture;
            } else {
                seen += picked;
            }
        }
        // Beer-Lambert absorption through the thickness: the colour light has after
        // 'attenuationDistance', as a rate per metre
        float3 rate = -log(max(material.attenuation.rgb, float3(1e-4, 1e-4, 1e-4))) / max(material.attenuation.w, 1e-4);
        float3 absorbed = exp(-rate * thicknessM);
        float3 reflectance = s.f0 * ab.x + ab.y;
        float3 transmitted = seen * absorbed * base.rgb * (1.0 - reflectance);
        // Over the picture behind it, the glass is the reflections and highlights it adds, and what passes
        // through it in place of the picture: premultiplied, with the share that passes as the alpha
        color += transmitted * glassShare;
        outAlpha = glassShare;
    } else if (alphaMode == ALPHA_BLEND) {
        float3 envDiffuse = s.diffuseColor * irradiance * (1.0 - coatFv) * (strength * occlusion);
        color = color - (directDiffuse + envDiffuse) * (1.0 - base.a);
    }
    return float4(color * frame.exposure.x, outAlpha);
}

#endif
