// slangc: -fp-mode precise
//
// The render package's shadow casters for the physically based material,
// drawn into one cascade of the sun's shadow map, depth only: the cut-out
// ones (ALPHA_MASK, with pbr.slang's own alpha test: base colour alpha times
// the texture's times the copy's tint), skinned ones, and the ones that
// flutter (a leaf's shadow turns as the leaf does: flutter.slang, the module
// pbr.slang's vertexSway calls too). Compiled ahead of time to
// shadowcutpbr.spv by tools/shaders/shaders.py and embedded in shadow.cone.
// '-fp-mode precise' makes the flutter's arithmetic agree with pbr.slang's.
// Group 1 is pbr.slang's material group.

import renderframe;
import skinning;
import shadowparams;
import shadowcasting;
import flutter;

struct PbrParams {
    float4 baseColor;              // 0: rgba, linear
    float4 surface;                // 16
    float4 film;                   // 32
    float4 filmNoise;              // 48
    float4 wet;                    // 64
    float4 maps;                   // 80: z the alpha mode (0 opaque, 1 cut out, 2 see-through), w the cut-off
    float4 emissive;               // 96
    float4 foliage;                // 112: z the flutter's most turn (radians, 0: none)
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

// A mesh with a sway stream (location 2, vertex buffer 2: each vertex's
// offset from its leaf's pivot and the leaf's phase)
struct SwayCutInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD;
    float4 sway : COLOR;
};

// The same without uvs, for a flutter material that does not cut out
// (sway in location 1, vertex buffer 1)
struct SwayInput {
    float3 position : POSITION;
    float4 sway : COLOR;
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

[shader("vertex")]
CutOutput vertexCutSway(SwayCutInput input, uint id : SV_VulkanInstanceID) {
    CutOutput output;
    Instance inst = instances[id];
    float3 position = input.position;
    float3 normal = float3(0.0, 1.0, 0.0);
    flutterLeaf(input.sway.xyz, input.sway.w, material.foliage.z, frame.viewport.z, position, normal);
    float4 world = mul(inst.model, float4(position, 1.0));
    output.position = casterClip(world.xyz);
    output.uv = input.uv;
    output.tintAlpha = inst.tint.a;
    return output;
}

[shader("vertex")]
float4 vertexSway(SwayInput input, uint id : SV_VulkanInstanceID) : SV_Position {
    Instance inst = instances[id];
    float3 position = input.position;
    float3 normal = float3(0.0, 1.0, 0.0);
    flutterLeaf(input.sway.xyz, input.sway.w, material.foliage.z, frame.viewport.z, position, normal);
    float4 world = mul(inst.model, float4(position, 1.0));
    return casterClip(world.xyz);
}

[shader("fragment")]
void fragmentCut(CutOutput input) {
    float alpha = material.baseColor.a * baseTexture.Sample(baseSampler, input.uv).a * input.tintAlpha;
    if (uint(material.maps.z + 0.5) == ALPHA_MASK && alpha < material.maps.w) {
        discard;
    }
}

[shader("fragment")]
void fragmentOpaque() {
}
