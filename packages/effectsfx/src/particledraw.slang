// The effectsfx package's particle drawing: the pool (particlecommon.slang) drawn into
// the HDR frame after the opaque scene and before bloom and tone mapping, as one draw of
// POOL instances a style, no vertex buffer: instance i is slot i, and a dead slot or one
// of the other style collapses to nothing.
//
// - Sparks (style 0): additive streaks, a soft glow stretched along the particle's motion
//   across the view, its head at the particle (the same velocity-aligned billboard as
//   the stateless sprites of the vfx package). Light added in any order, so nothing is
//   sorted.
// - Smoke (style 1): soft puffs over what is behind them, premultiplied alpha, lit by the
//   sun and the ambient light from the way the puff would face (a billboard is given
//   the normal of a hemisphere, so the lit side is toward the sun), drawn unsorted
//   (a puff-on-puff overlap is therefore order dependent; an honest limit, see
//   Design\effectsfx.md).
//
// The depth test is done here: the scene's depth is a sampled texture, so the pass
// has no depth attachment (a texture cannot be both sampled and written in one pass).
// A fragment behind the stored surface is dropped; one just in front of it is faded over
// the class's softness distance ("soft particles", Lorach, NVIDIA 2007), so a puff
// meets the floor without a hard line.
//
// Everything written is multiplied by the exposure the scene was drawn with
// (fxcommon's FxFrame.sunDirection.w), as the scene's own pixels are.
//
// slangc: -fp-mode precise

import noise;
import fxcommon;
import particlecommon;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] ConstantBuffer<SimFrame> sim;
[[vk::binding(2, 0)]] StructuredBuffer<float4> classes;
[[vk::binding(4, 0)]] StructuredBuffer<float4> pool;
[[vk::binding(5, 0)]] Texture2D<float> sceneDepth;

struct VertexOutput {
    float4 position : SV_Position;
    float2 corner;              // in [-1, 1]
    float4 color;               // a spark's rgb is light to add; a smoke's rgb is its albedo and a its opacity
    float3 lightBasis;          // the sun's direction in the billboard's right, up and toward-camera
    float softness;             // metres over which it fades into a surface
};

VertexOutput nothing() {
    VertexOutput o;
    o.position = float4(2.0, 2.0, 2.0, 1.0);
    o.corner = float2(0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    o.lightBasis = float3(0.0, 0.0, 0.0);
    o.softness = 1.0;
    return o;
}

VertexOutput particleQuad(uint vid, uint iid, uint wantStyle) {
    if (iid >= sim.counts.x) { return nothing(); }
    float4 a = pool[iid * 3u];
    float4 b = pool[iid * 3u + 1u];
    float4 m = pool[iid * 3u + 2u];
    if (a.w >= b.w) { return nothing(); }
    uint cls = asuint(m.x);
    float4 c3 = classRow(classes, cls, 3u);
    float4 c4 = classRow(classes, cls, 4u);
    if (uint(c4.w + 0.5) != wantStyle) { return nothing(); }
    float t = a.w / b.w;
    float4 col = colorOver(classes, cls, t);
    float fade = smoothstep(0.0, max(c4.y, 1e-4), t) * (1.0 - smoothstep(1.0 - max(c4.z, 1e-4), 1.0, t));
    float size = lerp(c3.z, c3.w, t) * m.w;
    float3 eye = fx.cameraPosition.xyz;
    float3 toCamera = normalize(eye - a.xyz);
    float2 c = quadCorner(vid);
    VertexOutput o;
    o.corner = c;
    o.softness = (wantStyle == 0u) ? 0.04 : max(sim.right.w, 0.001);
    float3 p;
    if (wantStyle == 0u) {
        float3 v = b.xyz;
        float3 across = v - toCamera * dot(v, toCamera);
        float speed = length(across);
        float streak = speed * c4.x;
        float3 axis = sim.up.xyz;
        if (streak > 0.0001 * size) { axis = across / speed; }
        float3 side = normalize(cross(axis, toCamera));
        float half = 0.5 * size;
        float3 middle = a.xyz - axis * (0.5 * streak);
        p = middle + side * (c.x * half) + axis * (c.y * (half + 0.5 * streak));
        o.color = float4(col.xyz * (col.w * fade * fx.sunDirection.w), 1.0);
        o.lightBasis = float3(0.0, 0.0, 0.0);
    } else {
        // A puff turns slowly as it ages, so a column of them does not look stamped
        float ang = 6.2831853 * m.y + a.w * 0.35;
        float ca = cos(ang);
        float sa = sin(ang);
        float3 r = sim.right.xyz * ca + sim.up.xyz * sa;
        float3 u = -sim.right.xyz * sa + sim.up.xyz * ca;
        p = a.xyz + (r * c.x + u * c.y) * (0.5 * size);
        float3 sun = fx.sunDirection.xyz;
        o.lightBasis = float3(dot(sun, r), dot(sun, u), dot(sun, toCamera));
        o.color = float4(col.xyz, col.w * fade);
    }
    o.position = mul(fx.viewProjection, float4(p, 1.0));
    return o;
}

[shader("vertex")]
VertexOutput sparkVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    return particleQuad(vid, iid, 0u);
}

[shader("vertex")]
VertexOutput smokeVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    return particleQuad(vid, iid, 1u);
}

// 1 where the fragment is in front of the scene by 'softness', 0 where it is behind
float depthFade(float4 svPosition, float softness) {
    float z = sceneDepth.Load(int3(int2(svPosition.xy), 0));
    float sceneD = viewDepth(fx, z);
    float myD = viewDepth(fx, svPosition.z);
    return saturate((sceneD - myD) / softness);
}

[shader("fragment")]
float4 sparkFragment(VertexOutput input) : SV_Target {
    float r2 = dot(input.corner, input.corner);
    float glow = max(1.0 - r2, 0.0);
    glow = glow * glow;
    float core = pow(max(1.0 - r2, 0.0), 6.0);
    float f = depthFade(input.position, input.softness);
    return float4(input.color.xyz * ((glow + 1.5 * core) * f), 0.0);
}

[shader("fragment")]
float4 smokeFragment(VertexOutput input) : SV_Target {
    float r2 = dot(input.corner, input.corner);
    if (r2 >= 1.0) { discard; }
    // A ragged edge from a little noise, so the puff is not a disc
    float n = value3(float3(input.corner * 1.7, 0.5), 3u).value;
    float soft = saturate(1.0 - r2 * (1.0 + 0.5 * n));
    soft = soft * soft * (3.0 - 2.0 * soft);
    float a = saturate(input.color.w) * soft * depthFade(input.position, input.softness);
    float z = sqrt(max(1.0 - r2, 0.0));
    float ndl = dot(float3(input.corner, z), input.lightBasis);
    float wrap = saturate(0.5 + 0.5 * ndl);
    float3 lit = fx.ambient.xyz + fx.sunColor.xyz * (wrap * 0.3183099);
    float3 rgb = input.color.xyz * lit * fx.sunDirection.w;
    return float4(rgb * a, a);
}
