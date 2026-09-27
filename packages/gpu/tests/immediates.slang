// Flat-colored 2-D shapes placed by immediates (Vulkan's push constants): a
// scale and an offset applied to each position, and the color, set with
// 'pass.setImmediates' between draws. Compiled ahead of time to
// immediates.spv by tools/shaders/shaders.py.

struct Immediates {
    float2 offset;                 // bytes 0-7
    float scale;                   // bytes 8-11
    float depth;                   // bytes 12-15: the z every vertex gets
    float4 color;                  // bytes 16-31
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(float2 position : POSITION) {
    VertexOutput output;
    output.position = float4(position * immediates.scale + immediates.offset, immediates.depth, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return immediates.color;
}
