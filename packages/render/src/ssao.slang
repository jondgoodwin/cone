// The render package's screen-space ambient occlusion: how much of the
// hemisphere above each visible point the nearby geometry hides, from the
// depth buffer alone. Full-screen passes, compiled ahead of time to ssao.spv
// by tools/shaders/shaders.py and embedded in ssao.cone.
//
// THE METHOD is GTAO, ground-truth ambient occlusion (Jimenez, Wu, Pesce and
// Jarabo, "Practical Real-Time Strategies for Accurate Indirect Occlusion",
// SIGGRAPH 2016), in the form of Intel's XeGTAO (Pavlov, 2021): for a few
// slices through the point (screen directions, rotated by noise), the highest
// horizon on each side of the point is found by marching the depth buffer,
// and the cosine-weighted visible arc between the two horizons, about the
// surface normal projected into the slice, is integrated in closed form,
// slice by slice, then averaged. No random hemisphere samples and no
// blurring away the noise of a few of them: the noise is the slice angle and
// the step offsets, and a depth-aware blur and (with temporal AA) the
// history take it out.
// - It is computed at HALF RESOLUTION from the depth at every other pixel,
//   then blurred along x and y by a bilateral filter (a Gaussian weighted by
//   the depth difference, so that AO does not bleed over an edge), and
//   applied at full resolution through a depth-aware upsample (the four
//   nearest half-resolution texels weighted by bilinear weight and depth
//   similarity).
// - The normal is rebuilt from the depth's neighbours (fxcommon.slang).
// - The occluder's weight falls off to nothing at the radius ('falloff' of
//   it), so a distant wall does not darken a floor.
// The AO is applied to the frame as a multiplier (see fx.cone).
//
// Group 0: the frame uniform (0), the depth (1: depth32float, loaded), and
// the AO texture the blur and the apply read (2). The immediates: ssao.cone's
// SsaoImmediates.

import fxcommon;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> aoTexture;

struct SsaoImmediates {
    float radius;                  // 0: world units
    float power;                   // 4: the visibility raised to this
    float falloff;                 // 8: the share of the radius over which an occluder fades out
    float strength;                // 12: how much of the AO is applied (0 to 1)
    uint slices;                   // 16
    uint steps;                    // 20
    float noise;                   // 24: this frame's offset of the noise, 0 to 1
    float maxPixels;               // 28: the most the radius spans on the screen
    float2 direction;              // 32: the blur's, in half-resolution texels
    float depthSensitivity;        // 40: the blur's and the upsample's: relative depth difference that halves a weight
    float pad;
};

[[vk::push_constant]] ConstantBuffer<SsaoImmediates> ssao;

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

static const float HALF_PI = 1.57079632679;

float depthAt(int2 c) {
    int2 size = int2(fx.screen.xy);
    return sceneDepth.Load(int3(clamp(c, int2(0, 0), size - 1), 0));
}

// The camera-space position of the full-resolution pixel c, or a very
// distant one for the sky
float3 positionAt(int2 c) {
    float z = depthAt(c);
    if (z >= 1.0) {
        return float3(0.0, 0.0, -1e6);
    }
    return viewPosition(fx, (float2(c) + 0.5) * fx.screen.zw, z);
}

[shader("fragment")]
float4 aoMain(FullscreenOutput input) : SV_Target {
    int2 hp = int2(input.position.xy);
    int2 fp = hp * 2;
    float z = depthAt(fp);
    if (z >= 1.0) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    float3 P = positionAt(fp);
    float3 N = normalFromNeighbours(P, positionAt(fp + int2(-1, 0)), positionAt(fp + int2(1, 0)),
        positionAt(fp + int2(0, -1)), positionAt(fp + int2(0, 1)));
    float3 V = normalize(-P);
    float d = -P.z;
    // The radius on the screen, in full-resolution pixels
    float radiusPx = ssao.radius * 0.5 * fx.screen.y * fx.projection.w / d;
    radiusPx = min(radiusPx, ssao.maxPixels);
    if (radiusPx < 1.5) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    float noiseSlice = frac(interleavedGradientNoise(float2(fp)) + ssao.noise);
    float noiseStep = frac(interleavedGradientNoise(float2(fp) + float2(5.0, 11.0)) + ssao.noise * 0.618);
    float falloffRange = max(ssao.falloff * ssao.radius, 1e-4);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = (ssao.radius - falloffRange) / falloffRange + 1.0;
    float visibility = 0.0;
    uint sliceCount = max(ssao.slices, 1u);
    uint stepCount = max(ssao.steps, 1u);
    for (uint slice = 0u; slice < sliceCount; slice++) {
        float phi = (float(slice) + noiseSlice) * FX_PI / float(sliceCount);
        float2 omega = float2(cos(phi), sin(phi));
        float3 directionVec = float3(omega.x, omega.y, 0.0);
        float3 orthoDirectionVec = directionVec - dot(directionVec, V) * V;
        float3 axisVec = normalize(cross(orthoDirectionVec, V));
        float3 projectedNormalVec = N - axisVec * dot(N, axisVec);
        float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));
        float projectedLength = max(length(projectedNormalVec), 1e-5);
        float cosNorm = saturate(dot(projectedNormalVec, V) / projectedLength);
        float n = signNorm * acos(cosNorm);
        float lowHorizonCos0 = cos(n + HALF_PI);
        float lowHorizonCos1 = cos(n - HALF_PI);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;
        float2 pixelDir = float2(omega.x, -omega.y);
        for (uint stp = 0u; stp < stepCount; stp++) {
            float s = (float(stp) + noiseStep) / float(stepCount);
            s = s * s;
            float2 offset = pixelDir * max(1.0, s * radiusPx);
            int2 c0 = fp + int2(round(offset));
            int2 c1 = fp - int2(round(offset));
            float3 d0 = positionAt(c0) - P;
            float3 d1 = positionAt(c1) - P;
            float len0 = max(length(d0), 1e-5);
            float len1 = max(length(d1), 1e-5);
            float shc0 = dot(d0 / len0, V);
            float shc1 = dot(d1 / len1, V);
            float w0 = saturate(len0 * falloffMul + falloffAdd);
            float w1 = saturate(len1 * falloffMul + falloffAdd);
            shc0 = lerp(lowHorizonCos0, shc0, w0);
            shc1 = lerp(lowHorizonCos1, shc1, w1);
            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }
        float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));
        float h1 = acos(clamp(horizonCos0, -1.0, 1.0));
        h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
        h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);
        float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += projectedLength * (iarc0 + iarc1);
    }
    // A few slices through an open surface add up to a little under 1 (the
    // estimate of an unoccluded plane measures 0.97 to 0.99, from the
    // normal rebuilt out of quantized depths): a gain of 1.04 gives it back
    visibility = saturate(visibility * 1.04 / float(sliceCount));
    float v = pow(visibility, ssao.power);
    // Where the radius spans only a few pixels, or the surface is seen nearly
    // edge on, the depth buffer cannot say what is occluded (its normal is
    // a guess from neighbours that are far apart): fade the AO out
    float reliable = saturate((radiusPx - 1.5) / 3.0) * saturate((dot(N, V) - 0.1) / 0.3);
    v = lerp(1.0, v, reliable);
    return float4(v, v, v, 1.0);
}

// The bilateral blur along 'direction' (half-resolution texels): seven taps
// of a Gaussian, each weighted down by its depth's difference from the
// centre's
[shader("fragment")]
float4 blurMain(FullscreenOutput input) : SV_Target {
    int2 hp = int2(input.position.xy);
    int2 halfSize = int2(max(fx.screen.xy * 0.5, float2(1.0, 1.0)));
    float centre = viewDepth(fx, depthAt(hp * 2));
    float sum = aoTexture.Load(int3(hp, 0)).r;
    float total = 1.0;
    for (int i = -2; i <= 2; i++) {
        if (i == 0) {
            continue;
        }
        int2 q = clamp(hp + int2(round(ssao.direction * float(i))), int2(0, 0), halfSize - 1);
        float dz = abs(viewDepth(fx, depthAt(q * 2)) - centre) / max(centre, 1e-3);
        float w = exp(-float(i * i) * 0.35) * exp(-dz * ssao.depthSensitivity);
        sum += aoTexture.Load(int3(q, 0)).r * w;
        total += w;
    }
    float v = sum / total;
    return float4(v, v, v, 1.0);
}

// The occlusion applied: a full-resolution pixel's AO is the half-resolution
// texels about it, weighted by bilinear weight and depth similarity; the
// frame is multiplied by it (blended: nothing added, 1 - AO of what is there
// taken away). The sky is left alone.
[shader("fragment")]
float4 applyMain(FullscreenOutput input) : SV_Target {
    int2 p = int2(input.position.xy);
    float z = depthAt(p);
    if (z >= 1.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    int2 halfSize = int2(max(fx.screen.xy * 0.5, float2(1.0, 1.0)));
    float centre = viewDepth(fx, z);
    // The half-resolution texel whose centre is at or before this pixel's
    float2 hpos = (float2(p) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(hpos));
    float2 f = hpos - float2(base);
    float sum = 0.0;
    float total = 0.0;
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            int2 q = clamp(base + int2(x, y), int2(0, 0), halfSize - 1);
            float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float dz = abs(viewDepth(fx, depthAt(q * 2)) - centre) / max(centre, 1e-3);
            float w = bilinear * exp(-dz * ssao.depthSensitivity * 2.0) + 1e-4;
            sum += aoTexture.Load(int3(q, 0)).r * w;
            total += w;
        }
    }
    float ao = lerp(1.0, sum / total, ssao.strength);
    return float4(0.0, 0.0, 0.0, 1.0 - ao);
}
