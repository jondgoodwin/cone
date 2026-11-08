// The groundcover package's placement kernels, in compute, compiled ahead of time
// to generate.spv by tools/shaders/shaders.py and embedded in renderer.cone, which says
// how they are run.
//
// 'generateMain' is place.cone's 'placeCell' for every cell of a square of the
// lattice at once, one thread a cell: the cluster centre, the score (cover.slang),
// the cluster's members, the distance keep, and a view-frustum test on the
// instance's bounding sphere. A kept instance is appended to its batch's
// range of the instance buffer by an atomic counter (the order is not the CPU's,
// the set is). A batch is one type in one ring.
//
// 'finishMain' turns the counters into the indirect draws' instance counts,
// clamped to each batch's capacity, keeps the raw count for the statistics, and
// zeroes the counter for the next frame. The CPU never learns how many were
// drawn.
//
// slangc: -fp-mode precise

import noise;
import cover;

struct CoverInstance {
    float4 a;                  // x, y (the ground), z, height
    float4 b;                  // yaw, width, lean, tint
    float4 c;                  // the seed (as bits), the ground normal's x and z, the exposure
};

struct GenParams {
    uint typeIndex;
    uint ring;
    uint batch;
    uint base;                 // the batch's first instance in the buffer
    uint cap;                  // its capacity
    int originX;               // the lattice cell of the square's first thread
    int originZ;
    uint across;               // cells across the square
    float cell;                // this ring's lattice cell
    float nearR;               // the ring's span of distances
    float farR;
    float widthScale;          // ringStep ^ ring
    float cameraX;
    float cameraZ;
    uint batches;              // 'finishMain': how many batches
    uint pad;
};

[[vk::push_constant]] ConstantBuffer<GenParams> gen;

[[vk::binding(3, 0)]] RWStructuredBuffer<CoverInstance> instances;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> counters;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> args;       // 8 words a batch: vertices, instances, first vertex, first instance, capacity, raw count

// The bounding radius of an instance, about (x, y + h / 2, z)
float boundRadius(CoverGpu t, uint ring, float h, float w, float ringScale) {
    uint kind = t.k0.x;
    if (kind == KIND_BLADES) { return h * (0.7 + 0.3 * ringScale); }
    if (kind == KIND_FLOWER) { return h * 0.9 + w * 2.0; }
    if (kind == KIND_PATCH) { return w * 1.6 + h; }
    if (kind == KIND_LITTER) { return max(h, w) + 0.05; }
    return t.r2[ring] * max(h, w) + h * 0.5;
}

bool insideFrustum(float3 centre, float radius) {
    uint planes = uint(frame.misc.x);
    for (uint p = 0u; p < planes; p++) {
        float4 pl = frame.planes[p];
        if (dot(pl.xyz, centre) + pl.w < 0.0 - radius) { return false; }
    }
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void generateMain(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= gen.across || gid.y >= gen.across) { return; }
    int cx = gen.originX + int(gid.x);
    int cz = gen.originZ + int(gid.y);
    CoverGpu t = coverTypes.types[gen.typeIndex];
    uint seedT = t.k0.z;
    float cell = gen.cell;
    // a MESH type's rings are levels of detail of one lattice (place.cone's latticeRing)
    uint lat = (t.k0.x == KIND_MESH) ? 0u : gen.ring;
    uint4 h0;
    float2 centre = centreOf(seedT, lat, cx, cz, cell, h0);
    float clusterRadius = t.p0.y;
    float dxC = centre.x - gen.cameraX;
    float dzC = centre.y - gen.cameraZ;
    float dC = sqrt(dxC * dxC + dzC * dzC);
    if (dC + clusterRadius < gen.nearR || dC - clusterRadius > gen.farR) { return; }
    // A cluster exists with probability min(1, score cf^2), and a score is at most 1: a cell whose
    // draw is not below cf^2 has none whatever the ground is, and need not read the maps
    if (toUnit(h0.z) >= t.p0.z * t.p0.z) { return; }
    float scores[12];
    for (uint s = 0u; s < 12u; s++) { scores[s] = 0.0; }
    float sigC[8];
    signalsAt(centre.x, centre.y, sigC);
    float scoreC = fullScore(gen.typeIndex, sigC, centre.x, centre.y, scores);
    if (scoreC <= 0.0) { return; }
    float cf = t.p0.z;
    if (toUnit(h0.z) >= min(1.0, scoreC * cf * cf)) { return; }
    float sigma = clusterRadius * 0.5;
    uint perCluster = t.k0.y;
    for (uint j = 0u; j < perCluster; j++) {
        uint4 hj = gcHash(cx, cz, seedT ^ (0x68E31DA4u * (j + 1u)), 512u + lat);
        float px = centre.x;
        float pz = centre.y;
        if (clusterRadius > 0.0) {
            float r = sigma * sqrt(0.0 - 2.0 * log(max(toUnit(hj.x), 0.0001)));
            if (r > clusterRadius) { r = clusterRadius; }
            float ang = COVER_TAU * toUnit(hj.y);
            px = px + r * cos(ang);
            pz = pz + r * sin(ang);
        }
        float sigP[8];
        for (uint q = 0u; q < 8u; q++) { sigP[q] = sigC[q]; }
        bool keepRule = true;
        if (clusterRadius > 0.0 && perCluster > 1u) {
            signalsAt(px, pz, sigP);
            float sP = fullScore(gen.typeIndex, sigP, px, pz, scores);
            if (toUnit(hj.z) >= min(1.0, sP / scoreC)) { keepRule = false; }
        }
        if (!keepRule) { continue; }
        float dx = px - gen.cameraX;
        float dz = pz - gen.cameraZ;
        float d = sqrt(dx * dx + dz * dz);
        if (!(toUnit(hj.w) < ringKeep(t, gen.ring, d))) { continue; }
        uint4 hk = gcHash(cx, cz, seedT ^ (0x1B873593u * (j + 1u)), 768u + lat);
        float patch = patchAt(t, px, pz);
        float height = (t.p2.x + (t.p2.y - t.p2.x) * toUnit(hk.x)) * (1.0 + t.p1.w * (patch * 2.0 - 1.0));
        float width = (t.p2.z + (t.p2.w - t.p2.z) * toUnit(hk.y)) * gen.widthScale;
        float ground = mapSample(0u, px, pz);
        float radius = boundRadius(t, gen.ring, height, width, gen.widthScale);
        if (!insideFrustum(float3(px, ground + height * 0.5, pz), radius)) { continue; }
        float3 n = mapNormal(px, pz);
        float tint = fbm2g(px * 0.07, pz * 0.07, seedT + 999u);
        CoverInstance inst;
        inst.a = float4(px, ground, pz, height);
        inst.b = float4(COVER_TAU * toUnit(hk.z), width, t.p3.x * toUnit(hk.w), tint);
        inst.c = float4(asfloat(hj.z), n.x, n.z, sigP[3]);
        uint slot;
        InterlockedAdd(counters[gen.batch], 1u, slot);
        if (slot < gen.cap) { instances[gen.base + slot] = inst; }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void finishMain(uint3 gid : SV_DispatchThreadID) {
    uint b = gid.x;
    if (b >= gen.batches) { return; }
    uint raw = counters[b];
    uint cap = args[b * 8u + 4u];
    args[b * 8u + 1u] = min(raw, cap);
    args[b * 8u + 5u] = raw;
    counters[b] = 0u;
}
