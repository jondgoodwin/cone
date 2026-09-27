# sdf

Signed distance fields: shapes as functions from a point to its distance
from the surface, negative inside. Cone source in `src/*.cone`; the same
functions, in the same order of operations, for shaders in `src/sdf.slang`.

```cone
import sdf use *;

fn scene(p Vec3) f32 {
  imm body = roundBox(p, Vec3[1., 0.5, 0.5], 0.1);
  imm hole = cylinder(rotate(p, turn), 1., 0.3);
  smoothSubtract(body, hole, 0.05);
}

imm horn = Horn.make(3.4, 0.34, 0.97, 1.7, 22, 0.12);   // length, base radius, taper, curl, ribs, rib depth
imm d = horn.distance(p);
imm n = normal(&horn, p, 0.001);
```

```slang
// slangc: -fp-mode precise
import sdf;
Horn horn = makeHorn(3.4, 0.34, 0.97, 1.7, 22, 0.12);
float d = horn.distance(p);
float3 n = normal(horn, p, 0.001);
```

`examples/hornmarch.cone` sphere-traces the horn on the GPU, in render's
chitin under the dusk sky (a preview: meshing is to come).

## API

Numbers are `f32`, points `geomath`'s `Vec3` (`float3` in Slang), rotations
unit `Quat`s (`float4` x, y, z, w). "Exact" is the Euclidean distance; a
"bound" never exceeds it. Every function says which in its comment.

**Primitives** (`primitive.cone`), at the origin, round ones about y:
`sphere(p, radius)`, `box(p, extent)`, `roundBox(p, extent, radius)`,
`capsule(p, a, b, radius)`, `torus(p, major, minor)`,
`cylinder(p, halfHeight, radius)`, `cone(p, halfHeight, bottom, top)`,
`roundCone(p, bottom, top, height)`, `plane(p, n, offset)`: exact.
`ellipsoid(p, radii)`: Quilez's approximation, not a bound (below).

**Operators on distances** (`operator.cone`): `union`, `subtract`,
`intersect`; `smoothUnion(a, b, k)`, `smoothSubtract`, `smoothIntersect`,
Quilez's quadratic smooth minimum normalized so that `k` is the most the
blend swells; `smoothUnionBlend(a, b, k) Blend` with the blend factor
(0 all a's material, 1 all b's); `onion(d, thickness)`, `round(d, radius)`,
`exclusive(a, b)`.

**Operators on the point** (`domain.cone`): `translate(p, offset)`,
`rotate(p, q)` (the point for a shape turned by q), `scalePoint(p, s)` with
`scaleDistance(d, s)` (both are needed, or the field is s-Lipschitz),
`Placement` (position, rotation, uniform scale: `toLocal(p)`,
`distance(d)`), `mirrorX`/`Y`/`Z`, `mirror(p, n, offset)`,
`repeat(p, period)`, `repeatLimited(p, period, limit)` (exact for shapes
symmetric in their cells), `elongate(p, extent)`.

**Fillets** (`fillet.cone`, from hg_sdf, MIT): `unionRound`,
`intersectRound`, `subtractRound`, `unionChamfer`,
`unionColumns(a, b, r, n)`, `unionStairs(a, b, r, n)`, `intersectStairs`,
`subtractStairs`, `pipe`, `groove`, `tongue`, `engrave`.

**Repetition along a curve** (`curve.cone`):

| Name | What it is |
|---|---|
| `Curve` (trait) | `length()`, `nearest(p)` (arc length of the nearest point), `frameAt(s) CurveFrame` |
| `Repeated` (trait) | `repeatedDistance(q, index, u)` (the copy in its frame: x normal, y binormal, z tangent; u from 0 to 1 along), `bound(index, u)` (a ball about the frame's origin holding the copy) |
| `repeatAlong(curve, shape, p, count)` | the exact union of `count` copies at even steps of arc length |
| `Arc.make(length, curl)` | a circular arc from the origin up +y, turning towards +x through `curl` radians; rotation-minimizing frames (binormal +z); `project(p)` (arc length and distance, no trigonometry inside the arc), `point(s)`, `cells(count) ArcCells` |
| `repeatAlongArc(arc, cells, shape, p)` | the same union on an arc, its cells' frames stepped round by rotation, no trigonometry |
| `PathCurve.fromPath(&path)` | a `sculpt.Path` with sculpt's rotation-minimizing frames (CPU only) |

A copy's size and twist are the shape's own functions of `u` (and of its
index, for hashed variation).

**The horn** (`horn.cone`): `Horn.make(length, baseRadius, taper, curl,
ribs, ribDepth)`, Latham's horn and Raup's unwound shell: an Arc cut into
one cell per rib, each cell a round cone along its chord (radius falling
linearly by `taper` of the base) smooth-unioned with a torus rib as thick as
`ribDepth` of the radius. `distance(p)`, `radiusAt(u)`.

**Gradient** (`shape.cone`): the `Shape` trait (`distance(p)`), `FnShape`
(a `&fn(p Vec3) f32` as a Shape), `gradient(shape, p, h)` (central
differences, six evaluations), `normal(shape, p, h)` (Quilez's tetrahedron,
four).

**Noise detail** (`detail.cone`, over `noise`):
`displace(d, p, seed, octaves, frequency, amplitude)` adds fBm and is not a
distance; `fbmDetail(d, p, seed, octaves, size)` is Quilez's fBm of
spheres, clipped to a band at the surface and smooth-unioned on, which stays
a bound; `latticeSpheres(p, seed)`, its octave.

In Slang the constructors are `makeArc`, `makeArcCells`, `makeHorn`; the
traits are the interfaces `IShape`, `ICurve`, `IRepeated`.

## Distances: what holds, measured

`tests/lipschitz.cone` samples 4096 pairs of points 1/64 apart for each
shape and checks |d(p) - d(q)| / |p - q|:

- Every primitive but the ellipsoid, every hard and smooth operator, onion,
  round, placement (turned, moved, scaled), mirror, limited repetition and
  elongation: at most 0.99999.
- **The horns** (curled 2 and 4.5 radians, and straight): at most 0.99991;
  the gradient's length at most 1.007 (central differences at the creases).
- **fBm detail**: 0.9994.
- **Fillets**: 1 where the two surfaces meet at a right angle. In general
  the gradient with respect to (a, b) has length at most 1 but the two
  shapes' gradients add, so the field is up to sqrt 2-Lipschitz: 1.30 for
  planes at 45 degrees, 1.36 on the test's sphere and box. A sphere tracer
  should shorten its steps there.
- **The ellipsoid**: 1.20. Not a bound.
- **Displacement**: 2.27 (amplitude 0.1 at frequency 4): not a distance.

Two findings shaped the code:

- **Repetition along a curve accounts for every copy.** A window of the
  point's cell and its neighbours (Quilez's advice for varying copies) is not
  enough along a curve: near the centre of curvature every copy is about as
  far away and the cell says nothing. Measured with such a window, the horn's
  ratio reached 2.2. `repeatAlong` asks the copy whose origin is nearest,
  then every other copy whose bounding ball is nearer than the best so far:
  the exact union, continuous everywhere. `tests/curves.cone` checks over
  2048 points that the pruning gives the same bits as asking every copy.
  Magniez and Zanni (I3D 2026, doi:10.1145/3804498) interpolate instead;
  that is not built. The horn's tube is itself cells, round cones along
  chords (a tube whose radius depends on the nearest point's arc length is
  not Lipschitz near the centre of curvature either).
- **hg_sdf's Columns switches to the plain union outside its band**, which
  cuts the field where a column crosses the band's edge (measured ratio
  6.7). `unionColumns` clips the columns to the band instead: the same
  surface, a continuous field.

## CPU and GPU agree

`tests/parity.cone` draws 58 quantities at 256 points with `sdf.slang` on
the GPU and compares each with the Cone package (the pattern of `noise`'s
parity test). Measured on the RTX 4060 and the UHD 770:

| Quantities | Agree |
|---|---|
| `plane`, `rotate`, `mirror` (add, subtract, multiply only) | bit for bit |
| every other primitive, operator, domain operator and fillet, fBm detail, displacement (square roots, divisions) | within 7.2e-7 (the ellipsoid's); from half (the ellipsoid) to 95% of samples bit for bit |
| the arc's projection, the horns, repetition along the arc (atan2, cos, sin) | within 4.8e-7 |
| the horn's gradient and normal (differences over 2 h = 1/32) | within 1.5e-5 |

The test holds them to 2^-19, 2^-16 and 2^-12: the measured differences
with a margin, not Vulkan's bounds (which allow trigonometry 2^-11), so a
change to the order of operations on one side shows. The horn itself uses
no trigonometry per evaluation (its cells step round by rotation), only
when it is made. The example's frames on the two GPUs differ by at most
3/255 in any channel.

Shaders that import `sdf` need `// slangc: -fp-mode precise`, since it
imports `noise` (whose README, "Determinism", says why).

## The representation is not decided

Shapes are composed here as code: functions of a point, the same functions
in Slang. Whether what is sent between machines is Cone source, a data tree
of operators that can be serialized and evaluated, or something else is an
open question. Each function here is one node of such a tree (a primitive
with its sizes, an operator with its constants, a domain operator on the
point, a repeated shape along a curve), so a tree can be built over them
later.

## Sources and licences

- Inigo Quilez's articles, iquilezles.org: the primitives
  (`distfunctions`), smooth minimum and its blend factor (`smin`), domain
  repetition (`sdfrepetition`), normals by a tetrahedron (`normalsSDF`), fBm
  detail (`fbmsdf`), and in the example soft shadows (`rmshadows`) and
  ambient occlusion (`raymarchingdf`). The pages state no licence for their
  code; the functions here are written from the mathematics, not copied.
- Mercury's hg_sdf (mercury.sexy/hg_sdf, version 2021-07-28), dual-licensed
  MIT or CC BY-NC: the fillets, `mirror` (its pReflect) and the repetition
  in Columns (pR45, pMod1), used under the MIT licence. `unionColumns`
  departs from it (above).
- Wang, Juttler, Zheng and Liu, "Computation of rotation minimizing
  frames", ACM TOG 27(1), 2008: the frames (`PathCurve` takes sculpt's; an
  arc's are analytic).
- William Latham's FormSynth horn (Todd and Latham, "Evolutionary Art and
  Computers", 1992) and Raup, "Geometric analysis of shell coiling", J.
  Paleontology 1966: the horn.
- Magniez and Zanni, "Improving Spatial Domain Repetition of Implicit
  Surfaces", PACMCGIT 9(1), 2026: cited for the neighbour problem, not
  implemented.
