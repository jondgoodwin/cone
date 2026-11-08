// The effectsfx package's small-grid smoke and fire simulation: a stable-fluids solver on a
// box of cells (tens of thousands) on the GPU, and the volume renderer that draws it.
//
// METHODS, each the published one:
// - Stable Fluids (Stam, "Stable Fluids", SIGGRAPH 1999; Stam, "Real-Time Fluid Dynamics for
//   Games", GDC 2003): semi-Lagrangian advection (trace each cell's centre back along the
//   velocity and sample there, trilinearly: unconditionally stable), then a pressure
//   projection to make the velocity divergence-free: the divergence of the advected field,
//   Jacobi iterations of the Poisson equation for the pressure, the pressure's gradient
//   subtracted.
// - Buoyancy of hot gas and the weight of soot (Fedkiw, Stam, Jensen, "Visual Simulation of
//   Smoke", SIGGRAPH 2001): an upward force (alpha (T - T_ambient) - beta density), with the
//   temperature cooling and the density dissipating as they are carried along.
// - Vorticity confinement (the same paper): the curl of the velocity, its magnitude's gradient
//   N, and a force epsilon h (N x curl) that puts back the small eddies advection smooths out.
// - A fire source: a soft ball of the box where fuel (density) and heat are injected and the
//   gas is given an upward speed, flickered by a noise in time.
// Boundaries are walls on the sides and floor (the pressure's Neumann condition, the neighbour
// outside being the cell itself) and open at the top (pressure 0 beyond it), so a plume leaves.
//
// THE VOLUME RENDERER (volumeFragment): one full-screen triangle scissored to the box's bounds on the
// screen; each pixel intersects its ray with the box (slab test), clips it at the scene's depth,
// and ray marches (Max 1995's emission-absorption, front to back): the gas glows with a blackbody-like
// ramp of its temperature and is shaded as soot, lit by the sun through one shadow tap and an
// ambient floor. Premultiplied alpha, multiplied by the scene's exposure.
//
// The fields are storage buffers, two copies of the velocity (float4) and of the gas (density,
// temperature) swapped each frame, two of the pressure swapped each Jacobi iteration, the
// divergence and the curl.
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
    uint parity;        // which copy of the velocity and the gas is current
    uint pressure;      // which copy of the pressure the kernel reads (it writes the other)
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ConstantBuffer<Immediates> imm;

[[vk::binding(0, 0)]] ConstantBuffer<FluidParams> P;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> vel0;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> vel1;
[[vk::binding(3, 0)]] RWStructuredBuffer<float2> gas0;
[[vk::binding(4, 0)]] RWStructuredBuffer<float2> gas1;
[[vk::binding(5, 0)]] RWStructuredBuffer<float> press0;
[[vk::binding(6, 0)]] RWStructuredBuffer<float> press1;
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> aux;      // the curl (xyz, |curl|) in the confinement kernels, the divergence (x) in the projection's

uint cellIndex(int3 c) {
    return (uint(c.z) * P.dims.y + uint(c.y)) * P.dims.x + uint(c.x);
}

int3 cellOf(uint i) {
    uint x = i % P.dims.x;
    uint y = (i / P.dims.x) % P.dims.y;
    uint z = i / (P.dims.x * P.dims.y);
    return int3(int(x), int(y), int(z));
}

int3 clampCell(int3 c) {
    return clamp(c, int3(0, 0, 0), int3(P.dims.xyz) - int3(1, 1, 1));
}

float3 cellCentre(int3 c) {
    return P.box.xyz + (float3(c) + 0.5) * P.box.w;
}

float4 velAt(uint which, int3 c) {
    uint i = cellIndex(clampCell(c));
    return which == 0u ? vel0[i] : vel1[i];
}

float2 gasAtCell(uint which, int3 c) {
    uint i = cellIndex(clampCell(c));
    return which == 0u ? gas0[i] : gas1[i];
}

// Pressure at a cell: the walls reflect (Neumann: the cell itself), the top is open (0)
float pressureAt(uint which, int3 c) {
    if (c.y >= int(P.dims.y)) { return 0.0; }
    int3 k = clampCell(c);
    uint i = cellIndex(k);
    return which == 0u ? press0[i] : press1[i];
}

float4 velTri(uint which, float3 world) {
    float3 g = (world - P.box.xyz) / P.box.w - 0.5;
    int3 i0 = int3(floor(g));
    float3 f = g - float3(i0);
    float4 a = lerp(lerp(velAt(which, i0 + int3(0, 0, 0)), velAt(which, i0 + int3(1, 0, 0)), f.x),
                    lerp(velAt(which, i0 + int3(0, 1, 0)), velAt(which, i0 + int3(1, 1, 0)), f.x), f.y);
    float4 b = lerp(lerp(velAt(which, i0 + int3(0, 0, 1)), velAt(which, i0 + int3(1, 0, 1)), f.x),
                    lerp(velAt(which, i0 + int3(0, 1, 1)), velAt(which, i0 + int3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}

float2 gasTri(uint which, float3 world) {
    float3 g = (world - P.box.xyz) / P.box.w - 0.5;
    int3 i0 = int3(floor(g));
    float3 f = g - float3(i0);
    float2 a = lerp(lerp(gasAtCell(which, i0 + int3(0, 0, 0)), gasAtCell(which, i0 + int3(1, 0, 0)), f.x),
                    lerp(gasAtCell(which, i0 + int3(0, 1, 0)), gasAtCell(which, i0 + int3(1, 1, 0)), f.x), f.y);
    float2 b = lerp(lerp(gasAtCell(which, i0 + int3(0, 0, 1)), gasAtCell(which, i0 + int3(1, 0, 1)), f.x),
                    lerp(gasAtCell(which, i0 + int3(0, 1, 1)), gasAtCell(which, i0 + int3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}

// ---- The solver ----

// Inject fuel and heat at the source, apply buoyancy and the soot's weight, damp (in place on the current fields)
[shader("compute")]
[numthreads(64, 1, 1)]
void forcesMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    float3 world = cellCentre(c);
    float dt = P.physics.x;
    float4 v = imm.parity == 0u ? vel0[id.x] : vel1[id.x];
    float2 g = imm.parity == 0u ? gas0[id.x] : gas1[id.x];
    // The source: a soft ball, its strength wavering in time
    float r = length(world - P.source.xyz) / P.source.w;
    if (r < 1.0) {
        float w = 1.0 - r * r;
        float flicker = 0.75 + 0.5 * value2(float2(float(P.dims.w) * 0.09, 3.0 + 7.0 * w), asuint(P.more.w)).value;
        g.x = g.x + P.burn.y * dt * w * flicker;
        g.y = max(g.y, P.burn.x * w * flicker);
        v.y = max(v.y, P.more.z * w);
        // The flame licks sideways: a wandering push, different in every cell, changing in time
        float3 q = float3(world.x * 5.0, world.y * 4.0 - float(P.dims.w) * 0.06, world.z * 5.0);
        v.x += dt * 38.0 * w * value3(q, asuint(P.more.w) + 1u).value;
        v.z += dt * 38.0 * w * value3(q + 17.3, asuint(P.more.w) + 2u).value;
    }
    // Buoyancy of the hot gas, the weight of the soot
    v.y += dt * (P.physics.y * (g.y - P.more.x) - P.physics.z * g.x);
    v.xyz *= exp(-P.more.y * dt);
    if (imm.parity == 0u) { vel0[id.x] = v; gas0[id.x] = g; } else { vel1[id.x] = v; gas1[id.x] = g; }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void curlMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    float h2 = 2.0 * P.box.w;
    float3 vxp = velAt(imm.parity, c + int3(1, 0, 0)).xyz;
    float3 vxm = velAt(imm.parity, c + int3(-1, 0, 0)).xyz;
    float3 vyp = velAt(imm.parity, c + int3(0, 1, 0)).xyz;
    float3 vym = velAt(imm.parity, c + int3(0, -1, 0)).xyz;
    float3 vzp = velAt(imm.parity, c + int3(0, 0, 1)).xyz;
    float3 vzm = velAt(imm.parity, c + int3(0, 0, -1)).xyz;
    float3 w = float3((vyp.z - vym.z) - (vzp.y - vzm.y),
                      (vzp.x - vzm.x) - (vxp.z - vxm.z),
                      (vxp.y - vxm.y) - (vyp.x - vym.x)) / h2;
    aux[id.x] = float4(w, length(w));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void confineMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    float h2 = 2.0 * P.box.w;
    float4 w = aux[id.x];
    float gx = aux[cellIndex(clampCell(c + int3(1, 0, 0)))].w - aux[cellIndex(clampCell(c + int3(-1, 0, 0)))].w;
    float gy = aux[cellIndex(clampCell(c + int3(0, 1, 0)))].w - aux[cellIndex(clampCell(c + int3(0, -1, 0)))].w;
    float gz = aux[cellIndex(clampCell(c + int3(0, 0, 1)))].w - aux[cellIndex(clampCell(c + int3(0, 0, -1)))].w;
    float3 eta = float3(gx, gy, gz) / h2;
    float len = length(eta);
    if (len > 1e-6) {
        float3 N = eta / len;
        float3 f = P.physics.w * P.box.w * cross(N, w.xyz);
        float4 v = imm.parity == 0u ? vel0[id.x] : vel1[id.x];
        v.xyz += P.physics.x * f;
        if (imm.parity == 0u) { vel0[id.x] = v; } else { vel1[id.x] = v; }
    }
}

// The divergence of the current velocity, into aux.x
[shader("compute")]
[numthreads(64, 1, 1)]
void divergenceMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    float dx = velAt(imm.parity, c + int3(1, 0, 0)).x - velAt(imm.parity, c + int3(-1, 0, 0)).x;
    float dy = velAt(imm.parity, c + int3(0, 1, 0)).y - velAt(imm.parity, c + int3(0, -1, 0)).y;
    float dz = velAt(imm.parity, c + int3(0, 0, 1)).z - velAt(imm.parity, c + int3(0, 0, -1)).z;
    aux[id.x] = float4((dx + dy + dz) / (2.0 * P.box.w), 0.0, 0.0, 0.0);
    // The pressure starts from nothing
    press0[id.x] = 0.0;
    press1[id.x] = 0.0;
}

// One Jacobi iteration of the Poisson equation: reads press[imm.pressure], writes the other
[shader("compute")]
[numthreads(64, 1, 1)]
void jacobiMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    uint r = imm.pressure;
    float sum = pressureAt(r, c + int3(1, 0, 0)) + pressureAt(r, c + int3(-1, 0, 0))
              + pressureAt(r, c + int3(0, 1, 0)) + pressureAt(r, c + int3(0, -1, 0))
              + pressureAt(r, c + int3(0, 0, 1)) + pressureAt(r, c + int3(0, 0, -1));
    float p = (sum - aux[id.x].x * P.box.w * P.box.w) / 6.0;
    if (r == 0u) { press1[id.x] = p; } else { press0[id.x] = p; }
}

// Subtract the pressure's gradient (the copy imm.pressure) from the current velocity
[shader("compute")]
[numthreads(64, 1, 1)]
void projectMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    uint r = imm.pressure;
    float gx = pressureAt(r, c + int3(1, 0, 0)) - pressureAt(r, c + int3(-1, 0, 0));
    float gy = pressureAt(r, c + int3(0, 1, 0)) - pressureAt(r, c + int3(0, -1, 0));
    float gz = pressureAt(r, c + int3(0, 0, 1)) - pressureAt(r, c + int3(0, 0, -1));
    float4 v = imm.parity == 0u ? vel0[id.x] : vel1[id.x];
    v.xyz -= float3(gx, gy, gz) / (2.0 * P.box.w);
    if (imm.parity == 0u) { vel0[id.x] = v; } else { vel1[id.x] = v; }
}

// Semi-Lagrangian advection of the velocity and the gas, current to next, with the gas cooling
// and thinning on the way
[shader("compute")]
[numthreads(64, 1, 1)]
void advectMain(uint3 id : SV_DispatchThreadID) {
    uint n = P.dims.x * P.dims.y * P.dims.z;
    if (id.x >= n) { return; }
    int3 c = cellOf(id.x);
    float3 world = cellCentre(c);
    float dt = P.physics.x;
    float4 v = velAt(imm.parity, c);
    float3 back = world - dt * v.xyz;
    float4 nv = velTri(imm.parity, back);
    float2 ng = gasTri(imm.parity, back);
    ng.y = P.more.x + (ng.y - P.more.x) * exp(-P.burn.z * dt);
    ng.x = ng.x * exp(-P.burn.w * dt);
    if (imm.parity == 0u) { vel1[id.x] = nv; gas1[id.x] = ng; } else { vel0[id.x] = nv; gas0[id.x] = ng; }
}
