// The noise package's functions for shaders: the same names, and the same
// arithmetic in the same order, as the Cone source beside it (hash.cone,
// lattice.cone, cellular.cone, fractal.cone), so that a shader and the CPU
// agree. A shader reaches it with 'import noise;'. It is a module, with no
// entry points: tools/shaders/shaders.py compiles the shaders that import
// it, not it alone.
//
// Compile what imports it with slangc's '-fp-mode precise' (a shader says so
// with a '// slangc: -fp-mode precise' line): it marks every float operation
// NoContraction, so no driver fuses a multiply and an add into one rounding
// where the CPU rounds twice. Slang's 'precise' keyword alone emits nothing
// (measured with slangc 2026.13.1). The package README has the rules.
//
// The larger functions are '[noinline]': slangc otherwise inlines every call,
// and a shader calling fBm from a domain warp grows to hundreds of
// kilobytes of SPIR-V. It changes no result.

// ======== Integer hashes ========

uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint2 pcg2d(uint2 v) {
    uint x = v.x * 1664525u + 1013904223u;
    uint y = v.y * 1664525u + 1013904223u;
    x = x + y * 1664525u;
    y = y + x * 1664525u;
    x = x ^ (x >> 16u);
    y = y ^ (y >> 16u);
    x = x + y * 1664525u;
    y = y + x * 1664525u;
    x = x ^ (x >> 16u);
    y = y ^ (y >> 16u);
    return uint2(x, y);
}

uint3 pcg3d(uint3 v) {
    uint x = v.x * 1664525u + 1013904223u;
    uint y = v.y * 1664525u + 1013904223u;
    uint z = v.z * 1664525u + 1013904223u;
    x = x + y * z;
    y = y + z * x;
    z = z + x * y;
    x = x ^ (x >> 16u);
    y = y ^ (y >> 16u);
    z = z ^ (z >> 16u);
    x = x + y * z;
    y = y + z * x;
    z = z + x * y;
    return uint3(x, y, z);
}

uint4 pcg4d(uint4 v) {
    uint x = v.x * 1664525u + 1013904223u;
    uint y = v.y * 1664525u + 1013904223u;
    uint z = v.z * 1664525u + 1013904223u;
    uint w = v.w * 1664525u + 1013904223u;
    x = x + y * w;
    y = y + z * x;
    z = z + x * y;
    w = w + y * z;
    x = x ^ (x >> 16u);
    y = y ^ (y >> 16u);
    z = z ^ (z >> 16u);
    w = w ^ (w >> 16u);
    x = x + y * w;
    y = y + z * x;
    z = z + x * y;
    w = w + y * z;
    return uint4(x, y, z, w);
}

uint hash1(int x, uint seed) { return pcg2d(uint2(uint(x), seed)).x; }
uint3 hash2(int x, int y, uint seed) { return pcg3d(uint3(uint(x), uint(y), seed)); }
uint4 hash3(int x, int y, int z, uint seed) { return pcg4d(uint4(uint(x), uint(y), uint(z), seed)); }

float toUnit(uint h) { return float(h >> 8u) * 0.000000059604644775390625; }
float toSigned(uint h) { return float(h >> 8u) * 0.00000011920928955078125 - 1.0; }

// ======== Lattice noise ========

struct Noise2 {
    float value;
    float2 d;
};

struct Noise3 {
    float value;
    float3 d;
};

float noiseFade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float noiseFadeD(float t) { return 30.0 * t * t * (t * (t - 2.0) + 1.0); }

int noiseWrap(int i, int period) {
    if (period <= 0) return i;
    if (i >= 0) return int(uint(i) % uint(period));
    return period - 1 - int(uint(-(i + 1)) % uint(period));
}

float noiseBlend2(float a, float b, float c, float d, float u, float v) {
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

float noiseBlend3(float a, float b, float c, float d, float e, float f, float g, float h, float u, float v, float w) {
    return a + (b - a) * u + (c - a) * v + (e - a) * w
        + (a - b - c + d) * u * v + (a - c - e + g) * v * w + (a - b - e + f) * w * u
        + (b - a + c - d + e - f - g + h) * u * v * w;
}

[noinline]
Noise2 value2Periodic(float2 p, int period, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    int ix = int(fx);
    int iy = int(fy);
    int x0 = noiseWrap(ix, period);
    int y0 = noiseWrap(iy, period);
    int x1 = noiseWrap(ix + 1, period);
    int y1 = noiseWrap(iy + 1, period);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float u = noiseFade(tx);
    float v = noiseFade(ty);
    float du = noiseFadeD(tx);
    float dv = noiseFadeD(ty);
    float a = toSigned(hash2(x0, y0, seed).x);
    float b = toSigned(hash2(x1, y0, seed).x);
    float c = toSigned(hash2(x0, y1, seed).x);
    float d = toSigned(hash2(x1, y1, seed).x);
    float k = a - b - c + d;
    Noise2 n;
    n.value = noiseBlend2(a, b, c, d, u, v);
    n.d = float2(du * ((b - a) + k * v), dv * ((c - a) + k * u));
    return n;
}

Noise2 value2(float2 p, uint seed) { return value2Periodic(p, 0, seed); }

[noinline]
Noise3 value3Periodic(float3 p, int period, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    float fz = floor(p.z);
    int ix = int(fx);
    int iy = int(fy);
    int iz = int(fz);
    int x0 = noiseWrap(ix, period);
    int y0 = noiseWrap(iy, period);
    int z0 = noiseWrap(iz, period);
    int x1 = noiseWrap(ix + 1, period);
    int y1 = noiseWrap(iy + 1, period);
    int z1 = noiseWrap(iz + 1, period);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    float u = noiseFade(tx);
    float v = noiseFade(ty);
    float w = noiseFade(tz);
    float a = toSigned(hash3(x0, y0, z0, seed).x);
    float b = toSigned(hash3(x1, y0, z0, seed).x);
    float c = toSigned(hash3(x0, y1, z0, seed).x);
    float d = toSigned(hash3(x1, y1, z0, seed).x);
    float e = toSigned(hash3(x0, y0, z1, seed).x);
    float f = toSigned(hash3(x1, y0, z1, seed).x);
    float g = toSigned(hash3(x0, y1, z1, seed).x);
    float h = toSigned(hash3(x1, y1, z1, seed).x);
    float k4 = a - b - c + d;
    float k5 = a - c - e + g;
    float k6 = a - b - e + f;
    float k7 = b - a + c - d + e - f - g + h;
    Noise3 n;
    n.value = noiseBlend3(a, b, c, d, e, f, g, h, u, v, w);
    n.d = float3(
        noiseFadeD(tx) * ((b - a) + k4 * v + k6 * w + k7 * v * w),
        noiseFadeD(ty) * ((c - a) + k4 * u + k5 * w + k7 * u * w),
        noiseFadeD(tz) * ((e - a) + k5 * v + k6 * u + k7 * u * v));
    return n;
}

Noise3 value3(float3 p, uint seed) { return value3Periodic(p, 0, seed); }

[noinline]
Noise2 gradient2Periodic(float2 p, int period, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    int ix = int(fx);
    int iy = int(fy);
    int x0 = noiseWrap(ix, period);
    int y0 = noiseWrap(iy, period);
    int x1 = noiseWrap(ix + 1, period);
    int y1 = noiseWrap(iy + 1, period);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float u = noiseFade(tx);
    float v = noiseFade(ty);
    uint3 ha = hash2(x0, y0, seed);
    uint3 hb = hash2(x1, y0, seed);
    uint3 hc = hash2(x0, y1, seed);
    uint3 hd = hash2(x1, y1, seed);
    float gax = toSigned(ha.x);
    float gay = toSigned(ha.y);
    float gbx = toSigned(hb.x);
    float gby = toSigned(hb.y);
    float gcx = toSigned(hc.x);
    float gcy = toSigned(hc.y);
    float gdx = toSigned(hd.x);
    float gdy = toSigned(hd.y);
    float va = gax * tx + gay * ty;
    float vb = gbx * (tx - 1.0) + gby * ty;
    float vc = gcx * tx + gcy * (ty - 1.0);
    float vd = gdx * (tx - 1.0) + gdy * (ty - 1.0);
    float k = va - vb - vc + vd;
    Noise2 n;
    n.value = noiseBlend2(va, vb, vc, vd, u, v);
    n.d = float2(
        noiseBlend2(gax, gbx, gcx, gdx, u, v) + noiseFadeD(tx) * ((vb - va) + k * v),
        noiseBlend2(gay, gby, gcy, gdy, u, v) + noiseFadeD(ty) * ((vc - va) + k * u));
    return n;
}

Noise2 gradient2(float2 p, uint seed) { return gradient2Periodic(p, 0, seed); }

float3 noiseGrad3(uint4 h) { return float3(toSigned(h.x), toSigned(h.y), toSigned(h.z)); }

[noinline]
Noise3 gradient3Periodic(float3 p, int period, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    float fz = floor(p.z);
    int ix = int(fx);
    int iy = int(fy);
    int iz = int(fz);
    int x0 = noiseWrap(ix, period);
    int y0 = noiseWrap(iy, period);
    int z0 = noiseWrap(iz, period);
    int x1 = noiseWrap(ix + 1, period);
    int y1 = noiseWrap(iy + 1, period);
    int z1 = noiseWrap(iz + 1, period);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    float u = noiseFade(tx);
    float v = noiseFade(ty);
    float w = noiseFade(tz);
    float sx = tx - 1.0;
    float sy = ty - 1.0;
    float sz = tz - 1.0;
    float3 ga = noiseGrad3(hash3(x0, y0, z0, seed));
    float3 gb = noiseGrad3(hash3(x1, y0, z0, seed));
    float3 gc = noiseGrad3(hash3(x0, y1, z0, seed));
    float3 gd = noiseGrad3(hash3(x1, y1, z0, seed));
    float3 ge = noiseGrad3(hash3(x0, y0, z1, seed));
    float3 gf = noiseGrad3(hash3(x1, y0, z1, seed));
    float3 gg = noiseGrad3(hash3(x0, y1, z1, seed));
    float3 gh = noiseGrad3(hash3(x1, y1, z1, seed));
    float va = ga.x * tx + ga.y * ty + ga.z * tz;
    float vb = gb.x * sx + gb.y * ty + gb.z * tz;
    float vc = gc.x * tx + gc.y * sy + gc.z * tz;
    float vd = gd.x * sx + gd.y * sy + gd.z * tz;
    float ve = ge.x * tx + ge.y * ty + ge.z * sz;
    float vf = gf.x * sx + gf.y * ty + gf.z * sz;
    float vg = gg.x * tx + gg.y * sy + gg.z * sz;
    float vh = gh.x * sx + gh.y * sy + gh.z * sz;
    float k4 = va - vb - vc + vd;
    float k5 = va - vc - ve + vg;
    float k6 = va - vb - ve + vf;
    float k7 = vb - va + vc - vd + ve - vf - vg + vh;
    Noise3 n;
    n.value = noiseBlend3(va, vb, vc, vd, ve, vf, vg, vh, u, v, w);
    n.d = float3(
        noiseBlend3(ga.x, gb.x, gc.x, gd.x, ge.x, gf.x, gg.x, gh.x, u, v, w)
            + noiseFadeD(tx) * ((vb - va) + k4 * v + k6 * w + k7 * v * w),
        noiseBlend3(ga.y, gb.y, gc.y, gd.y, ge.y, gf.y, gg.y, gh.y, u, v, w)
            + noiseFadeD(ty) * ((vc - va) + k4 * u + k5 * w + k7 * u * w),
        noiseBlend3(ga.z, gb.z, gc.z, gd.z, ge.z, gf.z, gg.z, gh.z, u, v, w)
            + noiseFadeD(tz) * ((ve - va) + k5 * v + k6 * u + k7 * u * v));
    return n;
}

Noise3 gradient3(float3 p, uint seed) { return gradient3Periodic(p, 0, seed); }

// ======== Cellular noise ========

struct Cell2 {
    float f1;
    float f2;
    uint id;
    float2 toNearest;
};

struct Cell3 {
    float f1;
    float f2;
    uint id;
    float3 toNearest;
};

float noiseGap(float t, float lo, float hi) {
    if (t < lo) return (lo - t) * (lo - t);
    if (t > hi) return (t - hi) * (t - hi);
    return 0.0;
}

[noinline]
Cell2 cellular2(float2 p, float jitter, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    int ix = int(fx);
    int iy = int(fy);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float half = jitter * 0.5;
    float f1 = 3.4028235e38;
    float f2 = 3.4028235e38;
    uint id = 0u;
    float nx = 0.0;
    float ny = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        for (int j = -2; j <= 2; j++) {
            for (int i = -2; i <= 2; i++) {
                int ring = (i < -1 || i > 1 || j < -1 || j > 1) ? 1 : 0;
                if (ring == pass) {
                    float ci = float(i);
                    float cj = float(j);
                    float reach = noiseGap(tx, ci + 0.5 - half, ci + 0.5 + half) + noiseGap(ty, cj + 0.5 - half, cj + 0.5 + half);
                    if (pass == 0 || reach < f2) {
                        uint3 h = hash2(ix + i, iy + j, seed);
                        float dx = ci + 0.5 + jitter * (toUnit(h.x) - 0.5) - tx;
                        float dy = cj + 0.5 + jitter * (toUnit(h.y) - 0.5) - ty;
                        float d2 = dx * dx + dy * dy;
                        if (d2 < f1) {
                            f2 = f1;
                            f1 = d2;
                            id = h.z;
                            nx = dx;
                            ny = dy;
                        } else if (d2 < f2) {
                            f2 = d2;
                        }
                    }
                }
            }
        }
    }
    Cell2 c;
    c.f1 = sqrt(f1);
    c.f2 = sqrt(f2);
    c.id = id;
    c.toNearest = float2(nx, ny);
    return c;
}

[noinline]
Cell3 cellular3(float3 p, float jitter, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    float fz = floor(p.z);
    int ix = int(fx);
    int iy = int(fy);
    int iz = int(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    float half = jitter * 0.5;
    float f1 = 3.4028235e38;
    float f2 = 3.4028235e38;
    uint id = 0u;
    float nx = 0.0;
    float ny = 0.0;
    float nz = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = -2; k <= 2; k++) {
            for (int j = -2; j <= 2; j++) {
                for (int i = -2; i <= 2; i++) {
                    int ring = (i < -1 || i > 1 || j < -1 || j > 1 || k < -1 || k > 1) ? 1 : 0;
                    if (ring == pass) {
                        float ci = float(i);
                        float cj = float(j);
                        float ck = float(k);
                        float reach = noiseGap(tx, ci + 0.5 - half, ci + 0.5 + half) + noiseGap(ty, cj + 0.5 - half, cj + 0.5 + half)
                            + noiseGap(tz, ck + 0.5 - half, ck + 0.5 + half);
                        if (pass == 0 || reach < f2) {
                            uint4 h = hash3(ix + i, iy + j, iz + k, seed);
                            float dx = ci + 0.5 + jitter * (toUnit(h.x) - 0.5) - tx;
                            float dy = cj + 0.5 + jitter * (toUnit(h.y) - 0.5) - ty;
                            float dz = ck + 0.5 + jitter * (toUnit(h.z) - 0.5) - tz;
                            float d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 < f1) {
                                f2 = f1;
                                f1 = d2;
                                id = h.w;
                                nx = dx;
                                ny = dy;
                                nz = dz;
                            } else if (d2 < f2) {
                                f2 = d2;
                            }
                        }
                    }
                }
            }
        }
    }
    Cell3 c;
    c.f1 = sqrt(f1);
    c.f2 = sqrt(f2);
    c.id = id;
    c.toNearest = float3(nx, ny, nz);
    return c;
}

// ======== Fractal sums and domain warp ========

static const int NOISE_LAYER_PLAIN = 0;
static const int NOISE_LAYER_RIDGED = 1;
static const int NOISE_LAYER_BILLOW = 2;

float noiseLayer(int kind, float n) {
    if (kind == NOISE_LAYER_RIDGED) return (1.0 - abs(n)) * (1.0 - abs(n));
    if (kind == NOISE_LAYER_BILLOW) return 2.0 * abs(n) - 1.0;
    return n;
}

float noiseLayerD(int kind, float n) {
    if (kind == NOISE_LAYER_RIDGED) return n < 0.0 ? 2.0 * (1.0 - abs(n)) : -2.0 * (1.0 - abs(n));
    if (kind == NOISE_LAYER_BILLOW) return n < 0.0 ? -2.0 : 2.0;
    return 1.0;
}

[noinline]
Noise2 noiseFractal2(int kind, float2 p, uint seed, int octaves, float lacunarity, float gain) {
    float sum = 0.0;
    float dx = 0.0;
    float dy = 0.0;
    float amp = 1.0;
    float freq = 1.0;
    for (int i = 0; i < octaves; i++) {
        Noise2 n = gradient2(float2(p.x * freq, p.y * freq), hash1(i, seed));
        float s = amp * noiseLayerD(kind, n.value) * freq;
        sum = sum + amp * noiseLayer(kind, n.value);
        dx = dx + s * n.d.x;
        dy = dy + s * n.d.y;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    Noise2 r;
    r.value = sum;
    r.d = float2(dx, dy);
    return r;
}

[noinline]
Noise3 noiseFractal3(int kind, float3 p, uint seed, int octaves, float lacunarity, float gain) {
    float sum = 0.0;
    float dx = 0.0;
    float dy = 0.0;
    float dz = 0.0;
    float amp = 1.0;
    float freq = 1.0;
    for (int i = 0; i < octaves; i++) {
        Noise3 n = gradient3(float3(p.x * freq, p.y * freq, p.z * freq), hash1(i, seed));
        float s = amp * noiseLayerD(kind, n.value) * freq;
        sum = sum + amp * noiseLayer(kind, n.value);
        dx = dx + s * n.d.x;
        dy = dy + s * n.d.y;
        dz = dz + s * n.d.z;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    Noise3 r;
    r.value = sum;
    r.d = float3(dx, dy, dz);
    return r;
}

Noise2 fbm2(float2 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal2(NOISE_LAYER_PLAIN, p, seed, octaves, lacunarity, gain);
}
Noise3 fbm3(float3 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal3(NOISE_LAYER_PLAIN, p, seed, octaves, lacunarity, gain);
}
Noise2 ridged2(float2 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal2(NOISE_LAYER_RIDGED, p, seed, octaves, lacunarity, gain);
}
Noise3 ridged3(float3 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal3(NOISE_LAYER_RIDGED, p, seed, octaves, lacunarity, gain);
}
Noise2 billow2(float2 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal2(NOISE_LAYER_BILLOW, p, seed, octaves, lacunarity, gain);
}
Noise3 billow3(float3 p, uint seed, int octaves, float lacunarity, float gain) {
    return noiseFractal3(NOISE_LAYER_BILLOW, p, seed, octaves, lacunarity, gain);
}

float warp2(float2 p, uint seed, int octaves, float strength) {
    float qx = fbm2(p, seed, octaves, 2.0, 0.5).value;
    float qy = fbm2(float2(p.x + 5.25, p.y + 1.25), seed, octaves, 2.0, 0.5).value;
    float rx = fbm2(float2(p.x + strength * qx + 1.75, p.y + strength * qy + 9.25), seed, octaves, 2.0, 0.5).value;
    float ry = fbm2(float2(p.x + strength * qx + 8.25, p.y + strength * qy + 2.75), seed, octaves, 2.0, 0.5).value;
    return fbm2(float2(p.x + strength * rx, p.y + strength * ry), seed, octaves, 2.0, 0.5).value;
}

float warp3(float3 p, uint seed, int octaves, float strength) {
    float qx = fbm3(p, seed, octaves, 2.0, 0.5).value;
    float qy = fbm3(float3(p.x + 5.25, p.y + 1.25, p.z + 3.5), seed, octaves, 2.0, 0.5).value;
    float qz = fbm3(float3(p.x + 2.5, p.y + 7.75, p.z + 4.25), seed, octaves, 2.0, 0.5).value;
    float sx = p.x + strength * qx;
    float sy = p.y + strength * qy;
    float sz = p.z + strength * qz;
    float rx = fbm3(float3(sx + 1.75, sy + 9.25, sz + 6.5), seed, octaves, 2.0, 0.5).value;
    float ry = fbm3(float3(sx + 8.25, sy + 2.75, sz + 0.75), seed, octaves, 2.0, 0.5).value;
    float rz = fbm3(float3(sx + 3.25, sy + 5.5, sz + 7.25), seed, octaves, 2.0, 0.5).value;
    return fbm3(float3(p.x + strength * rx, p.y + strength * ry, p.z + strength * rz), seed, octaves, 2.0, 0.5).value;
}
