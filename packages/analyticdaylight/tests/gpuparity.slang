// The sky's parity test shader: each pixel of a 64-wide target is the sky
// (daySkyRadiance, analyticdaylight.slang) in one direction, the pixel's
// column choosing it, times a scale the CPU picked so the result fits a
// half-float pixel; gpuparity.cone computes the same with the Cone package
// (DaySky.radiance) and compares. The direction is the 'direction' function
// here and in the Cone test, the same arithmetic.

import analyticdaylight;

struct Params {
    DaySkyParams sky;
    float4 extra;        // x the scale
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] Texture2D<float4> unusedTexture;
[[vk::binding(2, 1)]] SamplerState unused;

struct VertexInput {
    float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    output.position = float4(input.position.x, input.position.z, 0.5, 1.0);
    return output;
}

// Direction 'i' (0 to 63): heights from a little below the horizon to near
// the zenith, bearings a golden angle apart
float3 direction(uint i) {
    float f = frac(float(i) * 0.6180339 + 0.07);
    float el = asin(clamp(-0.15 + 1.12 * f, -0.99, 0.99));
    float az = float(i) * 2.3999632;
    return float3(cos(el) * sin(az), sin(el), cos(el) * cos(az));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint i = uint(input.position.x);
    float3 c = daySkyRadiance(direction(i), params.sky) * params.extra.x;
    return float4(c, 1.0);
}
