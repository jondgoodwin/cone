// The render package's materials that show a view (views.cone): a custom
// pipeline (custom.cone) each, compiled ahead of time to viewmat.spv by
// tools/shaders/shaders.py and embedded in views.cone.
//
// - screenFragment: a view's picture as light given off, on a quad (a
//   security-camera screen, a minimap, a portal's opening): the view's
//   colour texture read at the quad's uv (v up, as the meshes' are) times
//   the copy's tint. The texture holds exposed light already, so nothing
//   multiplies it by the exposure again.
// - mirrorFragment: a planar mirror or reflective water. The reflection
//   view (a Camera.reflectedAbout camera's picture, drawn the size of the
//   frame) is read at the fragment's own screen position, with x taken as
//   1 - u (the camera's picture is mirrored left to right; camera.cone).
//   With 'fresnel' 0 the mirror is perfect: the reflection times the tint.
//   With 'fresnel' 1 it is water: the body colour (tint, lit by nothing:
//   a flat radiance) and the reflection are blended by Schlick's Fresnel
//   term at the angle of the view to the surface normal (f0 0.02, water's),
//   and a normal map, scrolling with time, shifts the screen position the
//   reflection is read at by 'distortion' times its xy (the common
//   screen-space refraction/reflection offset; no refraction is drawn).
//
// Group 1: the parameters (binding 0), the view's texture (1), a normal map
// (2; the white texture if none, ignored when 'flow'.w is 0) and the sampler
// (3).

import renderframe;

struct ViewParams {
    float4 tint;                   // 0: rgb the colour multiplying the picture (screen, mirror) or the water's body colour
    float4 water;                  // 16: x 1 for water (Fresnel), 0 for a perfect mirror; y f0; z distortion; w the normal map's tiling
    float4 flow;                   // 32: xy the normal map's scroll a second; w 1 if a normal map is used
    float4 spare;                  // 48
};

[[vk::binding(0, 1)]] ConstantBuffer<ViewParams> params;
[[vk::binding(1, 1)]] Texture2D viewTexture;
[[vk::binding(2, 1)]] Texture2D normalMap;
[[vk::binding(3, 1)]] SamplerState viewSampler;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    nointerpolation float4 tint : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4 world = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = worldNormal(inst.model, input.normal);
    output.uv = input.uv;
    output.tint = inst.tint;
    return output;
}

[shader("fragment")]
float4 screenFragment(VertexOutput input) : SV_Target {
    // The picture's first row is its top, the meshes' v runs up
    float4 c = viewTexture.Sample(viewSampler, float2(input.uv.x, 1.0 - input.uv.y));
    return float4(c.rgb * params.tint.rgb * input.tint.rgb, 1.0);
}

[shader("fragment")]
float4 mirrorFragment(VertexOutput input) : SV_Target {
    float2 uv = input.position.xy / frame.viewport.xy;
    if (params.flow.w > 0.5) {
        float2 scroll = params.flow.xy * frame.viewport.z;
        float3 nm = normalMap.Sample(viewSampler, input.uv * params.water.w + scroll).xyz * 2.0 - 1.0;
        // The picture is mirrored in x, so the offset is too
        uv += float2(-nm.x, nm.y) * params.water.z;
    }
    uv = saturate(float2(1.0 - uv.x, uv.y));
    float3 reflection = viewTexture.Sample(viewSampler, uv).rgb;
    float3 color = reflection * params.tint.rgb * input.tint.rgb;
    if (params.water.x > 0.5) {
        float3 n = normalize(input.normal);
        float3 v = normalize(frame.cameraPosition.xyz - input.world);
        float nov = saturate(abs(dot(n, v)));
        float f = params.water.y + (1.0 - params.water.y) * pow(1.0 - nov, 5.0);
        float3 body = params.tint.rgb * input.tint.rgb * frame.exposure.x;
        color = lerp(body, reflection, f);
    }
    return float4(color, 1.0);
}
