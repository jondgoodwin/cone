// The effectsfx package's flipbook explosions: a fireball that billows and then rolls into
// smoke, BAKED once on the GPU from a procedural volumetric model into an atlas of frames,
// and played as camera-facing billboards.
//
// WHY A FLIPBOOK, BAKED AT START-UP. A real-time ray-marched volume is the look the best
// shooters want, but it costs fragment time on every pixel an explosion covers, every frame:
// too much for the integrated GPU, which must stay usable. A flipbook costs a textured quad.
// The industry's flipbooks (Unreal's and Unity's explosion sheets, Guerrilla's Horizon fire)
// are baked offline from a fluid simulation; here the model is baked at start-up from data,
// so an explosion is a description (FlipbookDesc), not an asset.
//
// THE MODEL (bakeFragment), for a frame at normalised time t in [0, 1], in units where the
// billboard spans [-1, 1]:
// - A ball of fire and gas whose radius grows fast and then slows (1 - e^(-k t)), its centre
//   rising with the heat, its edge displaced by fBm (Perlin gradient noise, the noise
//   package) whose amplitude grows with time, so it starts a smooth flash and billows.
// - A temperature that falls with time and towards the edge, plus noise: hot cores
//   (the first fraction of a second) and cooler skin.
// - Emission from the temperature through a blackbody-like ramp (dark red, orange, yellow,
//   white) and HDR scale, so bloom does the glare.
// - Soot: the gas goes dark as it cools; an albedo and an opacity that rise with t.
// - A ray march (orthographic, along the view axis, 32 steps; Max 1995's emission and
//   absorption model, front to back) integrates emitted light and shaded smoke into a
//   premultiplied colour and an opacity. The smoke is lit by one baked direction with a
//   single shadow tap towards the light (Kniss et al. 2003, "Volume Rendering Techniques"),
//   so it has a lit side and a dark one.
// - Premultiplied output (rgb may exceed alpha: that is the emission added over the scene).
//
// PLAYBACK (drawVertex, drawFragment): one billboard a live explosion, camera-facing, rolled
// by its own angle; the frame is its age over its duration, two neighbouring frames blended
// (linear frame interpolation; the motion-vector warp of Guerrilla's flipbooks is not done);
// the depth test is in the fragment, with a soft fade into surfaces; everything is
// multiplied by the exposure the scene was drawn with.
//
// slangc: -fp-mode precise

import noise;
import fxcommon;

struct BakeParams {
    float4 grid;        // frames along a side, cell size in pixels, seed (bits), 0
    float4 shape;       // radius at the start, radius at the end, growth rate, rise
    float4 gas;         // turbulence, cooling rate, fire brightness, soot start (fraction of time)
    float4 smoke;       // albedo rgb, opacity scale
    float4 light;       // direction to the light xyz, ambient
};

struct BillboardFrame {
    float4 timing;      // now, 0, 0, 0
    float4 right;       // xyz, w the soft-particle distance
    float4 up;
};

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] ConstantBuffer<BillboardFrame> board;
[[vk::binding(2, 0)]] ConstantBuffer<BakeParams> bake;
[[vk::binding(3, 0)]] Texture2D<float4> atlas;
[[vk::binding(4, 0)]] SamplerState atlasSampler;
[[vk::binding(5, 0)]] StructuredBuffer<float4> boards;
[[vk::binding(6, 0)]] Texture2D<float> sceneDepth;

// ---- Baking ----

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

// Gas density and temperature at 'p' (the cloud's own units), time 't'
float2 gasAt(float3 p, float t, uint seed, int octaves) {
    // (the density's soft edge reaches in to about 0.7 of this radius, so it is scaled up to fill the cell)
    float R = lerp(bake.shape.x, bake.shape.y, 1.0 - exp(-bake.shape.z * t)) * 1.45;
    float3 c = float3(0.0, -0.12 + bake.shape.w * pow(t, 1.3), 0.0);
    float3 q = p - c;
    // Squash a little as it rises and cools (a heavy ball flattens into a cap)
    q.y *= 1.0 + 0.25 * t;
    // The edge is bent by a domain-warped fBm: billowing lobes (a cauliflower), growing with time
    float3 f = q * 3.1 + float3(0.0, -t * 1.1, 0.0);
    float3 warp = float3(0.0, 0.0, 0.0);
    if (octaves >= 3) {
        // (the cheap taps the shadow takes skip the warp and the clumps)
        warp = float3(fbm3(f * 0.7 + 3.1, seed + 3u, 2, 2.0, 0.5).value,
                      fbm3(f * 0.7 + 8.7, seed + 4u, 2, 2.0, 0.5).value,
                      fbm3(f * 0.7 + 1.3, seed + 5u, 2, 2.0, 0.5).value);
    }
    float n = fbm3(f + warp * 0.9, seed, octaves, 2.1, 0.55).value;
    float amp = bake.gas.x * (0.55 + 1.2 * t);
    float d = length(q) / R + n * amp;
    float dens = saturate(1.0 - smoothstep(0.4, 0.95, d));
    // Clumps inside the gas, so it is not a uniform ball
    float inner = 0.0;
    if (octaves >= 3) { inner = fbm3(f * 1.7 + 11.0, seed + 7u, 2, 2.0, 0.5).value; }
    dens = saturate(dens * (0.85 + 0.55 * inner));
    float core = saturate(1.0 - d);
    float cool = exp(-bake.gas.y * t);
    // The skin cools first: heat falls steeply towards the edge, with hot veins through the noise
    float heat = saturate(pow(core, 1.2) * cool * 1.5 + 0.5 * n * cool + 0.2 * inner * cool - 0.1);
    return float2(dens, heat);
}

float3 fireColor(float heat) {
    // dark red, orange, yellow, white: a blackbody-like ramp
    float3 a = float3(0.25, 0.02, 0.0);
    float3 b = float3(1.0, 0.28, 0.03);
    float3 c = float3(1.0, 0.72, 0.22);
    float3 d = float3(1.0, 0.95, 0.80);
    if (heat < 0.33) { return lerp(a, b, heat / 0.33); }
    if (heat < 0.66) { return lerp(b, c, (heat - 0.33) / 0.33); }
    return lerp(c, d, (heat - 0.66) / 0.34);
}

[shader("fragment")]
float4 bakeFragment(FullscreenOutput input) : SV_Target {
    uint grid = uint(bake.grid.x);
    float cell = bake.grid.y;
    uint seed = asuint(bake.grid.z);
    float2 px = input.position.xy;
    uint cx = uint(px.x / cell);
    uint cy = uint(px.y / cell);
    if (cx >= grid || cy >= grid) { return float4(0.0, 0.0, 0.0, 0.0); }
    uint frame = cy * grid + cx;
    float t = float(frame) / float(max(grid * grid - 1u, 1u));
    float2 local = (px - float2(cx, cy) * cell) / cell;
    float2 uv = float2(local.x * 2.0 - 1.0, 1.0 - local.y * 2.0);
    // The march: along the view axis, front (z = +1) to back (z = -1)
    const int STEPS = 32;
    float dz = 2.0 / float(STEPS);
    float3 L = normalize(bake.light.xyz);
    float trans = 1.0;
    float3 acc = float3(0.0, 0.0, 0.0);
    float soot = smoothstep(bake.gas.w, bake.gas.w + 0.45, t);
    for (int i = 0; i < STEPS; i++) {
        if (trans < 0.01) { break; }
        float z = 1.0 - (float(i) + 0.5) * dz;
        float3 p = float3(uv, z);
        float2 g = gasAt(p, t, seed, 4);
        float dens = g.x;
        if (dens < 0.004) { continue; }
        float heat = g.y;
        // Emission: only the hot gas shines; it fades as the gas cools
        float shine = pow(heat, 2.0) * (1.0 - 0.85 * soot * (1.0 - heat));
        float3 emit = fireColor(heat) * (shine * bake.gas.z);
        // Smoke: albedo lit by one baked light with one shadow tap, and an ambient floor
        float shadowDens = gasAt(p + L * 0.22, t, seed, 2).x;
        float shadow = exp(-shadowDens * 2.6);
        float3 lit = bake.smoke.xyz * (bake.light.w + (1.0 - bake.light.w) * shadow);
        float sigma = dens * (1.4 + 5.0 * soot) * bake.smoke.w;
        float a = 1.0 - exp(-sigma * dz * 2.2);
        float emissive = dens * dz * 2.2 * (1.0 - soot * 0.5);
        acc += trans * (emit * emissive * 1.6 + lit * a * soot * 0.9 + lit * a * (1.0 - soot) * 0.15);
        trans *= (1.0 - a);
    }
    // The whole thing fades out at the end of the flipbook
    float fade = 1.0 - smoothstep(0.86, 1.0, t);
    // And a soft edge to the cell, so playback never shows its square
    float edge = smoothstep(1.0, 0.86, max(abs(uv.x), abs(uv.y)));
    float alpha = (1.0 - trans) * fade * edge;
    return float4(acc * fade * edge, alpha);
}

// ---- Playback ----

struct BoardOutput {
    float4 position : SV_Position;
    float2 corner;
    float4 tint;                // rgb, opacity
    float3 frames;              // frame a, frame b, the blend
};

float2 boardCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : -1.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0;
    return float2(x, y);
}

[shader("vertex")]
BoardOutput drawVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    float4 i0 = boards[iid * 3u];
    float4 i1 = boards[iid * 3u + 1u];
    float4 i2 = boards[iid * 3u + 2u];
    BoardOutput o;
    float age = board.timing.x - i1.x;
    float t = age / i1.y;
    if (t < 0.0 || t >= 1.0) {
        o.position = float4(2.0, 2.0, 2.0, 1.0);
        o.corner = float2(0.0, 0.0);
        o.tint = float4(0.0, 0.0, 0.0, 0.0);
        o.frames = float3(0.0, 0.0, 0.0);
        return o;
    }
    float2 c = boardCorner(vid);
    float ca = cos(i1.z);
    float sa = sin(i1.z);
    float3 r = board.right.xyz * ca + board.up.xyz * sa;
    float3 u = -board.right.xyz * sa + board.up.xyz * ca;
    float3 p = i0.xyz + (r * c.x + u * c.y) * i0.w;
    o.position = mul(fx.viewProjection, float4(p, 1.0));
    o.corner = c;
    o.tint = float4(i2.xyz, i2.w);
    float grid = bake.grid.x;
    float last = grid * grid - 1.0;
    float f = t * last;
    float f0 = floor(f);
    o.frames = float3(f0, min(f0 + 1.0, last), f - f0);
    return o;
}

float4 sampleFrame(float frame, float2 corner) {
    float grid = bake.grid.x;
    float fx0 = fmod(frame, grid);
    float fy0 = floor(frame / grid);
    float2 uv = (float2(fx0, fy0) + float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5)) / grid;
    return atlas.SampleLevel(atlasSampler, uv, 0.0);
}

[shader("fragment")]
float4 drawFragment(BoardOutput input) : SV_Target {
    float4 a = sampleFrame(input.frames.x, input.corner);
    float4 b = sampleFrame(input.frames.y, input.corner);
    float4 s = lerp(a, b, input.frames.z);
    float z = sceneDepth.Load(int3(int2(input.position.xy), 0));
    float sceneD = viewDepth(fx, z);
    float myD = viewDepth(fx, input.position.z);
    float soft = saturate((sceneD - myD) / max(board.right.w, 0.001));
    float k = soft * input.tint.w;
    return float4(s.rgb * input.tint.rgb * (k * fx.sunDirection.w), s.a * k);
}
