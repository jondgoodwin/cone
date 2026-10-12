// The render package's velocity pass's common ground, a Slang module
// ('import velocitycommon;'): what taavelocity.slang, taavelocitylit.slang and
// taavelocitypbr.slang share. The velocity pass draws the objects the
// program says are MOVING (a copy with a model matrix or a pose of last
// frame's: fx.cone's MotionList) into a texture of the picture's size, after
// the scene, depth tested against the scene's own depth; each fragment
// writes by how much, in uv, the point it shows has moved since last frame
// (the point of the object, carried by last frame's matrix and pose, seen by
// last frame's camera, minus where it is now). Temporal anti-aliasing reads
// it where an object wrote it; elsewhere it finds the motion from the
// depth and the camera.
//
// Group 0 is the renderer's frame group, bound as the main pass binds it
// (renderframe.slang and skinning.slang: the JITTERED view-projection the
// scene was drawn with, the instances, this frame's bones). Group 1, for a
// cut-out material, is its material's. Group 2 is the pass's own:
// the frame uniform (0), and, one for each instance as 'instances' has them,
// last frame's model matrix (1), and the skinned ones' last bones (2) and
// their bases (3).
//
// The position drawn is the same expression the material shaders use, so the
// depth agrees with theirs to within the rasterizer's small bias (the
// pipelines add a little).

import renderframe;
import skinning;
import fxcommon;

[[vk::binding(0, 2)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 2)]] StructuredBuffer<float4x4> previousModels;
[[vk::binding(2, 2)]] StructuredBuffer<BoneXform> previousBones;
[[vk::binding(3, 2)]] StructuredBuffer<uint> previousBoneBases;

struct VelocityVertex {
    float4 position : SV_Position;
    float4 current : TEXCOORD1;       // the unjittered clip position now
    float4 previous : TEXCOORD2;      // and last frame's
    float2 uv : TEXCOORD;
    nointerpolation float tintAlpha : COLOR;
};

// The skinning matrix of last frame's pose, or, for a copy that had none,
// this frame's
BoneXform previousSkin(uint instance, uint4 j, float4 w, BoneXform now) {
    uint base = previousBoneBases[instance];
    if (base == NO_BONES) {
        return now;
    }
    return blendBones(previousBones[base + j.x], previousBones[base + j.y], previousBones[base + j.z],
        previousBones[base + j.w], w);
}

VelocityVertex velocityVertex(float3 position, float2 uv, uint id, BoneXform skin, BoneXform before) {
    VelocityVertex output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(skinPoint(skin, position), 1.0));
    float4 worldBefore = mul(previousModels[id], float4(skinPoint(before, position), 1.0));
    output.position = mul(frame.viewProjection, world);
    output.current = mul(fx.viewProjection, world);
    output.previous = mul(fx.previousViewProjection, worldBefore);
    output.uv = uv;
    output.tintAlpha = inst.tint.a;
    return output;
}

BoneXform identityBone() {
    BoneXform id;
    id.row0 = float4(1.0, 0.0, 0.0, 0.0);
    id.row1 = float4(0.0, 1.0, 0.0, 0.0);
    id.row2 = float4(0.0, 0.0, 1.0, 0.0);
    return id;
}

// The uv displacement to where the fragment's point was, and the marker
// alpha 1
float4 velocityOf(VelocityVertex input) {
    float2 c = input.current.xy / input.current.w;
    float2 p = input.previous.xy / input.previous.w;
    return float4((p.x - c.x) * 0.5, (c.y - p.y) * 0.5, 0.0, 1.0);
}
