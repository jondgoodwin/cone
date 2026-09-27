// slangc: -fp-mode precise
//
// hornmarch's shader: the sdf package's horn, sphere-traced in a full-screen
// pass (no mesh), in black chitin under the dusk sky. It draws into the
// half-float frame that render's Post tone-maps.
//
// The material is render's physically based chitin (pbr.slang's layers,
// with PbrMaterial.chitin's numbers): a dark rough base, a smooth clear
// coat, and Belcour and Barla's thin film on the coat, its thickness noise's
// warped fBm of the horn's own coordinates, drifting with time. It is lit by
// the sun (the sky's direction, pipepbr's colour) and by the sky's
// image-based lighting, the renderer's Environment. The sun is shadowed and
// the sky occluded by the distance field itself (Quilez's soft shadows and
// ambient occlusion, iquilezles.org/articles/rmshadows and
// iquilezles.org/articles/raymarchingdf).
//
// Group 0 is the renderer's environment group (the sky's parameters, the
// cubes, the table and their sampler); the immediates are the camera and
// the horn.

import sdf;
import brdf;
import sky;
import noise;

struct Immediates {
    float4 eye;                    // 0: xyz the camera; w the time in seconds
    float4 right;                  // 16: xyz the camera's right, times tan(fov / 2) * aspect
    float4 up;                     // 32: xyz its up, times tan(fov / 2); w 1 if the film is on
    float4 forward;                // 48: xyz where it looks, unit; w the environment's strength
    float4 hornA;                  // 64: length, base radius, taper, curl
    float4 hornB;                  // 80: ribs, rib depth, and the horn's offset along x
    float4 turn;                   // 96: the quaternion turning the horn (x, y, z, w)
    float4 viewport;               // 112: width, height in pixels
};

[[vk::binding(0, 0)]] ConstantBuffer<SkyParams> skyParams;
[[vk::binding(1, 0)]] TextureCube irradianceCube;
[[vk::binding(2, 0)]] TextureCube specularCube;
[[vk::binding(3, 0)]] Texture2D brdfTable;
[[vk::binding(4, 0)]] SamplerState environmentSampler;
[[vk::push_constant]] ConstantBuffer<Immediates> draw;

static const float SPECULAR_LEVELS = 6.0;
static const float COAT_IOR = 1.5;
static const int MAX_STEPS = 160;
static const float FAR = 40.0;

// The scene: the horn, turned and set on its base
struct Scene : IShape {
    Horn horn;
    float4 turn;
    float offset;
    float distance(float3 p) {
        float3 q = rotate(p, turn);
        return horn.distance(float3(q.x + offset, q.y, q.z));
    }
};

Scene makeScene() {
    Scene s;
    s.horn = makeHorn(draw.hornA.x, draw.hornA.y, draw.hornA.z, draw.hornA.w, int(draw.hornB.x), draw.hornB.y);
    s.turn = draw.turn;
    s.offset = draw.hornB.z;
    return s;
}

// The horn's own coordinates at world point p, where the film's noise is read
float3 hornLocal(Scene s, float3 p) {
    float3 q = rotate(p, s.turn);
    return float3(q.x + s.offset, q.y, q.z);
}

// Where the ray first meets the surface, or a negative distance
float trace(Scene s, float3 ro, float3 rd) {
    // A ball round the horn: miss it and miss the horn
    float3 oc = ro - float3(0.0, 1.2, 0.0);
    float b = dot(oc, rd);
    float c = dot(oc, oc) - 2.6 * 2.6;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float t = max(-b - sqrt(disc), 0.0);
    float tEnd = -b + sqrt(disc);
    for (int i = 0; i < MAX_STEPS && t < tEnd; i++) {
        float d = s.distance(ro + rd * t);
        if (d < 0.0004 * t) return t;
        t += d;
    }
    return -1.0;
}

// Soft shadow towards the light: 1 lit, 0 in shadow
float softShadow(Scene s, float3 ro, float3 rd) {
    float result = 1.0;
    float t = 0.02;
    for (int i = 0; i < 48 && t < 6.0; i++) {
        float h = s.distance(ro + rd * t);
        result = min(result, 10.0 * h / t);
        if (result < 0.001) break;
        t += clamp(h, 0.01, 0.25);
    }
    return saturate(result);
}

// Ambient occlusion along the normal: 1 open, 0 closed in
float occlusion(Scene s, float3 p, float3 n) {
    float occ = 0.0;
    float weight = 1.0;
    for (int i = 1; i <= 5; i++) {
        float h = 0.02 + 0.06 * float(i);
        occ += (h - s.distance(p + n * h)) * weight;
        weight *= 0.7;
    }
    return saturate(1.0 - 2.5 * occ);
}

// The film's thickness at horn point p, at time t: PbrMaterial.chitin's
// noise (scale 1.6, drift 0.08 a second, warp 1.5, seed 7), 180 to 520 nm
float filmThickness(float3 p, float t) {
    float3 q = p * 1.6 + float3(0.31, 1.0, 0.17) * (t * 0.08);
    float w = warp3(q, 7u, 2, 1.5);
    float k = saturate(0.5 + 0.8 * w);
    return lerp(180.0, 520.0, k);
}

// The chitin, lit, at point p with normal n, seen along -v
float3 shade(Scene s, float3 p, float3 n, float3 v) {
    float nov = clamp(dot(n, v), 1e-4, 1.0);
    float3 l = skyParams.sunDirection.xyz;
    float3 h = normalize(l + v);
    float nol = saturate(dot(n, l));
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float3 r = reflect(-v, n);
    float3 baseColor = float3(0.010, 0.009, 0.012);
    float roughness = 0.45;
    float a = roughness * roughness;
    float coatRoughness = 0.06;
    float coatA = coatRoughness * coatRoughness;
    float3 f0 = float3(0.04, 0.04, 0.04);
    float2 ab = brdfTable.SampleLevel(environmentSampler, float2(nov, roughness), 0.0).xy;
    float3 energy = 1.0 + f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0);
    bool film = draw.up.w > 0.5;
    float thickness = film ? filmThickness(hornLocal(s, p), draw.eye.w) : 0.0;
    float coatF0 = iorToF0(COAT_IOR);
    float3 coatFv = film ? thinFilmReflectance(nov, 1.8, COAT_IOR, thickness)
        : float3(1.0, 1.0, 1.0) * fresnelSchlick1(coatF0, nov);
    float3 coatFl = film ? thinFilmReflectance(loh, 1.8, COAT_IOR, thickness)
        : float3(1.0, 1.0, 1.0) * fresnelSchlick1(coatF0, loh);
    // The sun, shadowed by the horn itself
    float3 direct = float3(0.0, 0.0, 0.0);
    if (nol > 0.0) {
        float shadow = softShadow(s, p + n * 0.002, l);
        float3 f = fresnelSchlick(f0, loh);
        float3 specular = distributionGGX(noh, a) * visibilitySmithGGX(nov, nol, a) * f * energy;
        float3 diffuse = baseColor * (1.0 / PI);
        float3 baseLobe = (diffuse + specular) * (1.0 - coatFl);
        float3 coatLobe = distributionGGX(noh, coatA) * visibilityKelemen(loh) * coatFl;
        direct = (baseLobe + coatLobe) * float3(2.0, 0.95, 0.45) * (nol * shadow);
    }
    // The sky, occluded by the horn itself
    float occ = occlusion(s, p, n);
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb;
    float3 prefiltered = specularCube.SampleLevel(environmentSampler, r, roughness * (SPECULAR_LEVELS - 1.0)).rgb;
    float3 envSpecular = prefiltered * (f0 * ab.x + ab.y) * energy;
    float3 envBase = (baseColor * irradiance + envSpecular) * (1.0 - coatFv);
    float2 coatAb = brdfTable.SampleLevel(environmentSampler, float2(nov, coatRoughness), 0.0).xy;
    float3 coatPrefiltered = specularCube.SampleLevel(environmentSampler, r,
        coatRoughness * (SPECULAR_LEVELS - 1.0)).rgb;
    float3 envCoat = coatPrefiltered * coatFv * (coatAb.x + coatAb.y);
    return direct + (envBase + envCoat) * (draw.forward.w * occ);
}

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 ndc = float2(input.position.x / draw.viewport.x * 2.0 - 1.0,
        1.0 - input.position.y / draw.viewport.y * 2.0);
    float3 ro = draw.eye.xyz;
    float3 rd = normalize(draw.forward.xyz + draw.right.xyz * ndc.x + draw.up.xyz * ndc.y);
    Scene s = makeScene();
    float t = trace(s, ro, rd);
    if (t < 0.0) return float4(skyRadiance(rd, skyParams, true), 1.0);
    float3 p = ro + rd * t;
    float3 n = normal(s, p, 0.0005);
    return float4(shade(s, p, n, -rd), 1.0);
}
