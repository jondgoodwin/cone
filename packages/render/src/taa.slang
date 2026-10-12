// The render package's temporal anti-aliasing: the resolve (each frame's
// jittered picture blended into the history, reprojected to where it now is)
// and the sharpening that follows. Full-screen passes, compiled ahead of time
// to taa.spv by tools/shaders/shaders.py and embedded in taa.cone.
//
// THE METHOD, from the papers it names:
// - JITTER. Each frame the camera's projection is moved by a sub-pixel offset
//   from the Halton (2, 3) sequence (Karis, "High Quality Temporal
//   Supersampling", SIGGRAPH 2014), so over the sequence every pixel sees the
//   scene at many positions. taa.cone makes the offsets.
// - REPROJECTION. The history is looked up where the pixel was last frame: a
//   static pixel by its depth and the camera's last matrix, a moving object's
//   by the velocity its own pass wrote (taavelocity.slang). The velocity is
//   taken from the CLOSEST pixel of the 3 x 3 around (Karis 2014; Pedersen,
//   "Temporal Reprojection Anti-Aliasing in INSIDE", GDC 2016), so an edge's
//   pixels follow the object in front, not the background behind it.
// - HISTORY FILTER. The history is read with a Catmull-Rom bicubic filter in
//   five taps (Jimenez, "Filmic SMAA", SIGGRAPH 2016, after Sousa and
//   Jarosz), which blurs far less than a bilinear read when it is carried by
//   a motion that is not whole pixels.
// - NEIGHBOURHOOD CLIPPING. The history is clipped to a box in YCoCg around
//   the colours of the current frame's 3 x 3: the box of their mean and
//   standard deviation (variance clipping: Salvi, "An Excursion in Temporal
//   Supersampling", GDC 2016), cut to their min and max (Karis). What the
//   history holds that the picture no longer can (a thing that moved away, a
//   light that changed) is pulled back to what the picture can show, which
//   removes ghosts.
// - ACCUMULATION. The history's alpha counts the frames it averages. A new
//   frame is given the weight 1 / (count + 1), a running average, up to a
//   floor of 1 / maxSamples (an exponential average after), so a still picture
//   converges to the mean of the jitter sequence (a supersampled picture), and a
//   disoccluded pixel (count 0) takes the new colour whole. Weighted by
//   1 / (1 + luma) (Karis 2014) so that one bright sample cannot dominate:
//   the average is of the colours as a tone mapper will show them.
// - SHARPENING, last and outside the history: contrast-adaptive sharpening
//   (Lottes, AMD FidelityFX CAS, 2019) of the resolved picture, in values
//   compressed to 0..1, which gives back what the temporal filter softened.
//
// Group 0: the frame uniform (0), a linear clamped sampler (1), the current
// frame (2), the history (3), the depth (4: depth32float, read by load) and
// the velocity of the moving objects (5: rg the uv displacement to where the
// pixel was, a 1 where an object wrote it). The immediates: taa.cone's
// TaaImmediates.

import fxcommon;

[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;
[[vk::binding(1, 0)]] SamplerState linearClamp;
[[vk::binding(2, 0)]] Texture2D<float4> currentFrame;
[[vk::binding(3, 0)]] Texture2D<float4> historyFrame;
[[vk::binding(4, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(5, 0)]] Texture2D<float4> velocityFrame;

struct TaaImmediates {
    float maxSamples;              // 0: the floor of a new frame's weight is 1 / this
    float clipGamma;               // 4: the variance box's half-width in standard deviations
    float hdrWeight;               // 8: 1 weights by 1 / (1 + luma), 0 not at all
    float sharpness;               // 12: 0 to 1
    uint flags;                    // 16: 1 the velocity texture holds moving objects
    float motionBlend;             // 20: the weight a new frame has at least, at 8 pixels of motion a frame
    float pad0;
    float pad1;
};

[[vk::push_constant]] ConstantBuffer<TaaImmediates> taa;

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
FullscreenOutput fullscreenMain(uint id : SV_VulkanVertexID) {
    FullscreenOutput output;
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y);
    return output;
}

float3 sane(float3 c) {
    return min(max(c, float3(0.0, 0.0, 0.0)), float3(65000.0, 65000.0, 65000.0));
}

float3 loadColor(int2 p, int2 size) {
    return sane(currentFrame.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb);
}

// The history at 'uv' by a Catmull-Rom filter in five bilinear taps
float4 sampleHistory(float2 uv, float2 size) {
    float2 samplePos = uv * size;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);
    float2 texPos0 = (texPos1 - 1.0) / size;
    float2 texPos3 = (texPos1 + 2.0) / size;
    float2 texPos12 = (texPos1 + offset12) / size;
    float4 result = historyFrame.SampleLevel(linearClamp, float2(texPos12.x, texPos0.y), 0.0) * (w12.x * w0.y)
        + historyFrame.SampleLevel(linearClamp, float2(texPos0.x, texPos12.y), 0.0) * (w0.x * w12.y)
        + historyFrame.SampleLevel(linearClamp, float2(texPos12.x, texPos12.y), 0.0) * (w12.x * w12.y)
        + historyFrame.SampleLevel(linearClamp, float2(texPos3.x, texPos12.y), 0.0) * (w3.x * w12.y)
        + historyFrame.SampleLevel(linearClamp, float2(texPos12.x, texPos3.y), 0.0) * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return result / weight;
}

// 'q' clipped into the box about 'center' of half-extent 'extent', along the
// line to the centre (Playdead's, "Temporal Reprojection Anti-Aliasing in
// INSIDE", after Karis)
float3 clipToBox(float3 center, float3 extent, float3 q) {
    float3 v = q - center;
    float3 a = abs(v / max(extent, float3(1e-5, 1e-5, 1e-5)));
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) {
        return center + v / m;
    }
    return q;
}

[shader("fragment")]
float4 resolveMain(FullscreenOutput input) : SV_Target {
    int2 p = int2(input.position.xy);
    int2 size = int2(fx.screen.xy);
    float2 texel = fx.screen.zw;
    float2 uv = (float2(p) + 0.5) * texel;

    // The current frame's 3 x 3, in YCoCg: its mean, spread, min and max; and
    // the closest depth among them
    float3 current = float3(0.0, 0.0, 0.0);
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    float3 lo = float3(1e9, 1e9, 1e9);
    float3 hi = float3(-1e9, -1e9, -1e9);
    float closest = 2.0;
    int2 closestAt = int2(0, 0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), size - 1);
            float3 c = loadColor(q, size);
            if (x == 0 && y == 0) {
                current = c;
            }
            float3 yc = rgbToYCoCg(c);
            m1 += yc;
            m2 += yc * yc;
            lo = min(lo, yc);
            hi = max(hi, yc);
            float d = sceneDepth.Load(int3(q, 0));
            if (d < closest) {
                closest = d;
                closestAt = q;
            }
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, float3(0.0, 0.0, 0.0)));
    float3 boxLo = max(lo, mean - taa.clipGamma * sigma);
    float3 boxHi = min(hi, mean + taa.clipGamma * sigma);

    // Where the pixel was last frame: by its object's own velocity if a
    // moving object wrote one, else by the camera's motion and its depth
    float2 closestUv = (float2(closestAt) + 0.5) * texel;
    float2 velocity;
    float4 written = float4(0.0, 0.0, 0.0, 0.0);
    if ((taa.flags & 1u) != 0u) {
        written = velocityFrame.Load(int3(closestAt, 0));
    }
    if (written.a > 0.5) {
        velocity = written.xy;
    } else {
        float3 world = worldPosition(fx, closestUv, closest);
        float3 before = projectToUv(fx.previousViewProjection, world);
        velocity = before.xy - closestUv;
    }
    float2 historyUv = uv + velocity;

    float count = 0.0;
    float3 history = current;
    bool usable = fx.misc.z < 0.5 && historyUv.x >= 0.0 && historyUv.x <= 1.0 && historyUv.y >= 0.0 && historyUv.y <= 1.0;
    if (usable) {
        float4 h = sampleHistory(historyUv, float2(size));
        history = sane(h.rgb);
        count = h.a;
    }

    float3 yHistory = rgbToYCoCg(history);
    float3 center = mean;
    float3 extent = max(0.5 * (boxHi - boxLo), float3(1e-4, 1e-4, 1e-4));
    center = 0.5 * (boxHi + boxLo);
    float3 yClipped = clipToBox(center, extent, yHistory);
    // How far the clip moved it: a history the picture disagrees with
    // forgets most of what it counted
    float moved = length(yClipped - yHistory);
    float scale = max(length(yHistory - mean), 1e-3);
    count = count * (1.0 - saturate(moved / (0.25 * scale + 0.02)));
    history = max(yCoCgToRgb(yClipped), float3(0.0, 0.0, 0.0));

    // The weight of the new frame: a running average while the history is
    // short, an exponential one after; more in motion, where the history is
    // blurred by being carried
    float alpha = max(1.0 / (count + 1.0), 1.0 / taa.maxSamples);
    float speed = length(velocity * fx.screen.xy);
    alpha = max(alpha, taa.motionBlend * saturate(speed / 8.0));
    if (!usable) {
        alpha = 1.0;
    }
    float wc = alpha / (1.0 + taa.hdrWeight * luma(current));
    float wh = (1.0 - alpha) / (1.0 + taa.hdrWeight * luma(history));
    float3 result = (current * wc + history * wh) / max(wc + wh, 1e-6);
    float newCount = usable ? min(count + 1.0, taa.maxSamples) : 1.0;
    return float4(result, newCount);
}

// Contrast-adaptive sharpening of the resolved picture (Lottes, FidelityFX CAS):
// the cross of neighbours, the weight of each negative and the less the
// nearer the signal is to its limits, in values compressed to 0..1
float3 compress(float3 c) {
    return c / (1.0 + c);
}

float3 expand(float3 c) {
    return c / max(float3(1.0, 1.0, 1.0) - c, float3(1e-4, 1e-4, 1e-4));
}

[shader("fragment")]
float4 sharpenMain(FullscreenOutput input) : SV_Target {
    int2 p = int2(input.position.xy);
    int2 size = int2(fx.screen.xy);
    float4 centre = currentFrame.Load(int3(p, 0));
    float3 e = compress(sane(centre.rgb));
    float3 b = compress(loadColor(p + int2(0, -1), size));
    float3 d = compress(loadColor(p + int2(-1, 0), size));
    float3 f = compress(loadColor(p + int2(1, 0), size));
    float3 h = compress(loadColor(p + int2(0, 1), size));
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 amp = sqrt(saturate(min(mn, float3(1.0, 1.0, 1.0) - mx) / max(mx, float3(1e-4, 1e-4, 1e-4))));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(taa.sharpness));
    float3 w = amp * peak;
    float3 sharp = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(expand(saturate(sharp)), 1.0);
}
