// The render package's occlusion culling by a hierarchical depth buffer (the
// Hi-Z pyramid), in compute. Compiled ahead of time to hiz.spv by
// tools/shaders/shaders.py and embedded in hiz.cone, which says how it is run.
//
// THE METHOD is the two-pass one of Assassin's Creed Unity (Haar and
// Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015) as Wihlidal
// gave it for Frostbite ("Optimizing the Graphics Pipeline with Compute",
// GDC 2016):
//   1. 'cull' with pass 0 tests every candidate's box against the pyramid of
//      LAST frame's depth, with last frame's camera. Those that pass are
//      copied to the next free place of their batch (the batch's counter
//      counts them) and drawn at once; those that do not are flagged.
//   2. The first draw's depth is made into the pyramid ('buildLevel0', then
//      'reduceLevel' for each level).
//   3. 'cull' with pass 1 tests the flagged ones against THIS frame's
//      pyramid with this frame's camera. What was culled wrongly (it moved, the
//      camera moved, an occluder it stood behind has gone) passes now and is
//      drawn by a second draw; the rest were hidden.
// A false positive in the first pass (drawn though hidden) costs only the
// draw; a false negative is made good by the second, so nothing visible is
// lost for want of a depth buffer of the right frame.
//
// THE TEST. A box is projected: its eight corners through the matrix; if any
// is behind the camera or in front of its near plane, the box is not tested
// (it is in the nearest part of the picture, where nothing hides it). The
// rectangle the corners cover on the screen, a texel wider each side, picks
// the pyramid level at which that rectangle is at most two texels across, and
// the farthest depth of those texels is the farthest anything drawn is there.
// The box is hidden if its nearest corner is farther than that: strictly,
// so that a box flush with a surface is not culled. The pyramid holds the
// FARTHEST depth of the texels below (a 'max' pyramid: depth is 0 near, 1
// far); its level 0 is a power of two a side (at most the depth buffer's),
// each texel the farthest of the depth pixels it covers.
//
// THE COMPACTION. A candidate is an instance (80 bytes: a model matrix and a
// tint) and a word (the per-instance word of fade.slang and skinning.slang),
// kept in a source buffer in batch order. A batch that can be culled has two
// places in the renderer's instance buffer, one for each pass, sized for all its
// candidates; a passing candidate is copied to the batch's next place by an
// atomic counter. The draw is an indirect draw whose instance count the
// 'finalizeArgs' kernel writes from the counter, so the CPU never learns how
// many were drawn. (Instances of a batch land in any order.)
//
// Group 0, for 'cull' and 'finalizeArgs': the parameters (0), the candidates'
// instances (1), their boxes, batches and words (2), the renderer's instance
// buffer (3) and bone-base buffer (4) that the survivors are copied into, the
// batches (5), the counters, then one flag per candidate (6), the indirect
// arguments (7) and the pyramid (8). Group 0, for the pyramid's two kernels:
// the depth buffer (1), a level written (2) and a level read (3).

struct CullParams {
    float4x4 viewProjection;       // 0: the camera the boxes are tested with
    float4 pyramid;                // 64: level 0's width and height, the levels, 1 if the pyramid may be used
    uint4 counts;                  // 80: candidates, pass, batches, indirect draws a pass
};

struct InstanceData {
    float4 model0;
    float4 model1;
    float4 model2;
    float4 model3;
    float4 tint;
};

// A candidate's box (centre, half size), its batch and its per-instance word
struct CandInfo {
    float3 center;
    uint batch;
    float3 half;
    uint word;
};

// A batch that can be culled: where its survivors go in each pass, and its
// indirect draws (one a material group of its mesh)
struct OccBatch {
    uint dest0;
    uint dest1;
    uint argFirst;
    uint argCount;
};

[[vk::binding(0, 0)]] ConstantBuffer<CullParams> cull;
[[vk::binding(1, 0)]] StructuredBuffer<InstanceData> sourceInstances;
[[vk::binding(2, 0)]] StructuredBuffer<CandInfo> candidates;
[[vk::binding(3, 0)]] RWStructuredBuffer<InstanceData> destInstances;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> destWords;
[[vk::binding(5, 0)]] StructuredBuffer<OccBatch> batches;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> work;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> args;
[[vk::binding(8, 0)]] Texture2D<float> pyramid;

// Whether the box about 'center' of half size 'half' is hidden by the pyramid
bool hidden(float3 center, float3 half) {
    float zNear = 2.0;
    float2 lo = float2(1e9, 1e9);
    float2 hi = float2(-1e9, -1e9);
    for (uint k = 0u; k < 8u; k++) {
        float3 corner = center + half * float3((k & 1u) != 0u ? 1.0 : -1.0, (k & 2u) != 0u ? 1.0 : -1.0,
            (k & 4u) != 0u ? 1.0 : -1.0);
        float4 c = mul(cull.viewProjection, float4(corner, 1.0));
        if (c.w <= 1e-4 || c.z < 0.0) {
            return false;
        }
        float3 n = c.xyz / c.w;
        lo = min(lo, n.xy);
        hi = max(hi, n.xy);
        zNear = min(zNear, n.z);
    }
    if (hi.x < -1.0 || lo.x > 1.0 || hi.y < -1.0 || lo.y > 1.0) {
        // Off the screen the camera the pyramid was made with had: it cannot say
        return false;
    }
    lo = max(lo, float2(-1.0, -1.0));
    hi = min(hi, float2(1.0, 1.0));
    float2 size0 = cull.pyramid.xy;
    // The rectangle in level 0's texels (rows from the top), a texel wider each side
    float2 t0 = float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * size0 - 1.0;
    float2 t1 = float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * size0 + 1.0;
    t0 = clamp(t0, float2(0.0, 0.0), size0 - 1.0);
    t1 = clamp(t1, float2(0.0, 0.0), size0 - 1.0);
    float extent = max(t1.x - t0.x, t1.y - t0.y) + 1.0;
    uint levels = uint(cull.pyramid.z + 0.5);
    uint level = min(uint(ceil(log2(max(extent, 1.0)))), levels - 1u);
    float scale = float(1u << level);
    int2 dims = max(int2(size0) >> int(level), int2(1, 1));
    int2 a = clamp(int2(floor(t0 / scale)), int2(0, 0), dims - 1);
    int2 b = clamp(int2(floor(t1 / scale)), int2(0, 0), dims - 1);
    float m = max(max(pyramid.Load(int3(a, int(level))), pyramid.Load(int3(b.x, a.y, int(level)))),
        max(pyramid.Load(int3(a.x, b.y, int(level))), pyramid.Load(int3(b, int(level)))));
    return zNear > m;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cullInstances(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= cull.counts.x) {
        return;
    }
    uint pass = cull.counts.y;
    // The second pass tests only what the first culled
    if (pass == 1u && work[2u * cull.counts.z + i] == 0u) {
        return;
    }
    CandInfo c = candidates[i];
    bool visible = true;
    if (cull.pyramid.w > 0.5) {
        visible = !hidden(c.center, c.half);
    }
    if (visible) {
        OccBatch b = batches[c.batch];
        uint slot;
        InterlockedAdd(work[pass * cull.counts.z + c.batch], 1u, slot);
        uint dst = (pass == 0u ? b.dest0 : b.dest1) + slot;
        destInstances[dst] = sourceInstances[i];
        destWords[dst] = c.word;
    } else if (pass == 0u) {
        work[2u * cull.counts.z + i] = 1u;
    }
}

// The indirect draws' instance counts, from the counters (one thread a batch)
[shader("compute")]
[numthreads(64, 1, 1)]
void finalizeArgs(uint3 id : SV_DispatchThreadID) {
    uint b = id.x;
    if (b >= cull.counts.z) {
        return;
    }
    OccBatch ob = batches[b];
    uint pass = cull.counts.y;
    uint count = work[pass * cull.counts.z + b];
    for (uint g = 0u; g < ob.argCount; g++) {
        uint a = pass * cull.counts.w + ob.argFirst + g;
        args[a * 5u + 1u] = count;
    }
}

// ---- The pyramid ----

struct BuildParams {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

[[vk::push_constant]] ConstantBuffer<BuildParams> build;
[[vk::binding(1, 0)]] Texture2D<float> depthSource;
[[vk::binding(2, 0)]] [format("r32f")] WTexture2D<float> destLevel;
[[vk::binding(3, 0)]] Texture2D<float> sourceLevel;

// Level 0: each texel the farthest depth of the pixels it covers
[shader("compute")]
[numthreads(8, 8, 1)]
void buildLevel0(uint3 id : SV_DispatchThreadID) {
    if (id.x >= build.dstWidth || id.y >= build.dstHeight) {
        return;
    }
    uint x0 = (id.x * build.srcWidth) / build.dstWidth;
    uint x1 = ((id.x + 1u) * build.srcWidth + build.dstWidth - 1u) / build.dstWidth;
    uint y0 = (id.y * build.srcHeight) / build.dstHeight;
    uint y1 = ((id.y + 1u) * build.srcHeight + build.dstHeight - 1u) / build.dstHeight;
    x1 = min(max(x1, x0 + 1u), build.srcWidth);
    y1 = min(max(y1, y0 + 1u), build.srcHeight);
    float m = 0.0;
    for (uint y = y0; y < y1; y++) {
        for (uint x = x0; x < x1; x++) {
            m = max(m, depthSource.Load(int3(int(x), int(y), 0)));
        }
    }
    destLevel.Store(id.xy, m);
}

// A level above: the farthest of the four texels below it
[shader("compute")]
[numthreads(8, 8, 1)]
void reduceLevel(uint3 id : SV_DispatchThreadID) {
    if (id.x >= build.dstWidth || id.y >= build.dstHeight) {
        return;
    }
    int2 s = int2(id.xy) * 2;
    int2 last = int2(int(build.srcWidth) - 1, int(build.srcHeight) - 1);
    float a = sourceLevel.Load(int3(min(s, last), 0));
    float b = sourceLevel.Load(int3(min(s + int2(1, 0), last), 0));
    float c = sourceLevel.Load(int3(min(s + int2(0, 1), last), 0));
    float d = sourceLevel.Load(int3(min(s + int2(1, 1), last), 0));
    destLevel.Store(id.xy, max(max(a, b), max(c, d)));
}
