// The render package's velocity pass for the standard lit material's cut-out
// meshes (ALPHA_MASK: leaves, fences), plain and skinned: the alpha test is
// lit.slang's own, so the velocity has the holes the surface has
// (velocitycommon.slang says what the pass is). Compiled ahead of time to
// taavelocitylit.spv by tools/shaders/shaders.py and embedded in taa.cone.
// Group 1 is lit.slang's material group.

import renderframe;
import skinning;
import fxcommon;
import velocitycommon;

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
    if (uint(material.alpha.x + 0.5) == ALPHA_MASK && alpha < material.alpha.y) {
        discard;
    }
    return velocityOf(input);
}
