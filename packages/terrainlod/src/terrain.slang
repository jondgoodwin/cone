// The terrain's shader (terrainlod): one grid mesh drawn once for each node
// of the quadtree, as a custom pipeline of the render package.
//
// THE GRID. The mesh holds only the integers (i, 0, j), i and j from 0 to
// nodeCells (and, for the skirts, copies of the edge vertices at y = -1).
// Each node is one instance whose tint carries (ix, iz, level, slot): the
// node's place in its level's lattice and the block of the atlas holding its
// height patch. The one material holds the atlas, an r32float texture of
// blocks of (nodeCells + 1 + 2 x BORDER) samples a side, read texel by
// texel; every node is drawn by one instanced draw.
//
// THE POSITION. x = (ix nodeCells + i) c and z likewise, c the level's cell:
// an integer times c, so two nodes that share a lattice point, at the same
// level or levels apart (c doubles exactly), compute the very same float and
// the clip-space positions agree bit for bit along a border.
//
// THE GEOMORPH (CDLOD, Strugar 2010). The height is the vertex's own sample
// morphed toward the next coarser level's surface by k, found from the
// vertex's distance to the camera across the level's morph band
// [morphStart, rangeEnd]. The coarser surface at a vertex is the sample
// itself where i and j are even, the middle of its two even neighbours
// along an edge where one is odd, and where both are odd the middle of the
// coarser cell's diagonal; the diagonal a quad has is the same rule at
// every level ((a + b) even: (a, b) to (a + 1, b + 1); odd: the other), so
// the morphed fine triangles lie in the coarser triangles' planes. The
// normal is morphed the same way, from the central difference at one
// sample's step to the one at two samples' step (the coarser level's own).
// lodmath.cone has the CPU's twin of this arithmetic.
//
// THE LOOK. The albedo is a stack of layers from data: layer 0 everywhere,
// each later layer over it by a weight from the height and the steepness
// (1 - normal.y) of the ground, each edge softened and moved about by noise.
// A little noise bump on the normal near the eye. Lit by render's one
// convention (the sun an irradiance, ambient a radiance), the sun shadowed by
// render's cascaded shadow maps if they are on, hazed to a colour with
// distance (exponential, thinning with height), times the exposure.
//
// Group 0 is render's frame; group 1 the material's: its parameters, the
// atlas and render's sampler (unused).

import renderframe;
import shadowing;

static const int BORDER = 2;
static const int MAX_LEVELS = 16;
static const int MAX_LAYERS = 8;

struct Params {
    float4 grid;                    // nodeCells, finest cell, levels, bump strength
    float4 haze;                    // rgb haze radiance, w density per metre
    float4 hazeB;                   // x haze height falloff per metre, y debug mode, z layer count, w sun shadows (0/1)
    float4 atlas;                   // x patches across the atlas, y a patch's side in texels, z the skirts' drop in cells
    float4 morph[MAX_LEVELS];       // x cell, y morph start, z range end
    float4 layerA[MAX_LAYERS];      // rgb albedo, w noise amount
    float4 layerB[MAX_LAYERS];      // height low, height high, height softness, noise scale in metres
    float4 layerC[MAX_LAYERS];      // steepness low, steepness high, steepness softness, spare
};

[[vk::binding(0, 1)]] ConstantBuffer<Params> params;
[[vk::binding(1, 1)]] Texture2D<float> patch;

struct VertexInput {
    float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float morph : TEXCOORD2;
    nointerpolation float level : TEXCOORD3;
};

// The texel of the current node's patch in the atlas where its (0, 0) sample
// lies, set by the vertex stage before it reads
static int2 gOrigin;

float H(int i, int j) {
    return patch.Load(int3(gOrigin.x + i + BORDER, gOrigin.y + j + BORDER, 0));
}

// The normal from central differences 'step' samples out, 'c' metres a sample
float3 stencilNormal(int i, int j, int step, float c) {
    float dx = (H(i + step, j) - H(i - step, j)) / (2.0 * float(step) * c);
    float dz = (H(i, j + step) - H(i, j - step)) / (2.0 * float(step) * c);
    return normalize(float3(-dx, 1.0, -dz));
}

// The coarser level's surface height at vertex (i, j) (lodmath.cone's coarseHeight)
float coarseHeight(int i, int j) {
    bool oi = (i & 1) != 0;
    bool oj = (j & 1) != 0;
    if (!oi && !oj) { return H(i, j); }
    if (oi && !oj) { return 0.5 * (H(i - 1, j) + H(i + 1, j)); }
    if (!oi && oj) { return 0.5 * (H(i, j - 1) + H(i, j + 1)); }
    int bi = i >> 1;
    int bj = j >> 1;
    if (((bi + bj) & 1) == 0) {
        return 0.5 * (H(i - 1, j - 1) + H(i + 1, j + 1));
    }
    return 0.5 * (H(i + 1, j - 1) + H(i - 1, j + 1));
}

// The coarser level's normal at (i, j): the stencil of two samples' step at
// the even vertices, averaged as the heights are between them
float3 coarseNormal(int i, int j, float c) {
    bool oi = (i & 1) != 0;
    bool oj = (j & 1) != 0;
    if (!oi && !oj) { return stencilNormal(i, j, 2, c); }
    if (oi && !oj) {
        return normalize(stencilNormal(i - 1, j, 2, c) + stencilNormal(i + 1, j, 2, c));
    }
    if (!oi && oj) {
        return normalize(stencilNormal(i, j - 1, 2, c) + stencilNormal(i, j + 1, 2, c));
    }
    int bi = i >> 1;
    int bj = j >> 1;
    if (((bi + bj) & 1) == 0) {
        return normalize(stencilNormal(i - 1, j - 1, 2, c) + stencilNormal(i + 1, j + 1, 2, c));
    }
    return normalize(stencilNormal(i + 1, j - 1, 2, c) + stencilNormal(i - 1, j + 1, 2, c));
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    int level = int(inst.tint.z + 0.5);
    int slot = int(inst.tint.w + 0.5);
    int across = int(params.atlas.x + 0.5);
    int side = int(params.atlas.y + 0.5);
    gOrigin = int2((slot % across) * side, (slot / across) * side);
    float4 lv = params.morph[level];
    float c = lv.x;
    float nodeCells = params.grid.x;
    int i = int(input.position.x + 0.5);
    int j = int(input.position.z + 0.5);
    float gx = inst.tint.x * nodeCells + float(i);
    float gz = inst.tint.y * nodeCells + float(j);
    float x = gx * c;
    float z = gz * c;
    float hFine = H(i, j);
    float d = length(frame.cameraPosition.xyz - float3(x, hFine, z));
    float k = saturate((d - lv.y) / (lv.z - lv.y));
    float h = hFine;
    float3 n = stencilNormal(i, j, 1, c);
    if (k > 0.0) {
        float hc = coarseHeight(i, j);
        h = (k >= 1.0) ? hc : lerp(hFine, hc, k);
        n = normalize(lerp(n, coarseNormal(i, j, c), k));
    }
    // A skirt vertex (the mesh puts it at y = -1) hangs below its edge vertex
    if (input.position.y < -0.5) {
        h -= params.atlas.z * c;
    }
    float4 world = float4(x, h, z, 1.0);
    output.position = mul(frame.viewProjection, world);
    output.world = world.xyz;
    output.normal = n;
    output.morph = k;
    output.level = float(level);
    return output;
}

// ---- Noise ----

float hash21(float2 p) {
    uint2 q = uint2(int2(floor(p)));
    q = q * 1664525u + 1013904223u;
    q.x += q.y * 1664525u;
    q.y += q.x * 1664525u;
    q ^= q >> 16u;
    q.x += q.y * 1664525u;
    q.y += q.x * 1664525u;
    q ^= q >> 16u;
    return float(q.x >> 8u) * (1.0 / 16777216.0);
}

float vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbm3(float2 p) {
    float s = 0.0;
    float a = 0.5;
    for (int o = 0; o < 3; o++) {
        s += a * vnoise(p);
        p = p * 2.03 + float2(17.1, 9.2);
        a *= 0.5;
    }
    return s * 1.14;
}

// 0 below lo - soft, 1 above lo + soft (a smooth step across the band)
float rampUp(float v, float lo, float soft) {
    return smoothstep(lo - soft, lo + soft, v);
}

float3 levelColor(int level) {
    float3 colors[8] = {
        float3(1.0, 0.1, 0.1), float3(1.0, 0.6, 0.1), float3(0.9, 0.9, 0.1), float3(0.2, 0.8, 0.2),
        float3(0.1, 0.8, 0.8), float3(0.2, 0.3, 1.0), float3(0.7, 0.2, 0.9), float3(0.9, 0.9, 0.9)
    };
    return colors[level & 7];
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 p = input.world;
    float3 n = normalize(input.normal);
    int mode = int(params.hazeB.y + 0.5);

    // Debug: the height as floor and fraction of (height + 1000) / 4 (so
    // that a half float holds it to a few millimetres), and the distance
    // from the camera, raw (no exposure, no tone map)
    if (mode == 1) {
        float t = (p.y + 1000.0) * 0.25;
        return float4(floor(t), frac(t), length(frame.cameraPosition.xyz - p), 1.0);
    }
    if (mode == 2) {
        return float4(levelColor(int(input.level + 0.5)) * frame.exposure.x * (0.4 + 0.6 * n.y), 1.0);
    }
    if (mode == 3) {
        return float4(float3(input.morph, 1.0 - input.morph, 0.0) * frame.exposure.x, 1.0);
    }

    float dist = length(frame.cameraPosition.xyz - p);
    float near = saturate(1.0 - dist / 60.0);
    // A little bump from fine noise, near the eye
    float bumpStrength = params.grid.w * near;
    if (bumpStrength > 0.0) {
        float e = 0.3;
        float2 q = p.xz * 3.0;
        float bx = vnoise(q + float2(e, 0.0)) - vnoise(q - float2(e, 0.0));
        float bz = vnoise(q + float2(0.0, e)) - vnoise(q - float2(0.0, e));
        n = normalize(n + float3(bx, 0.0, bz) * bumpStrength);
    }

    float steep = 1.0 - n.y;
    int layers = int(params.hazeB.z + 0.5);
    float3 albedo = params.layerA[0].rgb;
    for (int l = 0; l < MAX_LAYERS; l++) {
        if (l >= layers) { break; }
        float4 a = params.layerA[l];
        float4 b = params.layerB[l];
        float4 cc = params.layerC[l];
        float nz = fbm3(p.xz / max(b.w, 0.01) + float(l) * 7.3);
        float jitter = (nz - 0.5) * 2.0 * a.w;
        float hh = p.y + jitter * b.z * 2.0;
        float ss = steep + jitter * cc.z * 2.0;
        float w = rampUp(hh, b.x, b.z) * (1.0 - rampUp(hh, b.y, b.z));
        w *= rampUp(ss, cc.x, cc.z) * (1.0 - rampUp(ss, cc.y, cc.z));
        float3 col = a.rgb * (1.0 + (nz - 0.5) * a.w * 0.8);
        albedo = (l == 0) ? col : lerp(albedo, col, w);
    }

    float visible = 1.0;
    if (params.hazeB.w > 0.5) {
        visible = sunVisibility(p, n, frame.lightDirection.xyz, input.position.xy);
    }
    float diffuse = max(dot(n, frame.lightDirection.xyz), 0.0) * visible;
    float3 amb = frame.ambient.rgb * (0.65 + 0.35 * n.y);
    float3 color = albedo * (amb + frame.lightColor.rgb * (diffuse / RENDER_PI));

    // Haze: an exponential in the distance, thinner higher up (the mean
    // height along the sight line)
    float hm = 0.5 * (p.y + frame.cameraPosition.y);
    float density = params.haze.w * exp(-max(hm, 0.0) * params.hazeB.x);
    float fogAmount = 1.0 - exp(-dist * density);
    color = lerp(color, params.haze.rgb, fogAmount);
    return float4(color * frame.exposure.x, 1.0);
}
