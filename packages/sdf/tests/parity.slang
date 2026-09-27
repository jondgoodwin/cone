// The sdf parity test's shader: each pixel of a 64 x 256 target is one
// quantity (its column) at one sample point (its row), written as the 32
// bits of an f32, one byte per channel of an rgba8unorm pixel (r the lowest
// byte), so the CPU reads back the exact bits. parity.cone computes the same
// table with the Cone package and compares.
//
// slangc: -fp-mode precise

import sdf;

struct VertexOutput {
    float4 position : SV_Position;
};

// A triangle covering the whole target (as noise's parity shader)
[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Sample point 'a' (0 to 255): a 16 x 16 grid in x and y, z varying,
// every coordinate a multiple of 1/32
float3 samplePoint(uint a) {
    uint gx = a % 16u;
    uint gy = a / 16u;
    float x = float(gx) * 0.25 - 1.96875;
    float y = float(gy) * 0.25 - 0.46875;
    float z = float(gx ^ gy) * 0.125 - 0.96875;
    return float3(x, y, z);
}

// Beads along an arc: a box shrinking along it and turning about the
// tangent, 1.5 radians over its length
struct Beads : IRepeated {
    float size;
    float repeatedDistance(float3 q, int index, float u) {
        float s = size * (1.0 - 0.5 * u);
        float c = cos(1.5 * u);
        float sn = sin(1.5 * u);
        float3 t = float3(q.x * c + q.y * sn, q.y * c - q.x * sn, q.z);
        return box(t, float3(s, s * 0.5, s * 0.25));
    }
    float bound(int index, float u) { return size * (1.0 - 0.5 * u) * 1.25; }
};

static const float4 TURN = float4(0.36, 0.48, 0.0, 0.8);

// Quantity 'q' at sample point 'p'. Each function is called at one place
float quantity(uint q, float3 p) {
    float a = sphere(p - float3(0.25, 1.0, 0.0), 0.75);
    float b = box(p - float3(-0.25, 0.5, 0.0), float3(0.5, 0.5, 0.5));
    if (q == 0u) return sphere(p, 1.25);
    if (q == 1u) return box(p, float3(0.75, 1.0, 0.5));
    if (q == 2u) return roundBox(p, float3(0.75, 1.0, 0.5), 0.25);
    if (q == 3u) return capsule(p, float3(-0.5, 0.0, 0.0), float3(0.5, 1.5, 0.25), 0.375);
    if (q == 4u) return torus(p, 1.0, 0.25);
    if (q == 5u) return cylinder(p, 1.0, 0.5);
    if (q == 6u) return cone(p, 1.0, 0.75, 0.25);
    if (q == 7u) return roundCone(p, 0.5, 0.25, 1.5);
    if (q == 8u) return plane(p, float3(0.6, 0.8, 0.0), 0.5);
    if (q == 9u) return ellipsoid(p, float3(1.0, 0.5, 0.75));
    if (q == 10u) return union(a, b);
    if (q == 11u) return subtract(a, b);
    if (q == 12u) return intersect(a, b);
    if (q == 13u) return smoothUnion(a, b, 0.25);
    if (q == 14u) return smoothUnionBlend(a, b, 0.25).distance;
    if (q == 15u) return smoothUnionBlend(a, b, 0.25).blend;
    if (q == 16u) return smoothSubtract(a, b, 0.25);
    if (q == 17u) return smoothIntersect(a, b, 0.25);
    if (q == 18u) return onion(a, 0.125);
    if (q == 19u) return round(b, 0.125);
    if (q == 20u) return exclusive(a, b);
    if (q < 24u) { float3 r = rotate(p, TURN); return q == 21u ? r.x : (q == 22u ? r.y : r.z); }
    if (q == 24u) {
        Placement pl;
        pl.position = float3(0.5, 1.0, 0.0);
        pl.rotation = TURN;
        pl.scale = 1.5;
        return pl.distance(box(pl.toLocal(p), float3(0.5, 0.25, 0.375)));
    }
    if (q < 27u) { float3 m = mirror(p, float3(0.6, 0.8, 0.0), -1.0); return q == 25u ? m.x : m.y; }
    if (q == 27u) return sphere(repeat(p, float3(1.0, 1.5, 0.75)), 0.3);
    if (q == 28u) return sphere(repeatLimited(p, 0.75, float3(1.0, 2.0, 1.0)), 0.25);
    if (q == 29u) return torus(elongate(p, float3(0.5, 0.0, 0.25)), 0.75, 0.2);
    if (q == 30u) return unionRound(a, b, 0.3);
    if (q == 31u) return intersectRound(a, b, 0.3);
    if (q == 32u) return subtractRound(a, b, 0.3);
    if (q == 33u) return unionChamfer(a, b, 0.3);
    if (q == 34u) return unionColumns(a, b, 0.5, 4.0);
    if (q == 35u) return unionStairs(a, b, 0.5, 4.0);
    if (q == 36u) return intersectStairs(a, b, 0.5, 4.0);
    if (q == 37u) return subtractStairs(a, b, 0.5, 4.0);
    if (q == 38u) return pipe(a, b, 0.1);
    if (q == 39u) return groove(a, b, 0.1, 0.1);
    if (q == 40u) return tongue(a, b, 0.1, 0.1);
    if (q == 41u) return engrave(a, b, 0.1);
    if (q < 44u) {
        ArcProjection pr = makeArc(3.0, 2.0).project(p);
        return q == 42u ? pr.s : pr.distance;
    }
    Horn horn = makeHorn(3.0, 0.3, 0.9, 2.0, 14, 0.12);
    if (q == 44u) return horn.distance(p);
    if (q < 48u) { float3 g = gradient(horn, p, 0.015625); return q == 45u ? g.x : (q == 46u ? g.y : g.z); }
    if (q < 51u) { float3 n = normal(horn, p, 0.015625); return q == 48u ? n.x : (q == 49u ? n.y : n.z); }
    if (q == 51u) return makeHorn(2.5, 0.4, 0.5, 0.0, 9, 0.2).distance(p);
    if (q == 52u) {
        Beads beads;
        beads.size = 0.25;
        return repeatAlong(makeArc(3.0, 2.0), beads, p, 10);
    }
    if (q == 53u) return displace(a, p, 7u, 3, 2.0, 0.1);
    if (q == 54u) return fbmDetail(a, p, 7u, 4, 0.5);
    if (q == 55u) return latticeSpheres(p * 2.0, 7u);
    if (q == 56u) {
        Beads beads;
        beads.size = 0.25;
        Arc arc = makeArc(3.0, 2.0);
        return repeatAlongArc(arc, makeArcCells(arc, 10), beads, p);
    }
    if (q == 57u) return makeHorn(4.0, 0.35, 1.0, 4.5, 20, 0.15).distance(p);
    if (q == 58u) return fluted(makeHorn(3.0, 0.3, 0.9, 2.0, 14, 0.12), 12, 0.15, -2.0).distance(p);
    return 0.0;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint q = uint(input.position.x);
    uint a = uint(input.position.y);
    uint bits = asuint(quantity(q, samplePoint(a)));
    return float4(float(bits & 255u), float((bits >> 8u) & 255u), float((bits >> 16u) & 255u),
        float(bits >> 24u)) / 255.0;
}
