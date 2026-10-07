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
// Its alpha (the base color's times the texture's times the tint's) is used
// as the material's alpha mode says (material.cone): not at all; to cut out
// a fragment below the cut-off; or, see-through, as premultiplied alpha: the
// diffuse light (ambient and Lambert) times alpha, the highlight and the
// glow added whole, alpha written for the blend to keep 1 - alpha of what
// is behind.
//
// Group 0 is the frame's (renderframe.slang): the frame's uniforms and the
// instances, each copy's model matrix and a tint the color (and the
// emissive color) is multiplied by; group 1 is the material's.

import renderframe;
import skinning;

struct MaterialParams {
    float4 baseColor;              // 0: rgba, linear
    float4 specular;               // 16: rgb the specular color, w the shininess
    float4 emissive;               // 32: rgb radiance (nits in physical units)
    float4 alpha;                  // 48: the alpha mode (0 opaque, 1 cut out, 2 see-through), the cut-off
};

static const uint ALPHA_MASK = 1;
static const uint ALPHA_BLEND = 2;

[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;

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
    nointerpolation float4 tint : COLOR;
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
    output.tint = inst.tint;
    return output;
}

// The same for a skinned mesh (skinning.slang): its position and normal
// carried by the vertex's blended bones first, then placed by the copy's
// model matrix
[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    float4 world = mul(inst.model, float4(skinPoint(skin, input.position), 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, skinVector(skin, input.normal));
    output.uv = input.uv;
    output.tint = inst.tint;
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
    float4 base = material.baseColor * baseTexture.Sample(baseSampler, input.uv) * input.tint;
    uint mode = uint(material.alpha.x + 0.5);
    if (mode == ALPHA_MASK) {
        if (base.a < material.alpha.y) {
            discard;
        }
        base.a = 1.0;
    }
    float3 direct = frame.lightColor.rgb * (1.0 / RENDER_PI);
    float3 lit = base.rgb * (frame.ambient.rgb + direct * diffuse);
    float3 added = material.specular.rgb * direct * specular;
    float3 glow = material.emissive.rgb * input.tint.rgb;
    float3 color = lit + added + glow;
    if (mode == ALPHA_BLEND) {
        // Premultiplied: of the diffuse light only the share alpha covers
        color = lit * base.a + added + glow;
    }
    return float4(color * frame.exposure.x, base.a);
}
