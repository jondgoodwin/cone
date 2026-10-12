// The render package's decals: box-projected, deferred-style decals on opaque
// surfaces (moss, stains, footprints, bullet holes). Compiled ahead of time to
// decal.spv by tools/shaders/shaders.py and embedded in decal.cone.
//
// THE METHOD is the screen-space decal of deferred renderers (Persson,
// "Deferred Decals", 2008 and Unreal's and Unity's decal projectors, Godot's
// Decal node), made for a forward renderer that has no G-buffer to write
// into: each decal is a box, drawn after the scene by its back faces (so that
// a camera inside it still sees it) with no depth test; for every pixel
// under the box the scene's depth gives the world position the pixel shows,
// the position is carried into the decal's own space by the inverse of its
// box's matrix, and the pixel is inside if it lies in the box. The decal's
// texture is read at the position's x and z in the box (it projects along
// the box's local y: down onto a floor, as Godot's does), faded by
// - the decal's own fade, and by distance;
// - how far into the box (along y) the position is, toward either face;
// - the angle between the surface and the box's up (a decal does not smear
//   across a wall it was meant for the floor): from 'angle' to 'angle' + 0.1;
// then SHADED here, since the scene's own shading cannot be changed after
// the fact: the surface's normal from the depth's neighbours, perturbed by
// the decal's normal map (in the frame of the surface's normal and the box's
// x axis) where it has one; Lambert diffuse from the sun with its cascaded
// shadows (shadowing.slang) and the environment's diffuse light, plus a GGX
// highlight from the sun and the environment's specular light at the decal's
// roughness (dielectric: f0 = 0.04). The result is blended over the frame
// with premultiplied alpha. What it cannot do: change the metallic or
// the base colour under it (the surface keeps its own highlight), or light
// itself by the clustered lights (a decal sees the sun and the environment).
//
// Group 0 is the renderer's frame group (the sun, its shadows), group 1 the
// decal material's (the albedo, the normal map, the roughness map and their
// sampler), group 2 the renderer's environment's, group 3 the pass's own (the
// frame uniform, the scene's depth). The immediates: the box's inverse
// matrix and the decal's parameters (decal.cone's DecalImmediates).

import renderframe;
import shadowparams;
import shadowing;
import fxcommon;

[[vk::binding(0, 1)]] Texture2D albedoMap;
[[vk::binding(1, 1)]] Texture2D normalMap;
[[vk::binding(2, 1)]] Texture2D roughnessMap;
[[vk::binding(3, 1)]] SamplerState decalSampler;
[[vk::binding(1, 2)]] TextureCube irradianceCube;
[[vk::binding(2, 2)]] TextureCube specularCube;
[[vk::binding(3, 2)]] Texture2D brdfTable;
[[vk::binding(4, 2)]] SamplerState environmentSampler;
[[vk::binding(0, 3)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 3)]] Texture2D<float> sceneDepth;

struct DecalImmediates {
    float4x4 inverseModel;         // 0: world to the box's own space (the unit cube, from -0.5 to 0.5)
    float4 tint;                   // 64: rgba
    float4 a;                      // 80: x fade, y normal strength, z roughness (without a map), w the angle's cosine
    float4 b;                      // 96: x the share of the box's depth faded at each face, y fade distance start, z fade distance end, w flags
};

static const uint FLAG_NORMAL_MAP = 1;
static const uint FLAG_ROUGHNESS_MAP = 2;

[[vk::push_constant]] ConstantBuffer<DecalImmediates> decal;

static const float SPECULAR_LEVELS = 6.0;
static const float3 CORNERS[8] = {
    float3(-0.5, -0.5, -0.5), float3(0.5, -0.5, -0.5), float3(0.5, 0.5, -0.5), float3(-0.5, 0.5, -0.5),
    float3(-0.5, -0.5, 0.5), float3(0.5, -0.5, 0.5), float3(0.5, 0.5, 0.5), float3(-0.5, 0.5, 0.5)
};
// 12 triangles, counter-clockwise seen from outside
static const uint CUBE[36] = {
    0, 3, 2, 0, 2, 1,      // -z
    4, 5, 6, 4, 6, 7,      // +z
    0, 4, 7, 0, 7, 3,      // -x
    1, 2, 6, 1, 6, 5,      // +x
    0, 1, 5, 0, 5, 4,      // -y
    3, 7, 6, 3, 6, 2       // +y
};

struct DecalVertex {
    float4 position : SV_Position;
};

[shader("vertex")]
DecalVertex vertexMain(uint id : SV_VulkanVertexID) {
    DecalVertex output;
    // The box's own matrix, the inverse of the one pushed (an affine
    // matrix: its 3 x 3 inverted by cross products, then the translation)
    float3 r0 = float3(decal.inverseModel[0][0], decal.inverseModel[0][1], decal.inverseModel[0][2]);
    float3 r1 = float3(decal.inverseModel[1][0], decal.inverseModel[1][1], decal.inverseModel[1][2]);
    float3 r2 = float3(decal.inverseModel[2][0], decal.inverseModel[2][1], decal.inverseModel[2][2]);
    float3 t = float3(decal.inverseModel[0][3], decal.inverseModel[1][3], decal.inverseModel[2][3]);
    float invDet = 1.0 / dot(r0, cross(r1, r2));
    float3 c0 = cross(r1, r2) * invDet;
    float3 c1 = cross(r2, r0) * invDet;
    float3 c2 = cross(r0, r1) * invDet;
    float3 origin = -(c0 * t.x + c1 * t.y + c2 * t.z);
    float3 lp = CORNERS[CUBE[id]];
    float3 w = c0 * lp.x + c1 * lp.y + c2 * lp.z + origin;
    output.position = mul(frame.viewProjection, float4(w, 1.0));
    return output;
}

// The surface's world normal at a pixel, from the depth's neighbours: each
// axis from the neighbour nearer in depth, so an edge does not bend it
float3 worldAt(int2 p) {
    int2 size = int2(fx.screen.xy);
    int2 q = clamp(p, int2(0, 0), size - 1);
    float z = sceneDepth.Load(int3(q, 0));
    return worldPosition(fx, (float2(q) + 0.5) * fx.screen.zw, z);
}

float viewZ(int2 p) {
    int2 size = int2(fx.screen.xy);
    return viewDepth(fx, sceneDepth.Load(int3(clamp(p, int2(0, 0), size - 1), 0)));
}

float distributionGGX(float noh, float a) {
    float a2 = a * a;
    float d = noh * noh * (a2 - 1.0) + 1.0;
    return a2 / (FX_PI * d * d);
}

float visibilitySmith(float nov, float nol, float a) {
    float a2 = a * a;
    float gv = nol * sqrt(nov * nov * (1.0 - a2) + a2);
    float gl = nov * sqrt(nol * nol * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

[shader("fragment")]
float4 fragmentMain(DecalVertex input) : SV_Target {
    int2 p = int2(input.position.xy);
    float z = sceneDepth.Load(int3(p, 0));
    float2 uv = (float2(p) + 0.5) * fx.screen.zw;
    float3 world = worldPosition(fx, uv, z);
    // The pixel in the box's own space
    float3 local = mul(decal.inverseModel, float4(world, 1.0)).xyz;
    bool inside = z < 1.0 && max(abs(local.x), max(abs(local.y), abs(local.z))) <= 0.5;

    // The surface's normal: from the neighbour nearer in depth along each axis
    float dc = viewZ(p);
    float3 pc = world;
    float3 pl = worldAt(p + int2(-1, 0));
    float3 pr = worldAt(p + int2(1, 0));
    float3 pu = worldAt(p + int2(0, -1));
    float3 pd = worldAt(p + int2(0, 1));
    float3 dx = abs(viewZ(p + int2(1, 0)) - dc) < abs(viewZ(p + int2(-1, 0)) - dc) ? (pr - pc) : (pc - pl);
    float3 dy = abs(viewZ(p + int2(0, -1)) - dc) < abs(viewZ(p + int2(0, 1)) - dc) ? (pu - pc) : (pc - pd);
    float3 ng = normalize(cross(dx, dy));

    // The decal's frame: its up (local y) and x axes in the world, from the
    // rows of the inverse
    float3 up = normalize(float3(decal.inverseModel[1][0], decal.inverseModel[1][1], decal.inverseModel[1][2]));
    float3 axisX = normalize(float3(decal.inverseModel[0][0], decal.inverseModel[0][1], decal.inverseModel[0][2]));
    float facing = dot(ng, up);
    float angle = saturate((facing - decal.a.w) / 0.1);
    float edge = saturate((0.5 - abs(local.y)) / max(decal.b.x * 0.5, 1e-4));
    float2 texCoord = float2(local.x + 0.5, local.z + 0.5);
    // Mip by the screen derivatives of the texture coordinate
    float2 dtx = ddx(texCoord);
    float2 dty = ddy(texCoord);
    float lod = log2(max(max(length(dtx), length(dty)) * 256.0, 1.0));
    float4 albedo = albedoMap.SampleLevel(decalSampler, texCoord, lod);
    float distanceFade = 1.0 - saturate((length(world - fx.cameraPosition.xyz) - decal.b.y) / max(decal.b.z - decal.b.y, 1e-3));
    float alpha = albedo.a * decal.tint.a * decal.a.x * angle * edge * distanceFade;

    // The normal: the surface's, perturbed by the decal's map
    float3 n = ng;
    if ((uint(decal.b.w + 0.5) & FLAG_NORMAL_MAP) != 0u) {
        float3 nm = normalMap.SampleLevel(decalSampler, texCoord, lod).xyz * 2.0 - 1.0;
        float3 t = normalize(axisX - ng * dot(axisX, ng));
        float3 b = cross(ng, t);
        float3 perturbed = normalize(t * nm.x + b * nm.y + ng * max(nm.z, 0.05));
        n = normalize(lerp(ng, perturbed, decal.a.y));
    }
    float roughness = decal.a.z;
    if ((uint(decal.b.w + 0.5) & FLAG_ROUGHNESS_MAP) != 0u) {
        roughness = roughnessMap.SampleLevel(decalSampler, texCoord, lod).g;
    }
    roughness = clamp(roughness, 0.05, 1.0);
    float a = roughness * roughness;

    float3 base = albedo.rgb * decal.tint.rgb;
    float3 v = normalize(fx.cameraPosition.xyz - world);
    float3 l = frame.lightDirection.xyz;
    float nol = max(dot(n, l), 0.0);
    float nov = max(dot(n, v), 1e-4);
    // Every pixel of a quad calls this: the derivatives it takes
    float visible = sunVisibility(world, ng, l, input.position.xy);
    float3 h = normalize(l + v);
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float f0 = 0.04;
    float f = f0 + (1.0 - f0) * pow(1.0 - loh, 5.0);
    float3 sun = frame.lightColor.rgb * (nol * visible);
    float3 direct = base * (1.0 / FX_PI) * sun + distributionGGX(noh, a) * visibilitySmith(nov, nol, a) * f * sun;
    float strength = frame.viewport.w;
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb;
    float2 ab = brdfTable.SampleLevel(environmentSampler, float2(nov, roughness), 0.0).xy;
    float3 r = reflect(-v, n);
    float3 prefiltered = specularCube.SampleLevel(environmentSampler, r, roughness * (SPECULAR_LEVELS - 1.0)).rgb;
    // The environment's light, and the light's own ambient term (what a lit
    // material is lit by with no sky)
    float3 ambient = (base * irradiance + prefiltered * (f0 * ab.x + ab.y)) * strength + base * fx.ambient.rgb;
    float3 color = (direct + ambient) * frame.exposure.x;
    if (!inside) {
        alpha = 0.0;
    }
    return float4(color * alpha, alpha);
}
