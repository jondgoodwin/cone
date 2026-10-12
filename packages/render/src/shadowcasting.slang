// The render package's shadow casters' common ground, a Slang module
// ('import shadowcasting;'): what shadowcast.slang, shadowcutlit.slang and
// shadowcutpbr.slang share. A CASTER is a copy of a mesh drawn into one
// cascade of the shadow map, depth only, from the sun.
//
// Group 0 is the renderer's caster group: the frame's uniform buffer
// (binding 0: the time, for flutter), the instances (1), the bones (2) and
// their bases (3), as the main pass binds them, and the shadow parameters
// (4, shadowparams.slang); not the shadow map itself, which the pass draws
// into. Group 1, for a caster that cuts out or flutters, is the material's.
// The one immediate is the cascade the pass draws.
//
// PANCAKING. A caster between the sun and the cascade's near plane (a tall
// tree outside the slice that still shades it) would be clipped away. The
// near plane is drawn well back (the settings' caster distance), and any
// depth still in front of it is held at the near plane, as Wang and others
// describe for cascaded maps: the caster still shades what is behind it,
// and only a triangle that crosses the plane has its depth bent.

import renderframe;
import skinning;
import shadowparams;

struct CasterImmediates {
    uint cascade;
};

[[vk::push_constant]] ConstantBuffer<CasterImmediates> casterDraw;

// The clip position of the world point 'world' in the view being drawn: the
// immediate 0 to 3 is a cascade of the sun's, 4 and on a point or spot
// light's view (lightshadow.cone)
float4 casterClip(float3 world) {
    uint view = casterDraw.cascade;
    float4 clip;
    if (view < 4) {
        clip = mul(shadowParams.cascade[view], float4(world, 1.0));
    } else {
        clip = mul(shadowParams.localView[view - 4], float4(world, 1.0));
    }
    clip.z = max(clip.z, 0.0);
    return clip;
}
