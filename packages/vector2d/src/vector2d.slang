// The vector2d package's shader: triangles with a position already in clip
// space, a pair of edge coverages and a straight-alpha colour. The pixel's
// coverage is cx * cy (1 inside, falling to 0 across the one-pixel fringe the
// tessellator built); the result is premultiplied, blended ONE, ONE_MINUS_SRC_ALPHA.

struct VertexInput {
    float2 position : POSITION;
    float2 cover : TEXCOORD0;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 cover : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.cover = input.cover;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float a = input.color.a * saturate(input.cover.x) * saturate(input.cover.y);
    return float4(input.color.rgb * a, a);
}
