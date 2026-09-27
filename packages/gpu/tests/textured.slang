// A texture sampled: each vertex a 2-D position (clip space, y up) and a
// texture coordinate (u across, v down from the texture's first row, as
// WebGPU has them), the texture at group 0, binding 0, and its sampler at
// binding 1. Compiled ahead of time to textured.spv by
// tools/shaders/shaders.py.

[[vk::binding(0, 0)]]
Texture2D colors;

[[vk::binding(1, 0)]]
SamplerState colorSampler;

struct VertexInput {
    float2 position : POSITION;    // location 0
    float2 uv : TEXCOORD;          // location 1
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return colors.Sample(colorSampler, input.uv);
}
