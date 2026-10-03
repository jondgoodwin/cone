# noise

Coherent noise for displacement and procedural textures: a pure function of
a seed and a point, the same on every CPU and, for all but Phacelle noise,
bit for bit the same on the GPU. Cone source in `src/*.cone`; the same
functions for shaders in `src/noise.slang` and `src/phacelle.slang`.

```cone
import noise use *;

imm n = fbm2(new Vec2(x, y), seed, 6, 2., 0.5);   // n.value, and n.d, its derivative
imm c = cellular3(p, 1., seed);                // c.f1, c.f2, c.id
```

```slang
// slangc: -fp-mode precise
import noise;
Noise2 n = fbm2(float2(x, y), seed, 6, 2.0, 0.5);
```

## API

Numbers are `f32`; points are `geomath`'s `Vec2` and `Vec3` (`float2`,
`float3` in Slang). Every function takes the seed, a `u32`, explicitly; none
keeps state. The Slang module has the same names and fields.

**Hashes** (`hash.cone`)

| Function | What it gives |
|---|---|
| `pcg(v u32) u32` | PCG hash, one word to one |
| `pcg2d(UVec2) UVec2`, `pcg3d(UVec3) UVec3`, `pcg4d(UVec4) UVec4` | Jarzynski and Olano's multi-dimensional hashes, n words to n |
| `hash1(x i32, seed) u32` | a coordinate under a seed: `pcg2d(x, seed).x` |
| `hash2(x, y, seed) UVec3` | a 2-D lattice point: `pcg3d(x, y, seed)` |
| `hash3(x, y, z, seed) UVec4` | a 3-D lattice point: `pcg4d(x, y, z, seed)` |
| `toUnit(h u32) f32` | a word to [0, 1): its top 24 bits times 2^-24, exact |
| `toSigned(h u32) f32` | a word to [-1, 1): its top 24 bits times 2^-23, minus 1, exact |
| `heavySigned(u, coin u32, ratio f32, cap i32) f32` | a heavy-tailed draw: `toSigned(u)` times `ratio` once per low set bit of `coin` (up to the first clear one, at most `cap`); a power-law tail of index log 2 / log ratio; multiplications only. Not yet in Slang |

**Lattice noise** (`lattice.cone`), each returning `Noise2 {value, d}` or
`Noise3 {value, d}`, `d` the analytic derivative with respect to the point
(Quilez's formulation; Perlin's quintic fade):

| Function | Range |
|---|---|
| `value2(p, seed)`, `value3(p, seed)` | [-1, 1) |
| `gradient2(p, seed)` | [-1, 1] (0 at lattice points) |
| `gradient3(p, seed)` | [-1.5, 1.5] |
| `value2Periodic(p, period, seed)`, `value3Periodic`, `gradient2Periodic`, `gradient3Periodic` | the same, repeating every `period` cells |

The gradient at a lattice point is two or three hashed components each in
[-1, 1), not a unit vector from a table; the ranges above are proved in
`tests/lattice.cone`'s header.

**Cellular noise** (`cellular.cone`): `cellular2(p, jitter, seed) Cell2` and
`cellular3(p, jitter, seed) Cell3`, with `f1` and `f2` (distances to the
nearest and second nearest feature point), `id` (a hash word of the nearest
point's cell) and `toNearest` (the vector to it). `jitter` from 0 (a regular
grid) to 1 (anywhere in the cell). The search is exact (the 3 x 3 cells, then
the ring beyond, pruned by F2).

**Fractal sums** (`fractal.cone`), `(p, seed, octaves, lacunarity, gain)`,
returning `Noise2` or `Noise3` with the derivative:

| Function | Each octave's layer |
|---|---|
| `fbm2`, `fbm3` | gradient noise n |
| `ridged2`, `ridged3` | (1 - abs(n))^2 |
| `billow2`, `billow3` | 2 abs(n) - 1 |

Octave i is gradient noise at `lacunarity^i` times the frequency, seeded by
`hash1(i, seed)`, weighted `gain^i`. The sum is not normalized (a division is
not exact on a GPU): with gain g it lies within 1 / (1 - g) of one octave's
range.

**Domain warp**: `warp2(p, seed, octaves, strength) f32` and `warp3`,
Quilez's two-level warp f(p + s r), r = f(p + s q + c), q = f(p + c'), f an
fBm (lacunarity 2, gain 0.5). The offsets c are exact in binary. Value only.

**Multiplicative cascade**: `cascade2(p, seed, octaves, lacunarity, gain,
intermittency) f32`, fBm whose octave i is weighted `gain^i` times
W_0 ... W_i, each W_j = 1 + intermittency times a gradient noise at octave
j's frequency seeded by `hash1(j, pcg(seed))`: mean 1, but detail clusters
where coarse weights were large. Intermittency 0 is `fbm2`'s value, bit for
bit; keep it at most 1. Value only, and not yet in Slang.

**Phacelle noise** (`phacelle.cone`, MPL 2.0):
`phacelle2(p, dir, freq, offset, normalization, seed) Phacelle`, with
`value` = (cos, sin) of the stripes' phase and `side` (across the stripes,
times 2 pi freq; `-value.y * side` is the derivative of `value.x`). `dir` is
the stripes' direction at `p`, unit length, and may vary from point to point.

## Determinism

These are the reproduction plan's rules (§3) as this package keeps them.

1. **Hash, don't stream.** Every random number is an integer hash of
   (seed, coordinates). There is no generator state and no platform RNG.
2. **No float hash.** Nothing hashes with `fract(sin(x) * k)`, whose result
   depends on the GPU's `sin`, nor with Hoskins's float "hash without sine".
   Words become floats only by `toUnit` and `toSigned`, exactly.
3. **Decisions on integers or on exactly computed floats.** Which cell is
   nearest is decided on squared distances, cells visited in one fixed order,
   ties kept by the first; the id returned is a hash word.
4. **One order of operations, no contraction.** Each float expression is
   written the same way in Cone and Slang: additions, subtractions,
   multiplications and `floor`, and constants exact in binary (the warp's
   offsets are quarters, not iq's 5.2 and 1.3). No division, and no
   transcendental function, outside Phacelle.
5. **Integer remainder on non-negative operands only.** The periodic forms
   wrap lattice coordinates with an unsigned remainder.

### What the toolchain does, measured

**CPU.** conec generates code for a `generic` x86-64 CPU (no FMA
instructions) and emits separate `fmul` and `fadd` with no fast-math flags,
so LLVM does not contract or reassociate; debug and release builds of the
parity test gave the same bits.

**Slang.** With slangc 2026.13.1 (Vulkan SDK 1.4.357), the `precise`
keyword on a variable or function emits **no** `NoContraction` decoration in
the SPIR-V: it is silently dropped. The compile option `-fp-mode precise`
decorates every float operation `NoContraction`. `tools/shaders/shaders.py`
passes it to a shader whose source has the line `// slangc: -fp-mode precise`;
the option applies to the whole compile, the imported modules included, so
**a shader that imports `noise` needs that line**. Without it, the driver is
free to fuse a multiply and an add into one rounding.

**Drivers.** The parity test, compiled without `-fp-mode precise`, found
that both drivers do fuse: on the RTX 4060 and the UHD 770 alike, about half
of the samples of every noise differed from the CPU (gradient noise by up to
5e-7, the domain warps by up to 9e-5, which amplify it), and no longer
matched each other. With it, they match the CPU exactly.

**Signed remainder.** Vulkan does not define `OpSRem` (Slang's `%` on `int`)
with a negative operand. The first periodic noise used `((i % p) + p) % p`;
on the RTX 4060 a period of 3 or 5 gave a different lattice from the CPU
for negative coordinates (a period of 4 did not). The package now uses an
unsigned remainder of a non-negative value.

**What stays inexact, and why.**

- **Square roots** (cellular `f1`, `f2`): Vulkan does not require `sqrt` to
  be correctly rounded; measured within 1 ulp on both GPUs. The squared
  distances they come from, the id and `toNearest` are exact.
- **Phacelle noise**: `exp`, `cos`, `sin` and division, which Vulkan
  computes to bounds (absolute 2^-11 for `sin` and `cos` in [-pi, pi], a few
  ulps for the rest) and the CPU's C library differently again. Measured
  within 6.3e-7 on the RTX 4060 and 2.2e-5 on the UHD 770. Its `side` vector
  is exact. Phacelle is for appearance, not for decisions.
- **Denormals**: a GPU may flush them to zero, the CPU does not. Neither the
  parity test's samples produced none; a noise value below 1.2e-38 could
  differ by that much.
- **CPU trigonometry**: Phacelle calls `geomath.cos` and `geomath.sin` (the C
  library's, for now) and `libc.expf`, so two CPUs with different C
  libraries may differ too, until geomath's deterministic trigonometry.

### The parity test

`tests/parity.cone` draws `tests/parity.slang` into a 64 x 256 rgba8unorm
target: 64 quantities (every hash, and each field of every noise's result)
at 256 points (half of them near (1000, -777, 4096), where an f32 has fewer
bits below the point), each quantity's 32 bits spread over a pixel's four
bytes, and compares the readback with the Cone package. It needs no compute
pipeline. Set `NOISE_PARITY_REPORT=1` to print the per-quantity table, and
`GPU_POWER_PREFERENCE=low-power` to test the integrated GPU.

Measured 27 Sep 2026 (NVIDIA driver 592.82, Intel 101.6790):

| Quantities | RTX 4060 | UHD 770 |
|---|---|---|
| hashes, `toUnit`, `toSigned`, cell ids | 256/256 exact | 256/256 exact |
| value, gradient (plain and periodic), their derivatives | exact | exact |
| cellular `toNearest` | exact | exact |
| fBm, ridged, billow (2-D and 3-D, with derivatives) | exact | exact |
| `warp2`, `warp3` | exact | exact |
| cellular F1, F2 | within 1 ulp (203-222 of 256 exact) | within 1 ulp (227-240 exact) |
| Phacelle (cos, sin) | within 6.3e-7 | within 2.2e-5 |
| Phacelle `side` | exact | exact |

### The bake test: compute

`tests/bake.cone` bakes fBm into a 3-D texture with a compute shader
(`tests/bake.slang`): 64 x 64 x 64 voxels, each the point (-2.03125,
0.53125, 1.25) plus a sixteenth per voxel, fBm of 4 octaves (lacunarity 2,
gain 0.5, seed 7) into an r32float 3-D storage texture, the voxel's
`hash3` into an r32uint one, and the derivative and value into a storage
buffer. It compares every voxel with the Cone package. Set
`NOISE_BAKE_REPORT=1` to print the table, and `GPU_POWER_PREFERENCE=low-power`
for the integrated GPU. `examples/volume.cone` draws the same volume: a
slice, and a sphere textured through it.

Measured 27 Sep 2026 (same drivers), 262,144 voxels:

| Quantity | RTX 4060 | UHD 770 |
|---|---|---|
| `hash3` | all exact | all exact |
| fBm, in the texture and in the buffer | all exact | all exact |
| its derivative, x, y and z | all exact | all exact |

Compiled without `-fp-mode precise`, the same shader matched the CPU at
only 18-27% of the voxels, on both GPUs (fBm off by up to 1.3e-6, its
derivative by up to 2.7e-6).

## Sources

- Mark Jarzynski and Marc Olano, "Hash Functions for GPU Rendering", Journal
  of Computer Graphics Techniques 9(3), 2020, https://jcgt.org/published/0009/03/02/
  (`pcg3d`, `pcg4d` from the paper; `pcg2d` from its companion Shadertoy,
  XlGcRh).
- Melissa O'Neill, PCG, 2014 (the permuted congruential generator); Nathan
  Reed, "Hash Functions for GPU Rendering" (blog, 2021), for `pcg`'s form.
- Ken Perlin, "Improving Noise", SIGGRAPH 2002 (the quintic fade).
- Inigo Quilez: value and gradient noise with analytic derivatives
  (Shadertoy XdXBRH and his articles on iquilezles.org), and his article on
  domain warping.
- Steven Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996.
- F. Kenton Musgrave, ridged multifractals, in Ebert et al., *Texturing and
  Modeling: A Procedural Approach*; `ridged` is the simple form, each octave
  (1 - abs(n))^2, without Musgrave's weighting by the octave before.
- Rune Skovbo Johansen, "Phacelle - Cheap Directional Noise"
  (blog.runevision.com, 22 January 2026), and his Shadertoys (t3KczR,
  "Simple Phacelle Noise Profile"; the erosion filter, wXcfWn, carries the
  same function). Shadertoy refused an automated fetch, so the function was
  ported from its text as reproduced, with the author's notice, in a public
  repository (MangoButtermilch/texturinator); it matches the blog's
  description (4 x 4 cells, cosine and sine blended, normalized).
  **Licence: Mozilla Public License 2.0**, which allows the port on the
  condition that the ported files stay under the MPL; so `phacelle.cone` and
  `phacelle.slang` carry the original's notice and are under the MPL 2.0,
  and the rest of the package is under the repository's MIT licence. The
  port replaces the original's float hash with this package's integer hash.
- Thibault Tricard et al., "Procedural Phasor Noise", SIGGRAPH 2019: the
  noise Phacelle approximates at a fraction of the cost.
