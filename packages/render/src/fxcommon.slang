// The render package's image-quality passes' common ground, a Slang module
// ('import fxcommon;'): what temporal anti-aliasing, FXAA, the exposure
// histogram, SSAO, depth of field, the volumetric fog and decals share
// (fx.cone writes the uniform buffer once a frame and binds it).
//
// THE FRAME. 'FxFrame' is the uniform every one of these passes reads; a
// pass declares its own binding of it ('[[vk::binding(0, 0)]] ConstantBuffer<FxFrame> fx;',
// or set 2 where the pass also binds the renderer's frame group at set 0)
// and hands it by value to the functions here. Its matrices are the
// UNJITTERED ones (the camera without temporal anti-aliasing's sub-pixel
// shift), current and previous, because a reprojection must not see the
// shift as motion.
//
// DEPTH. The renderer's depth is a perspective projection with depth from 0
// at the near plane to 1 at the far (geomath's Mat4.perspective). Its
// third and fourth rows give, for a point d in front of the camera, a depth
// z = B / d - A (A = far / (near - far), B = near far / (near - far)), so the
// distance in front of the camera is d = B / (z + A).
//
// COORDINATES. A pixel's uv has v down the rows (as the textures are laid
// out); NDC has y up.

struct FxFrame {
    float4x4 viewProjection;           // 0:   unjittered, this frame's
    float4x4 inverseViewProjection;    // 64
    float4x4 previousViewProjection;   // 128: unjittered, last frame's
    float4 cameraPosition;             // 192: xyz
    float4 cameraForward;              // 208: xyz, unit; w the near plane's distance
    float4 screen;                     // 224: width, height, 1 / width, 1 / height of the full-size frame
    float4 jitter;                     // 240: this frame's jitter in pixels (x right, y down), then last frame's
    float4 projection;                 // 256: A, B (above), the projection's [0][0] and [1][1]
    float4 sunDirection;               // 272: xyz towards the sun, unit; w the exposure every output was multiplied by
    float4 sunColor;                   // 288: rgb irradiance head on; w the time in seconds
    float4 ambient;                    // 304: rgb radiance; w the seconds since the last frame
    float4 misc;                       // 320: x the frame number, y the far plane's distance, z 1 to ignore the history (a cut), w 1 if the camera is clipped (an oblique near plane: its depth row is not a plain perspective's)
};

static const float FX_PI = 3.14159265358979;

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// YCoCg, the colour space the history is clipped in (Karis 2014)
float3 rgbToYCoCg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                  0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 yCoCgToRgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The distance in front of the camera of a point whose depth is 'z'
float viewDepth(FxFrame f, float z) {
    return f.projection.y / (z + f.projection.x);
}

// A pixel's uv and depth to its position in the camera's space (x right, y
// up, the camera looking down -z)
float3 viewPosition(FxFrame f, float2 uv, float z) {
    float d = viewDepth(f, z);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * d / f.projection.z, ndc.y * d / f.projection.w, -d);
}

// A pixel's uv and depth to its world position
float3 worldPosition(FxFrame f, float2 uv, float z) {
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    float4 w = mul(f.inverseViewProjection, clip);
    return w.xyz / w.w;
}

// A world position to its uv (v down) and its depth, with the matrix 'm'
float3 projectToUv(float4x4 m, float3 world) {
    float4 c = mul(m, float4(world, 1.0));
    float2 ndc = c.xy / c.w;
    return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, c.z / c.w);
}

// The surface normal (camera space, towards the camera) at the pixel whose
// position is 'c', from its four neighbours' (left, right, up, down): each
// axis uses the neighbour nearer in depth, so an edge does not bend it
// (Wang, "Accurate normal reconstruction from depth buffer", 2020, after
// Turanszkij's and Mathias's)
float3 normalFromNeighbours(float3 c, float3 l, float3 r, float3 u, float3 d) {
    float3 dx = abs(r.z - c.z) < abs(l.z - c.z) ? (r - c) : (c - l);
    float3 dy = abs(u.z - c.z) < abs(d.z - c.z) ? (u - c) : (c - d);
    return normalize(cross(dx, dy));
}

// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare", 2014): 0 to 1, in screen space
float interleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// The Halton sequence's 'index'th number in 'base' (the jitter's, in C++
// there; fx.cone has the CPU's twin and the two agree)
float halton(uint index, uint b) {
    float result = 0.0;
    float f = 1.0;
    uint i = index;
    while (i > 0u) {
        f = f / float(b);
        result += f * float(i % b);
        i = i / b;
    }
    return result;
}
