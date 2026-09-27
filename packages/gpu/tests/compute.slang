// The compute test's shaders: storage buffers read and written, dispatches
// that read what the one before wrote, indirect arguments written by the
// GPU, and a triangle drawn from them. compute.cone says what each checks.
// Compiled ahead of time to compute.spv by tools/shaders/shaders.py.

struct Immediates {
    uint count;                    // bytes 0-3: how many elements
    uint add;                      // bytes 4-7: added to each square
};

[[vk::push_constant]]
ConstantBuffer<Immediates> immediates;

[[vk::binding(0, 0)]] StructuredBuffer<uint> input;          // read-only storage
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> squares;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> chained;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> arguments;    // indirect: a dispatch's 3, a draw's 4
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> counter;

// squares[i] = input[i]^2 + add
[shader("compute")]
[numthreads(64, 1, 1)]
void squareMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i < immediates.count) {
        squares[i] = input[i] * input[i] + immediates.add;
    }
}

// chained[i] = squares[i] + i: reads what the dispatch before wrote
[shader("compute")]
[numthreads(64, 1, 1)]
void chainMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i < immediates.count) {
        chained[i] = squares[i] + i;
    }
}

// The arguments of an indirect dispatch (4 x 1 x 1 workgroups) and of an
// indirect draw (3 vertices, 1 instance), from the first square (7^2 + add,
// add being 12: 61, so 61 - 57 = 4 workgroups)
[shader("compute")]
[numthreads(1, 1, 1)]
void argumentsMain(uint3 id : SV_DispatchThreadID) {
    arguments[0] = squares[0] - 57u;
    arguments[1] = 1u;
    arguments[2] = 1u;
    arguments[3] = 0u;
    arguments[4] = 3u;
    arguments[5] = 1u;
    arguments[6] = 0u;
    arguments[7] = 0u;
    counter[0] = 0u;
}

// Each invocation of the indirect dispatch counts itself: an integer atomic,
// whose sum does not depend on the order
[shader("compute")]
[numthreads(64, 1, 1)]
void countMain(uint3 id : SV_DispatchThreadID) {
    uint before;
    InterlockedAdd(counter[0], 1u, before);
}

struct VertexOutput {
    float4 position : SV_Position;
};

// A triangle covering the whole target, drawn by the indirect draw
[shader("vertex")]
VertexOutput vertexMain(uint id : SV_VulkanVertexID) {
    VertexOutput output;
    float2 corner = float2(float((id << 1u) & 2u), float(id & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(1.0, 0.5, 0.25, 1.0);
}
