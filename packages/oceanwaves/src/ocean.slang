// The ocean's shader, a custom pipeline of render (render's custom.cone has the contract): one mesh of
// vertices at rest on a grid (x and z only), moved by the displacement the FFT cascades hold, and
// shaded per fragment from their slopes and foam.
//
// The cascades' fields are r32float textures 3 N wide and N high: cascade c occupies columns c N to
// c N + N - 1. The texture of each field (height, horizontal displacement x and z, slope x and z, foam)
// is read texel by texel (not every GPU filters r32float) and blended by hand, wrapping at the tile's
// edge. A vertex's REST position (x0, z0), its place before the waves moved it, is what the textures are
// indexed by; the vertex's world position is x0 + lambda D(x0), height h(x0). The fragment gets the rest
// position interpolated, so it reads the slopes and the foam where the water that is there came from.
//
// SHADING: the sea is opaque. Its colour is Fresnel's mix (Schlick's approximation, F0 = 0.02 for water)
// of the sky and the sun reflected in the wave's normal, and the body of the water: a deep blue-green
// that scatters the sun, brighter and greener toward the crests where light shines through the thin
// water (a cheap subsurface term, after Jensen and Golias in GPU Gems 1, ch. 1: the sun behind the
// wave seen through its crest), and FOAM, white and rough, where the Jacobian says the surface folded.
// Detail fades with distance (the smallest cascades first) so that a wave a pixel wide does not shimmer,
// and the horizon fogs into the sky's colour.

import renderframe;

struct Params {
    float4 tile;           // x, y, z: the cascades' tile lengths (m); w: samples a side (N)
    float4 sunDir;         // xyz: toward the sun, unit
    float4 sunColor;       // rgb: the sun's glint (display units)
    float4 skyZenith;      // rgb: the sky overhead (display units)
    float4 skyHorizon;     // rgb: along the horizon
    float4 deepColor;      // rgb: the body of the water
    float4 shallowColor;   // rgb: the water through a crest
    float4 wave;           // x: choppiness; y: significant wave height (m); z, w: displacement fades out between these distances (m)
    float4 foam;           // x: foam brightness; y: fog density (1 / m); z: spacing of the mesh (m); w: 0
    float4 lod;            // x, y, z: how much each cascade displaces the vertices (0 .. 1); w: 0
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] Texture2D<float> heightField;
[[vk::binding(2, 1)]] Texture2D<float> dispXField;
[[vk::binding(3, 1)]] Texture2D<float> dispZField;
[[vk::binding(4, 1)]] Texture2D<float> slopeXField;
[[vk::binding(5, 1)]] Texture2D<float> slopeZField;
[[vk::binding(6, 1)]] Texture2D<float> foamField;
[[vk::binding(7, 1)]] SamplerState unused;

struct VertexInput {
    float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float2 rest : TEXCOORD1;
    float height : TEXCOORD2;
    float fade : TEXCOORD3;
};

float fetchTexel(Texture2D<float> t, int c, int n, int ix, int iz) {
    return t.Load(int3((ix & (n - 1)) + c * n, iz & (n - 1), 0));
}

// The field's value for cascade c at the rest position (metres), bilinear with wrap
float sampleField(Texture2D<float> t, int c, float2 rest) {
    float n = params.tile.w;
    float len = params.tile[c];
    float2 g = rest / len * n;
    float2 f = floor(g);
    float2 fr = g - f;
    int ix = int(f.x);
    int iz = int(f.y);
    int ni = int(n);
    float a = fetchTexel(t, c, ni, ix, iz);
    float b = fetchTexel(t, c, ni, ix + 1, iz);
    float cc = fetchTexel(t, c, ni, ix, iz + 1);
    float d = fetchTexel(t, c, ni, ix + 1, iz + 1);
    return lerp(lerp(a, b, fr.x), lerp(cc, d, fr.x), fr.y);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4 rest4 = mul(inst.model, float4(input.position, 1.0));
    float2 rest = rest4.xz;
    float dist = length(rest - frame.cameraPosition.xz);
    float fade = 1.0 - smoothstep(params.wave.z, params.wave.w, dist);
    float3 d = float3(0.0, 0.0, 0.0);
    for (int c = 0; c < 3; c++) {
        float w = params.lod[c] * fade;
        if (w > 0.0) {
            d.x += w * sampleField(dispXField, c, rest);
            d.z += w * sampleField(dispZField, c, rest);
            d.y += w * sampleField(heightField, c, rest);
        }
    }
    float3 world = float3(rest.x + params.wave.x * d.x, rest4.y + d.y, rest.y + params.wave.x * d.z);
    output.position = mul(frame.viewProjection, float4(world, 1.0));
    output.world = world;
    output.rest = rest;
    output.height = d.y;
    output.fade = fade;
    return output;
}

float3 skyColor(float3 dir) {
    float up = saturate(dir.y);
    float3 s = lerp(params.skyHorizon.rgb, params.skyZenith.rgb, pow(up, 0.45));
    return s;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 rest = input.rest;
    float3 toCam = frame.cameraPosition.xyz - input.world;
    float dist = length(toCam);
    float3 V = toCam / max(dist, 0.001);
    float3 L = normalize(params.sunDir.xyz);

    // How much of each cascade's detail survives at this distance
    float w0 = 1.0;
    float w1 = 1.0 - smoothstep(350.0, 1100.0, dist);
    float w2 = 1.0 - smoothstep(70.0, 320.0, dist);
    float sx = w0 * sampleField(slopeXField, 0, rest) + w1 * sampleField(slopeXField, 1, rest) + w2 * sampleField(slopeXField, 2, rest);
    float sz = w0 * sampleField(slopeZField, 0, rest) + w1 * sampleField(slopeZField, 1, rest) + w2 * sampleField(slopeZField, 2, rest);
    // The foam is made on the second cascade's grid from the whole surface's folding (ocean.cone)
    float foamAmount = sampleField(foamField, 1, rest);
    // broken up by the finest cascade's slopes, so a patch has the texture of froth, not of a blur
    float breakup = saturate(length(float2(sampleField(slopeXField, 2, rest), sampleField(slopeZField, 2, rest))) * 7.0 + 0.15);
    foamAmount = saturate(foamAmount * 1.3) * saturate(0.25 + breakup * 1.3);
    float3 N = normalize(float3(-sx, 1.0, -sz));

    float NdotV = max(dot(N, V), 0.0);
    float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    float3 R = reflect(-V, N);
    R.y = max(R.y, 0.02);
    R = normalize(R);
    float3 reflected = skyColor(R);
    float glint = pow(saturate(dot(R, L)), 700.0);
    reflected += params.sunColor.rgb * glint * 6.0;

    // The body of the water: lit by the sun at its angle, and by the sky; brighter and greener where a
    // crest lets the sun through toward the eye
    float sunUp = saturate(L.y);
    float3 body = params.deepColor.rgb * (0.30 + 0.70 * sunUp * saturate(dot(N, L) * 0.5 + 0.5));
    float crest = saturate(input.height / max(params.wave.y, 0.05) * 1.4 + 0.25) * input.fade;
    float through = pow(saturate(dot(V, -L + N * 0.25)), 4.0);
    body += params.shallowColor.rgb * crest * through * sunUp * 2.2 + params.shallowColor.rgb * 0.06 * crest;

    float3 color = body * (1.0 - fresnel) + reflected * fresnel;

    // Foam: white, rough, lit by the sun and the sky, hiding the reflection and the body
    float f = saturate(foamAmount * params.foam.x);
    float3 foamColor = float3(0.9, 0.93, 0.95) * (0.28 + 0.72 * sunUp) * (0.5 + 0.5 * saturate(dot(N, L)));
    color = lerp(color, foamColor, f * 0.9);

    // Aerial perspective: the far sea takes the horizon's colour
    float fog = 1.0 - exp(-dist * params.foam.y);
    color = lerp(color, params.skyHorizon.rgb, saturate(fog));
    return float4(color * frame.exposure.x, 1.0);
}
