// The render package's view of the frame so far, a Slang module
// ('import scenecapture;'): what the extended physically based fragment stage
// (pbr.slang's fragmentExtended, compiled into pbrx.spv) reads from group 3 to
// see the scene behind and around a surface: screen-space reflections, glass
// that refracts what is behind it, and the two tables the lobes read.
//
// GROUP 3 holds
// - the colour pyramid: a copy of the frame drawn so far (already multiplied
//   by the exposure, like everything a material writes), full size at level
//   0, each level below made from the one above by a 13-tap filter, so a
//   sample at a higher level is a blurrier picture (a rough reflection, frosted
//   glass);
// - a copy of the depth, as r32float;
// - the diffusion table (scenecapture.cone bakes it with capture.slang): the
//   pre-integrated, curvature-dependent diffuse of a translucent surface;
// - the sheen's directional-albedo table;
// - a sampler, and the capture's parameters.
// While nothing has been captured ('scene.info.y' is 0) the pyramid and the
// depth are one-texel stand-ins and every function here answers "nothing
// seen"; the tables are always real.
//
// SCREEN-SPACE RAY MARCH. A reflection ray is carried into clip space (with
// the segment clipped to the near plane), then marched in uniform steps of
// 'stride' pixels along its screen-space line, perspective-correct: 1/w and the
// depth z/w are both linear along that line (McGuire and Mara, "Efficient GPU
// Screen-Space Ray Tracing", JCGT 2014), so each step costs one depth read
// and no matrix. The ray hits where it passes from in front of the depth
// buffer's surface to behind it by less than the thickness (the surface is
// assumed 'thickness' deep, since the buffer holds only its front); the hit
// is refined by bisection. The first step is jittered by interleaved gradient
// noise so a stride's banding becomes grain a temporal filter removes. The
// colour is read from the pyramid at a level that grows with the distance the
// ray travelled and the surface's roughness (the cone of the lobe: Stachowiak
// and Uludag, "Stochastic Screen-Space Reflections", SIGGRAPH 2015, in the
// level-of-detail form of Uludag, "Hi-Z Screen-Space Cone-Tracing", GPU Pro 5).
// What the march cannot answer (the ray leaves the screen, goes behind a
// surface, or points back at the camera) it reports as a confidence below 1,
// and the material falls back, by that confidence, to the environment: the
// box-projected reflection probe whose box holds it, or the sky.

import renderframe;

struct SceneParams {
    float4 size;             // 0: the captured frame's width and height, and their reciprocals
    float4 info;             // 16: x the pyramid's last level; y 1 if a capture was made, 0 if not;
                             //     z, w the depth projection's A and B: a point at depth z is B / (z + A) in front of the camera
    float4 trace;            // 32: x the longest ray (metres); y the most steps; z the stride (pixels); w the thickness (metres)
    float4 fade;             // 48: x the roughness above which no ray is cast; y the screen-edge fade's width (a fraction of
                             //     the frame); z 1 / the distance over which a long ray's confidence fades; w the frame number
    float4 causticMin;       // 64: xyz the caustic volume's box, its least corner; w the strength (0: no caustics)
    float4 causticMax;       // 80: xyz its greatest corner; w the height of the rippled surface above it
    float4 causticWave;      // 96: x the ripples' height (metres), y the longest wavelength, z their speed (a time scale), w the index
};

[[vk::binding(0, 3)]] Texture2D sceneColor;
[[vk::binding(1, 3)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 3)]] Texture2D diffusionTable;
[[vk::binding(3, 3)]] Texture2D sheenTable;
[[vk::binding(4, 3)]] SamplerState sceneSampler;
[[vk::binding(5, 3)]] ConstantBuffer<SceneParams> scene;

static const float SCENE_SKY = 0.999999;

// Interleaved gradient noise (Jimenez, Call of Duty: Advanced Warfare, 2014),
// turned by the frame number so a temporal filter sees a sequence
float sceneNoise(float2 pixel) {
    float2 p = pixel + float(uint(scene.fade.w) % 16u) * 5.588238;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Whether a capture was made
bool sceneCaptured() {
    return scene.info.y > 0.5;
}

// The distance in front of the camera of a depth-buffer value
float sceneViewDepth(float z) {
    return scene.info.w / (z + scene.info.z);
}

// A world point to the frame's uv (v down) and its depth, through the
// frame's view-projection; w is 0 when the point is behind the camera
float3 sceneProject(float3 world, out float behind) {
    float4 c = mul(frame.viewProjection, float4(world, 1.0));
    behind = c.w > 1e-4 ? 0.0 : 1.0;
    float w = max(c.w, 1e-4);
    return float3(c.x / w * 0.5 + 0.5, 0.5 - c.y / w * 0.5, c.z / w);
}

// The pyramid at 'uv' and level 'lod', smoothed by four taps a texel of that
// level apart on the diagonals, so a high level's bilinear blocks do not show
// in a frosted glass or a rough reflection
float3 sceneSample(float2 uv, float lod) {
    float l = clamp(lod, 0.0, scene.info.x);
    if (l < 0.5) {
        return sceneColor.SampleLevel(sceneSampler, uv, l).rgb;
    }
    float2 texel = scene.size.zw * exp2(l) * 0.5;
    float3 c = sceneColor.SampleLevel(sceneSampler, uv + float2(-texel.x, -texel.y), l).rgb;
    c += sceneColor.SampleLevel(sceneSampler, uv + float2(texel.x, -texel.y), l).rgb;
    c += sceneColor.SampleLevel(sceneSampler, uv + float2(-texel.x, texel.y), l).rgb;
    c += sceneColor.SampleLevel(sceneSampler, uv + float2(texel.x, texel.y), l).rgb;
    return c * 0.25;
}

// The depth buffer's value at a pixel position (the nearest texel, inside the frame)
float sceneDepthAtPixel(float2 s) {
    int2 p = int2(clamp(s, float2(0.0, 0.0), scene.size.xy - 1.0));
    return sceneDepth.Load(int3(p, 0));
}

// The depth buffer's value at a uv
float sceneDepthAt(float2 uv) {
    return sceneDepthAtPixel(uv * scene.size.xy);
}

struct ScreenHit {
    float2 uv;               // where the ray hit, in the frame (v down), when 'confidence' is above 0
    float travel;            // how many pixels the ray travelled to it
    float confidence;        // 0: nothing found; 1: a clean hit well inside the frame
};

// The radiance (exposed) a ray of the lobe of 'roughness' sees at 'hit': the
// pyramid at the level the lobe's footprint at the hit covers
float3 screenHitColor(ScreenHit hit, float roughness) {
    float footprint = hit.travel * roughness * roughness * 1.2;
    return sceneSample(hit.uv, log2(max(footprint * 2.0, 1.0)));
}

// March the reflection ray from the world point 'p' in the direction 'r'
// (unit) for a surface of 'roughness' (perceptual), 'pixel' its fragment's
// position (the noise's seed). The caller falls back to the environment by
// 1 - confidence.
ScreenHit traceScreen(float3 p, float3 r, float roughness, float2 pixel) {
    ScreenHit miss;
    miss.uv = float2(0.0, 0.0);
    miss.travel = 0.0;
    miss.confidence = 0.0;
    if (!sceneCaptured() || roughness > scene.fade.x) {
        return miss;
    }
    float maxDist = scene.trace.x;
    float4 h0 = mul(frame.viewProjection, float4(p, 1.0));
    float4 h1 = mul(frame.viewProjection, float4(p + r * maxDist, 1.0));
    if (h0.w < 1e-3) {
        return miss;
    }
    // The segment's end, clipped to a plane just in front of the camera
    float nearW = 0.02;
    float endScale = 1.0;
    if (h1.w < nearW) {
        endScale = (nearW - h0.w) / (h1.w - h0.w);
        h1 = lerp(h0, h1, endScale);
    }
    float k0 = 1.0 / h0.w;
    float k1 = 1.0 / h1.w;
    float2 s0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5) * scene.size.xy;
    float2 s1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5) * scene.size.xy;
    float q0 = h0.z * k0;
    float q1 = h1.z * k1;
    float2 delta = s1 - s0;
    float len = length(delta);
    if (len < 2.0) {
        return miss;
    }
    // Shorten the line to where it leaves the frame
    float tEnd = 1.0;
    if (delta.x > 0.0) { tEnd = min(tEnd, (scene.size.x - 1.0 - s0.x) / delta.x); }
    if (delta.x < 0.0) { tEnd = min(tEnd, (0.0 - s0.x) / delta.x); }
    if (delta.y > 0.0) { tEnd = min(tEnd, (scene.size.y - 1.0 - s0.y) / delta.y); }
    if (delta.y < 0.0) { tEnd = min(tEnd, (0.0 - s0.y) / delta.y); }
    float reach = len * tEnd;
    if (reach < 2.0) {
        return miss;
    }
    float stride = scene.trace.z;
    int steps = min(int(scene.trace.y), int(reach / stride) + 1);
    float jitter = sceneNoise(pixel);
    float w0 = h0.w;
    float w1 = h1.w;
    float prevT = 0.0;
    float hitT = -1.0;
    float thickBase = scene.trace.w;
    for (int i = 0; i < steps; i++) {
        float t = ((float(i) + jitter) * stride + 1.5) / len;
        if (t > tEnd) {
            break;
        }
        float2 s = s0 + delta * t;
        float q = lerp(q0, q1, t);
        if (q >= 1.0 || q <= 0.0) {
            break;
        }
        float z = sceneDepthAtPixel(s);
        if (z < SCENE_SKY) {
            float dRay = sceneViewDepth(q);
            float dSurf = sceneViewDepth(z);
            float diff = dRay - dSurf;
            float thick = thickBase + dSurf * 0.03;
            if (diff > 0.004 * dSurf && diff < thick) {
                // Bisect between the last step in front and this one
                float lo = prevT;
                float hi = t;
                for (int b = 0; b < 6; b++) {
                    float mid = 0.5 * (lo + hi);
                    float2 sm = s0 + delta * mid;
                    float zm = sceneDepthAtPixel(sm);
                    float dm = sceneViewDepth(lerp(q0, q1, mid));
                    if (zm < SCENE_SKY && dm - sceneViewDepth(zm) > 0.0) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                hitT = hi;
                break;
            }
        }
        prevT = t;
    }
    if (hitT < 0.0) {
        return miss;
    }
    float2 sHit = s0 + delta * hitT;
    float2 uv = sHit * scene.size.zw;
    // Metres along the ray: w is linear along the clip segment
    float tWorld = saturate((1.0 / lerp(k0, k1, hitT) - w0) / (w1 - w0)) * endScale;
    float distance = tWorld * maxDist;
    ScreenHit hit;
    hit.uv = uv;
    hit.travel = hitT * len;
    // Confidence: away from the frame's edge, not a ray that points back at
    // the camera, not a very long one, not a very rough surface
    float2 edge = min(uv, 1.0 - uv) / max(scene.fade.y, 1e-3);
    float edgeFade = saturate(min(edge.x, edge.y));
    float toward = saturate(dot(r, normalize(frame.cameraPosition.xyz - p)) * -4.0 + 1.0);
    float farFade = saturate(1.0 - distance * scene.fade.z);
    float roughFade = saturate((scene.fade.x - roughness) / max(scene.fade.x * 0.4, 1e-3));
    hit.confidence = edgeFade * toward * farFade * roughFade;
    return hit;
}

// The pre-integrated diffuse of a translucent surface: the light a Lambert
// surface of cosine 'nol' (-1 to 1, not clamped) and curvature 'curvature'
// (1 / radius, per metre) gives, spread by scattering whose width is 'width'
// (the material's radius times a channel's scale, in the table's skin units) and
// so softened past the terminator (Penner and Borshukov, "Pre-Integrated Skin
// Shading", GPU Pro 2, 2011). For a flat surface, or none that scatters, it is
// the Lambert cosine clamped.
float diffusionLookup(float nol, float curvature, float width) {
    // The table's v is the square root of the curvature in the skin's own
    // units (per millimetre), up to 0.5 per millimetre: a 2 mm radius
    float perMm = curvature * 0.001 * width;
    float v = sqrt(saturate(perMm / 0.5));
    return diffusionTable.SampleLevel(sceneSampler, float2(nol * 0.5 + 0.5, v), 0.0).r;
}

// The sheen lobe's directional albedo for the view cosine 'nov' and roughness
float sheenAlbedo(float nov, float roughness) {
    return sheenTable.SampleLevel(sceneSampler, float2(nov, roughness), 0.0).r;
}

// CAUSTICS beneath a rippled surface (water, rippled glass): how much brighter or dimmer than
// unfocused the sun is at the world point 'world' of a receiver inside the caustic volume, 'l' the
// unit direction to the sun, 'amount' the receiver's share (0 to 1).
//
// The surface is a height field of four crossing sine waves (heights h = sum of a_i sin(k_i . x +
// phase_i + w_i t), each shorter and lower than the last, the speeds following the deep-water
// dispersion w ~ sqrt(k)). A ray of the sun enters at (x, y = surface), is bent by the slope, and
// lands at depth d displaced by -d (1 - 1/n) grad h, to first order in the slope; the light in a
// bundle of rays is spread by that map, so the brightness where they land is the reciprocal of
// the Jacobian's determinant, 1 / |det(I - d (1 - 1/n) H)|, H the Hessian of h (Stam, "Random
// Caustics: Natural Textures and Wave Theory Revisited", SIGGRAPH 1996 sketches, and the
// "focusing of a wavefront" of geometrical optics). Where the determinant goes to 0 the rays
// cross in a caustic line; the reciprocal is capped (at 5) as an area light would cap it. The
// receiver is looked up at the point where the sun's ray to it crosses the surface, which is
// the point it came through to first order. The effect fades out over a few depths of water:
// diffuse skylight does not focus.
float causticLight(float3 world, float3 l, float amount) {
    float strength = scene.causticMin.w * amount;
    if (strength <= 0.0) {
        return 1.0;
    }
    float3 lo = scene.causticMin.xyz;
    float3 hi = scene.causticMax.xyz;
    float depth = scene.causticMax.w - world.y;
    if (world.x < lo.x || world.y < lo.y || world.z < lo.z || world.x > hi.x || world.y > hi.y || world.z > hi.z
        || depth <= 0.0 || l.y < 0.05) {
        return 1.0;
    }
    float2 s = world.xz + l.xz * (depth / l.y);
    float time = frame.viewport.z * scene.causticWave.z;
    float k0 = 6.28318530718 / max(scene.causticWave.y, 1e-3);
    float hxx = 0.0;
    float hzz = 0.0;
    float hxz = 0.0;
    for (int i = 0; i < 4; i++) {
        float angle = 0.7 + 1.9 * float(i);
        float2 dir = float2(cos(angle), sin(angle));
        float k = k0 * (1.0 + 0.55 * float(i));
        float a = scene.causticWave.x / (1.0 + 0.8 * float(i));
        float phase = k * dot(dir, s) + time * sqrt(1.0 + 0.55 * float(i)) + 2.1 * float(i);
        float c = -a * k * k * sin(phase);
        hxx += c * dir.x * dir.x;
        hzz += c * dir.y * dir.y;
        hxz += c * dir.x * dir.y;
    }
    float kk = depth * (1.0 - 1.0 / max(scene.causticWave.w, 1.0001));
    float det = (1.0 - kk * hxx) * (1.0 - kk * hzz) - kk * kk * hxz * hxz;
    float focus = min(1.0 / max(abs(det), 0.2), 5.0);
    float fade = saturate(1.0 - depth / (6.0 * scene.causticWave.y));
    return max(1.0 + strength * fade * (focus - 1.0), 0.0);
}
