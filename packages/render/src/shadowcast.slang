// The render package's opaque shadow casters: a copy of a mesh drawn into
// one cascade of the sun's shadow map, depth only (shadowcasting.slang).
// Compiled ahead of time to shadowcast.spv by tools/shaders/shaders.py and
// embedded in shadow.cone. Position only: a static mesh reads one vertex
// buffer, a skinned mesh two more (its joints and weights), blended by the
// copy's bones as the lit and physically based materials blend them
// (skinning.slang), so a posed character casts its pose.

import renderframe;
import skinning;
import shadowparams;
import shadowcasting;

struct StaticInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
};

struct SkinnedInput {
    float3 position : POSITION;    // location 0, vertex buffer 0
    uint4 joints : BLENDINDICES;   // location 1, vertex buffer 1
    float4 weights : BLENDWEIGHT;  // location 2, vertex buffer 2
};

[shader("vertex")]
float4 vertexOpaque(StaticInput input, uint id : SV_VulkanInstanceID) : SV_Position {
    float4 world = mul(instances[id].model, float4(input.position, 1.0));
    return casterClip(world.xyz);
}

[shader("vertex")]
float4 vertexOpaqueSkinned(SkinnedInput input, uint id : SV_VulkanInstanceID) : SV_Position {
    BoneXform skin = vertexSkin(id, input.joints, input.weights);
    float4 world = mul(instances[id].model, float4(skinPoint(skin, input.position), 1.0));
    return casterClip(world.xyz);
}

// Nothing to write: the depth is the result
[shader("fragment")]
void fragmentOpaque() {
}
