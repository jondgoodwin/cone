// The noise parity test's shader: each pixel of a 64 x 256 target is one
// quantity (its column) at one sample point (its row), written as the 32
// bits of a word or of an f32, one byte per channel of an rgba8unorm pixel
// (r the lowest byte), so the CPU reads back the exact bits. parity.cone
// computes the same table with the Cone package and compares.
//
// slangc: -fp-mode precise

import noise;
import phacelle;

struct VertexOutput {
    float4 position : SV_Position;
};

// A triangle covering the whole target. SV_VulkanVertexID is Vulkan's
// VertexIndex; SV_VertexID would subtract BaseVertex, which needs the
// DrawParameters capability the gpu package does not enable
[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Sample point 'a' (0 to 255) in 2-D and 3-D, and its four words: the
// numbers the Cone side computes in parity.cone's 'point'
float3 samplePoint(uint a) {
    uint gx = a % 16u;
    uint gy = a / 16u;
    float x = float(gx) * 0.40625 - 3.21875;
    float y = float(gy) * 0.34375 + 0.53125;
    float z = float(gx ^ gy) * 0.28125 - 1.90625;
    if (a >= 128u) {
        x = x + 1000.25;
        y = y - 777.5;
        z = z + 4096.125;
    }
    return float3(x, y, z);
}

uint4 sampleWords(uint a) {
    return uint4(a * 2654435761u, a + 2147483632u, a * a * 40503u, ~a);
}

// Component 'c' of a noise result
float pick2(Noise2 n, uint c) { return c == 0u ? n.value : (c == 1u ? n.d.x : n.d.y); }
float pick3(Noise3 n, uint c) { return c == 0u ? n.value : (c == 1u ? n.d.x : (c == 2u ? n.d.y : n.d.z)); }

// Quantity 'q' at sample point 'a', as 32 bits. Each function is called at
// one place, so the shader stays small
uint quantity(uint q, uint a) {
    float3 p3 = samplePoint(a);
    float2 p2 = p3.xy;
    uint4 w = sampleWords(a);
    uint seed = 7u;
    if (q == 0u) return pcg(w.x);
    if (q < 3u) { uint2 h = pcg2d(w.xy); return q == 1u ? h.x : h.y; }
    if (q < 6u) { uint3 h = pcg3d(w.xyz); return q == 3u ? h.x : (q == 4u ? h.y : h.z); }
    if (q < 10u) { uint4 h = pcg4d(w); return q == 6u ? h.x : (q == 7u ? h.y : (q == 8u ? h.z : h.w)); }
    if (q == 10u) return hash1(int(a) - 128, seed);
    if (q == 11u) return asuint(toUnit(pcg(w.x)));
    if (q == 12u) return asuint(toSigned(pcg(w.x)));
    if (q < 16u) return asuint(pick2(value2(p2, seed), q - 13u));
    if (q < 19u) return asuint(pick2(gradient2(p2, seed), q - 16u));
    if (q < 23u) return asuint(pick3(value3(p3, seed), q - 19u));
    if (q < 27u) return asuint(pick3(gradient3(p3, seed), q - 23u));
    if (q < 30u) return asuint(pick2(gradient2Periodic(p2, 4, seed), q - 27u));
    if (q < 34u) return asuint(pick3(gradient3Periodic(p3, 5, seed), q - 30u));
    if (q < 39u) {
        Cell2 c = cellular2(p2, 1.0, seed);
        if (q == 36u) return c.id;
        return asuint(q == 34u ? c.f1 : (q == 35u ? c.f2 : (q == 37u ? c.toNearest.x : c.toNearest.y)));
    }
    if (q < 45u) {
        Cell3 c = cellular3(p3, 1.0, seed);
        if (q == 41u) return c.id;
        return asuint(q == 39u ? c.f1 : (q == 40u ? c.f2 : (q == 42u ? c.toNearest.x
            : (q == 43u ? c.toNearest.y : c.toNearest.z))));
    }
    if (q < 48u) return asuint(pick2(fbm2(p2, seed, 5, 2.0, 0.5), q - 45u));
    if (q == 48u) return asuint(ridged2(p2, seed, 5, 2.0, 0.5).value);
    if (q == 49u) return asuint(billow2(p2, seed, 5, 2.0, 0.5).value);
    if (q < 54u) return asuint(pick3(fbm3(p3, seed, 4, 2.0, 0.5), q - 50u));
    if (q == 54u) return asuint(ridged3(p3, seed, 4, 2.0, 0.5).value);
    if (q == 55u) return asuint(billow3(p3, seed, 4, 2.0, 0.5).value);
    if (q == 56u) return asuint(warp2(p2, seed, 4, 4.0));
    if (q == 57u) return asuint(warp3(p3, seed, 3, 4.0));
    if (q < 62u) {
        Phacelle ph = phacelle2(p2, float2(0.6, 0.8), 1.0, 0.25, 0.5, seed);
        return asuint(q == 58u ? ph.value.x : (q == 59u ? ph.value.y : (q == 60u ? ph.side.x : ph.side.y)));
    }
    if (q == 62u) return asuint(value2Periodic(p2, 3, seed).value);
    return asuint(value3Periodic(p3, 3, seed).value);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint q = uint(input.position.x);
    uint a = uint(input.position.y);
    uint bits = quantity(q, a);
    return float4(float(bits & 255u), float((bits >> 8u) & 255u), float((bits >> 16u) & 255u),
        float(bits >> 24u)) / 255.0;
}
