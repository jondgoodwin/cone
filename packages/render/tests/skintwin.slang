// render's skinning test's compute twin (skinning.cone): each vertex of a skinned mesh
// carried by its blended bones with skinning.slang's own functions, the ones
// the skinned vertex stages call, its position written out for the test to
// compare with the CPU's (Skin.skinPoint). The joints and weights are read
// from their bytes as the vertex fetch reads them (the weights as unorm8,
// a byte over 255). Compiled by tools/shaders/shaders.py and embedded in
// skinning.cone.

import skinning;

struct Immediates {
    uint count;                    // how many vertices
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

// Bindings from 4: skinning.slang declares 2 and 3 of group 0 for the
// renderer's bones and bone bases, which this does not read
[[vk::binding(4, 0)]] StructuredBuffer<float> positions;      // three a vertex
[[vk::binding(5, 0)]] StructuredBuffer<uint> joints;          // four bytes a vertex
[[vk::binding(6, 0)]] StructuredBuffer<uint> weights;         // four bytes a vertex
[[vk::binding(7, 0)]] StructuredBuffer<BoneXform> pose;       // the skinning matrices
[[vk::binding(8, 0)]] RWStructuredBuffer<float> skinned;      // three a vertex

uint4 bytesOf(uint v) {
    return uint4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, (v >> 24) & 0xFFu);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void skinMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= immediates.count) {
        return;
    }
    uint4 j = bytesOf(joints[i]);
    float4 w = float4(bytesOf(weights[i])) / 255.0;
    BoneXform s = blendBones(pose[j.x], pose[j.y], pose[j.z], pose[j.w], w);
    float3 p = skinPoint(s, float3(positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]));
    skinned[3 * i] = p.x;
    skinned[3 * i + 1] = p.y;
    skinned[3 * i + 2] = p.z;
}
