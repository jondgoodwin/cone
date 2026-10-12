// The render package's shadow parameters, a Slang module ('import
// shadowparams;'): what the sun's cascaded shadow map needs, written once a
// render by shadow.cone (ShadowParams there is the CPU's twin; the layout
// is this one). Both the shadow-casting shaders (shadowcast.slang and the
// two cut-out ones) and the lit and physically based materials
// (shadowing.slang) read it, from the frame group (group 0), binding 4.

struct ShadowParams {
    float4x4 cascade[4];           // 0: world coordinates to the cascade's light clip space: x, y from -1 to 1
                                   //    across the map, depth z from 0 (nearest the light) to 1
    float4 splitFar;               // 256: the view depth (along the camera's forward) at which each cascade ends
    float4 texelWorld;             // 272: the world length of one texel of each cascade
    float4 depthRange;             // 288: the world length of each cascade's depth 0 to 1
    float4 bandLen;                // 304: the length of the band at the end of each cascade across which it blends
                                   //      into the next (the last: fades to unshadowed)
    float4 config;                 // 320: x the cascades' count (0: no shadows), y the map's size in texels, z 0, w the filter's radius in texels
    float4 bias;                   // 336: x the constant depth bias (texels), y the normal offset (texels), z the filter's taps, w 0
    float4 forward;                // 352: xyz the camera's forward, unit length
    // The point and spot lights' shadow views (lightshadow.cone, localshadow.slang):
    float4 localConfig;            // 368: x the view's side in texels, y the filter's radius in texels, z the constant bias (texels), w the normal offset (texels)
    float4 localMore;              // 384: x the filter's taps
    float4x4 localView[24];        // 400: world coordinates to the view's clip space (a perspective view from the light), as a cascade's; a
                                   //      caster draws view i as the immediate 4 + i
};

[[vk::binding(4, 0)]] ConstantBuffer<ShadowParams> shadowParams;
