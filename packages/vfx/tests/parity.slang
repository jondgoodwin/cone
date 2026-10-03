// The vfx parity test's shader: each pixel of a 16 x 256 target is one
// quantity of a particle's state (its column: position, velocity, size,
// colour, life, alive) for one (spawn index, age) (its row), evaluated by
// vfx.slang's particleAt from the emitter in the uniform buffer, and
// written as the 32 bits of an f32, one byte per channel of an rgba8unorm
// pixel (r the lowest byte), so the CPU reads back the exact bits.
// parity.cone computes the same table with the Cone package and compares.
//
// slangc: -fp-mode precise

import vfx;

[[vk::binding(0, 0)]] ConstantBuffer<EmitterParams> emitter;

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

// Row 'a' (0 to 255): its spawn index and its age, as parity.cone's
uint sampleIndex(uint a) { return a * 37u + (a % 3u) * 1000000u; }
float sampleAge(uint a) { return float(a % 16u) * 0.0625 - 0.0625; }

float quantity(uint q, Particle p) {
    if (q == 0u) { return p.position.x; }
    if (q == 1u) { return p.position.y; }
    if (q == 2u) { return p.position.z; }
    if (q == 3u) { return p.velocity.x; }
    if (q == 4u) { return p.velocity.y; }
    if (q == 5u) { return p.velocity.z; }
    if (q == 6u) { return p.size; }
    if (q == 7u) { return p.color.x; }
    if (q == 8u) { return p.color.y; }
    if (q == 9u) { return p.color.z; }
    if (q == 10u) { return p.color.w; }
    if (q == 11u) { return p.life; }
    if (q == 12u) { return p.alive; }
    return 0.0;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint q = uint(input.position.x);
    uint a = uint(input.position.y);
    float value;
    if (q >= 13u) {
        // At the emitter's elapsed time, as the draw shaders evaluate
        Particle now = particleNow(emitter, sampleIndex(a));
        value = (q == 13u) ? now.position.x : now.color.w;
    } else {
        value = quantity(q, particleAt(emitter, sampleIndex(a), sampleAge(a)));
    }
    uint b = asuint(value);
    return float4(float(b & 255u), float((b >> 8u) & 255u), float((b >> 16u) & 255u), float(b >> 24u)) / 255.0;
}
