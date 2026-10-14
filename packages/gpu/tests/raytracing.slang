// slangc: -capability spvRayQueryKHR
//
// The ray-tracing test's shader: eight rays cast by a compute shader into a top-level acceleration
// structure with a ray query, and what each hit, written out. raytracing.cone says what is expected.
// Compiled ahead of time to raytracing.spv by tools/shaders/shaders.py.

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> results;    // 5 words a ray: hit, t as bits, custom index, primitive, front face

[shader("compute")]
[numthreads(8, 1, 1)]
void castMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    // Rays 0 to 3 go up from z = 0 at four places; 4 to 5 come down from z = 10; 6 and 7 are parallel to the quads
    float3 origin = float3(0.0, 0.0, 0.0);
    float3 direction = float3(0.0, 0.0, 1.0);
    if (i == 0) { origin = float3(0.3, 0.1, 0.0); }
    if (i == 1) { origin = float3(0.8, 0.8, 0.0); }
    if (i == 2) { origin = float3(2.0, 2.0, 0.0); }
    if (i == 3) { origin = float3(-0.3, 0.1, 0.0); }
    if (i == 4) { origin = float3(0.2, 0.2, 10.0); direction = float3(0.0, 0.0, -1.0); }
    if (i == 5) { origin = float3(0.8, 0.8, 10.0); direction = float3(0.0, 0.0, -1.0); }
    if (i == 6) { origin = float3(0.2, 0.2, 0.0); direction = float3(1.0, 0.0, 0.0); }
    if (i == 7) { origin = float3(0.2, 0.2, 4.0); direction = float3(0.0, 0.0, 1.0); }    // beyond the near quad, before the far
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    uint hit = 0;
    float t = 0.0;
    uint custom = 0;
    uint primitive = 0;
    uint front = 0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hit = 1;
        t = q.CommittedRayT();
        custom = q.CommittedInstanceID();
        primitive = q.CommittedPrimitiveIndex();
        front = q.CommittedTriangleFrontFace() ? 1 : 0;
    }
    results[i * 5 + 0] = hit;
    results[i * 5 + 1] = asuint(t);
    results[i * 5 + 2] = custom;
    results[i * 5 + 3] = primitive;
    results[i * 5 + 4] = front;
}
