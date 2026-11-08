// The effectsfx package's fluid volume renderer: the smoke and fire the solver (fluid.slang)
// simulates, drawn by ray marching.
//
// One full-screen triangle, scissored by the program to the box's bounds on the screen; each
// pixel intersects its ray with the box (slab test), clips it at the scene's depth (so the
// scene hides the volume, and the volume sits in front of the scene it is in front of), and ray
// marches (Max 1995's emission-absorption, front to back, a jittered start so the steps do not
// band): the gas glows with a blackbody-like ramp of its temperature and is shaded as soot, lit
// by the sun through one shadow tap and an ambient floor. Premultiplied alpha, multiplied by the
// exposure the scene was drawn with.
//
// slangc: -fp-mode precise

import noise;
import fxcommon;

struct FluidParams {
    uint4 dims;         // nx, ny, nz, frame
    float4 box;         // origin xyz (world), cell size
    float4 physics;     // dt, buoyancy, soot weight, vorticity
    float4 source;      // emitter centre xyz (world), radius
    float4 burn;        // emitter temperature, fuel rate (density a second), cooling a second, dissipation a second
    float4 more;        // ambient temperature, velocity damping a second, emitter upward speed, seed (bits)
    float4 look;        // brightness, absorption, steps (bits), shadow strength
    float4 soot;        // soot albedo rgb, ambient light
};

struct Immediates {
    uint parity;        // which copy of the gas to draw
    uint pressure;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ConstantBuffer<Immediates> imm;

[[vk::binding(0, 0)]] ConstantBuffer<FluidParams> P;
[[vk::binding(3, 0)]] StructuredBuffer<float2> gas0;
[[vk::binding(4, 0)]] StructuredBuffer<float2> gas1;
[[vk::binding(8, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(9, 0)]] ConstantBuffer<FxFrame> fx;

uint cellIndex(int3 c) {
    return (uint(c.z) * P.dims.y + uint(c.y)) * P.dims.x + uint(c.x);
}

float2 gasAtCell(int3 c) {
    c = clamp(c, int3(0, 0, 0), int3(P.dims.xyz) - int3(1, 1, 1));
    uint i = cellIndex(c);
    return imm.parity == 0u ? gas0[i] : gas1[i];
}

float2 gasTri(float3 world) {
    float3 g = (world - P.box.xyz) / P.box.w - 0.5;
    int3 i0 = int3(floor(g));
    float3 f = g - float3(i0);
    float2 a = lerp(lerp(gasAtCell(i0 + int3(0, 0, 0)), gasAtCell(i0 + int3(1, 0, 0)), f.x),
                    lerp(gasAtCell(i0 + int3(0, 1, 0)), gasAtCell(i0 + int3(1, 1, 0)), f.x), f.y);
    float2 b = lerp(lerp(gasAtCell(i0 + int3(0, 0, 1)), gasAtCell(i0 + int3(1, 0, 1)), f.x),
                    lerp(gasAtCell(i0 + int3(0, 1, 1)), gasAtCell(i0 + int3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}

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

float3 fireRamp(float heat) {
    float3 a = float3(0.25, 0.02, 0.0);
    float3 b = float3(1.0, 0.28, 0.03);
    float3 c = float3(1.0, 0.72, 0.22);
    float3 d = float3(1.0, 0.95, 0.80);
    if (heat < 0.33) { return lerp(a, b, heat / 0.33); }
    if (heat < 0.66) { return lerp(b, c, (heat - 0.33) / 0.33); }
    return lerp(c, d, (heat - 0.66) / 0.34);
}

[shader("fragment")]
float4 volumeFragment(FullscreenOutput input) : SV_Target {
    float2 pixel = input.position.xy;
    // (a draw into a layer of half the frame's size has a scale of 2: its pixel is two of the frame's)
    float scale = asfloat(imm.pad0);
    float2 uv = (pixel * scale) * fx.screen.zw;
    float3 nearP = worldPosition(fx, uv, 0.0);
    float3 farP = worldPosition(fx, uv, 1.0);
    float3 eye = fx.cameraPosition.xyz;
    float3 dir = normalize(farP - nearP);
    float3 lo = P.box.xyz;
    float3 hi = P.box.xyz + float3(P.dims.xyz) * P.box.w;
    float3 inv = 1.0 / dir;
    float3 t0 = (lo - eye) * inv;
    float3 t1 = (hi - eye) * inv;
    float3 tmin3 = min(t0, t1);
    float3 tmax3 = max(t0, t1);
    float tmin = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
    float tmax = min(min(tmax3.x, tmax3.y), tmax3.z);
    float z = sceneDepth.Load(int3(int2(pixel * scale), 0));
    if (z < 1.0) {
        float3 w = worldPosition(fx, uv, z);
        tmax = min(tmax, dot(w - eye, dir));
    }
    if (tmax <= tmin) { discard; }
    int steps = int(asuint(P.look.z));
    float dt = (tmax - tmin) / float(steps);
    float jitter = toUnit(pcg(uint(pixel.x) + 4093u * uint(pixel.y) + 7919u * P.dims.w));
    float3 sun = fx.sunDirection.xyz;
    float trans = 1.0;
    float3 acc = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < steps; i++) {
        float t = tmin + (float(i) + jitter) * dt;
        float3 p = eye + dir * t;
        float2 g = gasTri(p);
        float dens = max(g.x, 0.0);
        if (dens < 0.003) { continue; }
        float heat = saturate((g.y - P.more.x));
        float sigma = dens * P.look.y;
        float a = 1.0 - exp(-sigma * dt);
        float3 emit = fireRamp(heat) * (pow(heat, 2.2) * P.look.x);
        float shadow = exp(-gasTri(p + sun * 0.25).x * P.look.w);
        float3 lit = P.soot.xyz * (P.soot.w + (1.0 - P.soot.w) * shadow) * fx.sunColor.xyz * 0.3183099
                   + P.soot.xyz * fx.ambient.xyz;
        acc += trans * (emit * a * 2.0 + lit * a);
        trans *= (1.0 - a);
        if (trans < 0.01) { break; }
    }
    return float4(acc * fx.sunDirection.w, 1.0 - trans);
}
