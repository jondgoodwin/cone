// The render package's dithered fade, a Slang module ('import fade;'): how a
// copy of a mesh is made to appear or vanish a share of its pixels at a time
// instead of all at once. It is what a level-of-detail cross-fade and a
// visibility range's edge draw with (visibility.cone), the "screen-door" or
// stochastic transparency of Unreal's dithered LOD transitions and Godot's
// visibility fade (Mittring, "Rendering Techniques in Unreal Engine 3", 2009;
// Unity's LOD cross-fade), which writes depth like any opaque draw and so
// needs no sorting.
//
// THE CODE. A copy's fade rides in the per-instance word 'boneBases' holds
// (skinning.slang) for a copy with no bones: that word is NO_BONES for all
// but the copies of a skinned mesh, which never read it through here, so an
// unskinned copy may keep a fade in it, and no struct and no binding of any
// custom pipeline changes. A fade is the high half 0xFADE and a signed 16-bit
// number in the low half, the fade times 32767:
//   0 (or no code at all): drawn whole.
//   f > 0: drawn where the pixel's noise is at least f; f -> 1 is gone. A
//     copy fading OUT by a share f, or any copy at the edge of a range.
//   f < 0: drawn where the noise is below -f; f -> -1 is whole. A copy
//     fading IN to a share -f.
// A pair at the same share (one fading out by f, one in by -f) are exact
// complements: every pixel is drawn by one of them and never by both, so a
// cross-fade of two levels covers each pixel once.
//
// THE NOISE is interleaved gradient noise (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014) at the
// pixel: a fixed pattern in screen space, so a temporal anti-aliaser
// averages it, and the same in every draw so the complement holds.

import skinning;

static const uint FADE_TAG = 0xFADE;

// Copy 'id''s fade, -1 to 1: 0 if it has none
float instanceFade(uint id) {
    uint e = boneBases[id];
    if ((e >> 16) != FADE_TAG) {
        return 0.0;
    }
    int q = int((e & 0xFFFF) << 16) >> 16;
    return float(q) / 32767.0;
}

// The pixel's noise, 0 to 1
float fadeNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Drop the pixel at 'pixel' (SV_Position's xy) if the fade 'f' says so
void fadeClip(float f, float2 pixel) {
    if (f == 0.0) {
        return;
    }
    float n = fadeNoise(pixel);
    if (f > 0.0) {
        if (n < f) {
            discard;
        }
    } else {
        if (n >= -f) {
            discard;
        }
    }
}
