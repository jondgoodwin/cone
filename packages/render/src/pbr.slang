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
import shadowing;
import flutter;
import locallights;

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
    return output;
}

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
