// The horn demo's GPU parity pass (horn.cone): the demo's horn evaluated by
// sdf.slang at 4096 fixed points, each pixel of a 64 x 256 target one
// quantity at one point, its 32 bits one byte per channel of an rgba8unorm
// pixel (r the lowest byte), so the CPU reads back the exact bits, as the
// sdf package's parity test does. horn.cone computes the same table with
// the Cone package, compares, and hashes both.
//
// Pixel (x, y) is quantity x % 4 (the distance, then the normal's x, y
// and z) at point y * 16 + x / 4. The points are a 16 x 16 x 16 grid over
// the horn's box, every coordinate a multiple of 1/16, exact in binary.
//
// slangc: -fp-mode precise

import sdf;

struct VertexOutput {
    float4 position : SV_Position;
};

// A triangle covering the whole target
[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Point 'a' (0 to 4095), as horn.cone's 'parityPoint'
float3 parityPoint(uint a) {
    uint gx = a % 16u;
    uint gy = (a / 16u) % 16u;
    uint gz = a / 256u;
    return float3(float(gx) * 0.1875 - 0.5, float(gy) * 0.1875 - 0.5, float(gz) * 0.0625 - 0.5);
}

// The demo's horn: horn.cone's LENGTH, BASE, TAPER, CURL, RIBS, RIB_DEPTH,
// FLUTES, FLUTE_DEPTH and FLUTE_TWIST
float quantity(uint q, float3 p) {
    Horn horn = fluted(makeHorn(3.4, 0.34, 0.97, 1.7, 22, 0.12), 40, 0.018, 0.8);
    if (q == 0u) return horn.distance(p);
    float3 n = normal(horn, p, 0.015625);
    return q == 1u ? n.x : (q == 2u ? n.y : n.z);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint x = uint(input.position.x);
    uint y = uint(input.position.y);
    uint bits = asuint(quantity(x % 4u, parityPoint(y * 16u + x / 4u)));
    return float4(float(bits & 255u), float((bits >> 8u) & 255u), float((bits >> 16u) & 255u),
        float(bits >> 24u)) / 255.0;
}
