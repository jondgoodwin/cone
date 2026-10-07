// render's blend test's custom pipeline: no textures (its material's group
// is the parameters and the sampler alone), see-through, light added: the
// copy's position from the instances, and the colour the material's
// parameters times the copy's tint and the exposure, with an alpha of 0, so
// premultiplied blending adds it to what is behind. Compiled by
// tools/shaders/shaders.py and embedded in blend.cone.

import renderframe;

struct Params {
    float4 color;                  // rgb: the light added
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] SamplerState unused;

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
    output.color = params.color * inst.tint;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color.rgb * frame.exposure.x, 0.0);
}
