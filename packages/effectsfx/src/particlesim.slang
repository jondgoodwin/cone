// The effectsfx package's GPU particle simulation: one compute kernel that, for every
// slot of the pool (particlecommon.slang), either gives it a new particle or moves the
// one it holds by a frame, colliding it with the scene.
//
// METHODS, each the published one:
// - The pool as a ring buffer, one thread a slot, no atomics and no free list: the
//   CPU says which slots this frame's new particles take (a run after the last
//   frame's); a slot is only ever written by its own thread. (Gareth Thomas,
//   "Compute-based GPU Particle Systems", GDC 2014, uses an atomic free list; a ring
//   trades "the oldest die early when the pool is full" for no atomics.)
// - Integration: semi-implicit Euler for the accelerations (gravity, wind, turbulence)
//   and the exact solution of linear drag, v *= exp(-k dt), so a large step stays stable.
// - Turbulence: curl noise (Bridson, Hourihan and Nordenstam, "Curl-Noise for Procedural
//   Fluid Flow", 2007): the curl of a vector potential made of three gradient-noise
//   fields, so the flow has no sources or sinks and smoke eddies instead of clumping.
// - Collision with the depth buffer ("screen-space collision", Thomas 2014; Unreal
//   Niagara's "Collision (Depth Buffer)"): the particle's new position is projected into
//   the frame the scene was just drawn in; where it lies behind the stored surface by
//   less than a thickness it has gone through it. The surface's normal is rebuilt from
//   the depth's neighbours (each axis from the nearer neighbour, Wang 2020), the particle
//   is put back on the surface and its velocity reflected: the normal part scaled by the
//   restitution, the tangent part by (1 - friction). It sees only what is on screen, so a
//   particle off screen or behind a surface falls through; the ground plane, which is
//   analytic, has no such limit and catches what the depth buffer cannot.
//
// slangc: -fp-mode precise

import noise;
import fxcommon;
import particlecommon;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] ConstantBuffer<SimFrame> sim;
[[vk::binding(2, 0)]] StructuredBuffer<float4> classes;
[[vk::binding(3, 0)]] StructuredBuffer<float4> batches;
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> pool;
[[vk::binding(5, 0)]] Texture2D<float> sceneDepth;

float3 perpendicular(float3 n) {
    float3 a = abs(n.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    return normalize(cross(a, n));
}

// The world position of the scene at pixel 'p' (clamped to the frame)
float3 surfaceAt(int2 p) {
    int2 hi = int2(fx.screen.xy) - int2(1, 1);
    p = clamp(p, int2(0, 0), hi);
    float z = sceneDepth.Load(int3(p, 0));
    return worldPosition(fx, (float2(p) + 0.5) * fx.screen.zw, z);
}

// Collide with the depth buffer: true if 'pos' went through the surface (moved back onto
// it, 'vel' reflected)
bool depthCollide(inout float3 pos, inout float3 vel, float restitution, float friction) {
    float3 uvz = projectToUv(fx.viewProjection, pos);
    if (uvz.z <= 0.0 || uvz.z >= 1.0) { return false; }
    if (uvz.x < 0.0 || uvz.y < 0.0 || uvz.x >= 1.0 || uvz.y >= 1.0) { return false; }
    int2 px = int2(uvz.xy * fx.screen.xy);
    float z = sceneDepth.Load(int3(px, 0));
    if (z >= 1.0) { return false; }
    float sceneD = viewDepth(fx, z);
    float partD = viewDepth(fx, uvz.z);
    float pen = partD - sceneD;
    // Through the surface, but not so far that it is behind the thing (a wall seen edge on)
    if (pen <= 0.0 || pen > 0.25 + 0.05 * sceneD) { return false; }
    float3 eye = fx.cameraPosition.xyz;
    float3 c = surfaceAt(px);
    float3 l = surfaceAt(px + int2(-1, 0));
    float3 r = surfaceAt(px + int2(1, 0));
    float3 u = surfaceAt(px + int2(0, -1));
    float3 d = surfaceAt(px + int2(0, 1));
    float dc = length(c - eye);
    float3 dx = abs(length(r - eye) - dc) < abs(length(l - eye) - dc) ? (r - c) : (c - l);
    float3 dy = abs(length(u - eye) - dc) < abs(length(d - eye) - dc) ? (u - c) : (c - d);
    float3 n = cross(dx, dy);
    float nl = length(n);
    if (nl < 1e-12) { n = normalize(eye - c); } else { n = n / nl; }
    if (dot(n, eye - c) < 0.0) { n = -n; }
    float vn = dot(vel, n);
    if (vn >= 0.0) { return false; }
    float3 vnv = n * vn;
    float3 vt = vel - vnv;
    // Slow contact comes to rest on the surface instead of buzzing
    float rest = (-vn < 0.9) ? 0.0 : restitution;
    vel = vt * (1.0 - friction) - vnv * rest;
    pos = c + n * (0.012 + 0.002 * sceneD);
    return -vn >= 0.9;
}

bool groundCollide(inout float3 pos, inout float3 vel, float restitution, float friction) {
    float3 n = sim.ground.xyz;
    float h = dot(n, pos) + sim.ground.w;
    if (h >= 0.0) { return false; }
    float vn = dot(vel, n);
    pos = pos - n * (h - 0.004);
    if (vn >= 0.0) { return false; }
    float3 vnv = n * vn;
    float3 vt = vel - vnv;
    float rest = (-vn < 0.9) ? 0.0 : restitution;
    vel = vt * (1.0 - friction) - vnv * rest;
    return -vn >= 0.9;
}

float3 curl(float3 q, uint seed) {
    Noise3 a = gradient3(q, seed);
    Noise3 b = gradient3(q + float3(31.416, 7.1, 11.3), seed + 1u);
    Noise3 c = gradient3(q + float3(-17.9, 23.7, 3.9), seed + 2u);
    return float3(c.d.y - b.d.z, a.d.z - c.d.x, b.d.x - a.d.y);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void simMain(uint3 id : SV_DispatchThreadID) {
    uint s = id.x;
    uint cap = sim.counts.x;
    if (s >= cap) { return; }
    float4 a = pool[s * 3u];
    float4 b = pool[s * 3u + 1u];
    float4 m = pool[s * 3u + 2u];
    float dt = sim.timing.y;
    float time = sim.timing.x;
    uint start = asuint(sim.timing.z);
    uint total = asuint(sim.timing.w);
    uint local = (s + cap - start) % cap;

    if (local < total) {
        // A new particle: find the batch that holds spawn 'local'
        uint found = 0u;
        uint first = 0u;
        for (uint i = 0u; i < sim.counts.y; i++) {
            uint4 info = asuint(batches[i * 4u + 3u]);
            if (local >= info.y && local < info.y + info.x) { found = i; first = info.y; }
        }
        float4 b0 = batches[found * 4u];
        float4 b1 = batches[found * 4u + 1u];
        float4 b2 = batches[found * 4u + 2u];
        uint cls = asuint(b0.w);
        uint seed = asuint(b1.w);
        uint k = local - first;
        uint4 h = pcg4d(uint4(k, seed, cls, 17u));
        uint4 g = pcg4d(uint4(k, seed, cls, 91u));
        float r0 = toUnit(h.x); float r1 = toUnit(h.y); float r2 = toUnit(h.z); float r3 = toUnit(h.w);
        float q0 = toUnit(g.x); float q1 = toUnit(g.y); float q2 = toUnit(g.z); float q3 = toUnit(g.w);
        float4 c0 = classRow(classes, cls, 0u);
        float4 c1 = classRow(classes, cls, 1u);
        float4 c11 = classRow(classes, cls, 11u);
        float3 axis = normalize(b1.xyz);
        float3 t = perpendicular(axis);
        float3 bt = cross(axis, t);
        float3 pos = b0.xyz;
        if (c0.x > 1.5) {
            float zz = 2.0 * r0 - 1.0;
            float aa = 6.2831853 * r1;
            float s2 = sqrt(max(1.0 - zz * zz, 0.0));
            pos += float3(s2 * cos(aa), zz, s2 * sin(aa)) * (c0.y * pow(r2, 0.33333334));
        } else if (c0.x > 0.5) {
            float aa = 6.2831853 * r0;
            pos += (t * cos(aa) + bt * sin(aa)) * (c0.y * sqrt(r1));
        }
        float cosT = 1.0 - r3 * (1.0 - cos(c0.z));
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float phi = 6.2831853 * q0;
        float3 dir = axis * cosT + (t * cos(phi) + bt * sin(phi)) * sinT;
        float speed = c0.w * (1.0 + c1.x * (2.0 * q1 - 1.0));
        float life = c1.y * (1.0 + c1.z * (2.0 * q2 - 1.0));
        float3 vel = dir * speed + b2.xyz * c11.z;
        // Born somewhere in the frame, not all at its start: the stream is smooth
        float age = q3 * dt;
        pos += vel * age;
        pool[s * 3u] = float4(pos, age);
        pool[s * 3u + 1u] = float4(vel, max(life, 0.001));
        pool[s * 3u + 2u] = float4(asfloat(cls), toUnit(pcg(h.x ^ g.y)), 0.0, 1.0 + c11.x * (2.0 * toUnit(pcg(g.w + 5u)) - 1.0));
        return;
    }
    if (a.w >= b.w) { return; }

    uint cls = asuint(m.x);
    float4 c1 = classRow(classes, cls, 1u);
    float4 c2 = classRow(classes, cls, 2u);
    float4 c3 = classRow(classes, cls, 3u);
    float4 c5 = classRow(classes, cls, 5u);
    float4 c11 = classRow(classes, cls, 11u);
    float3 pos = a.xyz;
    float3 vel = b.xyz;
    float age = a.w + dt;
    vel += c2.xyz * dt + sim.wind.xyz * (c11.w * dt);
    if (c2.w != 0.0) {
        float3 q = pos * c3.x + float3(0.0, -time * c3.y * c3.x, 0.0);
        vel += curl(q, cls * 7919u + 13u) * (c2.w * dt);
    }
    vel *= exp(-c1.w * dt);
    pos += vel * dt;
    if (c5.w > 0.5) {
        bool hit = false;
        if ((sim.counts.z & 2u) != 0u) { hit = groundCollide(pos, vel, c5.x, c5.y) || hit; }
        if ((sim.counts.z & 1u) != 0u) { hit = depthCollide(pos, vel, c5.x, c5.y) || hit; }
        if (hit) {
            m.z += 1.0;
            age += b.w * c5.z;
        }
    }
    pool[s * 3u] = float4(pos, age);
    pool[s * 3u + 1u] = float4(vel, b.w);
    pool[s * 3u + 2u] = m;
}
