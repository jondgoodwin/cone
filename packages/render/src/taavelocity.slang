// The render package's velocity pass for opaque meshes, plain and skinned
// (velocitycommon.slang says what the pass is). Compiled ahead of time to
// taavelocity.spv by tools/shaders/shaders.py and embedded in taa.cone. A
// static mesh reads one vertex buffer (positions), a skinned one two more (its
// joints and weights).

import renderframe;
import skinning;
import fxcommon;
import velocitycommon;

struct StaticInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
};

struct SkinnedInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
    uint4 joints : BLENDINDICES;   // location 1, vertex buffer 1
    float4 weights : BLENDWEIGHT;  // location 2, vertex buffer 2
};

// No uvs or tint to pass on: the opaque fragment needs only the positions
struct OpaqueVertex {
    float4 position : SV_Position;
    float4 current : TEXCOORD1;
    float4 previous : TEXCOORD2;
};

OpaqueVertex lite(VelocityVertex v) {
    OpaqueVertex o;
    o.position = v.position;
    o.current = v.current;
    o.previous = v.previous;
    return o;
}

[shader("vertex")]
OpaqueVertex vertexOpaque(StaticInput input, uint id : SV_VulkanInstanceID) {
    BoneXform none = identityBone();
    return lite(velocityVertex(input.position, float2(0.0, 0.0), id, none, none));
}

[shader("vertex")]
OpaqueVertex vertexOpaqueSkinned(SkinnedInput input, uint id : SV_VulkanInstanceID) {
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    BoneXform before = previousSkin(id, input.joints, input.weights, skin);
    return lite(velocityVertex(input.position, float2(0.0, 0.0), id, skin, before));
}

[shader("fragment")]
float4 fragmentOpaque(OpaqueVertex input) : SV_Target {
    float2 c = input.current.xy / input.current.w;
    float2 p = input.previous.xy / input.previous.w;
    return float4((p.x - c.x) * 0.5, (c.y - p.y) * 0.5, 0.0, 1.0);
}
