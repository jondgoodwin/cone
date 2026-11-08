// The effectsfx package's particle pool: what the simulation kernel
// (particlesim.slang) and the drawing shaders (particledraw.slang) share. A
// Slang module ('import particlecommon;'), with no entry points.
//
// THE POOL is a ring of POOL slots of three float4s each, in one storage buffer:
//   a: position.xyz, age (seconds since birth)
//   b: velocity.xyz, life (seconds it lives; a slot with age >= life is dead)
//   c: class (uint bits), the particle's random number in [0, 1), bounces so far, a size factor
// A new particle takes the slot after the last one given out, so the oldest
// give way when the ring is full: no free list, no atomics, and every slot is
// one thread's alone.
//
// A CLASS is a particle's description as data, twelve float4s a class in one
// storage buffer (the Cone side, particles.cone, writes them from a
// ParticleClass):
//    0: shape (0 point, 1 disc, 2 sphere), radius, spread (radians), speed
//    1: speed jitter, life, life jitter, drag (1/s)
//    2: gravity.xyz, turbulence (m/s^2)
//    3: turbulence scale (1/m), turbulence speed (m/s), size at birth, size at death
//    4: stretch (seconds of motion a sprite spans), fade in, fade out (fractions of life), style (0 spark, 1 smoke)
//    5: restitution, friction, life lost per bounce (fraction), collide (0 or 1)
//    6-9: four colours (rgb HDR, a: a spark's intensity, a smoke's opacity)
//   10: where the colours sit in life (four numbers, rising from 0 to 1)
//   11: size jitter, spin (unused), velocity inherited from the emitter, wind factor
//
// A BATCH is one spawn order for the frame: four float4s
//   0: origin.xyz, class (bits)
//   1: axis.xyz (the launch cone's axis, unit), seed (bits)
//   2: inherited velocity.xyz, 0
//   3: count (bits), the batch's first index among the frame's spawns (bits), 0, 0
//
// THE FRAME, 'SimFrame': time, dt, the ring's first new slot (bits) and how many
// are new (bits); the ring's size, the batch count, flags (1 collide with the
// depth buffer, 2 with the ground plane), the frame number (all bits); the ground
// plane (normal.xyz, d: a point p is above it when dot(n, p) + d > 0); the wind
// (xyz); the camera's right and up, and the softness of soft particles in metres.

import noise;
import fxcommon;

struct SimFrame {
    float4 timing;
    uint4 counts;
    float4 ground;
    float4 wind;
    float4 right;      // xyz, w the soft-particle distance
    float4 up;
};

static const uint CLASS_FLOATS = 12u;

float4 classRow(StructuredBuffer<float4> classes, uint c, uint row) {
    return classes[c * CLASS_FLOATS + row];
}

// A class's colour at fraction 't' of its life: four keys, linear between them
float4 colorOver(StructuredBuffer<float4> classes, uint c, float t) {
    float4 at = classRow(classes, c, 10u);
    float4 k0 = classRow(classes, c, 6u);
    float4 k1 = classRow(classes, c, 7u);
    float4 k2 = classRow(classes, c, 8u);
    float4 k3 = classRow(classes, c, 9u);
    if (t < at.y) { return lerp(k0, k1, saturate((t - at.x) / max(at.y - at.x, 1e-4))); }
    if (t < at.z) { return lerp(k1, k2, saturate((t - at.y) / max(at.z - at.y, 1e-4))); }
    return lerp(k2, k3, saturate((t - at.z) / max(at.w - at.z, 1e-4)));
}

float2 quadCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : -1.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0;
    return float2(x, y);
}
