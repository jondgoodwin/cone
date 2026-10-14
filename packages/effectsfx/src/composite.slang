// The effectsfx package's low-resolution layer composite: a layer drawn into a texture of half the
// frame's size (smoke and flipbooks, whose cost is the fragments they blend), laid over the frame
// with one full-screen pass, bilinearly filtered, premultiplied (ONE, ONE_MINUS_SRC_ALPHA).
//
// The layer's own passes did their depth test per half-resolution pixel against the full-resolution
// depth, so a surface in front hides what is behind it; what half resolution cannot do is respect an
// edge finer than its own pixel, so a very thin object in front of a puff shows a one-pixel fringe
// of it (the cost named in Design\effectsfx.md).

[[vk::binding(0, 0)]] Texture2D<float4> layer;
[[vk::binding(1, 0)]] SamplerState layerSampler;

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 uv;
};

[shader("vertex")]
FullscreenOutput fullscreenVertex(uint vid : SV_VulkanVertexID) {
    FullscreenOutput o;
    float2 p = float2((vid << 1u) & 2u, vid & 2u);
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    // (clip y up, as WebGPU's: the top of the frame is v = 0)
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

[shader("fragment")]
float4 compositeFragment(FullscreenOutput input) : SV_Target {
    return layer.SampleLevel(layerSampler, input.uv, 0.0);
}
