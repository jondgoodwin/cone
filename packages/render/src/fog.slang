// The render package's volumetric fog: a froxel volume of participating
// medium lit by the sun through the cascaded shadow map (so shafts of light
// stand in the fog where something shades it: god rays), integrated front to
// back along each pixel's ray and applied over the frame. Compute passes
// (inject, integrate) and a full-screen pass (apply), compiled ahead of time
// to fog.spv by tools/shaders/shaders.py and embedded in fog.cone.
//
// THE METHOD is the froxel one of Wronski ("Volumetric Fog: Unified compute
// shader based solution to atmospheric scattering", SIGGRAPH 2014, Assassin's
// Creed 4) and Hillaire ("Physically Based and Unified Volumetric Rendering in
// Frostbite", SIGGRAPH 2015):
// - A VOLUME of froxels (frustum voxels): gridX x gridY cells across the
//   screen, 'slices' deep, the slices spaced EXPONENTIALLY in distance (finer
//   near the camera) from 'near' to 'far'.
// - INJECT, one thread a froxel: the medium's extinction and scattering at
//   the froxel's world position (a uniform density plus an exponential
//   height fog, density d0 e^(-k max(y - y0, 0)) as Unreal's exponential height
//   fog and Wronski's, the scattering the albedo times it), and the light
//   scattered towards the camera: the sun's irradiance times its visibility
//   (the cascaded shadow map, a few taps) times the Henyey-Greenstein phase
//   function (1941; cosine of the angle between the light's direction of
//   travel and the view ray; anisotropy g, forward scattering for g > 0),
//   plus the ambient radiance (isotropic: its phase integrates to 1). Each
//   froxel's position is JITTERED within its cell by the Halton sequence
//   each frame and the volume is blended with the last frame's, reprojected
//   (Wronski; Hillaire's temporal filter), so the few taps' noise and the
//   froxels' banding average out.
// - INTEGRATE, one thread a column of froxels: from the camera outwards,
//   the in-scattered light accumulated along the ray, attenuated by the
//   transmittance so far (Beer-Lambert), in the energy-conserving form of
//   Hillaire's: (L - L e^(-sigma dz)) / sigma over a slice. Each slice then
//   holds the light scattered in front of it and the transmittance to it.
// - APPLY, a pixel: the integrated volume read at the pixel's depth (trilinear)
//   and composited over the frame: frame x transmittance + scattered light.
//   The sky (no depth) takes the whole volume's.
// Light is in the frame's units: the sun's irradiance in lux and the ambient
// in nits if the scene is, times the exposure the frame was drawn with.
//
// The inject and integrate passes' group: the frame uniform (0), the sun's
// shadow parameters (4), the cascade map (5, depth array) and its comparison
// sampler (6), the volume read (7, the history, or the volume to integrate),
// a linear sampler (8) and the volume written (9, a storage texture). The
// apply pass: the frame uniform (0), the depth (1), the integrated volume (7)
// and the sampler (8). The immediates: fog.cone's FogImmediates.

import fxcommon;
import shadowparams;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(5, 0)]] Texture2DArray<float> shadowMap;
[[vk::binding(6, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(7, 0)]] Texture3D<float4> readVolume;
[[vk::binding(8, 0)]] SamplerState linearClamp;
[[vk::binding(9, 0)]] [format("rgba16f")] WTexture3D<float4> writeVolume;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;

struct FogImmediates {
    float density;                 // 0: the uniform extinction, per metre
    float heightDensity;           // 4: the height fog's, at its base height
    float heightFalloff;           // 8: per metre above it
    float baseHeight;              // 12: metres
    float4 albedo;                 // 16: rgb, the scattering share of the extinction
    float anisotropy;              // 32: Henyey-Greenstein g
    float sunScattering;           // 36: a multiplier on the sun's light
    float ambientScattering;       // 40: and on the ambient's
    float temporal;                // 44: the share of this frame in the volume (1: no history)
    float nearDistance;            // 48: where the first slice begins, metres
    float farDistance;             // 52: and the last ends
    uint slices;                   // 56
    uint gridX;                    // 60
    uint gridY;                    // 64
    uint frame;                    // 68
    uint shadowTaps;               // 72: 0 to 8
    float pad;                     // 76
};

[[vk::push_constant]] ConstantBuffer<FogImmediates> fog;

static const float2 POISSON[8] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.094184101, -0.92938870),
    float2(0.34495938, 0.29387760), float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379)
};

// How much of the sun reaches 'world', which is 'depth' in front of the
// camera: the cascade whose range holds it, a few comparison taps over a disc
// of a texel or two, blended into the next cascade (and, past the last,
// faded out) in the band at the end of each, as shadowing.slang does it
float cascadeShadow(int c, float3 world, float turn) {
    float size = shadowParams.config.y;
    float texel = shadowParams.texelWorld[c];
    float range = shadowParams.depthRange[c];
    float4 clip = mul(shadowParams.cascade[c], float4(world, 1.0));
    float3 uvz = float3(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5, clip.z);
    if (uvz.z > 1.0 || uvz.z < 0.0) {
        return 1.0;
    }
    float biasZ = (shadowParams.bias.x + 1.5) * texel / range;
    float radius = shadowParams.config.w / size;
    float sn = sin(turn);
    float cs = cos(turn);
    float sum = 0.0;
    uint taps = max(fog.shadowTaps, 1u);
    for (uint i = 0u; i < taps; i++) {
        float2 d = POISSON[i];
        float2 offset = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) * radius;
        sum += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uvz.xy + offset, float(c)), uvz.z - biasZ);
    }
    return sum / float(taps);
}

float sunShadow(float3 world, float depth, float turn) {
    int count = int(shadowParams.config.x);
    if (count == 0) {
        return 1.0;
    }
    int c = 0;
    for (int i = 0; i < count; i++) {
        if (depth >= shadowParams.splitFar[i]) {
            c = i + 1;
        }
    }
    if (c >= count) {
        return 1.0;
    }
    float v = cascadeShadow(c, world, turn);
    float toEnd = shadowParams.splitFar[c] - depth;
    float band = shadowParams.bandLen[c];
    if (toEnd < band) {
        float t = 1.0 - toEnd / band;
        if (c + 1 < count) {
            v = lerp(v, cascadeShadow(c + 1, world, turn), t);
        } else {
            v = lerp(v, 1.0, t);
        }
    }
    return v;
}

// Henyey and Greenstein's phase function (1941): the light scattered through
// an angle whose cosine is 'c', per steradian
float phaseHG(float c, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * c;
    return (1.0 - g2) / (4.0 * FX_PI * pow(max(denom, 1e-4), 1.5));
}

// The distance of the slice boundary 'k' (0 to slices)
float boundary(float k) {
    return fog.nearDistance * pow(fog.farDistance / fog.nearDistance, k / float(fog.slices));
}

[shader("compute")]
[numthreads(8, 8, 4)]
void injectMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fog.gridX || id.y >= fog.gridY || id.z >= fog.slices) {
        return;
    }
    // The froxel's sample, jittered inside it by this frame's Halton numbers
    uint h = fog.frame % 16u + 1u;
    float3 jitter = float3(halton(h, 2u), halton(h, 3u), halton(h, 5u));
    if (fog.temporal >= 1.0) {
        jitter = float3(0.5, 0.5, 0.5);
    }
    float2 uv = (float2(id.xy) + jitter.xy) / float2(float(fog.gridX), float(fog.gridY));
    float k = float(id.z) + jitter.z;
    float d = boundary(k);
    float zNdc = fx.projection.y / d - fx.projection.x;
    float3 world = worldPosition(fx, uv, zNdc);

    // The medium here
    float density = fog.density + fog.heightDensity * exp(-fog.heightFalloff * max(world.y - fog.baseHeight, 0.0));
    float3 scattering = fog.albedo.rgb * density;
    float3 toPoint = normalize(world - fx.cameraPosition.xyz);
    float cosTheta = dot(toPoint, fx.sunDirection.xyz);
    float turn = 6.2831853 * interleavedGradientNoise(float2(id.xy) + float2(float(id.z) * 3.7, 0.0));
    float visible = sunShadow(world, d, turn);
    float3 sun = fx.sunColor.rgb * (visible * phaseHG(cosTheta, fog.anisotropy) * fog.sunScattering);
    float3 ambient = fx.ambient.rgb * fog.ambientScattering;
    float3 inscattered = scattering * (sun + ambient) * fx.sunDirection.w;
    float4 now = float4(inscattered, density);

    // Blended with last frame's volume where the same point was
    if (fog.temporal < 1.0 && fx.misc.z < 0.5) {
        float3 before = projectToUv(fx.previousViewProjection, world);
        float dBefore = viewDepth(fx, before.z);
        float w = log(dBefore / fog.nearDistance) / log(fog.farDistance / fog.nearDistance);
        if (before.x >= 0.0 && before.x <= 1.0 && before.y >= 0.0 && before.y <= 1.0 && w >= 0.0 && w <= 1.0) {
            float4 old = readVolume.SampleLevel(linearClamp, float3(before.xy, w), 0.0);
            now = lerp(old, now, fog.temporal);
        }
    }
    writeVolume.Store(id, now);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fog.gridX || id.y >= fog.gridY) {
        return;
    }
    float3 scattered = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    for (uint z = 0u; z < fog.slices; z++) {
        float4 v = readVolume.Load(int4(int(id.x), int(id.y), int(z), 0));
        // The first slice takes in the stretch from the camera to the volume's near end
        float dz = boundary(float(z + 1u)) - (z == 0u ? 0.0 : boundary(float(z)));
        float sigma = max(v.a, 1e-6);
        float through = exp(-sigma * dz);
        // The light scattered in the slice, attenuated on its way to the camera
        // (L - L through) / sigma, with L the light per unit extinction here
        float3 light = v.rgb / sigma;
        scattered += transmittance * (light - light * through);
        transmittance *= through;
        writeVolume.Store(uint3(id.x, id.y, z), float4(scattered, transmittance));
    }
}

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y);
    return output;
}

// The integrated volume at the pixel's depth, composited (premultiplied):
// the light scattered in front of it, the frame behind it dimmed by the
// transmittance
[shader("fragment")]
float4 applyMain(FullscreenOutput input) : SV_Target {
    int2 p = int2(input.position.xy);
    float z = sceneDepth.Load(int3(p, 0));
    float d = z >= 1.0 ? fog.farDistance : viewDepth(fx, z);
    float b = clamp(log(max(d, fog.nearDistance) / fog.nearDistance) / log(fog.farDistance / fog.nearDistance), 0.0, 1.0) * float(fog.slices);
    // Layer k holds the integral to the END of slice k (boundary k + 1): the
    // value at boundary b is read at the layer coordinate b - 0.5
    float coordinate = clamp((b - 0.5) / float(fog.slices), 0.5 / float(fog.slices), 1.0 - 0.5 / float(fog.slices));
    float4 v = readVolume.SampleLevel(linearClamp, float3(input.uv, coordinate), 0.0);
    // Inside the first slice (which begins at the camera) the integral grows from nothing, by distance
    float first = boundary(1.0);
    if (d < first) {
        float f = d / first;
        float4 v0 = readVolume.SampleLevel(linearClamp, float3(input.uv, 0.5 / float(fog.slices)), 0.0);
        v = float4(v0.rgb * f, lerp(1.0, v0.a, f));
    }
    return float4(v.rgb, 1.0 - v.a);
}
