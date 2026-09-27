// The levels test's shaders: one triangle over the target; 'fillMain'
// writes the immediates' value into red, 'readMain' reads a cube view back,
// one face and level a block of the 64 x 64 target (face = x / 8, for x
// below 48; level = y / 16), writing a sixty-fourth of what it read.
// Compiled ahead of time to levels.spv by tools/shaders/shaders.py.

struct Fill {
    float4 value;                  // x the value
};

[[vk::binding(0, 0)]] TextureCube cube;
[[vk::binding(1, 0)]] SamplerState nearest;
[[vk::push_constant]] ConstantBuffer<Fill> fill;

struct Output {
    float4 position : SV_Position;
};

[shader("vertex")]
Output fullscreenMain(uint id : SV_VulkanVertexID) {
    Output output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fillMain(Output input) : SV_Target {
    return float4(fill.value.x, 0.0, 0.0, 1.0);
}

[shader("fragment")]
float4 readMain(Output input) : SV_Target {
    uint face = uint(input.position.x) / 8;
    uint level = uint(input.position.y) / 16;
    if (face > 5) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    // The middle of each face: its axis
    float3 axes[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1),
        float3(0, 0, -1) };
    float v = cube.SampleLevel(nearest, axes[face], float(level)).r;
    return float4(v / 64.0, 0.0, 0.0, 1.0);
}
