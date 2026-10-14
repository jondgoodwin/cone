// The cloth shader of garmentdrape: render's custom pipeline (render's custom.cone has the contract) that draws a
// garment's woven cloth, with a velvet-like sheen, wear and dirt. Compiled ahead of time to cloth.spv by
// tools/shaders/shaders.py and embedded in cloth.cone.
//
// WHAT IT DRAWS. The garment is a dynamic mesh whose uvs are metres on the PATTERN SHEET (the flat panels laid out on one
// sheet: drape.cone's atlas), divided by the sheet's size. So the uv scaled back by the sheet's size in metres is a
// point of the flat cloth in metres, the same on every panel, and the weave is drawn from it at its real size: threads
// per metre in `weave.x`. The sheet's own texture (cloth.cone paints it from each panel's shape) holds what a rule on the
// panels says: red the dirt, green the wear, blue the nearness of a seam.
//
//  - WEAVE. Threads cross in a plain, twill or satin order (`weave.z`): in each cell of the thread grid one of the two
//    threads is on top, which sets its colour (the warp's or the weft's), and each thread is a rounded bar, so the
//    cloth's height is known in closed form and its normal is the height's slope, bent in the surface's tangent frame
//    (Schuler's cotangent frame from the screen derivatives, "Followup: Normal Mapping Without Precomputed Tangents",
//    2006). The bumps and the colour variety between threads fade out when a thread is under a pixel wide, so a
//    distant garment is a flat colour and does not shimmer.
//  - SHEEN. A cloth that is fibres standing up scatters light back toward the viewer at grazing angles, where a hard
//    surface does not: the Charlie distribution (Estevez and Kulla, "Production Friendly Microfacet Sheen BRDF", SIGGRAPH
//    2017) with the Neubelt visibility term, in `sheen.rgb` times `sheen.a`; its roughness is `fuzz.x`. The sky's share
//    of the sheen is a fresnel-like rim on the ambient light. This stands in for the sheen lobe of render's PBR material,
//    which materialglass adds, and goes when that is on master.
//  - WEAR is faded cloth: lighter, greyer, its sheen gone and its threads uneven (more where `wear` is high).
//  - DIRT is the soil colour multiplied in, and takes the sheen away.
//  - LIGHT. The frame's one directional light and ambient, by the renderer's convention (the light's colour an
//    irradiance, the ambient a radiance, everything times the exposure). There are NO SHADOWS here: a custom pipeline has
//    no access to the shadow maps.

import renderframe;

struct Params {
    float4 warp;        // rgb: the warp thread's colour (linear)
    float4 weft;        // rgb: the weft thread's
    float4 sheen;       // rgb: the sheen's colour, a: its strength (0: none)
    float4 soil;        // rgb: the dirt's colour
    float4 weave;       // x: threads a metre; y: bump (0 flat .. 1); z: weave (0 plain, 1 twill, 2 satin); w: thread variety (0 .. 1)
    float4 fuzz;        // x: sheen roughness (0.2 .. 1); y: how much worn cloth loses its sheen; z, w: the pattern sheet's size (m)
    float4 amounts;     // x: wear gain; y: dirt gain; z: seed; w: unused
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] Texture2D<float4> sheet;
[[vk::binding(2, 1)]] SamplerState sheetSampler;

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
    return output;
}

float hash21(float2 p, float seed) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973) + seed * 0.137);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// Which thread is on top in the cell: 1 the warp (running along the cloth's y), 0 the weft
float warpOnTop(float2 cell, float kind) {
    float2 c = floor(cell);
    if (kind < 0.5) {
        return fmod(abs(c.x + c.y), 2.0) < 0.5 ? 1.0 : 0.0;                    // plain: over, under
    } else if (kind < 1.5) {
        return fmod(abs(c.x - c.y), 3.0) < 1.5 ? 1.0 : 0.0;                    // twill: over two, under one (about)
    }
    return fmod(abs(c.x * 2.0 + c.y), 5.0) < 0.5 ? 1.0 : 0.0;                  // satin: one in five
}

// The cloth's height at a point of the thread grid (a cell is one thread wide each way): the thread on top is a
// rounded bar over the one below, the lower thread a flatter one in the gaps
float clothHeight(float2 t, float kind) {
    float2 cell = floor(t);
    float2 f = t - cell;
    float warpTop = warpOnTop(cell, kind);
    float across = warpTop > 0.5 ? f.x : f.y;                                  // across the thread on top
    float bar = sin(3.14159265 * saturate(across));
    float lowerAcross = warpTop > 0.5 ? f.y : f.x;
    float lowerBar = sin(3.14159265 * saturate(lowerAcross));
    return 0.55 * pow(bar, 0.7) + 0.20 * lowerBar;
}

float3 cotangentTangent(float3 n, float3 p, float2 uv, out float3 bitangent) {
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, n);
    float3 dp1perp = cross(n, dp1);
    float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    bitangent = b * invmax;
    return t * invmax;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 N0 = normalize(input.normal);
    float3 toCam = frame.cameraPosition.xyz - input.world;
    float3 V = normalize(toCam);
    float3 L = normalize(frame.lightDirection.xyz);

    // The sheet: dirt, wear, seam
    float4 s = sheet.Sample(sheetSampler, input.uv);
    float dirt = saturate(s.r * params.amounts.y);
    float wear = saturate(s.g * params.amounts.x);
    float seam = s.b;

    // The weave, at its real size
    float2 metres = input.uv * float2(params.fuzz.z, params.fuzz.w);
    float2 t = metres * params.weave.x;
    float footprint = max(length(ddx(t)), length(ddy(t)));
    float fade = 1.0 - smoothstep(0.35, 0.9, footprint);
    float kind = params.weave.z;
    float h = clothHeight(t, kind);
    float e = 0.12;
    float hx = clothHeight(t + float2(e, 0.0), kind) - clothHeight(t - float2(e, 0.0), kind);
    float hy = clothHeight(t + float2(0.0, e), kind) - clothHeight(t - float2(0.0, e), kind);
    float2 slope = float2(hx, hy) / (2.0 * e);

    // tangent frame and the bent normal
    float3 B;
    float3 T = cotangentTangent(N0, input.world, input.uv, B);
    float bump = params.weave.y * fade * (1.0 - 0.6 * wear);
    float3 N = normalize(N0 - (T * slope.x + B * slope.y) * bump * 0.9);
    // a lit side that faces away is the back of the cloth: flip toward the viewer, as a two-sided surface does
    if (dot(N0, V) < 0.0) { N = -N; N0 = -N0; }

    // colour: the thread on top, thread to thread variety, the gaps dark
    float warpTop = warpOnTop(floor(t), kind);
    float3 thread = lerp(params.weft.rgb, params.warp.rgb, warpTop);
    float variety = (hash21(floor(t), params.amounts.z) - 0.5) * params.weave.w * (1.0 + 1.5 * wear);
    float3 albedo = thread * (1.0 + variety * fade);
    float3 meanThread = 0.5 * (params.warp.rgb + params.weft.rgb);
    albedo = lerp(meanThread, albedo, fade);
    float cavity = lerp(1.0, 0.72 + 0.28 * saturate(h / 0.75), fade);
    albedo *= cavity;

    // wear fades, dirt soils, a seam puckers dark
    float luma = dot(albedo, float3(0.299, 0.587, 0.114));
    albedo = lerp(albedo, float3(luma, luma, luma) * 1.25 + 0.03, wear * 0.55);
    albedo = lerp(albedo, albedo * params.soil.rgb, dirt);
    albedo *= 1.0 - 0.22 * seam;

    float sheenAmount = params.sheen.a * (1.0 - params.fuzz.y * wear) * (1.0 - 0.85 * dirt);

    float NoL = saturate(dot(N, L));
    float NoV = max(dot(N, V), 1e-3);
    float3 H = normalize(L + V);
    float NoH = saturate(dot(N, H));

    // diffuse: Lambert, softened a little (fibres scatter light into the shadow side)
    float diff = saturate((dot(N, L) + 0.2) / 1.2);
    float3 irradiance = frame.lightColor.rgb;
    float3 color = albedo * (irradiance / RENDER_PI * diff + frame.ambient.rgb);

    // sheen: Charlie distribution and Neubelt visibility, and the sky's share as a rim
    float r = clamp(params.fuzz.x, 0.15, 1.0);
    float sin2 = max(1.0 - NoH * NoH, 1e-4);
    float D = (2.0 + 1.0 / r) * pow(sin2, 0.5 / r) / (2.0 * RENDER_PI);
    float Vis = 1.0 / (4.0 * (NoL + NoV - NoL * NoV) + 1e-3);
    float3 sheenColor = params.sheen.rgb;
    float3 sheenDirect = sheenColor * D * Vis * NoL * irradiance;
    float rim = pow(1.0 - NoV, 3.0);
    float3 sheenAmbient = sheenColor * frame.ambient.rgb * rim * 0.55;
    float energy = 1.0 - sheenAmount * max(sheenColor.r, max(sheenColor.g, sheenColor.b)) * 0.4;
    color = color * energy + (sheenDirect + sheenAmbient) * sheenAmount;

    return float4(color * frame.exposure.x, 1.0);
}
