// The render package's reflection probe capture (views.cone): one
// full-screen pass that writes a face of a probe's cube from the picture a
// right-handed camera drew of it. Compiled ahead of time to probe.spv by
// tools/shaders/shaders.py and embedded in views.cone.
//
// A cube map's faces are left-handed: seen from inside, a face's right
// times its down is minus the way it looks (brdf.slang's cubeDirection,
// Vulkan's and WebGPU's faces), where a camera's right times its down is
// the way it looks. So the camera's picture of a face is that face mirrored
// left to right, and this pass writes it back mirrored. (Rendering the
// mirror image instead would turn the triangles' winding round.)
//
// Group 0: the camera's picture (binding 0), read texel by texel.

[[vk::binding(0, 0)]] Texture2D picture;

struct FullscreenOutput {
    float4 position : SV_Position;
};

// One triangle covering the target
[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 mirrorCopyMain(FullscreenOutput input) : SV_Target {
    uint w, h;
    picture.GetDimensions(w, h);
    int2 p = int2(input.position.xy);
    return float4(picture.Load(int3(int(w) - 1 - p.x, p.y, 0)).rgb, 1.0);
}
