// The render package's flutter, a Slang module ('import flutter;'): each leaf
// of a tree turning a few degrees about its stem on its own phase. Read by
// pbr.slang's vertexSway and by shadowcutpbr.slang's, so that a leaf's
// shadow moves as the leaf does (both compile with -fp-mode precise, so the
// two agree to the last bit).
//
// A mesh drawn with a flutter material carries, in a fourth vertex stream
// (the mesh's colors, gpumesh.cone), for each vertex: xyz its offset from its
// leaf's pivot (the leaf's stem), in model coordinates, and w the leaf's
// phase in turns, one number for the whole leaf. Each leaf is turned about
// its pivot by a small rotation vector w: a turn about a horizontal axis of
// its own (an azimuth from its phase) and a slower sway about the vertical,
// each a sine of the frame's time, on the leaf's own period (0.6 to 1.8
// seconds, from its phase) and phase. For turns of a few degrees the
// displacement of a vertex is w x offset (the small-angle rotation, which
// needs no matrix), and the normal turns by w x n so the light catches and
// loses the leaf as it moves. A vertex at the pivot (a stem's end) stays.
// Model space is y up, which is how a tree stands.

void flutterLeaf(float3 offset, float phase, float amount, float time, inout float3 position, inout float3 normal) {
    float period = 0.6 + 1.2 * frac(phase * 13.7);
    float s1 = sin(6.2831853 * frac(time / period + phase));
    float s2 = sin(6.2831853 * frac(time / (period * 1.7) + phase * 3.1));
    // The same axis for every vertex of the leaf (from its phase), so it
    // turns as one piece and is not bent
    float azimuth = 6.2831853 * frac(phase * 7.7);
    float3 pitch = float3(cos(azimuth), 0.0, sin(azimuth));
    float3 turn = amount * (s1 * pitch + (0.6 * s2) * float3(0.0, 1.0, 0.0));
    position += cross(turn, offset);
    normal += cross(turn, normal);
}
