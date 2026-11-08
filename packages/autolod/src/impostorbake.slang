// The autolod package's impostor bake, the second pass: render's custom pipeline
// (render\src\custom.cone) that draws an object's normals and depth for one frame
// of an octahedral impostor. Compiled ahead of time to impostorbake.spv by
// tools/shaders/shaders.py and embedded in bake.cone. (The first pass, the colour
// and coverage, is render's own lit material, lit by ambient light alone.)
//
// The object is drawn at the identity, so its world and object space are one.
// It writes the NORMAL in object space (n * 0.5 + 0.5) and, in alpha, the DEPTH:
// the surface point's height above the plane through the object's centre
// perpendicular to the frame's direction, scaled so that the frame's sphere of
// radius R maps to 0..1 (0.5 on the plane): h = dot(p - centre, dir) / (2 R) + 0.5.
// impostor.slang reads it back as the surface's height. Nothing is lit and the
// exposure does not apply: the target is plain RGBA8, not sRGB.
//
// A part with a cut-out material (a leaf card) discards below its cut-off by its
// texture's alpha, as the lit material does, so the bake and the first pass agree.

import renderframe;

struct BakeParams {
    float4 centerRadius;           // xyz the centre, w the frame's radius R
    float4 direction;              // xyz the frame's direction (unit, towards the camera), w the cut-off (0: none)
    float4 base;                   // x the base colour's alpha
};

[[vk::binding(0, 1)]] ConstantBuffer<BakeParams> bake;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, input.normal);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    if (bake.direction.w > 0.0) {
        float a = bake.base.x * baseTexture.Sample(baseSampler, input.uv).a;
        if (a < bake.direction.w) {
            discard;
        }
    }
    float3 n = normalize(input.normal);
    float h = saturate(dot(input.world - bake.centerRadius.xyz, bake.direction.xyz) / (2.0 * bake.centerRadius.w) + 0.5);
    return float4(n * 0.5 + 0.5, h);
}
