// The render package's line material: lines (a wireframe, a mesh's edges)
// in one flat color, unlit, drawn a little towards the camera so that lines
// lying on a surface drawn with the same depth win the depth test. Compiled
// ahead of time to lines.spv by tools/shaders/shaders.py and embedded in
// material.cone. Group 0 and the immediates are the lit material's.

struct Frame {
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 lightColor;
    float4 ambient;
};

struct Immediates {
    float4x4 model;
    float4 tint;                   // the lines' color
};

[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;
[[vk::push_constant]] ConstantBuffer<Immediates> draw;

// How far towards the camera a line is drawn, in depth (0 to 1): about a
// fiftieth of a unit at ten units from a camera whose near plane is 0.5
static const float DEPTH_BIAS = 0.0001;

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION) {
    VertexOutput output;
    float4 clip = mul(frame.viewProjection, mul(draw.model, float4(position, 1.0)));
    clip.z = clip.z - DEPTH_BIAS * clip.w;
    output.position = clip;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return draw.tint;
}
