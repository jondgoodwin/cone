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
// Lit by the one directional light, and by the sky through image-based
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

struct PbrParams {
    float4 baseColor;              // 0: rgba, linear
    float4 surface;                // 16: metallic, roughness, clear coat (0 to 1), the coat's roughness
    float4 film;                   // 32: the film's least and greatest thickness (nm), its index, 1 if there is a film
    float4 filmNoise;              // 48: the noise's scale (a unit of model space), drift a second, warp, seed
    float4 wet;                    // 64: where the coat lies: fBm's scale (0: everywhere), level, softness, seed
    float4 maps;                   // 80: the normal map's scale (0: no normal map), the occlusion map's strength,
                                   //     the alpha mode (0 opaque, 1 cut out, 2 see-through), the cut-off
    float4 emissive;               // 96: rgb radiance (nits in physical units), its strength multiplied in
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
[[vk::binding(0, 2)]] ConstantBuffer<SkyParams> skyParams;
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
// A mesh drawn with a flutter material carries, in a fourth vertex stream
// (location 3, the mesh's colors, gpumesh.cone), for each vertex: xyz its
// offset from its leaf's pivot (the leaf's stem), in model coordinates, and
// w the leaf's phase in turns, one number for the whole leaf. Each leaf is
// turned about its pivot by a small rotation vector w: a turn about a
// horizontal axis of its own (an azimuth from its phase) and a slower sway
// about the vertical, each a sine of the frame's time, on the leaf's own
// period (0.6 to 1.8 seconds, from its phase) and phase. For turns of a few degrees the
// displacement of a vertex is w x offset (the small-angle rotation, which
// needs no matrix), and the normal turns by w x n so the light catches and
// loses the leaf as it moves. A vertex at the pivot (a stem's end) stays.
// Model space is y up, which is how a tree stands.

struct SwayInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 sway : COLOR;
};

void flutterLeaf(float3 offset, float phase, float amount, float time, inout float3 position, inout float3 normal) {
    float period = 0.6 + 1.2 * frac(phase * 13.7);
    float s1 = sin(6.2831853 * frac(time / period + phase));
    float s2 = sin(6.2831853 * frac(time / (period * 1.7) + phase * 3.1));
    // The same axis for every vertex of the leaf (from its phase), so it
    // turns as one piece and is not bent
    float azimuth = 6.2831853 * frac(phase * 7.7);
    float3 pitch = float3(cos(azimuth), 0.0, sin(azimuth));
    float3 turn = amount * (s1 * pitch + (0.6 * s2) * float3(0.0, 1.0, 0.0));
    position += cross(turn, offset);
    normal += cross(turn, normal);
}

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
    float3 h = normalize(l + v);
    float nol = saturate(dot(n, l));
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float3 r = reflect(-v, n);

    float4 base = material.baseColor * baseTexture.Sample(baseSampler, input.uv) * input.tint;
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
        direct = (baseLobe + coatLobe) * frame.lightColor.rgb * nol;
        directDiffuse = diffuse * (1.0 - coatFl) * frame.lightColor.rgb * nol;
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
        float3 sunThrough = frame.lightColor.rgb * (back * (0.4 + 1.6 * lobe) * (1.0 / PI));
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
    return float4(skyRadiance(dir, skyParams, true) * frame.exposure.x, 1.0);
}
