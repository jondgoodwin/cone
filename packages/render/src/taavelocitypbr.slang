// The render package's velocity pass for the physically based material's
// cut-out meshes (ALPHA_MASK), plain and skinned, with pbr.slang's own alpha
// test (base colour alpha times the texture's times the copy's tint)
// (velocitycommon.slang says what the pass is). A flutter material's leaves
// are not moved by this pass (their turn is a few degrees, which the
// temporal filter's clipping absorbs). Compiled ahead of time to
// taavelocitypbr.spv by tools/shaders/shaders.py and embedded in taa.cone.
// Group 1 is pbr.slang's material group.

import renderframe;
import skinning;
import fxcommon;
import velocitycommon;

struct PbrParams {
    float4 baseColor;              // 0: rgba, linear
    float4 surface;                // 16
    float4 film;                   // 32
    float4 filmNoise;              // 48
    float4 wet;                    // 64
    float4 maps;                   // 80: z the alpha mode (0 opaque, 1 cut out, 2 see-through), w the cut-off
    float4 emissive;               // 96
    float4 foliage;                // 112
};

static const uint ALPHA_MASK = 1;

[[vk::binding(0, 1)]] ConstantBuffer<PbrParams> material;
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

[shader("vertex")]
VelocityVertex vertexCut(CutInput input, uint id : SV_VulkanInstanceID) {
    BoneXform none = identityBone();
    return velocityVertex(input.position, input.uv, id, none, none);
}

[shader("vertex")]
VelocityVertex vertexCutSkinned(SkinnedCutInput input, uint id : SV_VulkanInstanceID) {
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    BoneXform before = previousSkin(id, input.joints, input.weights, skin);
    return velocityVertex(input.position, input.uv, id, skin, before);
}

[shader("fragment")]
float4 fragmentCut(VelocityVertex input) : SV_Target {
    float alpha = material.baseColor.a * baseTexture.Sample(baseSampler, input.uv).a * input.tintAlpha;
    if (uint(material.maps.z + 0.5) == ALPHA_MASK && alpha < material.maps.w) {
        discard;
    }
    return velocityOf(input);
}
