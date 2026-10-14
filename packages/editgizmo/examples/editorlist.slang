// The editor demo's rectangles: the filled and rounded rectangles, borders and lines of a uilayout
// DrawList, one instanced quad each, drawn under the text. Compiled by tools/shaders/shaders.py and
// embedded in editordemo.cone. (A demo-grade renderer: uilayout has none of its own yet; the
// pegasus3d browser's HUD has a fuller one.)
//
// Each shape is its signed distance in pixels, one pixel of coverage at the edge (Inigo Quilez's rounded
// box and segment distances). Colours are straight alpha as the list has them; the output is
// premultiplied (the pipeline blends ONE, ONE_MINUS_SRC_ALPHA).
//
//   box   x0 y0 x1 y1   the quad, a pixel or two beyond the shape
//   shape cx cy hx hy   rect and border: centre and half size; line: the two end points
//   info  x kind (0 fill, 1 border, 2 line), y corner radius, z thickness
//   clip  x0 y0 x1 y1   nothing is drawn outside it

struct Params {
    float4 view;                   // x, y: the target's size in pixels
};

[[vk::binding(0, 0)]] ConstantBuffer<Params> params;

struct Prim {
    float4 box : POSITION;
    float4 shape : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 info : TEXCOORD2;
    float4 clip : TEXCOORD3;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 pixel : TEXCOORD0;
    nointerpolation float4 shape : TEXCOORD1;
    nointerpolation float4 color : TEXCOORD2;
    nointerpolation float4 info : TEXCOORD3;
    nointerpolation float4 clip : TEXCOORD4;
};

float2 quadCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : 0.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0;
    return float2(x, y);
}

[shader("vertex")]
VertexOutput vertexMain(Prim prim, uint vid : SV_VulkanVertexID) {
    float2 c = quadCorner(vid);
    float2 p = float2(lerp(prim.box.x, prim.box.z, c.x), lerp(prim.box.y, prim.box.w, c.y));
    VertexOutput o;
    o.position = float4(p.x / params.view.x * 2.0 - 1.0, 1.0 - p.y / params.view.y * 2.0, 0.0, 1.0);
    o.pixel = p;
    o.shape = prim.shape;
    o.color = prim.color;
    o.info = prim.info;
    o.clip = prim.clip;
    return o;
}

float roundedBox(float2 p, float2 centre, float2 half, float radius) {
    float r = min(radius, min(half.x, half.y));
    float2 q = abs(p - centre) - (half - float2(r, r));
    return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput i) : SV_Target {
    float2 p = i.pixel;
    if (p.x < i.clip.x || p.y < i.clip.y || p.x >= i.clip.z || p.y >= i.clip.w) { discard; }
    float kind = i.info.x;
    float radius = i.info.y;
    float thick = i.info.z;
    float cover = 0.0;
    if (kind < 0.5) {
        float d = roundedBox(p, i.shape.xy, i.shape.zw, radius);
        cover = clamp(0.5 - d, 0.0, 1.0);
    } else if (kind < 1.5) {
        float d = roundedBox(p, i.shape.xy, i.shape.zw, radius);
        cover = clamp(0.5 - d, 0.0, 1.0) * clamp(0.5 + d + thick, 0.0, 1.0);
    } else {
        float2 a = i.shape.xy;
        float2 e = i.shape.zw;
        float2 pa = p - a;
        float2 ba = e - a;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
        float d = length(pa - ba * h) - thick * 0.5;
        cover = clamp(0.5 - d, 0.0, 1.0);
    }
    float a = i.color.a * cover;
    return float4(i.color.rgb * a, a);
}
