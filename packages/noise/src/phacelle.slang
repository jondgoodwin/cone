// Phacelle noise for shaders: phacelle.cone's function, under the same name.
// A shader reaches it with 'import phacelle;' (which imports noise).
//
// Phacelle Noise function copyright (c) 2025 Rune Skovbo Johansen.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Ported from the author's Simple Phacelle Noise function (his Shadertoys
// t3KczR and wXcfWn). This
// file alone is under the MPL 2.0, as the original is. Changed from the
// original: each cell's random offset comes from the noise module's integer
// hash (hash2) of the cell under a seed, not from the original's float hash;
// and the result is a struct rather than a float4.

import noise;

struct Phacelle {
    float2 value;
    float2 side;
};

[noinline]
Phacelle phacelle2(float2 p, float2 dir, float freq, float offset, float normalization, uint seed) {
    float tau = 6.28318530717959;
    float sideX = -dir.y * freq * tau;
    float sideY = dir.x * freq * tau;
    float phase = offset * tau;
    float fx = floor(p.x);
    float fy = floor(p.y);
    int ix = int(fx);
    int iy = int(fy);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float cx = 0.0;
    float cy = 0.0;
    float weightSum = 0.0;
    for (int i = -1; i <= 2; i++) {
        for (int j = -1; j <= 2; j++) {
            uint3 h = hash2(ix + i, iy + j, seed);
            float vx = tx - float(i) - toSigned(h.x) * 0.5;
            float vy = ty - float(j) - toSigned(h.y) * 0.5;
            float sqrDist = vx * vx + vy * vy;
            float weight = exp(-sqrDist * 2.0) - 0.01111;
            if (weight < 0.0) weight = 0.0;
            weightSum = weightSum + weight;
            float wave = vx * sideX + vy * sideY + phase;
            cx = cx + cos(wave) * weight;
            cy = cy + sin(wave) * weight;
        }
    }
    float ax = cx / weightSum;
    float ay = cy / weightSum;
    float magnitude = sqrt(ax * ax + ay * ay);
    if (magnitude < 1.0 - normalization) magnitude = 1.0 - normalization;
    Phacelle r;
    r.value = float2(ax / magnitude, ay / magnitude);
    r.side = float2(sideX, sideY);
    return r;
}
