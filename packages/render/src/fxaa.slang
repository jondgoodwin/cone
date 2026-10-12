// The render package's FXAA: fast approximate anti-aliasing of the finished
// picture, the cheap tier (fx.cone). One full-screen pass over the tone-mapped
// frame, compiled ahead of time to fxaa.spv by tools/shaders/shaders.py and
// embedded in fxaa.cone.
//
// THE METHOD is Timothy Lottes's FXAA 3.11 (NVIDIA, 2011), the quality
// version, in its published steps:
// 1. Luma of the pixel and its four neighbours. If their contrast is under a
//    threshold (the larger of an absolute floor and a share of the brightest)
//    the pixel is not on an edge and is left alone.
// 2. From the eight neighbours, whether the edge is horizontal or vertical,
//    and which side of the pixel it lies on (the greater luma gradient).
// 3. A search along the edge, in both directions, for its ends (where the
//    luma across it stops differing as it did at the pixel), at growing steps
//    (1, 1, 1, 1, 1, 1.5, 2, 2, 2, 2, 4, 8 pixels), at most twelve.
// 4. The pixel is moved across the edge by the fraction of the way to the
//    nearer end that its position along the edge gives (the sub-pixel
//    position of the edge) and read there with the hardware's bilinear
//    filter; where the end of the edge is a bad one (the luma does not step
//    the right way), by 0.
// 5. A blend against sub-pixel aliasing (a lone bright pixel in a dark
//    field), by the local contrast, never more than the 'subpixel' share.
//
// The picture is read as linear light (the target is sRGB, so the sampler
// decodes it); luma is taken as the square root of the linear luma, an
// approximation of perceptual lightness, as FXAA's own input would be
// gamma-encoded.

struct FxaaImmediates {
    float2 rcpFrame;               // 1 / width, 1 / height
    float subpixel;                // 0 to 1: how much sub-pixel aliasing is removed (0.75)
    float edgeThreshold;           // the share of the brightest luma a contrast must reach (0.166; 0.125 is sharper)
    float edgeThresholdMin;        // and the floor under it (0.0833)
    float pad0;
    float pad1;
    float pad2;
};

[[vk::binding(0, 0)]] Texture2D frameTexture;
[[vk::binding(1, 0)]] SamplerState linearClamp;
[[vk::push_constant]] ConstantBuffer<FxaaImmediates> fxaa;

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

float3 fetch(float2 uv) {
    return frameTexture.SampleLevel(linearClamp, uv, 0.0).rgb;
}

float lumaOf(float3 c) {
    return sqrt(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0));
}

float lumaAt(float2 uv) {
    return lumaOf(fetch(uv));
}

float lumaOffset(float2 uv, float2 offset) {
    return lumaOf(fetch(uv + offset * fxaa.rcpFrame));
}

// The step lengths of the end-of-edge search (FXAA 3.11 quality preset 29)
static const float STEPS[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

[shader("fragment")]
float4 fxaaMain(FullscreenOutput input) : SV_Target {
    float2 posM = input.uv;
    float2 rcp = fxaa.rcpFrame;
    float3 rgbM = fetch(posM);
    float lumaM = lumaOf(rgbM);
    float lumaS = lumaOffset(posM, float2(0.0, 1.0));
    float lumaE = lumaOffset(posM, float2(1.0, 0.0));
    float lumaN = lumaOffset(posM, float2(0.0, -1.0));
    float lumaW = lumaOffset(posM, float2(-1.0, 0.0));
    float maxSM = max(lumaS, lumaM);
    float minSM = min(lumaS, lumaM);
    float maxESM = max(lumaE, maxSM);
    float minESM = min(lumaE, minSM);
    float maxWN = max(lumaN, lumaW);
    float minWN = min(lumaN, lumaW);
    float rangeMax = max(maxWN, maxESM);
    float rangeMin = min(minWN, minESM);
    float range = rangeMax - rangeMin;
    if (range < max(fxaa.edgeThresholdMin, rangeMax * fxaa.edgeThreshold)) {
        return float4(rgbM, 1.0);
    }
    float lumaNW = lumaOffset(posM, float2(-1.0, -1.0));
    float lumaSE = lumaOffset(posM, float2(1.0, 1.0));
    float lumaNE = lumaOffset(posM, float2(1.0, -1.0));
    float lumaSW = lumaOffset(posM, float2(-1.0, 1.0));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    float edgeVert1 = (-2.0 * lumaM) + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else {
        lengthSign = rcp.y;
    }
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) {
        lengthSign = -lengthSign;
    }
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = posM;
    float2 offNP;
    offNP.x = !horzSpan ? 0.0 : rcp.x;
    offNP.y = horzSpan ? 0.0 : rcp.y;
    if (!horzSpan) {
        posB.x += lengthSign * 0.5;
    } else {
        posB.y += lengthSign * 0.5;
    }
    float2 posN = posB - offNP * STEPS[0];
    float2 posP = posB + offNP * STEPS[0];
    float subpixD = (-2.0 * subpixC) + 3.0;
    float lumaEndN = lumaAt(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = lumaAt(posP);
    if (!pairN) {
        lumaNN = lumaSS;
    }
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) {
        posN -= offNP * STEPS[1];
    }
    if (!doneP) {
        posP += offNP * STEPS[1];
    }
    bool doneNP = (!doneN) || (!doneP);
    for (int i = 2; i < 12; i++) {
        if (doneNP) {
            if (!doneN) {
                lumaEndN = lumaAt(posN) - lumaNN * 0.5;
            }
            if (!doneP) {
                lumaEndP = lumaAt(posP) - lumaNN * 0.5;
            }
            doneN = abs(lumaEndN) >= gradientScaled;
            doneP = abs(lumaEndP) >= gradientScaled;
            if (!doneN) {
                posN -= offNP * STEPS[i];
            }
            if (!doneP) {
                posP += offNP * STEPS[i];
            }
            doneNP = (!doneN) || (!doneP);
        }
    }
    float dstN = posM.x - posN.x;
    float dstP = posP.x - posM.x;
    if (!horzSpan) {
        dstN = posM.y - posN.y;
        dstP = posP.y - posM.y;
    }
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / spanLength;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    float subpixH = subpixG * fxaa.subpixel;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) {
        posM.x += pixelOffsetSubpix * lengthSign;
    } else {
        posM.y += pixelOffsetSubpix * lengthSign;
    }
    return float4(fetch(posM), 1.0);
}
