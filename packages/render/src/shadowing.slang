// The render package's sun shadows as a receiver sees them, a Slang module
// ('import shadowing;'): lit.slang and pbr.slang ask 'sunVisibility' how much
// of the sun reaches a point, 1 in the open and 0 in full shadow.
//
// THE MAP. shadow.cone draws the scene's casters into a depth texture of up
// to four layers, one CASCADE each: an orthographic view from the sun of a
// slice of the camera's frustum, near cascades small and finely sampled, far
// ones large (Engel, "Cascaded Shadow Maps", ShaderX5, 2006; Dimitrov,
// "Cascaded Shadow Maps", NVIDIA 2007). 'shadowParams' (shadowparams.slang)
// holds each cascade's matrix, where it ends along the camera's view
// direction, and the filter's settings; the map and a comparison sampler
// are bindings 5 and 6 of group 0.
//
// WHICH CASCADE. The first whose end is beyond the point's view depth (its
// distance along the camera's forward, not its distance from the camera).
// In the band at a cascade's end the point is looked up in the next cascade
// too and the two are blended, so no seam shows; the last cascade's band
// fades to unshadowed, so shadows end softly at the shadow distance.
//
// FILTERING. Percentage-closer filtering (Reeves, Salesin and Cook, 1987):
// the comparison sampler gives the hardware's bilinear share of four texels
// that pass the depth test, and N such taps spread over a disk of the
// filter's radius, a Poisson disk (the 16-point set of the OpenGL tutorials,
// after GPU Gems), turned by a different angle at each pixel (Jimenez's
// interleaved gradient noise, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare", 2014) so the banding of a few taps becomes fine
// noise.
//
// BIAS, three parts, so that a surface does not shadow itself (acne) and a
// shadow does not detach from its caster (peter-panning):
// - RECEIVER-PLANE DEPTH BIAS (Isidoro, "Shadow Mapping: GPU-based Tips and
//   Techniques", GDC 2006): the depth the surface has at each tap, not only
//   at the middle one. The surface's depth gradient across the map is found
//   from the screen-space derivatives of the world position, which is
//   exact because an orthographic cascade is an affine map. A wide filter
//   would otherwise compare the middle's depth with a tilted plane's.
//   This is the slope-scaled part: the steeper the surface to the sun, the
//   more it moves the depth, exactly as much as the slope needs.
// - A NORMAL OFFSET (Holbert, "Normal Offset Shadows", 2011; Valient, "Shadow
//   Mapping", 2013's Killzone notes): the point is moved along its normal
//   by a texel or so, more at a grazing angle, before it is looked up.
// - A small constant, in texels of the cascade.
// The casters' pipelines add the rasterizer's own slope-scaled bias
// (shadow.cone).
//
// Called in uniform control flow: the derivatives need it.

import renderframe;
import shadowparams;

[[vk::binding(5, 0)]] Texture2DArray<float> shadowMap;
[[vk::binding(6, 0)]] SamplerComparisonState shadowSampler;

static const float2 POISSON[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

float interleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// The most a surface may tilt, as a slope (depth change per length across
// the map), before the receiver-plane gradient is held: 6 is about 80
// degrees from the light, where a surface is nearly edge on to the sun
static const float MAX_RECEIVER_SLOPE = 6.0;

// How much of the sun reaches 'world' in cascade 'c', 0 to 1; 'dWx' and 'dWy'
// are the world position's screen-space derivatives
float cascadeVisibility(int c, float3 world, float3 normal, float sinAngle, float3 dWx, float3 dWy, float turn) {
    float size = shadowParams.config.y;
    float texel = shadowParams.texelWorld[c];
    float range = shadowParams.depthRange[c];
    float4x4 m = shadowParams.cascade[c];
    // The normal offset
    float3 p = world + normal * (shadowParams.bias.y * texel * sinAngle);
    float4 clip = mul(m, float4(p, 1.0));
    float3 uvz = float3(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5, clip.z);
    if (uvz.z > 1.0 || uvz.z < 0.0) {
        return 1.0;
    }
    // The receiver plane's depth change across the map's u and v
    float3 cx = mul(m, float4(dWx, 0.0)).xyz;
    float3 cy = mul(m, float4(dWy, 0.0)).xyz;
    float ux = 0.5 * cx.x;
    float vx = -0.5 * cx.y;
    float uy = 0.5 * cy.x;
    float vy = -0.5 * cy.y;
    float det = ux * vy - vx * uy;
    float2 grad = float2(0.0, 0.0);
    if (abs(det) > 1e-14) {
        grad = float2(cx.z * vy - vx * cy.z, ux * cy.z - cx.z * uy) / det;
        float limit = MAX_RECEIVER_SLOPE * texel / range * size;
        grad = clamp(grad, float2(-limit, -limit), float2(limit, limit));
    }
    float biasZ = shadowParams.bias.x * texel / range;
    float radius = shadowParams.config.w / size;
    int taps = int(shadowParams.bias.z);
    float sn = sin(turn);
    float cs = cos(turn);
    float sum = 0.0;
    for (int i = 0; i < taps; i++) {
        float2 d = POISSON[i];
        float2 offset = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) * radius;
        float z = uvz.z + dot(grad, offset) - biasZ;
        sum += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uvz.xy + offset, float(c)), z);
    }
    return sum / float(max(taps, 1));
}

// How much of the sun, which travels the opposite way to the unit direction
// 'l', reaches 'world', on a surface whose geometric normal is 'ng'; seen
// at the pixel 'pixel'. A surface facing away from the sun is looked up as
// if it faced it (a leaf lit through from behind).
float sunVisibility(float3 world, float3 ng, float3 l, float2 pixel) {
    // Before any branch: the derivatives
    float3 dWx = ddx(world);
    float3 dWy = ddy(world);
    int count = int(shadowParams.config.x);
    if (count == 0) {
        return 1.0;
    }
    float viewDepth = dot(world - frame.cameraPosition.xyz, shadowParams.forward.xyz);
    int c = 0;
    for (int i = 0; i < count; i++) {
        if (viewDepth >= shadowParams.splitFar[i]) {
            c = i + 1;
        }
    }
    if (c >= count) {
        return 1.0;
    }
    float nl = dot(ng, l);
    float3 n = nl >= 0.0 ? ng : -ng;
    float cosAngle = abs(nl);
    float sinAngle = saturate(1.0 - cosAngle);
    float turn = 6.2831853 * interleavedGradientNoise(pixel);
    float v = cascadeVisibility(c, world, n, sinAngle, dWx, dWy, turn);
    float toEnd = shadowParams.splitFar[c] - viewDepth;
    float band = shadowParams.bandLen[c];
    if (toEnd < band) {
        float t = 1.0 - toEnd / band;
        if (c + 1 < count) {
            float next = cascadeVisibility(c + 1, world, n, sinAngle, dWx, dWy, turn);
            v = lerp(v, next, t);
        } else {
            v = lerp(v, 1.0, t);
        }
    }
    return v;
}
