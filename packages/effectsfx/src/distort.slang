// The effectsfx package's distortion: heat haze and shockwave rings, as refraction sprites.
//
// METHOD. The frame drawn so far (the HDR scene with its glow and smoke) is copied to a
// scratch texture (copyFragment, one full-screen pass), and each distortion is a camera-facing
// quad drawn over the frame whose fragments read the COPY at a displaced place and write what
// they read, blended by an edge mask: the "refraction" or "distortion" particle of Unreal
// and Unity, here with the displacement worked out from the effect's own description rather
// than a texture:
// - a SHOCKWAVE ring (kind 0): a front that leaves the centre and widens as it fades, its
//   displacement the derivative of a bump across the front (a lens: what is just inside the
//   front is pulled out, just outside pulled in), pointing along the radius;
// - HEAT HAZE (kind 1): a rising column whose displacement is two channels of gradient noise
//   scrolling upward, strongest in the middle of the column and fading at its edges and with age.
// The displacement is in the quad's own units times the quad's size on screen, so a ring
// bends the same share of itself near or far.
// Distortion sprites are depth tested in the fragment: one is dropped where the scene is
// nearer than the sprite by more than a hand's width (something in front of the blast is
// not bent by it), though what the displaced read picks up from in front is still read.
//
// slangc: -fp-mode precise

import noise;
import fxcommon;

struct DistortFrame {
    float4 timing;      // now, 0, 0, 0
    float4 right;       // xyz
    float4 up;
};

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] ConstantBuffer<DistortFrame> dframe;
[[vk::binding(2, 0)]] Texture2D<float4> copy;
[[vk::binding(3, 0)]] SamplerState copySampler;
[[vk::binding(4, 0)]] StructuredBuffer<float4> sprites;
[[vk::binding(5, 0)]] Texture2D<float> sceneDepth;

struct FullscreenOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOutput fullscreenVertex(uint vid : SV_VulkanVertexID) {
    FullscreenOutput o;
    float2 p = float2((vid << 1u) & 2u, vid & 2u);
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 copyFragment(FullscreenOutput input) : SV_Target {
    return copy.Load(int3(int2(input.position.xy), 0));
}

// A sprite is three float4s:
//   0: centre.xyz, radius (metres, the quad is twice as wide)
//   1: start time, duration, kind (0 ring, 1 haze), strength (a share of the radius in pixels)
//   2: ring width (0 to 1), rise speed (haze), seed (bits), 0
struct SpriteOutput {
    float4 position : SV_Position;
    float2 corner;
    float4 timing;          // t (age over duration), kind, strength, pixels per local unit
    float4 shape;
};

float2 spriteCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : -1.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0;
    return float2(x, y);
}

[shader("vertex")]
SpriteOutput spriteVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    float4 s0 = sprites[iid * 3u];
    float4 s1 = sprites[iid * 3u + 1u];
    float4 s2 = sprites[iid * 3u + 2u];
    SpriteOutput o;
    float t = (dframe.timing.x - s1.x) / s1.y;
    if (t < 0.0 || t >= 1.0) {
        o.position = float4(2.0, 2.0, 2.0, 1.0);
        o.corner = float2(0.0, 0.0);
        o.timing = float4(0.0, 0.0, 0.0, 0.0);
        o.shape = float4(0.0, 0.0, 0.0, 0.0);
        return o;
    }
    float2 c = spriteCorner(vid);
    // Tall for haze (a column), square for a ring
    float3 up = dframe.up.xyz;
    float3 right = dframe.right.xyz;
    float3 p = s0.xyz + right * (c.x * s0.w) + up * (c.y * s0.w * (s1.z > 0.5 ? 2.0 : 1.0));
    o.position = mul(fx.viewProjection, float4(p, 1.0));
    o.corner = c;
    float d = viewDepth(fx, o.position.z / o.position.w);
    float pixels = s0.w * fx.projection.w / max(d, 0.01) * fx.screen.y * 0.5;
    o.timing = float4(t, s1.z, s1.w, pixels);
    o.shape = float4(s2.x, s2.y, float(asuint(s2.z) & 65535u), 0.0);
    return o;
}

[shader("fragment")]
float4 spriteFragment(SpriteOutput input) : SV_Target {
    float2 pixel = input.position.xy;
    float z = sceneDepth.Load(int3(int2(pixel), 0));
    float sceneD = viewDepth(fx, z);
    float myD = viewDepth(fx, input.position.z);
    if (sceneD < myD - 0.15) { discard; }
    float t = input.timing.x;
    float2 c = input.corner;
    float strength = input.timing.z;
    float pixelsPerUnit = input.timing.w;
    float2 offset = float2(0.0, 0.0);
    float mask = 0.0;
    if (input.timing.y < 0.5) {
        // The ring: radius eased out (fast, then slow), a front of the given width, fading
        float r = length(c);
        float ring = 1.0 - (1.0 - t) * (1.0 - t);
        float front = ring * 0.92;
        float w = max(input.shape.x, 0.02) * (1.0 - 0.4 * t);
        float x = (r - front) / w;
        float bump = x * exp(-x * x * 1.2);
        float fall = (1.0 - t) * (1.0 - t);
        float2 dir = r > 1e-4 ? c / r : float2(0.0, 0.0);
        offset = dir * (bump * fall * strength);
        mask = smoothstep(1.0, 0.82, r) * saturate(abs(x) < 3.0 ? 1.0 : 0.0);
    } else {
        // Haze: a column of rising noise, strongest at its middle, gone at its edges and ends
        float3 q = float3(c.x * 2.2, c.y * 1.4 - t * 2.5 * input.shape.y, t * 0.6);
        uint seed = uint(input.shape.z);
        float nx = gradient3(q, seed).value;
        float ny = gradient3(q + float3(17.3, 5.1, 9.7), seed + 1u).value;
        float across = saturate(1.0 - c.x * c.x);
        float along = saturate(1.0 - abs(c.y + 0.1) * 0.9);
        mask = across * across * along * smoothstep(0.0, 0.15, t) * (1.0 - smoothstep(0.6, 1.0, t));
        offset = float2(nx, ny * 0.5 + 0.35) * (strength * mask);
    }
    // To screen: the sprite's y is up, the frame's v is down
    float2 shift = float2(offset.x, -offset.y) * pixelsPerUnit * fx.screen.zw;
    float2 uv = (pixel + 0.5) * fx.screen.zw + shift;
    float3 colour = copy.SampleLevel(copySampler, clamp(uv, 0.0, 0.9999), 0.0).rgb;
    return float4(colour, saturate(mask));
}
