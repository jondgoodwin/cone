// The classic triangle: each corner's color from its vertex, interpolated
// across the face. Compiled ahead of time to triangle.spv by
// tools/shaders/shaders.py, and embedded in triangle.cone.

struct VertexInput {
    float2 position : POSITION;    // location 0: clip space, y up
    float3 color : COLOR;          // location 1: linear RGB
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
