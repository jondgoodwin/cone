// Ray-traced glass, soap film and reflections: the part of the extended material that runs only where the
// card has ray tracing. This file is not a shader or a module: pbr.slang includes it, inside
// `#ifdef PBR_RT`, ahead of fragmentExtended, so that it can read the environment, the frame and the
// capture that pbr.slang and scenecapture.slang declare. It is compiled into pbrxrt.spv only
// (pbrxrt.slang defines PBR_EXTENDED and PBR_RT); pbr.spv and pbrx.spv do not have it, and a card without ray
// tracing never loads it.
//
// THE METHOD. A glass or soap-film fragment is rasterised as before (the front face of the object, at the
// depth the mesh gives), and from that point rays are cast with ray queries into the top-level structure the
// renderer built this frame from every mesh in the draw list (raytrace.cone): the sphere's inside, the far
// wall, the glass beside it, the floor, whatever is off the screen. Each ray is followed by a Whitted-style
// stack (Whitted, "An improved illumination model for shaded display", CACM 1980): at an interface the
// light splits by Fresnel's equations into a reflected ray and a transmitted one, each pushed; the stack is
// at most RT_STACK deep and RT_SEGMENTS rays long. What a ray finally meets decides its light:
// - the sky: the same analytic sky the sky pass draws, so a ball shows the sky it would be drawn over;
// - an opaque surface the camera also sees, whose picture the renderer already has: the frame captured
//   before the glass (scenecapture.slang), read where the hit projects, when the depth there agrees. This
//   is the whole shading of that surface, textures and point lights and shadows included, and it is why a
//   ball held near a figure shows the figure as drawn;
// - an opaque surface the camera does not see (behind the ball, off the screen, hidden): its record's base
//   colour lit by the sun (a shadow ray) and the environment, simply. This is the part a screen-space
//   method cannot do at all.
// - glass or film: the interface rule below.
//
// INTERFACES. A solid glass (a record of kind SOLID) bends the ray by Snell's law with the exact
// dielectric Fresnel reflectance (the mean of s and p) between the indices on either side; the ray is
// absorbed by Beer-Lambert's law over the distance it travels inside (rate = -ln(attenuationColor) /
// attenuationDistance, per colour channel); a dispersing glass sends three single-colour rays on entering,
// the indices spread by (ior - 1) 0.025 dispersion either side (glTF's KHR_materials_dispersion, as the
// raster path does). Total internal reflection is Fresnel's own (reflectance 1). The thickness the raster
// path assumes ('glass.z', a slab) is not used: the mesh is the volume.
// A soap film (SOLID's twin, kind FILM: a material with a film and a slab thickness under a millimetre)
// is a sheet of no thickness at each of its two faces: the ray goes straight on, the reflected share is the
// film's reflectance, Belcour and Barla's spectral thin-film interference (brdf.slang's thinFilmReflectance:
// Airy's series, Fourier-space sensitivities fitted to the CIE colour matching functions, both
// polarizations, exact Fresnel and phases), of the film's thickness at the point (the material's noise in
// the object's own coordinates) in air on both sides; what is not reflected passes. A bubble is a
// closed shell of one surface, so a ray meets two films, front and back.
//
// SHADOWS. The sun's light through glass is tinted: a ray to the sun that meets glass is multiplied by what
// the glass passes (absorption inside, the Fresnel loss at its faces, the film's transmission).

[[vk::binding(6, 3)]] RaytracingAccelerationStructure rtScene;
[[vk::binding(7, 3)]] StructuredBuffer<RtRecord> rtRecords;
[[vk::binding(8, 3)]] StructuredBuffer<float> rtNormals;
[[vk::binding(9, 3)]] StructuredBuffer<uint> rtIndices;

// One thing in the top-level structure, as raytrace.cone's RtRecord writes it. The instance's custom index is
// its place in this buffer.
struct RtRecord {
    float4 a;                      // 0: rgb the base colour; w the metallic
    float4 b;                      // 16: x the perceptual roughness; y the index of refraction; z the kind (RT_OPAQUE, RT_SOLID, RT_FILM);
                                   //     w the share of light that passes (glass)
    float4 c;                      // 32: as PbrParams.attenuation: rgb the colour light has after w metres inside
    float4 d;                      // 48: the film as PbrParams.film: least and greatest thickness (nm), the index, 1 if a film
    float4 e;                      // 64: the film's noise as PbrParams.filmNoise: scale, drift, warp, seed
    float4 f;                      // 80: rgb the emitted radiance; w the dispersion
    uint4 geometry;                // 96: x the first normal (in vertices), y the first index, zw unused
};

static const float RT_OPAQUE = 0.0;
static const float RT_SOLID = 1.0;
static const float RT_FILM = 2.0;
static const int RT_STACK = 8;
static const int RT_SEGMENTS = 24;
static const float RT_FAR = 10000.0;
static const float RT_EPSILON = 0.002;

struct RtRay {
    float3 o;
    float3 d;
    float3 w;                      // the share of the light still carried, per colour channel
    float3 rate;                   // the absorption per metre of the glass the ray is inside (0: in air)
    int chan;                      // -1: all colours; 0, 1, 2: one colour of a dispersed ray
    int depth;                     // reflections taken
};

struct RtHit {
    float t;
    float3 position;
    float3 normal;                 // the interpolated normal in world coordinates, unit, as the mesh faces it
    float3 local;                  // the hit in the instance's own coordinates
    uint record;
};

static RtRay rtStack[RT_STACK];
static int rtSp;

float3 rtLoadNormal(uint v) {
    uint i = v * 3u;
    return float3(rtNormals[i], rtNormals[i + 1u], rtNormals[i + 2u]);
}

// The nearest thing a ray meets among the instances whose mask shares a bit with 'mask'
bool rtCast(float3 o, float3 d, float tMin, uint mask, out RtHit h) {
    h.t = 0.0;
    h.position = o;
    h.normal = float3(0.0, 1.0, 0.0);
    h.local = o;
    h.record = 0u;
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = tMin;
    ray.TMax = RT_FAR;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(rtScene, RAY_FLAG_NONE, mask, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return false;
    }
    h.t = q.CommittedRayT();
    h.record = q.CommittedInstanceID();
    uint prim = q.CommittedPrimitiveIndex();
    float2 bary = q.CommittedTriangleBarycentrics();
    RtRecord r = rtRecords[h.record];
    uint i0 = rtIndices[r.geometry.y + prim * 3u];
    uint i1 = rtIndices[r.geometry.y + prim * 3u + 1u];
    uint i2 = rtIndices[r.geometry.y + prim * 3u + 2u];
    float3 n = rtLoadNormal(r.geometry.x + i0) * (1.0 - bary.x - bary.y) + rtLoadNormal(r.geometry.x + i1) * bary.x
        + rtLoadNormal(r.geometry.x + i2) * bary.y;
    // The normal to world coordinates by the inverse transpose of the instance's transform
    float3x4 w2o = q.CommittedWorldToObject3x4();
    h.position = o + d * h.t;
    float3 nw = n.x * w2o[0].xyz + n.y * w2o[1].xyz + n.z * w2o[2].xyz;
    h.normal = normalize(nw);
    h.local = float3(dot(w2o[0].xyz, h.position) + w2o[0].w, dot(w2o[1].xyz, h.position) + w2o[1].w,
        dot(w2o[2].xyz, h.position) + w2o[2].w);
    return true;
}

// The film's thickness (nanometres) at the instance's own point 'p', as filmThickness reads the material's
float rtFilmThickness(RtRecord r, float3 p) {
    float3 q = p * r.e.x + float3(0.31, 1.0, 0.17) * (frame.viewport.z * r.e.y);
    float w = warp3(q, uint(r.e.w), 2, r.e.z);
    float k = saturate(0.5 + 0.8 * w);
    return lerp(r.d.x, r.d.y, k);
}

// The index of refraction a ray of colour 'chan' sees in a glass of 'r'
float rtIor(RtRecord r, int chan) {
    float ior = max(r.b.y, 1.0);
    if (chan < 0) {
        return ior;
    }
    float spread = (ior - 1.0) * 0.025 * r.f.w;
    return ior + float(chan - 1) * spread;
}

// The absorption per metre inside a glass (Beer-Lambert's law): the colour left after the attenuation distance
float3 rtRate(RtRecord r) {
    return -log(max(r.c.rgb, float3(1e-4, 1e-4, 1e-4))) / max(r.c.w, 1e-4);
}

float rtLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What the sky and the environment give a ray that meets nothing; 'cheap' reads the prefiltered cube (a
// reflection that is a small share of the light), else the sky as the sky pass draws it
float3 rtSky(float3 d, bool cheap) {
    if (cheap) {
        return specularCube.SampleLevel(environmentSampler, d, 0.0).rgb * frame.viewport.w;
    }
    return skyRadiance(d, envParams.sky, true);
}

// Light passing from the point 'o' along 'dir' to the sun, through glass: 1 for open sky; less (and coloured)
// where glass is in the way; 0 where something opaque is. 'mask' 2 looks at glass alone (the shadow map
// decides the rest), 255 at everything.
float3 rtTransmittance(float3 o, float3 dir, uint mask) {
    float3 tint = float3(1.0, 1.0, 1.0);
    float3 start = o;
    float3 rate = float3(0.0, 0.0, 0.0);
    bool inside = false;
    for (int k = 0; k < 6; k++) {
        RtHit h;
        if (!rtCast(start, dir, RT_EPSILON, mask, h)) {
            break;
        }
        RtRecord r = rtRecords[h.record];
        if (r.b.z < 0.5) {
            return float3(0.0, 0.0, 0.0);
        }
        if (r.b.z > 1.5) {
            // A film passes what it does not reflect; a sheet this thin loses a few percent on average
            tint *= 0.95;
        } else if (dot(dir, h.normal) < 0.0) {
            // Into a glass: a few percent lost at the face, and the absorption starts. The distance to the
            // next face is taken from here, so the next hit's 't' is the path inside
            tint *= 0.92;
            rate = rtRate(r);
            inside = true;
        } else {
            // Out of it: absorbed over the distance travelled inside
            if (inside) {
                tint *= exp(-rate * h.t);
            }
            tint *= 0.92;
            inside = false;
            rate = float3(0.0, 0.0, 0.0);
        }
        start = h.position + dir * RT_EPSILON;
        if (rtLuminance(tint) < 0.01) {
            break;
        }
    }
    return tint;
}

// The light of an opaque surface the ray 'ray' met at 'h', looked at from where the ray came: as the camera saw it,
// when it did, else simply
float3 rtShadeOpaque(RtRecord r, RtHit h, float3 nf, float3 toEye) {
    float exposure = max(frame.exposure.x, 1e-9);
    // The camera's own picture of that point, if the depth buffer agrees it is the surface the camera sees
    if (sceneCaptured()) {
        float behind;
        float3 uvz = sceneProject(h.position, behind);
        if (behind < 0.5 && uvz.x > 0.0 && uvz.x < 1.0 && uvz.y > 0.0 && uvz.y < 1.0) {
            float stored = sceneDepthAt(uvz.xy);
            if (stored < SCENE_SKY) {
                float seen = sceneViewDepth(stored);
                float mine = sceneViewDepth(uvz.z);
                if (abs(seen - mine) < 0.02 + 0.015 * mine) {
                    return sceneSample(uvz.xy, 0.0) / exposure;
                }
            }
        }
    }
    float3 albedo = r.a.rgb;
    float metallic = r.a.w;
    float roughness = clamp(r.b.x, 0.045, 1.0);
    float3 diffuseColor = albedo * (1.0 - metallic);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 l = frame.lightDirection.xyz;
    float nol = saturate(dot(nf, l));
    float3 sun = float3(0.0, 0.0, 0.0);
    if (nol > 0.0) {
        sun = diffuseColor * (nol / PI) * frame.lightColor.rgb * rtTransmittance(h.position + nf * RT_EPSILON, l, 255u);
    }
    float strength = frame.viewport.w;
    float3 irradiance = irradianceCube.SampleLevel(environmentSampler, nf, 0.0).rgb * strength;
    float2 ab = brdfTable.SampleLevel(environmentSampler, float2(saturate(dot(nf, toEye)), roughness), 0.0).xy;
    float3 prefiltered = specularCube.SampleLevel(environmentSampler, reflect(-toEye, nf),
        roughness * (SPECULAR_LEVELS - 1.0)).rgb * strength;
    return sun + diffuseColor * irradiance + prefiltered * (f0 * ab.x + ab.y) + r.f.rgb;
}

void rtPush(RtRay r) {
    if (rtSp < RT_STACK) {
        rtStack[rtSp] = r;
        rtSp = rtSp + 1;
    }
}

// The light 'ray' sends back along the reflection 'rdir' leaving 'p' with the weight 'w': followed by the stack
// when it matters and the ray has not already reflected, else the environment's cheap answer
void rtReflected(RtRay ray, float3 p, float3 rdir, float3 w, inout float3 result) {
    if (rtLuminance(w) < 0.004) {
        return;
    }
    if (rtLuminance(w) > 0.5 * rtLuminance(ray.w) && rtSp < RT_STACK - 1) {
        // Most of the light reflects (total internal reflection, a grazing face): the ray goes on as itself
        RtRay r = ray;
        r.o = p;
        r.d = rdir;
        r.w = w;
        rtPush(r);
    } else if (ray.depth == 0 && rtLuminance(w) > 0.02 && rtSp < RT_STACK - 1) {
        RtRay r = ray;
        r.o = p;
        r.d = rdir;
        r.w = w;
        r.depth = 1;
        rtPush(r);
    } else {
        result += w * rtSky(rdir, true);
    }
}

// What happens to 'ray' at the face of 'r' it met at 'p', whose normal facing the ray is 'nf'; 'entering' when
// the face is a glass's outer side. The rays that go on are pushed on the stack; light that goes to
// the environment is added to 'result'.
void rtInterface(RtRecord r, RtRay ray, float3 p, float3 nf, bool entering, float3 local, inout float3 result) {
    float cosI = saturate(dot(-ray.d, nf));
    float3 rdir = reflect(ray.d, nf);
    if (r.b.z > 1.5) {
        // A film: it reflects the film's share of each colour and passes the rest straight on
        float thickness = rtFilmThickness(r, local);
        float3 refl = clamp(thinFilmReflectance(cosI, r.d.z, 1.0, thickness), float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
        rtReflected(ray, p + nf * RT_EPSILON, rdir, ray.w * refl, result);
        RtRay t = ray;
        t.o = p + ray.d * RT_EPSILON;
        t.w = ray.w * (float3(1.0, 1.0, 1.0) - refl);
        rtPush(t);
        return;
    }
    // Solid glass
    if (entering && r.f.w > 0.0 && ray.chan < 0) {
        // A dispersing glass: three single-colour rays, each with its own index
        for (int ch = 0; ch < 3; ch++) {
            float ior = rtIor(r, ch);
            float2 rr;
            float2 phase;
            float ct2;
            fresnelDielectricSP(cosI, 1.0, ior, rr, phase, ct2);
            float F = 0.5 * (rr.x + rr.y);
            float3 mask = float3(ch == 0 ? 1.0 : 0.0, ch == 1 ? 1.0 : 0.0, ch == 2 ? 1.0 : 0.0);
            float3 refr = refract(ray.d, nf, 1.0 / ior);
            if (dot(refr, refr) > 1e-6) {
                RtRay t = ray;
                t.d = normalize(refr);
                t.o = p + t.d * RT_EPSILON;
                t.w = ray.w * mask * (1.0 - F) * r.a.rgb;
                t.rate = rtRate(r);
                t.chan = ch;
                rtPush(t);
            }
            rtReflected(ray, p + nf * RT_EPSILON, rdir, ray.w * mask * F, result);
        }
        return;
    }
    float n1 = entering ? 1.0 : rtIor(r, ray.chan);
    float n2 = entering ? rtIor(r, ray.chan) : 1.0;
    float2 rr;
    float2 phase;
    float ct2;
    fresnelDielectricSP(cosI, n1, n2, rr, phase, ct2);
    float F = 0.5 * (rr.x + rr.y);
    rtReflected(ray, p + nf * RT_EPSILON, rdir, ray.w * F, result);
    float3 refr = refract(ray.d, nf, n1 / n2);
    if (dot(refr, refr) > 1e-6) {
        RtRay t = ray;
        t.d = normalize(refr);
        t.o = p + t.d * RT_EPSILON;
        t.w = ray.w * (1.0 - F) * (entering ? r.a.rgb : float3(1.0, 1.0, 1.0));
        t.rate = entering ? rtRate(r) : float3(0.0, 0.0, 0.0);
        rtPush(t);
    }
}

// Follow the rays on the stack until it is empty (or the budget is spent) and add what they find
float3 rtRun(float3 result) {
    int segments = 0;
    while (rtSp > 0 && segments < RT_SEGMENTS) {
        rtSp = rtSp - 1;
        RtRay ray = rtStack[rtSp];
        segments++;
        RtHit h;
        if (!rtCast(ray.o, ray.d, RT_EPSILON, 255u, h)) {
            result += ray.w * rtSky(ray.d, ray.depth > 0);
            continue;
        }
        RtRay moved = ray;
        // Absorption over the distance travelled inside a glass
        moved.w = ray.w * exp(-ray.rate * h.t);
        RtRecord r = rtRecords[h.record];
        bool entering = dot(ray.d, h.normal) < 0.0;
        float3 nf = entering ? h.normal : -h.normal;
        if (r.b.z < 0.5) {
            result += moved.w * rtShadeOpaque(r, h, nf, -ray.d);
            continue;
        }
        // A ray that is inside a glass meets that glass's inner face; one outside, a glass's outer face
        bool outer = (r.b.z > 1.5) ? true : entering;
        rtInterface(r, moved, h.position, nf, outer, h.local, result);
    }
    return result;
}

// The light that comes to a glass or film fragment of the material 'self' at 'p' (the object's own point 'local',
// its unit normal 'n' facing out), seen along 'dir' (from the eye): the reflection and the transmission both.
float3 rtGlass(RtRecord self, float3 p, float3 local, float3 n, float3 dir) {
    rtSp = 0;
    float3 result = float3(0.0, 0.0, 0.0);
    RtRay ray;
    ray.o = p;
    ray.d = dir;
    ray.w = float3(1.0, 1.0, 1.0);
    ray.rate = float3(0.0, 0.0, 0.0);
    ray.chan = -1;
    ray.depth = 0;
    rtInterface(self, ray, p, n, true, local, result);
    return rtRun(result);
}

// The light that comes along a reflection ray: what a mirror-like surface at 'p' shows in the direction 'r'
float3 rtReflection(float3 p, float3 r) {
    rtSp = 0;
    RtRay ray;
    ray.o = p;
    ray.d = r;
    ray.w = float3(1.0, 1.0, 1.0);
    ray.rate = float3(0.0, 0.0, 0.0);
    ray.chan = -1;
    ray.depth = 0;
    rtPush(ray);
    return rtRun(float3(0.0, 0.0, 0.0));
}
