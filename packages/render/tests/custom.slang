// render's custom test's pipeline: the copy's position from the instances,
// a value read from an r32float field in the vertex stage (texel (1, 0)),
// and the colour (that value, the material's two parameters) times the
// copy's tint and the exposure. Compiled by tools/shaders/shaders.py and
// embedded in custom.cone.

import renderframe;

struct Params {
    float4 values;                 // x, y: the green and blue written
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] Texture2D<float> field;
[[vk::binding(2, 1)]] SamplerState unused;

// Position only: the pipeline is made with one vertex stream
struct VertexInput {
    float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    output.position = mul(frame.viewProjection, mul(inst.model, float4(input.position, 1.0)));
    float red = field.Load(int3(1, 0, 0));
    output.color = float4(red, params.values.x, params.values.y, 1.0) * inst.tint;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color.rgb * frame.exposure.x, 1.0);
}
