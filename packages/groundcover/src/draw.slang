// The groundcover package's cover renderer: every kind of cover drawn from the
// instances the placement kernels (generate.slang) wrote, depth tested and
// written, into the renderer's HDR frame after the opaque scene. Compiled ahead
// of time to draw.spv by tools/shaders/shaders.py and embedded in renderer.cone.
//
// Nothing is read from a vertex buffer except a MESH type's mesh: instance i of
// an indirect draw is instance (base + i) of the buffer, and the vertex index says
// which corner of what. Each kind builds its own geometry:
//
// - KIND_BLADES: a tuft of blades, each a strip of segments that tapers to a point
//   and bends over (a parabola), the tuft splayed outward; the wind moves each
//   blade as a cantilever (the displacement grows with the square of its height) by a
//   rolling wave across the meadow, a travelling gust field and a quick flutter.
// - KIND_FLOWER: a stem like a blade, and a head: a quad turned toward the viewer
//   (as far as upright allows), whose petals are cut out in the fragment stage from a
//   polar curve.
// - KIND_PATCH: a grid of quads on the ground (heights read from the map for each
//   vertex, so it follows the terrain), its edge cut ragged by noise in the fragment stage.
// - KIND_LITTER: one flat leaf lying on the ground, its outline cut in the fragment stage.
// - KIND_MESH: the registered mesh, scaled, turned and stood on the ground.
//
// LIGHT: Lambert with a wrapped term and a translucent term for blades and petals
// (light through a thin leaf from behind), the sky's ambient light dimmed toward
// the root (a stand-in for the occlusion between blades), the sun's light scaled by
// the baked exposure of the ground (the terrain's own shadow, the canopy's) and
// the renderer's one convention: the light's colour is an irradiance, so diffuse is
// albedo / pi times it, and everything is multiplied by the exposure.
//
// THE FAR FIELD: with distance every colour moves toward the far-field map's colour
// of the ground under it, so where the cover thins and ends, the ground it ends on
// is the colour it was.

import noise;
import cover;

struct CoverInstance {
    float4 a;                  // x, y (the ground), z, height
    float4 b;                  // yaw, width, lean, tint
    float4 c;                  // the seed (as bits), the ground normal's x and z, the exposure
};

struct DrawParams {
    uint typeIndex;
    uint ring;
    uint base;
    uint debug;
};

[[vk::push_constant]] ConstantBuffer<DrawParams> draw;

[[vk::binding(3, 0)]] StructuredBuffer<CoverInstance> instances;
[[vk::binding(4, 0)]] StructuredBuffer<float4> meshData;       // two float4 a vertex: (position, u), (normal, v)
[[vk::binding(5, 0)]] Texture2D<float4> farField;
[[vk::binding(6, 0)]] SamplerState farSampler;

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float4 shade : TEXCOORD3;          // ambient occlusion, translucency, the sun's exposure, unused
    float4 uv : TEXCOORD4;             // u, v, the part (0 blade, 1 flower head, 2 patch, 3 leaf, 4 mesh), a per-instance random
};

float rand01(uint h) { return toUnit(pcg(h)); }

float2 quadCorner(uint v) {
    float x = (v == 1u || v == 3u || v == 4u) ? 1.0 : -1.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0;
    return float2(x, y);
}

float3 groundNormalOf(CoverInstance inst) {
    float nx = inst.c.y;
    float nz = inst.c.z;
    return float3(nx, sqrt(max(1.0 - nx * nx - nz * nz, 0.0)), nz);
}

// The wind's push on a point of height fraction 't01' on something 'height' tall
// standing at 'basePos': a rolling wave across the meadow, a travelling gust field
// and a quick flutter, bending as a cantilever (the square of t01). 'amount' is the
// type's wind response; 0 is none.
float3 windOffset(float3 basePos, float t01, float height, float amount, float seed01) {
    if (amount <= 0.0) { return float3(0.0, 0.0, 0.0); }
    float2 wd = frame.wind.xy;
    float speed = frame.wind.z;
    float gust = frame.wind.w;
    float time = frame.cameraPosition.w;
    float phase = dot(basePos.xz, wd) * 0.45 - time * (0.8 + speed * 0.35);
    float roll = 0.5 + 0.5 * sin(phase);
    float g = valueNoise(basePos.x * 0.09 - wd.x * time * speed * 0.5, basePos.z * 0.09 - wd.y * time * speed * 0.5, 777u);
    float amp = gust * (0.25 + 0.75 * g) * (0.35 + 0.65 * roll);
    float flutter = sin(time * (5.0 + seed01 * 4.0) + seed01 * 40.0 + basePos.x * 3.0) * 0.5;
    float bendAmt = t01 * t01;
    float2 perp = float2(0.0 - wd.y, wd.x);
    float2 lat = wd * (amp * 0.22 * height * bendAmt * amount) + perp * (flutter * 0.03 * height * bendAmt * amount * (0.4 + amp));
    float sag = (lat.x * lat.x + lat.y * lat.y) * 0.5 / max(height * max(t01, 0.05), 0.01);
    return float3(lat.x, 0.0 - sag, lat.y);
}

float3 farColorAt(float2 xz) {
    float2 uv = (xz - frame.mapRect.xy) / (frame.mapRect.z * frame.mapRect.w);
    return farField.SampleLevel(farSampler, uv, 0.0).rgb;
}

// Fill in the albedo, ambient occlusion, translucency and exposure that are common to a vertex
void finishVertex(inout VertexOutput o, CoverGpu T, CoverInstance inst, float3 albedo, float ao, float translucency, float3 basePos) {
    float3 toCamera = frame.cameraPosition.xyz - basePos;
    float dist = length(float2(toCamera.x, toCamera.z));
    float blend = gcSmoothstep(T.r0.x * 0.45, T.r0.x * 0.95, dist) * frame.misc.z;
    float3 far = farColorAt(basePos.xz);
    // dry and lush patches, by the slow noise
    float dryness = gcSmoothstep(0.55, 0.8, inst.b.w);
    albedo = lerp(albedo, T.dryColor.rgb, T.tipColor.w * dryness);
    o.albedo = lerp(albedo, far, blend);
    o.shade = float4(ao, translucency, inst.c.w, 0.0);
}

// A point of blade 'blade' of 'tuft' at height fraction t (0 to 1) and side (-1, 1): position, normal, fraction
struct BladeGeom {
    float3 position;
    float3 normal;
};

BladeGeom bladeAt(CoverGpu T, CoverInstance inst, uint blade, float t, float side, float widthAbs, float ringScale, bool single) {
    float3 base = inst.a.xyz;
    float h = inst.a.w;
    float yaw = inst.b.x;
    float wid = inst.b.y;
    float lean = inst.b.z;
    uint seed = asuint(inst.c.x);
    uint4 hb = pcg4d(uint4(seed, blade, 11u, draw.typeIndex));
    float tuftR = single ? 0.0 : 0.25 * h * ringScale;
    float ra = tuftR * sqrt(toUnit(hb.x));
    float aa = COVER_TAU * toUnit(hb.y);
    float2 off = ra * float2(cos(aa), sin(aa));
    float hf = single ? 1.0 : 0.55 + 0.45 * toUnit(hb.z);
    float2 outward = ra > 0.00001 ? off / ra : float2(cos(yaw), sin(yaw));
    float bladeYaw = COVER_TAU * toUnit(hb.w);
    float2 leanDir = normalize(outward * 0.7 + float2(cos(yaw), sin(yaw)) * 0.5 + float2(cos(bladeYaw), sin(bladeYaw)) * 0.3);
    float2 sideDir = float2(0.0 - leanDir.y, leanDir.x);
    float bh = h * hf;
    float bendAmount = T.p3.y * (0.5 + 0.5 * toUnit(hb.z));
    float reach = bh * (lean + bendAmount * t);
    float3 centre = base + float3(off.x, 0.0, off.y) + float3(leanDir.x, 0.0, leanDir.y) * (reach * t) + float3(0.0, bh * t, 0.0);
    float width = widthAbs * (1.0 - t * t * 0.9);
    float3 p = centre + float3(sideDir.x, 0.0, sideDir.y) * (side * width * 0.5);
    float seed01 = toUnit(pcg(seed));
    p += windOffset(base + float3(off.x, 0.0, off.y), t, bh, T.p3.z, seed01);
    float3 tangent = normalize(float3(leanDir.x * (bh * (lean + 2.0 * bendAmount * t)), bh, leanDir.y * (bh * (lean + 2.0 * bendAmount * t))));
    float3 nGeom = normalize(cross(tangent, float3(sideDir.x, 0.0, sideDir.y)));
    float3 toEye = frame.cameraPosition.xyz - p;
    if (dot(nGeom, toEye) < 0.0) { nGeom = 0.0 - nGeom; }
    BladeGeom g;
    g.position = p;
    g.normal = normalize(lerp(groundNormalOf(inst), nGeom, 0.45));
    return g;
}

uint bladesOf(CoverGpu T, uint ring) {
    float b = float(T.k1.w);
    for (uint r = 0u; r < ring; r++) { b = b * 0.65; }
    return max(3u, uint(floor(b)));
}

uint segmentsOf(uint ring) { return 4u - min(ring, 2u); }

VertexOutput bladesVertex(CoverGpu T, CoverInstance inst, uint vid, bool flower) {
    VertexOutput o;
    float ringScale = pow(T.r0.z, float(draw.ring));
    uint S = flower ? max(2u, 3u - min(draw.ring, 1u)) : segmentsOf(draw.ring);
    uint perBlade = 6u * S - 3u;
    uint blade = flower ? 0u : vid / perBlade;
    uint k = flower ? vid : vid % perBlade;
    // a blade's width is the instance's; a flower's stem is a thin strip of its own
    float stemWidth = flower ? 0.012 * ringScale : inst.b.y;
    uint segment = 0u;
    uint corner = 0u;
    float t = 0.0;
    float side = -1.0;
    bool head = false;
    if (flower && k >= perBlade) {
        head = true;
    } else if (k < 6u * (S - 1u)) {
        segment = k / 6u;
        corner = k % 6u;
        float rows = float(S);
        // 0: (L, seg) 1: (R, seg) 2: (L, seg+1) 3: (R, seg) 4: (R, seg+1) 5: (L, seg+1)
        float row = float(segment) + ((corner == 2u || corner == 4u || corner == 5u) ? 1.0 : 0.0);
        side = (corner == 1u || corner == 3u || corner == 4u) ? 1.0 : -1.0;
        t = row / rows;
    } else {
        uint tip = k - 6u * (S - 1u);
        float rows = float(S);
        if (tip == 2u) { t = 1.0; side = 0.0; }
        else { t = float(S - 1u) / rows; side = tip == 1u ? 1.0 : -1.0; }
    }
    float3 basePos = inst.a.xyz;
    float seed01 = toUnit(pcg(asuint(inst.c.x)));
    if (head) {
        // The flower's head: a quad at the stem's top turned toward the viewer, as far as upright allows
        BladeGeom top = bladeAt(T, inst, 0u, 1.0, 0.0, stemWidth, ringScale, true);
        float3 toEye = normalize(frame.cameraPosition.xyz - top.position);
        float3 n = normalize(lerp(float3(0.0, 1.0, 0.0), toEye, 0.6));
        float3 right = normalize(cross(float3(0.0, 1.0, 0.0), n));
        float3 up = cross(n, right);
        float2 c = quadCorner(k - perBlade);
        float radius = inst.b.y;
        float3 p = top.position + (right * c.x + up * c.y) * radius + n * 0.01;
        o.position = mul(frame.viewProjection, float4(p, 1.0));
        o.world = p;
        o.normal = n;
        o.uv = float4(c.x, c.y, 1.0, seed01);
        finishVertex(o, T, inst, float3(1.0, 1.0, 1.0), 1.0, T.p3.w, basePos);
        return o;
    }
    BladeGeom g = bladeAt(T, inst, blade, t, side, stemWidth, ringScale, flower);
    float3 p = g.position;
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.world = p;
    o.normal = g.normal;
    o.uv = float4(side, t, 0.0, seed01);
    float3 col = lerp(T.baseColor.rgb, T.tipColor.rgb, pow(t, 0.8));
    float v = toUnit(pcg(asuint(inst.c.x) ^ 0x9E3779B9u)) * T.baseColor.w;
    col = lerp(col, T.altColor.rgb, v);
    col = lerp(col, T.baseColor.rgb * 0.7, 0.0);
    float ao = lerp(0.25, 1.0, pow(t, 0.6));
    if (flower) { col = lerp(float3(0.06, 0.16, 0.03), float3(0.2, 0.4, 0.08), t); ao = lerp(0.4, 1.0, t); }
    finishVertex(o, T, inst, col, ao, T.p3.w, basePos);
    return o;
}

VertexOutput patchVertex(CoverGpu T, CoverInstance inst, uint vid) {
    VertexOutput o;
    uint g = max(2u, 6u - 2u * draw.ring);
    uint quad = vid / 6u;
    uint c = vid % 6u;
    float cx = (c == 1u || c == 3u || c == 4u) ? 1.0 : 0.0;
    float cy = (c == 2u || c == 4u || c == 5u) ? 1.0 : 0.0;
    float u = (float(quad % g) + cx) / float(g) * 2.0 - 1.0;
    float v = (float(quad / g) + cy) / float(g) * 2.0 - 1.0;
    float yaw = inst.b.x;
    float2 local = float2(u, v) * inst.b.y;
    float2 rot = float2(local.x * cos(yaw) - local.y * sin(yaw), local.x * sin(yaw) + local.y * cos(yaw));
    float3 base = inst.a.xyz;
    float x = base.x + rot.x;
    float z = base.z + rot.y;
    float r2 = min(1.0, u * u + v * v);
    float y = mapSample(0u, x, z) + 0.012 + inst.a.w * (1.0 - r2);
    float3 p = float3(x, y, z);
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.world = p;
    o.normal = mapNormal(x, z);
    o.uv = float4(u, v, 2.0, toUnit(pcg(asuint(inst.c.x))));
    finishVertex(o, T, inst, lerp(T.baseColor.rgb, T.tipColor.rgb, 0.5), 0.9, 0.0, base);
    return o;
}

VertexOutput litterVertex(CoverGpu T, CoverInstance inst, uint vid) {
    VertexOutput o;
    float2 c = quadCorner(vid);
    float3 n = groundNormalOf(inst);
    float yaw = inst.b.x;
    float3 f0 = float3(cos(yaw), 0.0, sin(yaw));
    float3 f = normalize(f0 - n * dot(f0, n));
    float3 s = cross(n, f);
    float3 base = inst.a.xyz;
    float curl = inst.b.z * abs(c.x) * inst.b.y;
    float3 p = base + n * (0.008 + curl) + s * (c.x * inst.b.y) + f * (c.y * inst.a.w * 0.5);
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.world = p;
    o.normal = n;
    o.uv = float4(c.x, c.y * 0.5 + 0.5, 3.0, toUnit(pcg(asuint(inst.c.x))));
    float v = toUnit(pcg(asuint(inst.c.x) ^ 0x85EBCA6Bu)) * T.baseColor.w;
    finishVertex(o, T, inst, lerp(T.baseColor.rgb, T.altColor.rgb, v), 0.85, T.p3.w, base);
    return o;
}

VertexOutput meshVertex(CoverGpu T, CoverInstance inst, uint vid) {
    VertexOutput o;
    uint first = T.k1[draw.ring];
    float4 pa = meshData[(first + vid) * 2u];
    float4 pb = meshData[(first + vid) * 2u + 1u];
    float h = inst.a.w;
    float wid = inst.b.y;
    float yaw = inst.b.x;
    float3 gn = groundNormalOf(inst);
    float3 up = normalize(lerp(float3(0.0, 1.0, 0.0), gn, T.dryColor.w));
    float3 ax = normalize(cross(up, float3(0.0, 0.0, 1.0)) + float3(0.0001, 0.0, 0.0));
    float3 az = cross(ax, up);
    float cy = cos(yaw);
    float sy = sin(yaw);
    float3 lx = ax * cy + az * sy;
    float3 lz = az * cy - ax * sy;
    float3 local = float3(pa.x * wid, pa.y * h, pa.z * wid);
    float radius = max(T.r2[draw.ring], 0.001);
    float t01 = saturate(pa.y / radius);
    float2 leanDir = float2(cos(yaw + 1.0), sin(yaw + 1.0));
    float3 p = inst.a.xyz + lx * local.x + up * local.y + lz * local.z - up * (T.flower.w * h);
    p.xz += leanDir * (inst.b.z * local.y);
    float seed01 = toUnit(pcg(asuint(inst.c.x)));
    p += windOffset(inst.a.xyz, t01, h * radius, T.p3.z, seed01);
    float3 nrm = normalize(lx * pb.x + up * pb.y + lz * pb.z);
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.world = p;
    o.normal = nrm;
    o.uv = float4(pa.w, pb.w, 4.0, seed01);
    float v = toUnit(pcg(asuint(inst.c.x) ^ 0xC2B2AE35u)) * T.baseColor.w;
    float3 col = lerp(T.baseColor.rgb, T.tipColor.rgb, t01);
    col = lerp(col, T.altColor.rgb, v);
    finishVertex(o, T, inst, col, lerp(0.55, 1.0, t01), T.p3.w, inst.a.xyz);
    return o;
}

[shader("vertex")]
VertexOutput vertexMain(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    CoverGpu T = coverTypes.types[draw.typeIndex];
    CoverInstance inst = instances[draw.base + iid];
    uint kind = T.k0.x;
    VertexOutput o;
    if (kind == KIND_BLADES) { o = bladesVertex(T, inst, vid, false); }
    else if (kind == KIND_FLOWER) { o = bladesVertex(T, inst, vid, true); }
    else if (kind == KIND_PATCH) { o = patchVertex(T, inst, vid); }
    else if (kind == KIND_LITTER) { o = litterVertex(T, inst, vid); }
    else { o = meshVertex(T, inst, vid); }
    // a diagnostic (CoverRenderer.debugMode 2): every vertex placed off the screen, so only the vertex stage costs
    if (draw.debug == 2u) { o.position = float4(2.0, 2.0, 2.0, 1.0); }
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target {
    // a diagnostic (CoverRenderer.debugMode 1): a flat colour, so only the rasteriser and the vertex stage cost
    if (draw.debug == 1u) { return float4(0.02, 0.06, 0.01, 1.0) * frame.lightDirection.w; }
    CoverGpu T = coverTypes.types[draw.typeIndex];
    uint kind = T.k0.x;
    float part = input.uv.z;
    float3 albedo = input.albedo;
    float3 n = normalize(input.normal);
    float seed01 = input.uv.w;
    float translucency = input.shade.y;
    float ao = input.shade.x;
    if (part > 0.5 && part < 1.5) {
        // a flower head: petals cut from a polar curve
        float r = length(input.uv.xy);
        float ang = atan2(input.uv.y, input.uv.x) + seed01 * COVER_TAU;
        float petals = T.flowerCenter.w;
        float edge = 0.62 + 0.38 * pow(abs(cos(0.5 * petals * ang)), 0.6);
        if (r > edge) { discard; }
        float3 petal = lerp(T.flower.rgb, T.altColor.rgb, T.baseColor.w * seed01);
        if (r < 0.22) {
            albedo = T.flowerCenter.rgb;
        } else {
            albedo = lerp(petal * 0.55, petal, gcSmoothstep(0.2, 0.65, r));
        }
        ao = 1.0;
        translucency = T.p3.w;
    } else if (part > 1.5 && part < 2.5) {
        // a patch: the edge ragged by noise, the surface speckled
        float2 q = input.uv.xy;
        float rough = fbm2g(q.x * 3.5 + seed01 * 31.0, q.y * 3.5 - seed01 * 17.0, 55u) - 0.5;
        float m = length(q) + rough * 0.8;
        if (m > 0.85) { discard; }
        float sp = valueNoise(input.world.x * 22.0, input.world.z * 22.0, 91u);
        float sp2 = valueNoise(input.world.x * 7.0 + 3.0, input.world.z * 7.0, 92u);
        albedo = lerp(T.baseColor.rgb, T.tipColor.rgb, sp2) * (0.75 + 0.5 * sp);
        ao = lerp(1.0, 0.55, gcSmoothstep(0.4, 0.85, m));
        translucency = 0.0;
    } else if (part > 2.5 && part < 3.5) {
        // a leaf: its outline, and a vein
        float u = input.uv.x;
        float v = input.uv.y;
        float hw = pow(max(sin(COVER_PI * pow(v, 0.85)), 0.0), 0.8);
        if (abs(u) > hw) { discard; }
        float vein = 1.0 - 0.35 * (1.0 - gcSmoothstep(0.0, 0.08, abs(u)));
        albedo = albedo * vein;
        if (!front) { n = 0.0 - n; }
    } else if (part > 3.5) {
        if (!front) { n = 0.0 - n; }
        if (T.altColor.w > 0.0) {
            float sp = valueNoise(input.world.x * 40.0, input.world.z * 40.0 + input.world.y * 40.0, 17u);
            albedo = albedo * (1.0 - T.altColor.w + T.altColor.w * 2.0 * sp);
        }
    }
    float3 l = frame.lightDirection.xyz;
    float3 v = normalize(frame.cameraPosition.xyz - input.world);
    float ndl = dot(n, l);
    float wrap = kind == KIND_MESH ? 0.0 : 0.25;
    float diffuse = max((ndl + wrap) / (1.0 + wrap), 0.0);
    float back = max(dot(0.0 - n, l), 0.0) * pow(max(dot(v, 0.0 - l), 0.0), 2.0);
    float exposure = input.shade.z;
    float3 direct = frame.lightColor.rgb * (1.0 / COVER_PI) * exposure;
    float3 sky = frame.ambient.rgb * (0.65 + 0.35 * n.y);
    float3 colour = albedo * (sky * ao + direct * (diffuse * lerp(0.6, 1.0, ao))) + albedo * direct * (translucency * back) * float3(1.0, 1.1, 0.5);
    return float4(colour * frame.lightDirection.w, 1.0);
}
