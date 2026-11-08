// The groundcover package's placement rules, for shaders: the same names and the
// same arithmetic, in the same order, as place.cone, types.cone and maps.cone,
// so that the CPU and the GPU place the same cover (tests/parity.cone checks it
// on a real GPU). A shader reaches it with 'import cover;'. It is a module, with no
// entry points: tools/shaders/shaders.py compiles the shaders that import it,
// not it alone.
//
// Compile what imports it with slangc's '-fp-mode precise' (a shader says so
// with a '// slangc: -fp-mode precise' line), so that no driver fuses a
// multiply and an add where the CPU rounds twice.
//
// Group 0 is the world's, for compute and for drawing alike: the frame (0), the
// cover types (1) and the ground maps (2).

import noise;

static const uint COVER_MAX_TYPES = 12u;
static const float COVER_TAU = 6.28318530717958647692;
static const float COVER_PI = 3.14159265358979;

static const uint KIND_BLADES = 0u;
static const uint KIND_FLOWER = 1u;
static const uint KIND_PATCH = 2u;
static const uint KIND_LITTER = 3u;
static const uint KIND_MESH = 4u;

static const uint SHARE = 1u;

struct CoverFrame {
    float4x4 viewProjection;   // 0
    float4 cameraPosition;     // 64: xyz, w the time (seconds)
    float4 lightDirection;     // 80: xyz towards the light (unit), w the exposure
    float4 lightColor;         // 96: rgb irradiance head on
    float4 ambient;            // 112: rgb radiance of the sky
    float4 wind;               // 128: x, z the direction the wind blows (unit), the speed (m/s), the gustiness (0 to 1)
    float4 mapRect;            // 144: originX, originZ, texel, res
    float4 misc;               // 160: the planes culled by (0: none), the far-field blend, the far-field strength, spare
    float4 planes[6];          // 176: frustum planes (xyz unit normal inward, w offset)
};

// One cover type, 27 float4s (432 bytes): the order types.cone's packer writes
struct CoverGpu {
    float4 win[8];             // the windows of the eight signals: a, b, c, d
    float4 p0;                 // density, cluster radius, cell factor, patch scale
    float4 p1;                 // patch threshold, patch softness, patch floor, size noise
    float4 p2;                 // size min, size max, width min, width max
    float4 p3;                 // lean, bend, wind, translucency
    float4 baseColor;          // rgb, variation
    float4 tipColor;           // rgb, dry amount
    float4 altColor;           // rgb, albedo noise
    float4 dryColor;           // rgb, alignment
    float4 flower;             // petal rgb, sink
    float4 flowerCenter;       // rgb, petals
    float4 r0;                 // range, fade start, ring step, rings
    float4 r1;                 // ring ends 0, 1, 2; the mesh bounds (metres, per ring in r2)
    float4 ia[4];              // interactions: other, kind, radius, strength
    uint4 k0;                  // kind, per cluster, seed, need mask
    uint4 k1;                  // mesh first vertex per ring (3), blades per tuft
    uint4 k2;                  // mesh vertex count per ring (3), interaction count
    float4 r2;                 // mesh bounding radius per ring (unit size), spare
};

struct CoverTypes {
    CoverGpu types[12];
};

[[vk::binding(0, 0)]] ConstantBuffer<CoverFrame> frame;
[[vk::binding(1, 0)]] ConstantBuffer<CoverTypes> coverTypes;
[[vk::binding(2, 0)]] Texture2D<float> coverMaps;

// ---- Hashes and noise ----

uint4 gcHash(int cx, int cz, uint a, uint b) {
    return pcg4d(uint4(uint(cx), uint(cz), a, b));
}

float gcFade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float valueNoise(float x, float z, uint seed) {
    float fx = floor(x);
    float fz = floor(z);
    int ix = int(fx);
    int iz = int(fz);
    float u = gcFade(x - fx);
    float v = gcFade(z - fz);
    float a = toUnit(gcHash(ix, iz, seed, 0u).x);
    float b = toUnit(gcHash(ix + 1, iz, seed, 0u).x);
    float c = toUnit(gcHash(ix, iz + 1, seed, 0u).x);
    float d = toUnit(gcHash(ix + 1, iz + 1, seed, 0u).x);
    float top = a + (b - a) * u;
    float bottom = c + (d - c) * u;
    return top + (bottom - top) * v;
}

float fbm2g(float x, float z, uint seed) {
    return valueNoise(x, z, seed) * 0.66666667 + valueNoise(x * 2.03 + 17.1, z * 2.03 - 9.7, seed + 1u) * 0.33333333;
}

float gcSmoothstep(float e0, float e1, float x) {
    if (e1 <= e0) { return x >= e0 ? 1.0 : 0.0; }
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float windowValue(float4 w, float x) {
    if (x < w.x || x >= w.w) { return 0.0; }
    if (x < w.y) { return gcSmoothstep(w.x, w.y, x); }
    if (x <= w.z) { return 1.0; }
    return 1.0 - gcSmoothstep(w.z, w.w, x);
}

// ---- The ground maps ----

float mapAt(uint c, int i, int j) {
    int res = int(frame.mapRect.w);
    return coverMaps.Load(int3(int(c) * res + i, j, 0));
}

int mapClamp(int v) {
    int res = int(frame.mapRect.w);
    return clamp(v, 0, res - 1);
}

float mapSample(uint c, float x, float z) {
    float texel = frame.mapRect.z;
    float u = (x - frame.mapRect.x) / texel - 0.5;
    float v = (z - frame.mapRect.y) / texel - 0.5;
    float fu = floor(u);
    float fv = floor(v);
    float tx = u - fu;
    float ty = v - fv;
    int i0 = mapClamp(int(fu));
    int i1 = mapClamp(int(fu) + 1);
    int j0 = mapClamp(int(fv));
    int j1 = mapClamp(int(fv) + 1);
    float a = mapAt(c, i0, j0);
    float b = mapAt(c, i1, j0);
    float cc = mapAt(c, i0, j1);
    float d = mapAt(c, i1, j1);
    float top = a + (b - a) * tx;
    float bottom = cc + (d - cc) * tx;
    return top + (bottom - top) * ty;
}

float3 mapNormal(float x, float z) {
    float texel = frame.mapRect.z;
    float dx = (mapSample(0u, x + texel, z) - mapSample(0u, x - texel, z)) / (2.0 * texel);
    float dz = (mapSample(0u, x, z + texel) - mapSample(0u, x, z - texel)) / (2.0 * texel);
    float inv = 1.0 / sqrt(dx * dx + 1.0 + dz * dz);
    return float3(0.0 - dx * inv, inv, 0.0 - dz * inv);
}

void signalsAt(float x, float z, out float s[8]) {
    for (uint k = 0u; k < 8u; k++) { s[k] = mapSample(k, x, z); }
}

// ---- The rules ----

float patchAt(CoverGpu t, float x, float z) {
    if (t.p0.w <= 0.0) { return 1.0; }
    float n = fbm2g(x / t.p0.w, z / t.p0.w, t.k0.z + 4242u);
    return gcSmoothstep(t.p1.x - t.p1.y, t.p1.x + t.p1.y, n);
}

float baseScore(CoverGpu t, float sig[8], float x, float z) {
    float s = 1.0;
    for (uint k = 0u; k < 8u; k++) {
        s = s * windowValue(t.win[k], sig[k]);
        if (s <= 0.0) { return 0.0; }
    }
    if (t.p0.w > 0.0) {
        float p = patchAt(t, x, z);
        s = s * (t.p1.z + (1.0 - t.p1.z) * p);
    }
    return s;
}

// The ring's lattice cell is passed in by the caller (the CPU computes it by repeated multiplication)
float2 centreOf(uint seedT, uint ring, int cx, int cz, float cell, out uint4 h) {
    h = gcHash(cx, cz, seedT, 256u + ring);
    return float2((float(cx) + toUnit(h.x)) * cell, (float(cz) + toUnit(h.y)) * cell);
}

// The cell of a type's ring 0 (CoverType.cellSize): sqrt(perCluster / density) x cellFactor
float cellSize0(CoverGpu t) {
    float spacing = sqrt(float(t.k0.y) / t.p0.x);
    return spacing * t.p0.z;
}

float nearOccupancy(uint other, float x, float z, float radius) {
    CoverGpu src = coverTypes.types[other];
    float cell = cellSize0(src);
    int reach = int(ceil(radius / cell));
    int cx = int(floor(x / cell));
    int cz = int(floor(z / cell));
    float best = 0.0;
    for (int dz = 0 - reach; dz <= reach; dz++) {
        for (int dx = 0 - reach; dx <= reach; dx++) {
            uint4 h;
            float2 c = centreOf(src.k0.z, 0u, cx + dx, cz + dz, cell, h);
            float ex = c.x - x;
            float ez = c.y - z;
            float d = sqrt(ex * ex + ez * ez);
            if (d < radius) {
                float sig[8];
                signalsAt(c.x, c.y, sig);
                float score = baseScore(src, sig, c.x, c.y);
                if (toUnit(h.z) < min(1.0, score * src.p0.z * src.p0.z)) {
                    float occ = 1.0 - d / radius;
                    if (occ > best) { best = occ; }
                }
            }
        }
    }
    return best;
}

float fullScore(uint index, float sig[8], float x, float z, inout float scores[12]) {
    uint mask = coverTypes.types[index].k0.w;
    for (uint i = 0u; i <= index; i++) {
        if (((mask >> i) & 1u) == 1u) {
            CoverGpu t = coverTypes.types[i];
            float s = baseScore(t, sig, x, z);
            uint count = t.k2.w;
            for (uint k = 0u; k < count; k++) {
                if (s <= 0.0) { break; }
                float4 ia = t.ia[k];
                if (uint(ia.y) == SHARE) {
                    s = s * (1.0 - ia.w * scores[uint(ia.x)]);
                } else {
                    s = s * (1.0 - ia.w * nearOccupancy(uint(ia.x), x, z, ia.z));
                }
            }
            scores[i] = s;
        }
    }
    return scores[index];
}

// ---- Distance: how much of ring k's instances are kept at horizontal distance d ----

float edgeCentre(CoverGpu t, uint k) {
    if (k + 1u >= uint(t.r0.w)) { return (t.r0.x + t.r0.y) * 0.5; }
    return t.r1[k];
}

float edgeHalf(CoverGpu t, uint k) {
    if (k + 1u >= uint(t.r0.w)) { return (t.r0.x - t.r0.y) * 0.5; }
    if (t.k0.x == KIND_MESH) { return 0.0; }
    return t.r1[k] * 0.15;
}

float ringKeep(CoverGpu t, uint k, float d) {
    float keep = 1.0;
    if (k > 0u) {
        float c = edgeCentre(t, k - 1u);
        float h = edgeHalf(t, k - 1u);
        keep = keep * gcSmoothstep(c - h, c + h, d);
    }
    float c2 = edgeCentre(t, k);
    float h2 = edgeHalf(t, k);
    return keep * (1.0 - gcSmoothstep(c2 - h2, c2 + h2, d));
}
