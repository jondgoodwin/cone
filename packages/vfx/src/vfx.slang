// The vfx package's stateless particle evaluation, for shaders: the same
// names, and the same arithmetic in the same order, as emitter.cone, so that
// the CPU and the GPU agree (tests/parity.cone checks it on a real GPU). A
// shader reaches it with 'import vfx;'. It is a module, with no entry points:
// tools/shaders/shaders.py compiles the shaders that import it, not it alone.
//
// A particle is a pure function of its emitter's parameters, its spawn index
// and its age: nothing is stored, nothing is simulated. Every random number is
// an integer hash of (event seed, layer seed, spawn index), from the noise
// package's PCG.
//
// Compile what imports it with slangc's '-fp-mode precise' (a shader says so
// with a '// slangc: -fp-mode precise' line), so that no driver fuses a
// multiply and an add where the CPU rounds twice.

import noise;

// An emitter bound to its event, as emitter.cone's EmitterParams packs it:
// 17 float4s and a uint4, 272 bytes
struct EmitterParams {
    float4 column[4];   // the event's placement, local to world, by columns
    float4 timing;      // elapsed since the event (s), the spawn interval (1 / rate), lifetime, lifetime jitter
    float4 launch;      // spawn shape, its radius, the cosine of the spread, speed
    float4 motion;      // speed jitter, drag (1/s), stretch (s), size jitter
    float4 gravity;     // the constant acceleration (world), buoyancy at birth (m/s^2, up)
    float4 heat;        // cooling (1/s), wobble (m/s), wobble rate (rad/s), trail time (s)
    float4 sizes;       // size at birth, size at death, 1 / fade-in fraction, 1 / fade-out fraction
    float4 colors[4];   // the colour keys: linear rgb, intensity
    float4 colorAt;     // each key's fraction of life
    float4 colorInv;    // 1 / each span between keys
    uint4 seeds;        // event seed, layer seed, first spawn index drawn, how many
};

static const float VFX_TAU = 6.28318530717958647692;
static const uint VFX_SHAPE_DISC = 1u;
static const uint VFX_SHAPE_SPHERE = 2u;

// What a particle is at an age
struct Particle {
    float3 position;
    float3 velocity;
    float size;
    float4 color;       // linear rgb, and the intensity it is drawn with
    float life;         // its age over its lifetime: 0 at birth, 1 at death
    float alive;        // 1 when its age is in [0, lifetime), else 0
};

// The integral of e^(-r s) over s from 0 to t: (1 - e^(-r t)) / r, its
// series where r t is small
float vfxDecay1(float r, float t) {
    float x = r * t;
    if (x < 0.001) {
        return t * (1.0 - x * 0.5 + x * x * 0.16666667);
    }
    return (1.0 - exp(0.0 - x)) / r;
}

// The integral of vfxDecay1 over t: (t - vfxDecay1(r, t)) / r, its series
// where r t is small
float vfxDecay2(float r, float t) {
    float x = r * t;
    if (x < 0.05) {
        return t * t * (0.5 - x * 0.16666667 + x * x * 0.041666668);
    }
    return (t - vfxDecay1(r, t)) / r;
}

// From 'a' to 'b' by 's' (written out, as the Cone side writes it)
float4 vfxMix(float4 a, float4 b, float s) {
    return a + (b - a) * s;
}

// A point (w 1) or a direction (w 0) carried by the placement's columns
float3 vfxPlace(EmitterParams e, float3 p, float w) {
    return e.column[0].xyz * p.x + e.column[1].xyz * p.y + e.column[2].xyz * p.z + e.column[3].xyz * w;
}

// Particle 'index' of the emitter 'e' at 'age' seconds
Particle particleAt(EmitterParams e, uint index, float age) {
    uint4 h1 = pcg4d(uint4(index, e.seeds.x, e.seeds.y, 0u));
    uint4 h2 = pcg4d(uint4(index, e.seeds.x, e.seeds.y, 1u));
    float lifetime = e.timing.z * (1.0 + e.timing.w * toSigned(h1.x));
    float speed = e.launch.w * (1.0 + e.motion.x * toSigned(h1.y));

    // The launch direction: uniform in the cone about local +y
    float cosTheta = 1.0 - toUnit(h1.z) * (1.0 - e.launch.z);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = VFX_TAU * toUnit(h1.w);
    float3 direction = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));

    // Where it is born, in the emitter's frame
    float3 start = float3(0.0, 0.0, 0.0);
    uint shape = uint(e.launch.x);
    if (shape == VFX_SHAPE_DISC) {
        float r = e.launch.y * sqrt(toUnit(h2.x));
        float psi = VFX_TAU * toUnit(h2.y);
        start = float3(r * cos(psi), 0.0, r * sin(psi));
    } else if (shape == VFX_SHAPE_SPHERE) {
        float z = toSigned(h2.x);
        float ring = sqrt(max(0.0, 1.0 - z * z));
        float psi = VFX_TAU * toUnit(h2.y);
        start = float3(ring * cos(psi), z, ring * sin(psi)) * e.launch.y;
    }
    float3 p0 = vfxPlace(e, start, 1.0);
    float3 v0 = vfxPlace(e, direction * speed, 0.0);

    // The closed form: linear drag k, a constant acceleration g, and
    // buoyancy b e^(-c t) upward
    float t = max(age, 0.0);
    float k = e.motion.y;
    float c = e.heat.x;
    float b = e.gravity.w;
    float3 g = e.gravity.xyz;
    float f1 = vfxDecay1(k, t);
    float f2 = vfxDecay2(k, t);
    float lift = b / (k - c);
    float rise = lift * (vfxDecay1(c, t) - f1);
    float riseSpeed = lift * (exp(0.0 - c * t) - exp(0.0 - k * t));
    float3 position = p0 + v0 * f1 + g * f2;
    float3 velocity = v0 * exp(0.0 - k * t) + g * f1;
    position.y = position.y + rise;
    velocity.y = velocity.y + riseSpeed;

    // The wobble: a horizontal spiral widening with age
    float angle = e.heat.z * t + VFX_TAU * toUnit(h2.z);
    float ca = cos(angle);
    float sa = sin(angle);
    float reach = e.heat.y * t;
    position.x = position.x + reach * ca;
    position.z = position.z + reach * sa;
    velocity.x = velocity.x + e.heat.y * ca - reach * e.heat.z * sa;
    velocity.z = velocity.z + e.heat.y * sa + reach * e.heat.z * ca;

    // Colour and size over life
    float life = t / lifetime;
    float4 color = e.colors[0];
    if (life >= e.colorAt.w) {
        color = e.colors[3];
    } else if (life >= e.colorAt.z) {
        color = vfxMix(e.colors[2], e.colors[3], (life - e.colorAt.z) * e.colorInv.z);
    } else if (life >= e.colorAt.y) {
        color = vfxMix(e.colors[1], e.colors[2], (life - e.colorAt.y) * e.colorInv.y);
    } else if (life >= e.colorAt.x) {
        color = vfxMix(e.colors[0], e.colors[1], (life - e.colorAt.x) * e.colorInv.x);
    }
    float fade = 1.0;
    if (e.sizes.z > 0.0) { fade = min(fade, life * e.sizes.z); }
    if (e.sizes.w > 0.0) { fade = min(fade, (1.0 - life) * e.sizes.w); }
    color.w = color.w * max(fade, 0.0);
    float size = (e.sizes.x + (e.sizes.y - e.sizes.x) * life) * (1.0 + e.motion.w * toSigned(h2.w));

    Particle q;
    q.position = position;
    q.velocity = velocity;
    q.size = size;
    q.color = color;
    q.life = life;
    q.alive = (age >= 0.0 && age < lifetime) ? 1.0 : 0.0;
    return q;
}

// Particle 'index' at the emitter's elapsed time: born at index times the
// spawn interval
Particle particleNow(EmitterParams e, uint index) {
    return particleAt(e, index, e.timing.x - float(index) * e.timing.y);
}
