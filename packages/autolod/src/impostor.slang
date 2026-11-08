// The autolod package's octahedral impostor, drawn as render's custom pipeline
// (render\src\custom.cone has the contract). Compiled ahead of time to
// impostor.spv by tools/shaders/shaders.py and embedded in impostor.cone.
//
// WHAT IT DRAWS. One card for an object, in place of its mesh: a square of the
// object's bounding sphere's size, turned to face the camera, textured from an
// atlas of pictures of the object taken from a grid of directions on the octahedral
// map (octahedral.cone has the maps and the reasons; this file is its GPU twin and
// the two must agree). Method: octahedral impostors (Ryan Brucks, "Octahedral
// Impostors", Epic, 2017), with the depth-corrected reprojection of
// parallax-corrected impostors (Schaufler, "Per-Object Image Warping with Layered
// Impostors", 1998; Brucks' ray-marched frame blend).
//
// THE CARD. The vertex stage works in the object's space, so that a copy turned
// about y is still shown from the right side: the camera is carried into it by the
// inverse of the copy's model matrix (rotation and scale, no shear), and the card
// is the square of half-size R about the object's centre, perpendicular to the way
// to the camera. A corner is the quad mesh's position, (+-1, +-1).
//
// THE PICTURE. For each pixel, the ray from the camera through the card. The
// three frames the octahedral map names for the view direction (the corners of the
// triangle of the grid the direction falls in) each give a sample: the ray is
// carried to where it meets the surface the frame saw, by looking up the frame's
// depth at the card's point, moving the point along the ray to that depth, and
// again (two steps, which settles for a convex shape); the frame's colour,
// coverage and normal are read there. The three are blended by their barycentric
// weights times their coverage; the pixel is dropped where the blended coverage is
// below the cut-off. A frame is baked orthographically from its direction, so a
// point maps to the frame's picture by projecting along it onto the plane through
// the object's centre.
//
// THE DEPTH is written (SV_Depth) from the surface point found, not the card's
// plane, so a building's foot meets the ground where its walls do and trees clip
// against each other and the terrain properly.
//
// THE LIGHT. Diffuse only, the baked colour (an albedo: flat-lit when baked) times
// the sun's Lambert term, shadowed by the cascaded map like any lit surface
// (shadowing.slang), plus the ambient light: the frame's ambient radiance, or the
// sky's diffuse irradiance cube (the physically based material's) when the
// material says. No specular, no point or spot lights: an impostor is a distant
// thing. The convention is renderframe.slang's (light colour an irradiance,
// ambient a radiance), the result times the exposure.
//
// THE CROSS-FADE. render's dithered LOD fade (fade.slang) applies as to any
// level: the copy's fade is read and fadeClip drops the pixels, so the level it
// fades with, a mesh drawn by the standard shaders, covers the rest exactly.

import renderframe;
import fade;
import shadowing;

struct ImpostorParams {
    float4 centerRadius;           // xyz the object's centre, w its radius R (object space)
    float4 atlas;                  // x frames a side, y the frame's size in pixels, z 1 for a hemisphere, w the coverage cut-off
    float4 shade;                  // x 1: lit by the sky's irradiance cube, 0: by the frame's ambient light; y a bias to the mip level; z 1: alpha hashed, 0: cut at the cut-off
    float4 spare;
};

[[vk::binding(0, 1)]] ConstantBuffer<ImpostorParams> impostor;
[[vk::binding(1, 1)]] Texture2D albedoAtlas;          // sRGB: colour, and coverage in alpha
[[vk::binding(2, 1)]] Texture2D normalAtlas;          // object-space normal * 0.5 + 0.5, and depth in alpha
[[vk::binding(3, 1)]] SamplerState atlasSampler;
// The sky's diffuse irradiance and its sampler (the physically based material's bindings of group 2)
[[vk::binding(1, 2)]] TextureCube irradianceCube;
[[vk::binding(4, 2)]] SamplerState environmentSampler;

struct VertexInput {
    float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 local : TEXCOORD0;                   // the card's point, in the object's space
    nointerpolation float3 camera : TEXCOORD1;  // the camera, in the object's space
    nointerpolation float4 tint : COLOR;
    nointerpolation float fade : TEXCOORD7;
    nointerpolation uint instance : TEXCOORD6;
};

static const float IMPOSTOR_PI = 3.14159265358979;

// octahedral.cone's octDecode / octEncode / frameBasis / pickFrames

float3 octDecode(float2 uv, bool hemisphere) {
    float2 p = uv * 2.0 - 1.0;
    if (hemisphere) {
        float x = (p.x + p.y) * 0.5;
        float z = (p.x - p.y) * 0.5;
        float m = max(abs(p.x), abs(p.y));
        return normalize(float3(x, 1.0 - m, z));
    }
    float3 d = float3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (d.y < 0.0) {
        float2 s = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
        d.x = (1.0 - abs(p.y)) * s.x;
        d.z = (1.0 - abs(p.x)) * s.y;
    }
    return normalize(d);
}

float2 octEncode(float3 d, bool hemisphere) {
    float dy = d.y;
    if (hemisphere && dy < 0.0) {
        dy = 0.0;
    }
    float l1 = abs(d.x) + abs(dy) + abs(d.z);
    if (l1 < 0.0000001) {
        return float2(0.5, 0.5);
    }
    float ox = d.x / l1;
    float oz = d.z / l1;
    if (hemisphere) {
        return float2((ox + oz + 1.0) * 0.5, (ox - oz + 1.0) * 0.5);
    }
    float px = ox;
    float pz = oz;
    if (dy < 0.0) {
        px = (1.0 - abs(oz)) * (ox < 0.0 ? -1.0 : 1.0);
        pz = (1.0 - abs(ox)) * (oz < 0.0 ? -1.0 : 1.0);
    }
    return float2((px + 1.0) * 0.5, (pz + 1.0) * 0.5);
}

void frameBasis(float3 d, out float3 right, out float3 up) {
    float3 n = normalize(d);
    float3 reference = abs(n.y) > 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    right = normalize(cross(reference, n));
    up = cross(n, right);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint id : SV_VulkanInstanceID) {
    VertexOutput output;
    Instance inst = instances[id];
    float4x4 m = inst.model;
    float3 c0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 c1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 c2 = float3(m[0][2], m[1][2], m[2][2]);
    float3 t = float3(m[0][3], m[1][3], m[2][3]);
    float3 toCamera = frame.cameraPosition.xyz - t;
    // The camera in the object's space: the inverse of rotation and scale
    float3 camera = float3(dot(c0, toCamera) / dot(c0, c0), dot(c1, toCamera) / dot(c1, c1), dot(c2, toCamera) / dot(c2, c2));
    float3 centre = impostor.centerRadius.xyz;
    float radius = impostor.centerRadius.w;
    float3 direction = normalize(camera - centre);
    float3 right;
    float3 up;
    frameBasis(direction, right, up);
    float3 local = centre + (right * input.position.x + up * input.position.y) * radius;
    float4 world = mul(m, float4(local, 1.0));
    output.position = mul(frame.viewProjection, world);
    output.local = local;
    output.camera = camera;
    output.tint = inst.tint;
    output.fade = instanceFade(id);
    output.instance = id;
    return output;
}

// One frame's sample for the ray 'origin' + t 'ray' through the card's point
// 'p': the final point on the surface the frame saw, and its colour, coverage,
// normal. 'cell' is the frame's (column, row).
struct FrameSample {
    float4 colour;                 // rgb, coverage
    float3 normal;                 // object space, unit
    float3 hit;                    // where the ray met the frame's surface
};

float2 frameUv(float3 x, float3 centre, float radius, float3 right, float3 up, float2 cell, float grid) {
    float3 r = x - centre;
    float a = 0.5 + dot(r, right) / (2.0 * radius);
    float b = 0.5 - dot(r, up) / (2.0 * radius);
    return (cell + float2(a, b)) / grid;
}

bool insideFrame(float3 x, float3 centre, float radius, float3 right, float3 up) {
    float3 r = x - centre;
    float a = 0.5 + dot(r, right) / (2.0 * radius);
    float b = 0.5 - dot(r, up) / (2.0 * radius);
    return a > 0.002 && a < 0.998 && b > 0.002 && b < 0.998;
}

FrameSample sampleFrame(float2 cell, float3 p, float3 ray, float lod) {
    float grid = impostor.atlas.x;
    bool hemi = impostor.atlas.z > 0.5;
    float3 centre = impostor.centerRadius.xyz;
    float radius = impostor.centerRadius.w;
    float3 dir = octDecode(cell / (grid - 1.0), hemi);
    float3 right;
    float3 up;
    frameBasis(dir, right, up);
    // Carry the point along the ray to the depth the frame holds there, twice
    float3 x = p;
    float rd = dot(ray, dir);
    if (abs(rd) > 0.05) {
        for (int k = 0; k < 2; k++) {
            float2 uv = frameUv(x, centre, radius, right, up, cell, grid);
            float h = normalAtlas.SampleLevel(atlasSampler, uv, lod).a;
            float surface = (h - 0.5) * 2.0 * radius;
            float t = (surface - dot(p - centre, dir)) / rd;
            x = p + ray * t;
        }
    }
    FrameSample s;
    s.hit = x;
    if (insideFrame(x, centre, radius, right, up)) {
        float2 uv = frameUv(x, centre, radius, right, up, cell, grid);
        s.colour = albedoAtlas.SampleLevel(atlasSampler, uv, lod);
        float4 nd = normalAtlas.SampleLevel(atlasSampler, uv, lod);
        float3 n = nd.xyz * 2.0 - 1.0;
        s.normal = dot(n, n) > 0.0001 ? normalize(n) : dir;
    } else {
        s.colour = float4(0.0, 0.0, 0.0, 0.0);
        s.normal = dir;
    }
    return s;
}

struct FragmentOutput {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
FragmentOutput fragmentMain(VertexOutput input) {
    // Before any branch that depends on the pixel: the derivatives
    float3 dPx = ddx(input.local);
    float3 dPy = ddy(input.local);
    float radius = impostor.centerRadius.w;
    float grid = impostor.atlas.x;
    float frameSize = impostor.atlas.y;
    float texelsPerPixel = max(length(dPx), length(dPy)) / (2.0 * radius / frameSize);
    float lod = clamp(log2(max(texelsPerPixel, 1.0)) + impostor.shade.y, 0.0, max(log2(frameSize) - 3.0, 0.0));

    fadeClip(input.fade, input.position.xy);

    float3 centre = impostor.centerRadius.xyz;
    bool hemi = impostor.atlas.z > 0.5;
    float3 toCamera = input.camera - centre;
    float3 view = normalize(toCamera);
    float3 ray = normalize(input.local - input.camera);

    // The three frames and their weights (octahedral.cone's pickFrames)
    float2 uv = octEncode(view, hemi);
    float cells = grid - 1.0;
    float2 g = clamp(uv * cells, float2(0.0, 0.0), float2(cells, cells));
    float2 base = min(floor(g), float2(cells - 1.0, cells - 1.0));
    float2 f = g - base;
    float2 cellA;
    float2 cellB;
    float2 cellC;
    float3 w;
    if (f.x + f.y < 1.0) {
        cellA = base;
        cellB = base + float2(1.0, 0.0);
        cellC = base + float2(0.0, 1.0);
        w = float3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        cellA = base + float2(1.0, 1.0);
        cellB = base + float2(0.0, 1.0);
        cellC = base + float2(1.0, 0.0);
        w = float3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }
    FrameSample sa = sampleFrame(cellA, input.local, ray, lod);
    FrameSample sb = sampleFrame(cellB, input.local, ray, lod);
    FrameSample sc = sampleFrame(cellC, input.local, ray, lod);
    float wa = w.x * sa.colour.a;
    float wb = w.y * sb.colour.a;
    float wc = w.z * sc.colour.a;
    float coverage = wa + wb + wc;
    if (impostor.shade.z > 0.5) {
        // Alpha hashing (Wyman and McGuire, "Hashed Alpha Testing", I3D 2017): the pixel is kept with
        // the probability of its coverage, by a fixed noise of the pixel (a pattern other than the
        // cross-fade's). A sparse crown, whose averaged alpha is a fraction everywhere, keeps its extent
        // and its density, and temporal anti-aliasing averages the noise; a cut at the cut-off instead
        // keeps only the densest core of it.
        float h = fadeNoise(input.position.xy + float2(31.0, 17.0));
        if (coverage < max(h, 0.03)) {
            discard;
        }
    } else if (coverage < impostor.atlas.w) {
        discard;
    }
    float3 albedo = (sa.colour.rgb * wa + sb.colour.rgb * wb + sc.colour.rgb * wc) / coverage;
    float3 normal = normalize(sa.normal * wa + sb.normal * wb + sc.normal * wc);
    float3 surface = (sa.hit * wa + sb.hit * wb + sc.hit * wc) / coverage;

    Instance inst = instances[input.instance];
    float4 world = mul(inst.model, float4(surface, 1.0));
    float3 n = normalize(worldNormal(inst.model, normal));
    float3 l = frame.lightDirection.xyz;
    float visible = sunVisibility(world.xyz, n, l, input.position.xy);
    float diffuse = max(dot(n, l), 0.0) * visible;
    float3 ambient = frame.ambient.rgb;
    if (impostor.shade.x > 0.5) {
        ambient = irradianceCube.SampleLevel(environmentSampler, n, 0.0).rgb * frame.viewport.w;
    }
    float3 direct = frame.lightColor.rgb * (1.0 / IMPOSTOR_PI);
    float3 colour = albedo * input.tint.rgb * (ambient + direct * diffuse);

    FragmentOutput output;
    output.color = float4(colour * frame.exposure.x, 1.0);
    float4 clip = mul(frame.viewProjection, world);
    output.depth = clip.z / clip.w;
    return output;
}
