// The render package's cut-out shadow casters for the standard lit material:
// a copy of a mesh whose material cuts out a fragment below its cut-off
// (ALPHA_MASK: leaves, fences), drawn into one cascade of the sun's shadow
// map, depth only. The alpha test is lit.slang's own: the base colour's
// alpha times the texture's times the copy's tint, so the shadow has the
// holes the surface has. Compiled ahead of time to shadowcutlit.spv by
// tools/shaders/shaders.py and embedded in shadow.cone. Group 1 is lit.slang's
// material group (the parameters, the texture, the sampler).

import renderframe;
import skinning;
import shadowparams;
import shadowcasting;

struct MaterialParams {
    float4 baseColor;              // 0: rgba, linear
    float4 specular;               // 16
    float4 emissive;               // 32
    float4 alpha;                  // 48: the alpha mode (0 opaque, 1 cut out, 2 see-through), the cut-off
};

static const uint ALPHA_MASK = 1;

[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material;
[[vk::binding(1, 1)]] Texture2D baseTexture;
[[vk::binding(2, 1)]] SamplerState baseSampler;

struct CutInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
    float2 uv : TEXCOORD;          // location 1, vertex buffer 1
};

struct SkinnedCutInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
    float2 uv : TEXCOORD;          // location 1, vertex buffer 1
    uint4 joints : BLENDINDICES;   // location 2, vertex buffer 2
    float4 weights : BLENDWEIGHT;  // location 3, vertex buffer 3
};

struct CutOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
    nointerpolation float tintAlpha : COLOR;
};

[shader("vertex")]
CutOutput vertexCut(CutInput input, uint id : SV_VulkanInstanceID) {
    CutOutput output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(input.position, 1.0));
    output.position = casterClip(world.xyz);
    output.uv = input.uv;
    output.tintAlpha = inst.tint.a;
    return output;
}

[shader("vertex")]
CutOutput vertexCutSkinned(SkinnedCutInput input, uint id : SV_VulkanInstanceID) {
    CutOutput output;
    Instance inst = instances[id];
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    float4 world = mul(inst.model, float4(skinPoint(skin, input.position), 1.0));
    output.position = casterClip(world.xyz);
    output.uv = input.uv;
    output.tintAlpha = inst.tint.a;
    return output;
}

[shader("fragment")]
void fragmentCut(CutOutput input) {
    float alpha = material.baseColor.a * baseTexture.Sample(baseSampler, input.uv).a * input.tintAlpha;
    if (uint(material.alpha.x + 0.5) == ALPHA_MASK && alpha < material.alpha.y) {
        discard;
    }
}
