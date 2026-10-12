// The msdftext package's text renderer: one quad a glyph, instanced, each
// shaded from the multi-channel atlas. Compiled ahead of time to
// msdftext.spv by tools/shaders/shaders.py and embedded in render.cone.
//
// MSDF RENDERING (Chlumsky's, the msdfgen README): the median of the three
// channels is the signed distance (1/2 at the outline); the distance in screen
// pixels is that, less 1/2, times the field's range in screen pixels, which
// the shader measures from how fast the atlas coordinate changes across the
// screen (fwidth), so the edge is one pixel wide at any size, scale or angle:
// coverage = clamp(distance in pixels + 1/2, 0, 1).
//
// OUTLINE: the same field read at a distance outward, the fill drawn over it.
// DROP SHADOW: the field read at an offset, as a second layer behind both,
// its edge widened to a soft ramp. All three layers composite premultiplied
// (the pipeline blends ONE, ONE_MINUS_SRC_ALPHA). An effect can only reach as
// far as the field does (half its range beyond the glyph's edge, the margin
// the atlas gives each glyph); past that the quad ends.
//
// Group 0: the draw's parameters (a uniform buffer, per text), the atlas and
// its sampler. The instance buffer holds, per glyph, the quad in the text's
// own space (x0, y0, x1, y1) and its atlas rectangle (u0, v0, u1, v1).

struct TextParams {
    float4x4 transform;            // the text's space (units, y down) to clip space
    float4 color;                  // the fill, straight alpha
    float4 outlineColor;
    float4 shadowColor;
    float4 field;                  // x: the field's whole range in the text's units, y: in atlas texels, zw: atlas uv per unit
    float4 effect;                 // x: outline width (units), y: shadow softness (units), zw: shadow offset (units, y down)
};

[[vk::binding(0, 0)]] ConstantBuffer<TextParams> text;
[[vk::binding(1, 0)]] Texture2D atlas;
[[vk::binding(2, 0)]] SamplerState atlasSampler;

struct GlyphInstance {
    float4 rect : POSITION;        // x0, y0 (top), x1, y1 (bottom) in the text's space
    float4 uvRect : TEXCOORD0;     // u0, v0 (top), u1, v1
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 uvRect : TEXCOORD1;
};

// A quad's corner, two triangles: (0,0) (1,0) (1,1) (0,0) (1,1) (0,1)
float2 quadCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : 0.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0;
    return float2(x, y);
}

[shader("vertex")]
VertexOutput vertexMain(GlyphInstance glyph, uint vid : SV_VulkanVertexID) {
    float2 c = quadCorner(vid);
    VertexOutput output;
    float2 p = float2(lerp(glyph.rect.x, glyph.rect.z, c.x), lerp(glyph.rect.y, glyph.rect.w, c.y));
    output.position = mul(text.transform, float4(p, 0.0, 1.0));
    output.uv = float2(lerp(glyph.uvRect.x, glyph.uvRect.z, c.x), lerp(glyph.uvRect.y, glyph.uvRect.w, c.y));
    output.uvRect = glyph.uvRect;
    return output;
}

float median3(float3 v) {
    return max(min(v.x, v.y), min(max(v.x, v.y), v.z));
}

// The signed distance at uv, in the field's own scale (1/2 at the outline,
// larger inside); outside the glyph's own rectangle, well outside
float fieldAt(float2 uv, float4 uvRect) {
    if (uv.x < uvRect.x || uv.x > uvRect.z || uv.y < uvRect.y || uv.y > uvRect.w) { return 0.0; }
    return median3(atlas.SampleLevel(atlasSampler, uv, 0.0).rgb);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    // The field's range in screen pixels: texels of range times screen pixels a texel
    float2 atlasSize;
    atlas.GetDimensions(atlasSize.x, atlasSize.y);
    float2 unitRange = float2(text.field.y, text.field.y) / atlasSize;
    float2 screenTexSize = float2(1.0, 1.0) / max(fwidth(input.uv), float2(1.0e-8, 1.0e-8));
    float screenRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
    float pxPerUnit = screenRange / text.field.x;

    float dist = (fieldAt(input.uv, input.uvRect) - 0.5) * screenRange;       // pixels, positive inside
    float fillA = clamp(dist + 0.5, 0.0, 1.0) * text.color.a;

    float4 result = float4(0.0, 0.0, 0.0, 0.0);

    // Drop shadow: the field read at an offset, with a soft edge
    if (text.shadowColor.a > 0.0) {
        float2 offsetUv = float2(text.effect.z * text.field.z, text.effect.w * text.field.w);
        float distS = (fieldAt(input.uv - offsetUv, input.uvRect) - 0.5) * screenRange;
        float soft = max(text.effect.y * pxPerUnit, 1.0);
        float a = clamp(distS / soft + 0.5, 0.0, 1.0) * text.shadowColor.a;
        result = float4(text.shadowColor.rgb * a, a);
    }
    // Outline: the glyph grown by the outline's width
    if (text.effect.x > 0.0 && text.outlineColor.a > 0.0) {
        float a = clamp(dist + text.effect.x * pxPerUnit + 0.5, 0.0, 1.0) * text.outlineColor.a;
        result = float4(text.outlineColor.rgb * a, a) + result * (1.0 - a);
    }
    result = float4(text.color.rgb * fillA, fillA) + result * (1.0 - fillA);
    return result;
}
