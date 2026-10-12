// The render package's point and spot lights as a shader sees them, a Slang
// module ('import locallights;'): lit.slang and pbr.slang loop over the
// lights of a fragment's cluster, found here, and ask here how much light
// each gives the point. The CPU's twin is lights.cone (the same formulas,
// 'pointFalloff', 'spotFalloff', and the grid it fills).
//
// CLUSTERED FORWARD SHADING (Olsson, Billeter and Assarsson, "Clustered
// Deferred and Forward Shading", HPG 2012; in a game, Persson's "Practical
// Clustered Shading", Avalanche Studios 2015, and Doom 2016: Sousa and
// Geoffroy, "The Devil is in the Details", SIGGRAPH 2016; in Godot 4 the
// same). The view frustum is cut into a grid of froxels: GRID tiles across
// and down the screen (equal in pixels) and SLICES slices of depth, spaced
// exponentially (the log of the depth, so a slice is as deep, in proportion,
// at 1 m as at 100 m; the first slice also holds everything nearer than the
// grid's near depth). Before the frame the CPU finds, for each froxel, which
// lights reach it (a light is a sphere of its range; a spot light's sphere
// is the cone's bounding sphere), and writes the lists to one buffer:
//   lightClusters[2 c], [2 c + 1]  the offset (in this buffer) and the count
//                                   of the lights of cluster c
//   lightClusters[offset + k]       the index of the k-th of them
// A fragment's cluster is its pixel's tile and the slice of its depth along
// the camera's forward.
//
// THE UNITS, the renderer's one convention (renderframe.slang). A light's
// colour here is its INTENSITY: the colour times the candela, so at distance
// d on the light's axis it gives an irradiance (lux) of that over d squared.
// A white diffuse surface facing a point light of I candela at d metres has
// a radiance of I / d^2 / pi nits, as a surface facing a sun of E lux has
// E / pi.
//
// THE FALLOFF. Inverse square, multiplied by a window that takes it
// smoothly to zero at the light's range (Karis, "Real Shading in Unreal
// Engine 4", SIGGRAPH 2013, course notes: saturate(1 - (d/R)^4)^2 / d^2),
// so a light touches only a bounded sphere and the clusters can be exact.
// A source of radius r (a bulb) does not give more than a point of distance
// r would: the denominator is at least r^2 (and a hair more, 1e-4 m^2, so a
// point source has none of infinity's).
//
// THE SPOT'S CONE. A smooth angular falloff between an inner cone (full) and
// an outer one (none): the square of the saturated linear ramp in the cosine
// of the angle to the axis (Filament's, from Frostbite's), a scale and an
// offset worked out once on the CPU.
//
// THE SPECULAR of a source of radius r > 0: the point of the sphere nearest
// the reflection ray stands in for the light's direction, and the lobe is
// renormalised for the width the sphere adds (Karis 2013, "representative
// point"): alpha' = saturate(alpha + r / (2 d)), the lobe scaled by
// (alpha / alpha')^2.
//
// LAYERS. A light has a mask of layers (bits 0 to 23) and a material has
// one: the light reaches a surface only where they share a bit. Both start
// as bit 0 alone, so every light lights every material until it is told
// otherwise; a bulb given bit 1, lighting the lantern's own materials (which
// have bits 0 and 1), lights its inside and not the whole path.

import renderframe;

struct LightParams {
    float4 forward;                // 0: xyz the camera's forward, unit length
    float4 grid;                   // 16: x tiles across, y tiles down, z slices of depth, w the lights in the scene (0: none, nothing to loop)
    float4 slices;                 // 32: x the depth at which slice 1 begins (the grid's near), y the slice scale, slices / ln(far / near),
                                   //     z 1 if the lights give highlights too (0: Lambert diffuse alone, the cheap setting)
};

[[vk::binding(7, 0)]] ConstantBuffer<LightParams> lightParams;
[[vk::binding(8, 0)]] StructuredBuffer<float4> lightData;       // four float4 a light (loadLocalLight)
[[vk::binding(9, 0)]] StructuredBuffer<uint> lightClusters;

struct LocalLight {
    float3 position;
    float invRange2;               // 1 / range^2: where its light ends
    float3 intensity;              // the colour times the candela
    float radius;                  // the source's, in metres
    float3 axis;                   // a spot light's axis, unit length (a point light's is zero)
    float layers;                  // the layer mask, as a float (24 bits are exact)
    bool spot;
    float spotScale;               // the cone's ramp: saturate(cos * scale + offset)
    float spotOffset;
};

// A light's row: float4 (position, 1 / range^2), (intensity, radius), (axis,
// layers), and for a spot light (scale, offset); a point light's fourth is
// not read
LocalLight loadLocalLight(uint i) {
    float4 a = lightData[i * 4 + 0];
    float4 b = lightData[i * 4 + 1];
    float4 c = lightData[i * 4 + 2];
    LocalLight l;
    l.position = a.xyz;
    l.invRange2 = a.w;
    l.intensity = b.xyz;
    l.radius = b.w;
    l.axis = c.xyz;
    l.layers = c.w;
    l.spot = dot(c.xyz, c.xyz) > 0.5;
    l.spotScale = 0.0;
    l.spotOffset = 0.0;
    if (l.spot) {
        float4 d = lightData[i * 4 + 3];
        l.spotScale = d.x;
        l.spotOffset = d.y;
    }
    return l;
}

// The cone's share for the cosine 'cosAngle' of the angle from the axis
float spotFalloff(float cosAngle, float scale, float offset) {
    float t = saturate(cosAngle * scale + offset);
    return t * t;
}

struct LightHit {
    float3 toLight;                // unit, to the light's centre
    float dist;
    float3 irradiance;             // what a surface facing the light gets here: lux, rgb
};

// What the light 'l' gives the point 'world': false if nothing. The window
// and the inverse square (lights.cone's pointFalloff): saturate(1 - x^2)^2 /
// max(d^2, r^2) for x = (d / range)^2
bool lightAt(LocalLight l, float3 world, out LightHit hit) {
    hit.toLight = float3(0.0, 0.0, 1.0);
    hit.dist = 1.0;
    hit.irradiance = float3(0.0, 0.0, 0.0);
    float3 d = l.position - world;
    float d2 = dot(d, d);
    float x = d2 * l.invRange2;
    if (x >= 1.0) {
        return false;
    }
    float inverse = rsqrt(max(d2, 1e-12));
    float3 toLight = d * inverse;
    float window = saturate(1.0 - x * x);
    float k = window * window / max(d2, 1e-4 + l.radius * l.radius);
    if (l.spot) {
        k *= spotFalloff(dot(-toLight, l.axis), l.spotScale, l.spotOffset);
    }
    if (k <= 0.0) {
        return false;
    }
    hit.toLight = toLight;
    hit.dist = d2 * inverse;
    hit.irradiance = l.intensity * k;
    return true;
}

// Whether a material on layers 'layers' is reached by the light
bool lightReaches(LocalLight l, uint layers) {
    return (uint(l.layers + 0.5) & layers) != 0;
}

// The cluster of a fragment at 'world', drawn at the pixel 'pixel': the
// index in lightClusters of its list's first entry, and the count
uint2 fragmentCluster(float3 world, float2 pixel) {
    uint nx = uint(lightParams.grid.x + 0.5);
    uint ny = uint(lightParams.grid.y + 0.5);
    uint nz = uint(lightParams.grid.z + 0.5);
    float depth = dot(world - frame.cameraPosition.xyz, lightParams.forward.xyz);
    float nearDepth = lightParams.slices.x;
    int slice = int(floor(log(max(depth, nearDepth) / nearDepth) * lightParams.slices.y));
    uint s = uint(clamp(slice, 0, int(nz) - 1));
    uint tx = min(uint(max(pixel.x, 0.0) / frame.viewport.x * float(nx)), nx - 1);
    uint ty = min(uint(max(pixel.y, 0.0) / frame.viewport.y * float(ny)), ny - 1);
    uint cluster = (s * ny + ty) * nx + tx;
    return uint2(lightClusters[cluster * 2], lightClusters[cluster * 2 + 1]);
}

// The standard lit material's share: for each light that reaches the point,
// its irradiance times the cosine goes into 'diffuse', and its irradiance
// times the Blinn-Phong lobe (the normal against the half vector, to the
// power 'shininess', where the surface faces the light) into 'specular'.
// The caller divides both by pi.
void litLocalLights(float3 world, float3 n, float3 v, float shininess, float2 pixel, uint layers,
        out float3 diffuse, out float3 specular) {
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    uint2 range = fragmentCluster(world, pixel);
    bool withSpecular = lightParams.slices.z > 0.5;
    for (uint k = 0; k < range.y; k++) {
        LocalLight l = loadLocalLight(lightClusters[range.x + k]);
        if (!lightReaches(l, layers)) {
            continue;
        }
        LightHit hit;
        if (!lightAt(l, world, hit)) {
            continue;
        }
        float nol = dot(n, hit.toLight);
        if (nol <= 0.0) {
            continue;
        }
        diffuse += hit.irradiance * nol;
        if (withSpecular) {
            float3 h = normalize(hit.toLight + v);
            specular += hit.irradiance * pow(max(dot(n, h), 0.0), shininess);
        }
    }
}

// A sphere light's representative point (Karis 2013): the direction from
// 'world' to the point of the sphere of radius 'radius' about 'l.position'
// nearest the reflection ray 'r' (unit), which stands in for the light
// direction where the lobe is sharp
float3 representativeDirection(LocalLight l, float3 world, float3 toLight, float dist, float3 r) {
    if (l.radius <= 0.0) {
        return toLight;
    }
    float3 centerToRay = dot(toLight * dist, r) * r - toLight * dist;
    float3 closest = toLight * dist + centerToRay * saturate(l.radius / max(length(centerToRay), 1e-6));
    return normalize(closest);
}
