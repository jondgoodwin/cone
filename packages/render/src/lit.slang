// The render package's standard lit material: Lambert diffuse and
// Blinn-Phong specular from one directional light, plus ambient light, over
// a base color times a texture (a 1 x 1 white one when the material has
// none), plus an emissive color. Compiled ahead of time to lit.spv by
// tools/shaders/shaders.py and embedded in material.cone. glTF's
// metallic-roughness PBR is its successor.
//
// It keeps the renderer's one lighting convention (renderframe.slang): the
// light's color is irradiance, so diffuse is the base color / pi times it
// times the cosine; the Blinn-Phong lobe (not energy conserving: an
// artist's highlight) is divided by pi alike; the ambient light is radiance,
// times the base color; the emissive color is radiance, added. All of it
// times the exposure.
//
// Group 0 is the frame's (renderframe.slang), group 1 the material's; the
// immediates (push constants) are the draw's: its model matrix, and a tint
// the color (and the emissive color) is multiplied by.

import renderframe;

struct MaterialParams {
    float4 baseColor;              // 0: rgba, linear
    float4 specular;               // 16: rgb the specular color, w the shininess
    float4 emissive;               // 32: rgb radiance (nits in physical units)
};

struct Immediates {
    float4x4 model;                // 0-63: the draw's model coordinates to the world's
    float4 tint;                   // 64-79
};

[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;
[[vk::push_constant]] ConstantBuffer<Immediates> draw;

struct VertexInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
    float3 normal : NORMAL;        // location 1, vertex buffer 1
    float2 uv : TEXCOORD;          // location 2, vertex buffer 2
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 world = mul(draw.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(draw.model, input.normal);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 n = normalize(input.normal);
    float3 l = frame.lightDirection.xyz;
    float3 v = normalize(frame.cameraPosition.xyz - input.world);
    float diffuse = max(dot(n, l), 0.0);
    float specular = 0.0;
    if (diffuse > 0.0) {
        float3 h = normalize(l + v);
        specular = pow(max(dot(n, h), 0.0), material.specular.w);
    }
    float4 base = material.baseColor * baseTexture.Sample(baseSampler, input.uv) * draw.tint;
    float3 direct = frame.lightColor.rgb * (1.0 / RENDER_PI);
    float3 color = base.rgb * (frame.ambient.rgb + direct * diffuse) + material.specular.rgb * direct * specular
        + material.emissive.rgb * draw.tint.rgb;
    return float4(color * frame.exposure.x, base.a);
}
