// The render package's morph targets (blend shapes), a compute pre-pass:
// each morphed copy of a mesh has its positions and normals rebuilt from the
// mesh as modelled and the weights of the targets that are not zero.
// Compiled ahead of time to morph.spv by tools/shaders/shaders.py and
// embedded in morph.cone.
//
// WHAT IT COMPUTES, glTF 2.0's rule (the Morph Targets section): a vertex's
// morphed position is its position plus the sum, over the targets, of the
// target's weight times the target's displacement of that vertex; the normal
// likewise, then made unit length. The result is written to the copy's own
// vertex buffers, which the ordinary vertex stages then read; so skinning
// (which runs in the vertex stage, on those buffers) comes after morphing,
// as glTF says, and the shadow casters, which read the same position buffer,
// cast the morphed shape.
//
// THE SPARSE LAYOUT. A target moves only some of a mesh's vertices (a mouth's
// corners, not the ears); so the displacements are kept per VERTEX, in
// compressed rows: 'rowStart' has one entry a vertex and one more, the
// entries of vertex v being rowStart[v] to rowStart[v + 1] - 1, each naming
// its target ('entryTarget', ascending within a row) and holding that
// target's displacement of the vertex ('entryDelta': three floats of
// position, three of normal). A dense target (every vertex moved) is the same
// layout with every row holding it.
//
// THE ACTIVE LIST. A face rig has 64 targets and, at any moment, a few of
// them not zero. The copy's active list holds just those, as (target, weight)
// pairs sorted by target; a vertex looks each one up in its own row by binary
// search. The work of a vertex is therefore the number of ACTIVE targets times
// the logarithm of the targets that touch it, and a target at weight zero
// costs nothing. Sorted ascending, the targets are summed in the order the CPU
// reference (morph.cone's MorphTargets.position) sums them.
//
// A vertex no active target touches is written exactly as modelled (its
// normal not made unit length again), so a copy with all weights zero is
// byte for byte the mesh it was made from.

struct Immediates {
    uint vertexCount;    // how many vertices the mesh has
    uint activeCount;    // how many pairs the active list holds
    uint targetCount;    // how many targets the set has: a row this long holds every target, in order
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

[[vk::binding(0, 0)]] StructuredBuffer<uint> rowStart;        // vertexCount + 1
[[vk::binding(1, 0)]] StructuredBuffer<uint> entryTarget;     // one an entry
[[vk::binding(2, 0)]] StructuredBuffer<float> entryDelta;     // six an entry: position, then normal
[[vk::binding(3, 0)]] StructuredBuffer<float> basePosition;   // three a vertex, as modelled
[[vk::binding(4, 0)]] StructuredBuffer<float> baseNormal;     // three a vertex
[[vk::binding(5, 0)]] StructuredBuffer<float> activeList;     // two floats a pair: the target (as a uint's bits), the weight
[[vk::binding(6, 0)]] RWStructuredBuffer<float> outPosition;  // three a vertex
[[vk::binding(7, 0)]] RWStructuredBuffer<float> outNormal;    // three a vertex

[shader("compute")]
[numthreads(64, 1, 1)]
void morphMain(uint3 id : SV_DispatchThreadID) {
    uint v = id.x;
    if (v >= immediates.vertexCount) {
        return;
    }
    float3 p = float3(basePosition[3 * v], basePosition[3 * v + 1], basePosition[3 * v + 2]);
    float3 n = float3(baseNormal[3 * v], baseNormal[3 * v + 1], baseNormal[3 * v + 2]);
    uint lo0 = rowStart[v];
    uint hi0 = rowStart[v + 1];
    bool touched = false;
    if (hi0 > lo0) {
        for (uint k = 0; k < immediates.activeCount; k++) {
            uint target = asuint(activeList[2 * k]);
            float w = activeList[2 * k + 1];
            uint lo = lo0;
            uint hi = hi0;
            if (hi0 - lo0 == immediates.targetCount) {
                // A dense row holds every target in order: the entry is at its number
                lo = lo0 + target;
                hi = lo + 1;
            } else {
                // The first entry of the row not below 'target'
                while (lo < hi) {
                    uint mid = (lo + hi) >> 1;
                    if (entryTarget[mid] < target) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
            }
            if (lo < hi0 && entryTarget[lo] == target) {
                uint e = 6 * lo;
                p += w * float3(entryDelta[e], entryDelta[e + 1], entryDelta[e + 2]);
                n += w * float3(entryDelta[e + 3], entryDelta[e + 4], entryDelta[e + 5]);
                touched = true;
            }
        }
    }
    if (touched) {
        float len = length(n);
        if (len > 1.0e-12) {
            n = n / len;
        }
    }
    outPosition[3 * v] = p.x;
    outPosition[3 * v + 1] = p.y;
    outPosition[3 * v + 2] = p.z;
    outNormal[3 * v] = n.x;
    outNormal[3 * v + 1] = n.y;
    outNormal[3 * v + 2] = n.z;
}
