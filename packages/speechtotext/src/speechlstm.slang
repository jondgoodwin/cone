// One LSTM cell step after its two matrix-vector products (torch.nn.LSTM's gate order i, f, g, o):
//   gates = gi + gh + bias            gi = W_ih x, gh = W_hh h, bias = b_ih + b_hh (4 n floats at float offset bOff of 'small')
//   i = sigmoid(gates[0 .. n)), f = sigmoid(gates[n .. 2n)), g = tanh(gates[2n .. 3n)), o = sigmoid(gates[3n .. 4n))
//   c' = f * c + i * g,  h' = o * tanh(c')
// One invocation for each of the n units, 256 a workgroup. Bindings: params (by value), small, gi, gh, c (previous
// cell), hNew, cNew. (A dispatch binds a buffer once, so the new state goes to buffers of its own.)
//
// slangc: -fp-mode precise

struct LstmParams {
    uint n;
    uint bOff;
    uint pad0;
    uint pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<LstmParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float> small;
[[vk::binding(2, 0)]] StructuredBuffer<float> gi;
[[vk::binding(3, 0)]] StructuredBuffer<float> gh;
[[vk::binding(4, 0)]] StructuredBuffer<float> cPrev;
[[vk::binding(5, 0)]] RWStructuredBuffer<float> hNew;
[[vk::binding(6, 0)]] RWStructuredBuffer<float> cNew;

float sigmoid(float v) { return 1.0 / (1.0 + exp(-v)); }

[shader("compute")]
[numthreads(256, 1, 1)]
void lstmCellMain(uint3 gid : SV_GroupID, uint lid : SV_GroupIndex) {
    LstmParams p = params[0];
    uint u = gid.x * 256u + lid;
    if (u >= p.n) { return; }
    float gI = gi[u] + gh[u] + small[p.bOff + u];
    float gF = gi[p.n + u] + gh[p.n + u] + small[p.bOff + p.n + u];
    float gG = gi[2u * p.n + u] + gh[2u * p.n + u] + small[p.bOff + 2u * p.n + u];
    float gO = gi[3u * p.n + u] + gh[3u * p.n + u] + small[p.bOff + 3u * p.n + u];
    float c = sigmoid(gF) * cPrev[u] + sigmoid(gI) * tanh(gG);
    cNew[u] = c;
    hNew[u] = sigmoid(gO) * tanh(c);
}
