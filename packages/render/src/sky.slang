// The render package's procedural sky, a Slang module ('import sky;'): a
// simple analytic dusk, a function of direction alone, in linear HDR
// radiance. It stands in for a physical atmosphere (Hillaire's, to come):
// the zenith fades into a horizon band, a glow gathers round the sun and
// lies along the horizon, and below the horizon the ground darkens out of a
// haze. Two optional structures give a glossy surface something to reflect:
// a thin bright line along the horizon, and streaks of cloud (the noise
// package's fBm on a flat layer overhead, drawn out along one heading, lit
// more brightly towards the sun). The sun's disc is added only where asked:
// the environment the renderer prefilters leaves it out, since the
// directional light is the sun.
//
// SkyParams is laid out as render's Sky writes it into its uniform buffer.
// A shader importing this module imports noise, so it carries
// '// slangc: -fp-mode precise' (noise's README, "Determinism").

import noise;

struct SkyParams {
    float4 sunDirection;           // 0: xyz towards the sun, unit; w the cosine of the disc's radius
    float4 sunColor;               // 16: rgb the disc's radiance
    float4 zenithColor;            // 32: rgb straight up
    float4 horizonColor;           // 48: rgb at the horizon; w how tightly the band hugs it (an exponent)
    float4 glowColor;              // 64: rgb round the sun; w how tightly (an exponent)
    float4 groundColor;            // 80: rgb below the horizon; w how fast the haze gives way to it
    float4 lineColor;              // 96: rgb the horizon's line; w its half-width (in the sine of elevation), 0: none
    float4 cloudColor;             // 112: rgb the streaks' radiance; w their cover (0: none)
    float4 cloudShape;             // 128: x their scale, y their stretch, z their heading (radians), w the seed
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
    // The streaks of cloud: fBm on a layer overhead, read where the
    // direction meets it, drawn out along the heading; fading out towards
    // the horizon, where the layer's features would crowd into shimmer
    if (sky.cloudColor.w > 0.0 && d.y > 0.0) {
        float2 onLayer = d.xz / (d.y + 0.12);
        float ch = cos(sky.cloudShape.z);
        float sh = sin(sky.cloudShape.z);
        float2 along = float2(onLayer.x * ch + onLayer.y * sh, onLayer.y * ch - onLayer.x * sh);
        float2 q = float2(along.x / sky.cloudShape.y, along.y) * sky.cloudShape.x;
        float n = fbm2(q, uint(sky.cloudShape.w), 5, 2.0, 0.5).value;
        float cover = smoothstep(1.0 - sky.cloudColor.w, 1.0, n * 0.5 + 0.5);
        float fade = smoothstep(0.0, 0.18, d.y);
        float lit = 0.6 + 1.4 * glow;
        c = lerp(c, sky.cloudColor.rgb * lit, cover * fade);
    }
    // The line along the horizon, brightest towards the sun
    if (sky.lineColor.w > 0.0) {
        float x = d.y / sky.lineColor.w;
        c += sky.lineColor.rgb * (exp(-x * x) * (0.5 + glow));
    }
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
