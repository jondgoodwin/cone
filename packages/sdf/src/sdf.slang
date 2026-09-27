// The sdf package's functions for shaders: the same names, and the same
// arithmetic in the same order, as the Cone source beside it (primitive,
// operator, domain, fillet, curve, horn, shape and detail .cone), so that a
// shader and the CPU agree. A shader reaches it with 'import sdf;'. It is a
// module, with no entry points.
//
// It imports noise (for 'displace' and 'fbmDetail'), so a shader that
// imports it needs '// slangc: -fp-mode precise', as noise's README says:
// without it a driver may fuse a multiply and an add into one rounding.
//
// Where the Cone package has a type's constructor ('Arc.make', 'arc.cells',
// 'Horn.make') this has a function ('makeArc', 'makeArcCells', 'makeHorn');
// a rotation is a float4 (x, y, z, w) for geomath's Quat; the Cone traits Shape, Curve
// and Repeated are the interfaces IShape, ICurve and IRepeated, and the
// generic functions are Slang generics over them. PathCurve is CPU only.
// Lengths and dot products are written out ('len3', 'dot3'), never Slang's
// 'length' or 'dot', whose order of additions a driver chooses. The parity
// test, tests/parity.cone, checks this module against the Cone package on a
// GPU.

import noise;

static const float SDF_PI = 3.14159265358979;
static const float SDF_HUGE = 3.4028235e38;
static const float SQRT_HALF = 0.70710678118654752;
static const float SQRT_TWO = 1.41421356237309505;

// ======== Scalar helpers ========

float sdfMod(float x, float y) { return x - y * floor(x / y); }
float len2(float x, float y) { return sqrt(x * x + y * y); }
float len3(float3 v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float dot3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Blend {
    float distance;
    float blend;
};

struct CurveFrame {
    float3 origin;
    float3 normal;
    float3 binormal;
    float3 tangent;

    float3 toLocal(float3 p) {
        float3 d = p - origin;
        return float3(dot3(d, normal), dot3(d, binormal), dot3(d, tangent));
    }
};

// ======== Primitives ========

float sphere(float3 p, float radius) { return len3(p) - radius; }

float box(float3 p, float3 extent) {
    float qx = abs(p.x) - extent.x;
    float qy = abs(p.y) - extent.y;
    float qz = abs(p.z) - extent.z;
    float outside = len3(float3(max(qx, 0.0), max(qy, 0.0), max(qz, 0.0)));
    return outside + min(max(qx, max(qy, qz)), 0.0);
}

float roundBox(float3 p, float3 extent, float radius) {
    return box(p, float3(extent.x - radius, extent.y - radius, extent.z - radius)) - radius;
}

float capsule(float3 p, float3 a, float3 b, float radius) {
    float3 pa = p - a;
    float3 ba = b - a;
    float bb = dot3(ba, ba);
    float h = bb > 0.0 ? clamp(dot3(pa, ba) / bb, 0.0, 1.0) : 0.0;
    return len3(float3(pa.x - ba.x * h, pa.y - ba.y * h, pa.z - ba.z * h)) - radius;
}

float torus(float3 p, float major, float minor) {
    return len2(len2(p.x, p.z) - major, p.y) - minor;
}

float cylinder(float3 p, float halfHeight, float radius) {
    float dx = len2(p.x, p.z) - radius;
    float dy = abs(p.y) - halfHeight;
    return min(max(dx, dy), 0.0) + len2(max(dx, 0.0), max(dy, 0.0));
}

float cone(float3 p, float halfHeight, float bottom, float top) {
    float qx = len2(p.x, p.z);
    float qy = p.y;
    float capR = qy < 0.0 ? bottom : top;
    float ax = qx - min(qx, capR);
    float ay = abs(qy) - halfHeight;
    float kx = top - bottom;
    float ky = 2.0 * halfHeight;
    float kk = kx * kx + ky * ky;
    float t = kk > 0.0 ? clamp(((top - qx) * kx + (halfHeight - qy) * ky) / kk, 0.0, 1.0) : 0.0;
    float sx = qx - top + kx * t;
    float sy = qy - halfHeight + ky * t;
    bool inside = sx < 0.0 && ay < 0.0;
    float d = sqrt(min(ax * ax + ay * ay, sx * sx + sy * sy));
    return inside ? -d : d;
}

float roundCone(float3 p, float bottom, float top, float height) {
    float b = (bottom - top) / height;
    float a = sqrt(max(1.0 - b * b, 0.0));
    float qx = len2(p.x, p.z);
    float qy = p.y;
    float k = qx * (0.0 - b) + qy * a;
    if (k < 0.0) return len2(qx, qy) - bottom;
    if (k > a * height) return len2(qx, qy - height) - top;
    return qx * a + qy * b - bottom;
}

float plane(float3 p, float3 n, float offset) { return dot3(p, n) + offset; }

float ellipsoid(float3 p, float3 radii) {
    float k0 = len3(float3(p.x / radii.x, p.y / radii.y, p.z / radii.z));
    float k1 = len3(float3(p.x / (radii.x * radii.x), p.y / (radii.y * radii.y), p.z / (radii.z * radii.z)));
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : 0.0 - min(radii.x, min(radii.y, radii.z));
}

// ======== Operators ========

float union(float a, float b) { return min(a, b); }
float subtract(float a, float b) { return max(a, 0.0 - b); }
float intersect(float a, float b) { return max(a, b); }

float smoothUnion(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    float h = max(k * 4.0 - abs(a - b), 0.0) / (k * 4.0);
    return min(a, b) - h * h * k;
}

Blend smoothUnionBlend(float a, float b, float k) {
    Blend r;
    if (k <= 0.0) {
        if (a < b) { r.distance = a; r.blend = 0.0; } else { r.distance = b; r.blend = 1.0; }
        return r;
    }
    float h = max(k * 4.0 - abs(a - b), 0.0) / (k * 4.0);
    float m = h * h * 0.5;
    float s = h * h * k;
    if (a < b) { r.distance = a - s; r.blend = m; } else { r.distance = b - s; r.blend = 1.0 - m; }
    return r;
}

float smoothSubtract(float a, float b, float k) { return 0.0 - smoothUnion(0.0 - a, b, k); }
float smoothIntersect(float a, float b, float k) { return 0.0 - smoothUnion(0.0 - a, 0.0 - b, k); }
float onion(float d, float thickness) { return abs(d) - thickness; }
float round(float d, float radius) { return d - radius; }
float exclusive(float a, float b) { return max(min(a, b), 0.0 - max(a, b)); }

// ======== Domain operators ========

float3 translate(float3 p, float3 offset) { return p - offset; }

// 'q' a unit quaternion (x, y, z, w)
float3 rotate(float3 p, float4 q) {
    float ux = 0.0 - q.x;
    float uy = 0.0 - q.y;
    float uz = 0.0 - q.z;
    float tx = (uy * p.z - uz * p.y) * 2.0;
    float ty = (uz * p.x - ux * p.z) * 2.0;
    float tz = (ux * p.y - uy * p.x) * 2.0;
    return float3(p.x + q.w * tx + (uy * tz - uz * ty), p.y + q.w * ty + (uz * tx - ux * tz),
        p.z + q.w * tz + (ux * ty - uy * tx));
}

float3 scalePoint(float3 p, float scale) { return p * (1.0 / scale); }
float scaleDistance(float d, float scale) { return d * scale; }

struct Placement {
    float3 position;
    float4 rotation;
    float scale;

    float3 toLocal(float3 p) { return scalePoint(rotate(p - position, rotation), scale); }
    float distance(float d) { return d * scale; }
};

float3 mirrorX(float3 p) { return float3(abs(p.x), p.y, p.z); }
float3 mirrorY(float3 p) { return float3(p.x, abs(p.y), p.z); }
float3 mirrorZ(float3 p) { return float3(p.x, p.y, abs(p.z)); }

float3 mirror(float3 p, float3 n, float offset) {
    float t = dot3(p, n) + offset;
    return t < 0.0 ? p - n * (t * 2.0) : p;
}

float3 repeat(float3 p, float3 period) {
    return float3(p.x - period.x * floor(p.x / period.x + 0.5), p.y - period.y * floor(p.y / period.y + 0.5),
        p.z - period.z * floor(p.z / period.z + 0.5));
}

float3 repeatLimited(float3 p, float period, float3 limit) {
    float cx = clamp(floor(p.x / period + 0.5), 0.0 - limit.x, limit.x);
    float cy = clamp(floor(p.y / period + 0.5), 0.0 - limit.y, limit.y);
    float cz = clamp(floor(p.z / period + 0.5), 0.0 - limit.z, limit.z);
    return float3(p.x - period * cx, p.y - period * cy, p.z - period * cz);
}

float3 elongate(float3 p, float3 extent) {
    return float3(p.x - clamp(p.x, 0.0 - extent.x, extent.x), p.y - clamp(p.y, 0.0 - extent.y, extent.y),
        p.z - clamp(p.z, 0.0 - extent.z, extent.z));
}

// ======== Fillets (hg_sdf, MIT) ========

float unionRound(float a, float b, float r) {
    float ux = max(r - a, 0.0);
    float uy = max(r - b, 0.0);
    return max(r, min(a, b)) - len2(ux, uy);
}

float intersectRound(float a, float b, float r) {
    float ux = max(r + a, 0.0);
    float uy = max(r + b, 0.0);
    return min(0.0 - r, max(a, b)) + len2(ux, uy);
}

float subtractRound(float a, float b, float r) { return intersectRound(a, 0.0 - b, r); }

float unionChamfer(float a, float b, float r) { return min(min(a, b), (a - r + b) * SQRT_HALF); }

// hg_sdf's, the columns clipped to the band rather than switched off
// outside it (see fillet.cone)
float unionColumns(float a, float b, float r, float n) {
    float columnRadius = r * SQRT_TWO / ((n - 1.0) * 2.0 + SQRT_TWO);
    float px = (a + b) * SQRT_HALF;
    float py = (b - a) * SQRT_HALF;
    px = px - SQRT_TWO / 2.0 * r;
    px = px + columnRadius * SQRT_TWO;
    if (sdfMod(n, 2.0) == 1.0) py = py + columnRadius;
    float size = columnRadius * 2.0;
    py = sdfMod(py + columnRadius, size) - columnRadius;
    float columns = min(len2(px, py) - columnRadius, px);
    return min(min(a, b), max(columns, max(a - r, b - r)));
}

float unionStairs(float a, float b, float r, float n) {
    float s = r / n;
    float u = b - r;
    return min(min(a, b), 0.5 * (u + a + abs(sdfMod(u - a + s, 2.0 * s) - s)));
}

float intersectStairs(float a, float b, float r, float n) { return 0.0 - unionStairs(0.0 - a, 0.0 - b, r, n); }
float subtractStairs(float a, float b, float r, float n) { return 0.0 - unionStairs(0.0 - a, b, r, n); }
float pipe(float a, float b, float r) { return len2(a, b) - r; }
float groove(float a, float b, float depth, float width) { return max(a, min(a + depth, width - abs(b))); }
float tongue(float a, float b, float height, float width) { return min(a, max(a - height, abs(b) - width)); }
float engrave(float a, float b, float r) { return max(a, (a + r - abs(b)) * SQRT_HALF); }

// ======== Curves, and repetition along them ========

interface ICurve {
    float length();
    float nearest(float3 p);
    CurveFrame frameAt(float s);
};

interface IRepeated {
    float repeatedDistance(float3 q, int index, float u);
    float bound(int index, float u);
};

bool mayBeNearer(float dd, float b, float d) {
    float lim = d + b;
    return lim > 0.0 && dd < lim * lim;
}

[noinline]
float repeatAlong<C : ICurve, S : IRepeated>(C curve, S shape, float3 p, int count) {
    int n = count < 1 ? 1 : count;
    float len = curve.length();
    float best = SDF_HUGE;
    int bestK = 0;
    for (int k = 0; k < n; k++) {
        float3 o = curve.frameAt((float(k) + 0.5) / float(n) * len).origin - p;
        float dd = dot3(o, o);
        if (dd < best) { best = dd; bestK = k; }
    }
    float bu = (float(bestK) + 0.5) / float(n);
    float d = shape.repeatedDistance(curve.frameAt(bu * len).toLocal(p), bestK, bu);
    for (int k = 0; k < n; k++) {
        if (k != bestK) {
            float u = (float(k) + 0.5) / float(n);
            CurveFrame frame = curve.frameAt(u * len);
            float3 o = frame.origin - p;
            if (mayBeNearer(dot3(o, o), shape.bound(k, u), d)) {
                d = min(d, shape.repeatedDistance(frame.toLocal(p), k, u));
            }
        }
    }
    return d;
}

static const float STRAIGHT = 0.0001;

struct ArcProjection {
    float s;
    float distance;
};

struct Arc : ICurve {
    float len;
    float curl;
    float radius;

    float length() { return len; }

    CurveFrame frameAt(float s) {
        CurveFrame f;
        if (radius == 0.0) {
            f.origin = float3(0.0, s, 0.0);
            f.normal = float3(-1.0, 0.0, 0.0);
            f.binormal = float3(0.0, 0.0, 1.0);
            f.tangent = float3(0.0, 1.0, 0.0);
            return f;
        }
        float t = clamp(s, 0.0, len);
        float a = t / radius;
        float c = cos(a);
        float sn = sin(a);
        float3 o = float3(radius - radius * c, radius * sn, 0.0);
        float3 tangent = float3(sn, c, 0.0);
        float beyond = s - t;
        f.origin = float3(o.x + tangent.x * beyond, o.y + tangent.y * beyond, 0.0);
        f.normal = float3(0.0 - c, sn, 0.0);
        f.binormal = float3(0.0, 0.0, 1.0);
        f.tangent = tangent;
        return f;
    }

    float3 point(float s) { return frameAt(s).origin; }

    [noinline]
    ArcProjection project(float3 p) {
        ArcProjection r;
        if (radius == 0.0) {
            float s = clamp(p.y, 0.0, len);
            r.s = s;
            r.distance = len3(float3(p.x, p.y - s, p.z));
            return r;
        }
        float vx = p.x - radius;
        float vy = p.y;
        float angle = atan2(vy, 0.0 - vx);
        float middle = curl * 0.5;
        float delta = angle - middle;
        if (delta < 0.0 - SDF_PI) delta = delta + SDF_PI * 2.0;
        float a = middle + delta;
        if (a <= 0.0) { r.s = 0.0; r.distance = len3(p); return r; }
        if (a >= curl) { r.s = len; r.distance = len3(p - point(len)); return r; }
        r.s = a * radius;
        r.distance = len2(len2(vx, vy) - radius, p.z);
        return r;
    }

    float nearest(float3 p) { return project(p).s; }
};

Arc makeArc(float length, float curl) {
    Arc arc;
    float c = curl < 0.0 ? 0.0 : curl;
    arc.len = length;
    if (c < STRAIGHT) { arc.curl = 0.0; arc.radius = 0.0; return arc; }
    arc.curl = c;
    arc.radius = length / c;
    return arc;
}

// Cone's 'arc.cells(count)'
struct ArcCells {
    int count;
    float step;
    float firstCos;
    float firstSin;
    float turnCos;
    float turnSin;
};

ArcCells makeArcCells(Arc arc, int count) {
    ArcCells cells;
    int n = count < 1 ? 1 : count;
    cells.count = n;
    cells.step = arc.len / float(n);
    if (arc.radius == 0.0) {
        cells.firstCos = 1.0; cells.firstSin = 0.0; cells.turnCos = 1.0; cells.turnSin = 0.0;
        return cells;
    }
    float turn = arc.curl / float(n);
    cells.firstCos = cos(turn * 0.5);
    cells.firstSin = sin(turn * 0.5);
    cells.turnCos = cos(turn);
    cells.turnSin = sin(turn);
    return cells;
}

CurveFrame arcCellFrame(Arc arc, ArcCells cells, int k, float c, float s) {
    CurveFrame f;
    f.binormal = float3(0.0, 0.0, 1.0);
    if (arc.radius == 0.0) {
        f.origin = float3(0.0, (float(k) + 0.5) * cells.step, 0.0);
        f.normal = float3(-1.0, 0.0, 0.0);
        f.tangent = float3(0.0, 1.0, 0.0);
        return f;
    }
    f.origin = float3(arc.radius - arc.radius * c, arc.radius * s, 0.0);
    f.normal = float3(0.0 - c, s, 0.0);
    f.tangent = float3(s, c, 0.0);
    return f;
}

[noinline]
float repeatAlongArc<S : IRepeated>(Arc arc, ArcCells cells, S shape, float3 p) {
    int n = cells.count;
    float best = SDF_HUGE;
    int bestK = 0;
    float bestC = cells.firstCos;
    float bestS = cells.firstSin;
    float c = cells.firstCos;
    float s = cells.firstSin;
    for (int k = 0; k < n; k++) {
        float3 o = arcCellFrame(arc, cells, k, c, s).origin - p;
        float dd = dot3(o, o);
        if (dd < best) { best = dd; bestK = k; bestC = c; bestS = s; }
        float nc = c * cells.turnCos - s * cells.turnSin;
        s = s * cells.turnCos + c * cells.turnSin;
        c = nc;
    }
    float bu = (float(bestK) + 0.5) / float(n);
    float d = shape.repeatedDistance(arcCellFrame(arc, cells, bestK, bestC, bestS).toLocal(p), bestK, bu);
    c = cells.firstCos;
    s = cells.firstSin;
    for (int k = 0; k < n; k++) {
        if (k != bestK) {
            float u = (float(k) + 0.5) / float(n);
            CurveFrame frame = arcCellFrame(arc, cells, k, c, s);
            float3 o = frame.origin - p;
            if (mayBeNearer(dot3(o, o), shape.bound(k, u), d)) {
                d = min(d, shape.repeatedDistance(frame.toLocal(p), k, u));
            }
        }
        float nc = c * cells.turnCos - s * cells.turnSin;
        s = s * cells.turnCos + c * cells.turnSin;
        c = nc;
    }
    return d;
}

// ======== The horn ========

interface IShape {
    float distance(float3 p);
};

struct Horn : IShape, IRepeated {
    float length;
    float baseRadius;
    float taper;
    float curl;
    int ribs;
    float ribDepth;
    Arc arc;
    ArcCells cells;
    float sag;
    float halfChord;

    float radiusAt(float u) { return baseRadius * (1.0 - taper * u); }

    bool ribbed() { return ribs > 0 && ribDepth > 0.0; }

    float repeatedDistance(float3 q, int index, float u) {
        float halfU = 0.5 / float(cells.count);
        float r0 = radiusAt(u - halfU);
        float r1 = radiusAt(u + halfU);
        float cx = q.x + sag;
        float segment = roundCone(float3(cx, q.z + halfChord, q.y), r0, r1, halfChord * 2.0);
        if (!ribbed()) return segment;
        float r = radiusAt(u);
        float m = r * ribDepth;
        float rib = len2(len2(cx, q.y) - r, q.z) - m;
        return smoothUnion(segment, rib, m * 0.35);
    }

    float bound(int index, float u) {
        float halfU = 0.5 / float(cells.count);
        float r0 = radiusAt(u - halfU);
        float r1 = radiusAt(u + halfU);
        float r = max(r0, r1);
        float rib = ribbed() ? r * ribDepth * 1.35 : 0.0;
        return halfChord + sag + r + rib;
    }

    float distance(float3 p) { return repeatAlongArc(arc, cells, this, p); }
};

Horn makeHorn(float length, float baseRadius, float taper, float curl, int ribs, float ribDepth) {
    Horn h;
    Arc arc = makeArc(length, curl);
    ArcCells cells = makeArcCells(arc, ribs > 0 ? ribs : 16);
    h.length = length;
    h.baseRadius = baseRadius;
    h.taper = taper;
    h.curl = arc.curl;
    h.ribs = ribs;
    h.ribDepth = ribDepth;
    h.arc = arc;
    h.cells = cells;
    h.sag = arc.radius == 0.0 ? 0.0 : arc.radius - arc.radius * cells.firstCos;
    h.halfChord = arc.radius == 0.0 ? cells.step * 0.5 : arc.radius * cells.firstSin;
    return h;
}

// ======== Gradient and normal ========

float3 gradient<S : IShape>(S shape, float3 p, float h) {
    float dx = shape.distance(float3(p.x + h, p.y, p.z)) - shape.distance(float3(p.x - h, p.y, p.z));
    float dy = shape.distance(float3(p.x, p.y + h, p.z)) - shape.distance(float3(p.x, p.y - h, p.z));
    float dz = shape.distance(float3(p.x, p.y, p.z + h)) - shape.distance(float3(p.x, p.y, p.z - h));
    float inv = 0.5 / h;
    return float3(dx * inv, dy * inv, dz * inv);
}

float3 normal<S : IShape>(S shape, float3 p, float h) {
    float a = shape.distance(float3(p.x + h, p.y - h, p.z - h));
    float b = shape.distance(float3(p.x - h, p.y - h, p.z + h));
    float c = shape.distance(float3(p.x - h, p.y + h, p.z - h));
    float d = shape.distance(float3(p.x + h, p.y + h, p.z + h));
    float3 g = float3(a - b - c + d, 0.0 - a - b + c + d, 0.0 - a + b - c + d);
    float len = len3(g);
    return len > 0.0 ? g * (1.0 / len) : float3(1.0, 0.0, 0.0);
}

// ======== Detail from noise ========

float displace(float d, float3 p, uint seed, int octaves, float frequency, float amplitude) {
    return d + fbm3(p * frequency, seed, octaves, 2.0, 0.5).value * amplitude;
}

[noinline]
float latticeSpheres(float3 p, uint seed) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    float fz = floor(p.z);
    int ix = int(fx);
    int iy = int(fy);
    int iz = int(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    float d = SDF_HUGE;
    for (int c = 0; c < 8; c++) {
        int cx = c & 1;
        int cy = (c >> 1) & 1;
        int cz = (c >> 2) & 1;
        float h = toUnit(hash3(ix + cx, iy + cy, iz + cz, seed).x);
        float radius = h * h * 0.5;
        d = min(d, len3(float3(tx - float(cx), ty - float(cy), tz - float(cz))) - radius);
    }
    return d;
}

[noinline]
float fbmDetail(float d, float3 p, uint seed, int octaves, float size) {
    float3 q = p * (1.0 / size);
    float s = size;
    float outD = d;
    for (int i = 0; i < octaves; i++) {
        float n = latticeSpheres(q, hash1(i, seed)) * s;
        float clipped = smoothIntersect(n, outD - s * 0.1, s * 0.075);
        outD = smoothUnion(clipped, outD, s * 0.075);
        q = float3(0.0 - q.y * 1.6 - q.z * 1.2, q.x * 1.6 + q.y * 0.72 - q.z * 0.96, q.x * 1.2 - q.y * 0.96 + q.z * 1.28);
        s = s * 0.5;
    }
    return outD;
}
