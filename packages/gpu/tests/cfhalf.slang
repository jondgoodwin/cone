// 16-bit floats, for computefeatures.cone: half-precision values kept in
// storage buffers and computed on as halves (SPIR-V's Float16 and
// UniformAndStorageBuffer16BitAccess capabilities). Each entry point does one
// kind of half arithmetic, so that the test's CPU twin can round after each
// operation as the GPU does, and compare bit for bit. Compiled ahead of time
// to cfhalf.spv by tools/shaders/shaders.py. 64 invocations a workgroup; the
// test dispatches whole workgroups.
// slangc: -fp-mode precise

[[vk::binding(0, 0)]] StructuredBuffer<half> a;
[[vk::binding(1, 0)]] StructuredBuffer<half> b;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> result;     // each half answer, widened to a float (exact)

// result[i] = a[i] * b[i], one rounding to half
[shader("compute")]
[numthreads(64, 1, 1)]
void halfMul(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = float(a[i] * b[i]);
}

// result[i] = a[i] + b[i], one rounding to half
[shader("compute")]
[numthreads(64, 1, 1)]
void halfAdd(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    result[i] = float(a[i] + b[i]);
}

// result[i] = a[8 i] + a[8 i + 1] + ... + a[8 i + 7], added in that order in half precision: eight roundings
[shader("compute")]
[numthreads(64, 1, 1)]
void halfSum(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    half s = half(0.0);
    for (uint k = 0; k < 8; k++) {
        s = s + a[i * 8 + k];
    }
    result[i] = float(s);
}
