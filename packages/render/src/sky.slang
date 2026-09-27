// The render package's procedural sky, a Slang module ('import sky;'): a
// simple analytic dusk, a function of direction alone, in linear HDR
// radiance. It stands in for a physical atmosphere (Hillaire's, to come):
// the zenith fades into a horizon band, a glow gathers round the sun and
// lies along the horizon, and below the horizon the ground darkens out of a
// haze. The sun's disc is added only where asked: the environment the
// renderer prefilters leaves it out, since the directional light is the sun.
//
// SkyParams is laid out as render's Sky writes it into its uniform buffer.

struct SkyParams {
    float4 sunDirection;           // 0: xyz towards the sun, unit; w the cosine of the disc's radius
    float4 sunColor;               // 16: rgb the disc's radiance
    float4 zenithColor;            // 32: rgb straight up
    float4 horizonColor;           // 48: rgb at the horizon; w how tightly the band hugs it (an exponent)
    float4 glowColor;              // 64: rgb round the sun; w how tightly (an exponent)
    float4 groundColor;            // 80: rgb below the horizon; w how fast the haze gives way to it
};

float3 skyRadiance(float3 dir, SkyParams sky, bool withSun) {
    float3 d = normalize(dir);
    float up = saturate(d.y);
    float band = pow(1.0 - up, sky.horizonColor.w);
    float3 c = lerp(sky.zenithColor.rgb, sky.horizonColor.rgb, band);
    // The glow: towards the sun, strongest low down
    float cosSun = dot(d, sky.sunDirection.xyz);
    float glow = pow(saturate(cosSun * 0.5 + 0.5), sky.glowColor.w);
    c += sky.glowColor.rgb * (glow * (0.3 + 0.7 * band));
    // Below the horizon: the ground, out of the horizon's haze
    if (d.y < 0.0) {
        float g = saturate(-d.y * sky.groundColor.w);
        c = lerp(c, sky.groundColor.rgb, g);
    }
    if (withSun) {
        // The disc, its edge softened over a tenth of its radius
        float edge = sky.sunDirection.w;
        float soft = (1.0 - edge) * 0.1;
        c += sky.sunColor.rgb * smoothstep(edge - soft, edge + soft, cosSun);
    }
    return c;
}
