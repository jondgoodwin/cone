// The render package's standard lit material: Lambert diffuse and
// Blinn-Phong specular from one directional light, plus ambient light, over
// a base color times a texture (a 1 x 1 white one when the material has
// none). Compiled ahead of time to lit.spv by tools/shaders/shaders.py and
// embedded in material.cone. glTF's metallic-roughness PBR is its successor.
//
// Group 0 is the frame's (the camera and the light), group 1 the
// material's; the immediates (push constants) are the draw's: its model
// matrix, and a tint the color is multiplied by.

struct Frame {
    float4x4 viewProjection;       // bytes 0-63
    float4 cameraPosition;         // 64: xyz, the camera in world coordinates
    float4 lightDirection;         // 80: xyz, towards the light, unit length
    float4 lightColor;             // 96: rgb
    float4 ambient;                // 112: rgb
};

struct MaterialParams {
    float4 baseColor;              // 0: rgba, linear
    float4 specular;               // 16: rgb the specular color, w the shininess
};

struct Immediates {
    float4x4 model;                // 0-63: the draw's model coordinates to the world's
    float4 tint;                   // 64-79
};

[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;
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
    // The normal by the cofactor matrix of the model's upper 3 x 3 (its
    // columns a0, a1, a2): the inverse transpose times the determinant, so
    // right under non-uniform scale, and normalized in the fragment stage
    float3 a0 = float3(draw.model[0][0], draw.model[1][0], draw.model[2][0]);
    float3 a1 = float3(draw.model[0][1], draw.model[1][1], draw.model[2][1]);
    float3 a2 = float3(draw.model[0][2], draw.model[1][2], draw.model[2][2]);
    output.normal = cross(a1, a2) * input.normal.x + cross(a2, a0) * input.normal.y
        + cross(a0, a1) * input.normal.z;
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
    float3 color = base.rgb * (frame.ambient.rgb + frame.lightColor.rgb * diffuse)
        + material.specular.rgb * frame.lightColor.rgb * specular;
    return float4(color, base.a);
}
