// A cube's faces, each its own color, placed by one matrix: the model, view
// and projection matrices multiplied together (column-major, as geomath's
// Mat4 is stored), in a uniform buffer bound as group 0, binding 0.
// Compiled ahead of time to cube.spv by tools/shaders/shaders.py.

struct Uniforms {
    float4x4 modelViewProjection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<Uniforms> uniforms;

struct VertexInput {
    float3 position : POSITION;    // location 0: the model's coordinates
    float3 color : COLOR;          // location 1: linear RGB
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(uniforms.modelViewProjection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
