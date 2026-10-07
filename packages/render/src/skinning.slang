// The render package's skinning, a Slang module ('import skinning;'):
// linear blend skinning, read by the skinned vertex stages of lit.slang and
// pbr.slang.
//
// A SKINNED MESH carries two more vertex streams beside its positions,
// normals and uvs: each vertex's four joints (bone numbers, a byte each,
// location 3) and their four weights (bytes read as 0 to 1, location 4,
// summing to 1: render.cone's Skin makes them sum to 255).
//
// THE BONES. A skinned copy's bones are BoneXforms in one storage buffer the
// renderer writes once a render (group 0, binding 2), the copy's own run of
// them starting at its 'boneBase', which the renderer writes for every
// instance into a second storage buffer (binding 3), indexed by the instance
// as 'instances' is. A BoneXform is the bone's SKINNING MATRIX: it carries a
// point of the mesh as it was modelled (the bind pose) to where the posed
// bone takes it, in the model's own coordinates (the bone's posed transform
// times the inverse of its bind transform); the copy's model matrix then
// places the model in the world, as for any mesh. It is three rows of a 3 x 4
// affine matrix (the last row 0 0 0 1 left out), 48 bytes, and opaque to
// the stages that use it: dual quaternions would change this module only.
//
// THE NORMAL is carried by the blended matrix's 3 x 3, unnormalized: right
// while the bones are rigid (a rotation and a translation, no scale), which
// is what a skeleton's pose is; the fragment stage normalizes.

struct BoneXform {
    float4 row0;
    float4 row1;
    float4 row2;
};

[[vk::binding(2, 0)]] StructuredBuffer<BoneXform> bones;
[[vk::binding(3, 0)]] StructuredBuffer<uint> boneBases;

// A copy drawn with no bones of its own: its mesh drawn as modelled
static const uint NO_BONES = 0xFFFFFFFFu;

// The four bones 'b0' to 'b3' blended by the weights 'w' (linear blend
// skinning: the matrices summed, each times its weight)
BoneXform blendBones(BoneXform b0, BoneXform b1, BoneXform b2, BoneXform b3, float4 w) {
    BoneXform s;
    s.row0 = b0.row0 * w.x + b1.row0 * w.y + b2.row0 * w.z + b3.row0 * w.w;
    s.row1 = b0.row1 * w.x + b1.row1 * w.y + b2.row1 * w.z + b3.row1 * w.w;
    s.row2 = b0.row2 * w.x + b1.row2 * w.y + b2.row2 * w.z + b3.row2 * w.w;
    return s;
}

// The point 'p' carried by 's'
float3 skinPoint(BoneXform s, float3 p) {
    float4 q = float4(p, 1.0);
    return float3(dot(s.row0, q), dot(s.row1, q), dot(s.row2, q));
}

// The direction 'v' carried by 's' (turned, not moved)
float3 skinVector(BoneXform s, float3 v) {
    return float3(dot(s.row0.xyz, v), dot(s.row1.xyz, v), dot(s.row2.xyz, v));
}

// The blended skinning matrix of a vertex of joints 'j' and weights 'w',
// drawn as instance 'instance': its copy's bones from its bone base, or the
// identity if the copy has none
BoneXform vertexSkin(uint instance, uint4 j, float4 w) {
    uint base = boneBases[instance];
    if (base == NO_BONES) {
        BoneXform id;
        id.row0 = float4(1.0, 0.0, 0.0, 0.0);
        id.row1 = float4(0.0, 1.0, 0.0, 0.0);
        id.row2 = float4(0.0, 0.0, 1.0, 0.0);
        return id;
    }
    return blendBones(bones[base + j.x], bones[base + j.y], bones[base + j.z], bones[base + j.w], w);
}

// What a skinned mesh's vertex stage takes: the three streams every mesh
// has (locations 0 to 2, vertex buffers 0 to 2), then the joints and weights
// (locations 3 and 4, vertex buffers 3 and 4)
struct SkinnedVertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};
