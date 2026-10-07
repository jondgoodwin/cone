// The analyticdaylight package's sky for shaders: Preetham's analytic sky,
// the twilight patch, the night's floor, the sun's disc, the moon's disc with
// a phase and a field of stars that turns with the hour. A shader reaches it
// with 'import analyticdaylight;'. It is a module with no entry points:
// tools/shaders/shaders.py compiles the shaders that import it.
//
// The numbers come from the CPU: 'DaySky.pack' (daysky.cone) writes the
// eight float4s of 'DaySkyParams', in this order, and the sky's radiance
// (daySkyRadiance) is the same arithmetic as 'DaySky.radiance' (preetham.cone
// and daysky.cone), so a ground lit on the CPU from the sky and a sky drawn
// here agree. The sun's and the moon's discs and the stars are drawn over it
// by shaders only (the CPU never needs them).
//
// All radiance is nits, linear RGB, before the exposure; a shader multiplies
// what it writes by 'frame.exposure.x' (render's renderframe.slang).
// Directions are unit length, y up, x east, z south, pointing from the viewer.

struct DaySkyParams {
    float4 sun;        // xyz towards the sun; w its height, radians
    float4 moon;       // xyz towards the moon; w the share of its disc lit
    float4 zenith;     // x the zenith's luminance, nits; y, z its x, y chromaticity; w the turbidity
    float4 twilight;   // x the twilight patch's mix; y its unit, nits; z the sun glow's weight; w the pink band's
    float4 sunDisc;    // rgb the sun disc's radiance, nits; w its angular radius, radians
    float4 nightSky;   // rgb the sky's floor, nits; w the night's share, 0 to 1 (the stars')
    float4 pole;       // xyz the celestial pole; w the sky's turn, radians
    float4 moonDisc;   // rgb the lit moon's radiance, nits; w its angular radius, radians
};

static const float DAYSKY_PI = 3.14159265358979;

// 0 at 'a', 1 at 'b' (either way round), the cubic smoothstep between
float daySmooth(float a, float b, float x) {
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

// Preetham's F(theta, gamma) with coefficients (A..E) in 'c' and 'e'
float perezF(float a, float b, float c, float d, float e, float cosTheta, float gamma) {
    float ct = max(cosTheta, 0.01);
    float cg = cos(gamma);
    return (1.0 + a * exp(b / ct)) * (1.0 + c * exp(d * gamma) + e * cg * cg);
}

// CIE xyY to linear sRGB (Rec. 709, D65)
float3 xyYToLinear(float x, float y, float lum) {
    float yy = max(y, 0.001);
    float bigX = x * lum / yy;
    float bigZ = (1.0 - x - yy) * lum / yy;
    return float3(3.2404542 * bigX - 1.5371385 * lum - 0.4985314 * bigZ,
                  -0.9692660 * bigX + 1.8760108 * lum + 0.0415560 * bigZ,
                  0.0556434 * bigX - 0.2040259 * lum + 1.0572252 * bigZ);
}

// Preetham's sky in direction 'dir' (appendix A.2, page 98 of the paper;
// preetham.cone has the sources): the coefficients are linear in the turbidity
float3 preethamRadiance(float3 dir, DaySkyParams p) {
    float t = p.zenith.w;
    float cosGamma = clamp(dot(dir, p.sun.xyz), -1.0, 1.0);
    float gamma = acos(cosGamma);
    float ct = max(dir.y, 0.0);
    // The sun's angle from the zenith, held to 1.5 rad as the CPU does
    float ts = min(acos(clamp(p.sun.y, -1.0, 1.0)), 1.5);
    float f0y = perezF(0.1787 * t - 1.4630, -0.3554 * t + 0.4275, -0.0227 * t + 5.3251, 0.1206 * t - 2.5771, -0.0670 * t + 0.3703, 1.0, ts);
    float f0x = perezF(-0.0193 * t - 0.2592, -0.0665 * t + 0.0008, -0.0004 * t + 0.2125, -0.0641 * t - 0.8989, -0.0033 * t + 0.0452, 1.0, ts);
    float f0c = perezF(-0.0167 * t - 0.2608, -0.0950 * t + 0.0092, -0.0079 * t + 0.2102, -0.0441 * t - 1.6537, -0.0109 * t + 0.0529, 1.0, ts);
    float fy = perezF(0.1787 * t - 1.4630, -0.3554 * t + 0.4275, -0.0227 * t + 5.3251, 0.1206 * t - 2.5771, -0.0670 * t + 0.3703, ct, gamma);
    float fx = perezF(-0.0193 * t - 0.2592, -0.0665 * t + 0.0008, -0.0004 * t + 0.2125, -0.0641 * t - 0.8989, -0.0033 * t + 0.0452, ct, gamma);
    float fc = perezF(-0.0167 * t - 0.2608, -0.0950 * t + 0.0092, -0.0079 * t + 0.2102, -0.0441 * t - 1.6537, -0.0109 * t + 0.0529, ct, gamma);
    float lum = max(p.zenith.x * fy / f0y, 0.0);
    return xyYToLinear(p.zenith.y * fx / f0x, p.zenith.z * fc / f0c, lum);
}

// The twilight patch in direction 'dir' (daysky.cone, DaySky.twilight)
float3 twilightRadiance(float3 dir, DaySkyParams p) {
    float h = clamp(dir.y, 0.0, 1.0);
    float hl = sqrt(dir.x * dir.x + dir.z * dir.z);
    float sl = sqrt(p.sun.x * p.sun.x + p.sun.z * p.sun.z);
    float az = 0.0;
    if (hl > 0.0001 && sl > 0.0001) { az = (dir.x * p.sun.x + dir.z * p.sun.z) / (hl * sl); }
    float side = pow(max(az, 0.0), 3.0);
    float3 zenithCol = float3(0.40, 0.70, 1.60);
    float3 horizonCol = float3(1.45, 0.95, 0.90);
    float3 base = zenithCol + (horizonCol - zenithCol) * exp(-5.0 * h);
    float3 glow = float3(7.0, 2.0, 0.45) * (side * exp(-7.0 * h) * p.twilight.z);
    float elDeg = asin(h) * (180.0 / DAYSKY_PI);
    float q = (elDeg - 8.0) / 5.0;
    float band = exp(-q * q) * (1.0 - side) * p.twilight.w * 0.5;
    return (base + glow + float3(0.9, 0.55, 0.65) * band) * p.twilight.y;
}

// The sky with no sun, moon or stars: DaySky.radiance
float3 daySkyRadiance(float3 dir, DaySkyParams p) {
    float3 c = float3(0.0, 0.0, 0.0);
    if (p.twilight.x < 1.0) { c = preethamRadiance(dir, p) * (1.0 - p.twilight.x); }
    if (p.twilight.x > 0.0) { c = c + twilightRadiance(dir, p) * p.twilight.x; }
    return c + p.nightSky.rgb;
}

// The sun's disc, and a faint corona, radiance to add to the sky
float3 sunDiscRadiance(float3 dir, DaySkyParams p) {
    float ang = acos(clamp(dot(dir, p.sun.xyz), -1.0, 1.0));
    float r = p.sunDisc.w;
    float disc = 1.0 - daySmooth(r * 0.85, r * 1.15, ang);
    float corona = 0.002 * exp(-ang / (r * 4.0));
    return p.sunDisc.rgb * (disc + corona);
}

// A hash of three integers to [0, 1) (own: a small integer mix; the stars'
// look is not a thing the CPU has to reproduce)
float dayHash(int3 c, int salt) {
    uint x = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ uint(salt) * 2654435761u;
    x = (x ^ (x >> 16u)) * 2246822519u;
    x = (x ^ (x >> 13u)) * 3266489917u;
    x = x ^ (x >> 16u);
    return float(x >> 8u) * (1.0 / 16777216.0);
}

// Smooth value noise in 3-D, [0, 1)
float dayNoise3(float3 x) {
    int3 i = int3(floor(x));
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = lerp(lerp(dayHash(i, 1), dayHash(i + int3(1, 0, 0), 1), f.x), lerp(dayHash(i + int3(0, 1, 0), 1), dayHash(i + int3(1, 1, 0), 1), f.x), f.y);
    float b = lerp(lerp(dayHash(i + int3(0, 0, 1), 1), dayHash(i + int3(1, 0, 1), 1), f.x), lerp(dayHash(i + int3(0, 1, 1), 1), dayHash(i + int3(1, 1, 1), 1), f.x), f.y);
    return lerp(a, b, f.z);
}

// The moon's disc: a lit sphere (its terminator by the phase, its bright
// limb towards the sun's direction), dark seas from noise, and a trace of
// earthshine on the dark side; radiance to add to the sky
float3 moonDiscRadiance(float3 dir, DaySkyParams p) {
    float3 m = p.moon.xyz;
    float cosd = dot(dir, m);
    if (cosd < 0.9) { return float3(0.0, 0.0, 0.0); }
    float r = p.moonDisc.w;
    // The disc's frame: forward the moon, 'limb' the sun's direction seen from
    // the moon (its bright side), 'right' across
    float3 s = p.sun.xyz - m * dot(p.sun.xyz, m);
    float3 limb = (dot(s, s) > 0.0001) ? normalize(s) : normalize(cross(m, float3(1.0, 0.0, 0.0)));
    float3 right = normalize(cross(limb, m));
    float u = dot(dir, right) / r;
    float v = dot(dir, limb) / r;
    float rr = u * u + v * v;
    if (rr > 1.1) { return float3(0.0, 0.0, 0.0); }
    float edge = 1.0 - daySmooth(0.92, 1.0, sqrt(rr));
    float z = sqrt(max(1.0 - rr, 0.0));
    float3 n = float3(u, v, z);
    // The phase: the lit share 'f' is (1 + cos psi) / 2 for the angle psi
    // between the directions to the sun and to the viewer
    float cosPsi = 2.0 * p.moon.w - 1.0;
    float sinPsi = sqrt(max(1.0 - cosPsi * cosPsi, 0.0));
    float3 L = float3(0.0, sinPsi, cosPsi);
    float lit = daySmooth(-0.03, 0.05, dot(n, L));
    // Seas: two octaves of noise over the disc, darker, and a little crater grain
    float3 q = float3(u, v, 0.0) * 2.2 + float3(11.0, 3.0, 7.0);
    float seas = 0.62 + 0.38 * (1.0 - daySmooth(0.42, 0.62, 0.65 * dayNoise3(q) + 0.35 * dayNoise3(q * 2.7)));
    float grain = 0.93 + 0.14 * dayNoise3(float3(u, v, 0.0) * 14.0);
    float bright = lit * seas * grain + 0.012;
    return p.moonDisc.rgb * bright * edge;
}

// The stars: points on the celestial sphere, rotated with the sky's turn,
// each a small soft dot of its own brightness and tint that twinkles, with a
// faint band of the Milky Way. 'pixel' is the angle one pixel covers
// (radians), so a star is about a pixel wide whatever the lens; 'time' is
// seconds; 'peak' is the radiance, nits, of the brightest star: the caller
// chooses it, since a star is far less than a nit and a scene that exposes for
// its ground at night (the browser's EV -1) sets it so the brightest star
// reads white whatever the exposure. Radiance to add to the sky, already
// times the night's share.
float3 starRadiance(float3 dir, DaySkyParams p, float pixel, float time, float peak) {
    float night = p.nightSky.w;
    if (night <= 0.0 || dir.y < -0.05) { return float3(0.0, 0.0, 0.0); }
    // To the celestial frame: dir turned about the pole by the sky's turn (Rodrigues)
    float3 ax = p.pole.xyz;
    float ca = cos(p.pole.w);
    float sa = sin(p.pole.w);
    float3 c = dir * ca + cross(ax, dir) * sa + ax * (dot(ax, dir) * (1.0 - ca));
    const float SCALE = 140.0;
    float3 cell = floor(c * SCALE);
    float3 sum = float3(0.0, 0.0, 0.0);
    float radius = max(pixel, 0.0008);
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int3 k = int3(cell) + int3(dx, dy, dz);
                float h = dayHash(k, 2);
                if (h < 0.012) {
                    float3 jitter = float3(dayHash(k, 3), dayHash(k, 4), dayHash(k, 5));
                    float3 q = (float3(k) + 0.15 + 0.7 * jitter) / SCALE;
                    float3 sd = normalize(q);
                    float d = length(c - sd);
                    float amp = 0.08 + 0.92 * pow(dayHash(k, 6), 8.0);
                    float tint = dayHash(k, 7);
                    float3 col = lerp(float3(1.0, 0.85, 0.7), float3(0.75, 0.85, 1.0), tint);
                    float tw = 1.0 + 0.2 * sin(time * 5.0 + h * 1300.0);
                    sum += col * (amp * tw * exp(-(d * d) / (radius * radius)));
                }
            }
        }
    }
    // The Milky Way: a faint band round a great circle, broken by noise
    float3 gal = normalize(float3(0.25, 1.0, 0.35));
    float g = dot(c, gal) / 0.16;
    float band = exp(-g * g) * (0.35 + 0.65 * dayNoise3(c * 5.0));
    float3 sky = (sum + float3(0.7, 0.8, 1.0) * (band * 0.05)) * peak;
    float horizon = daySmooth(0.0, 0.12, dir.y);
    return sky * (night * horizon);
}
