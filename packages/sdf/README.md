# sdf

Signed distance fields: shapes as functions from a point to its distance
from the surface, negative inside. Cone source in `src/*.cone`; the same
functions, in the same order of operations, for shaders in `src/sdf.slang`.

The package is in two. **`sdfcore`** (`packages/sdfcore`) is the GPU-safe
half: the primitives, the operators, the domain operators, the fillets,
repetition along a curve and the `Arc`, the `Shape` trait with the gradient
and normal, and noise detail, all plain arithmetic with no allocation, lists,
function references or recursion, so that it can be marked for the GPU and
remain the same source on the CPU. **`sdf`** is the CPU side: `PathCurve`
and `CurveCells` (lists), `FnShape` (a function reference), the mesher, and
the horn (GPU-safe, but kept here while whether it belongs in the library is
open). `sdf` re-exports all of `sdfcore` (`import sdfcore pub use *`), so
`import sdf use *` and `sdf.sphere` reach everything as before; code for the
GPU imports `sdfcore` alone. Everything below is about both.

```cone
import sdf use *;

fn scene(p Vec3) f32 {
  imm body = roundBox(p, new Vec3(1., 0.5, 0.5), 0.1);
  imm hole = cylinder(rotate(p, turn), 1., 0.3);
  smoothSubtract(body, hole, 0.05);
}

imm horn = Horn.make(3.4, 0.34, 0.97, 1.7, 22, 0.12);   // length, base radius, taper, curl, ribs, rib depth
imm fine = horn.fluted(40, 0.018, 0.8);                  // flutes, their depth, their twist
imm d = horn.distance(p);
imm n = normal(&horn, p, 0.001);
```

```slang
// slangc: -fp-mode precise
import sdf;
Horn horn = makeHorn(3.4, 0.34, 0.97, 1.7, 22, 0.12);
Horn fine = fluted(horn, 40, 0.018, 0.8);
float d = horn.distance(p);
float3 n = normal(horn, p, 0.001);
```

`examples/hornmarch.cone` sphere-traces the horn on the GPU, in render's
chitin under the dusk sky (a preview); `examples/hornmesh.cone` meshes it on
the CPU at four levels of detail and draws the mesh in the same chitin, the
same framing; `examples/horn.cone` is the chitin horn's demo: the fluted
horn, meshed, turning under a blue-hour sky on wet ground beside the same
horn in Blinn-Phong, its mesh hashes pinned, its distance and normal
compared with the GPU's (`hornparity.slang`), and a mode that dumps the
video's frames.

```cone
imm net = surfaceNet(&horn, horn.bounds(), 0.01, 1u32, true);   // cells of 0.01, relaxed once, sparse
imm m = net.toMesh();                                             // a mesh.Mesh, gradient normals
imm h = meshHash(&m);                                             // the same on every run and build
```

## API

Numbers are `f32`, points `geomath`'s `Vec3` (`float3` in Slang), rotations
unit `Quat`s (`float4` x, y, z, w). "Exact" is the Euclidean distance; a
"bound" never exceeds it. Every function says which in its comment.

**Primitives** (sdfcore, at the origin, round ones about y:
`sphere(p, radius)`, `box(p, extent)`, `roundBox(p, extent, radius)`,
`capsule(p, a, b, radius)`, `torus(p, major, minor)`,
`cylinder(p, halfHeight, radius)`, `cone(p, halfHeight, bottom, top)`,
`roundCone(p, bottom, top, height)`, `plane(p, n, offset)`: exact.
`ellipsoid(p, radii)`: Quilez's approximation, not a bound (below).

**Operators on distances** (sdfcore: `union`, `subtract`,
`intersect`; `smoothUnion(a, b, k)`, `smoothSubtract`, `smoothIntersect`,
Quilez's quadratic smooth minimum normalized so that `k` is the most the
blend swells; `smoothUnionBlend(a, b, k) Blend` with the blend factor
(0 all a's material, 1 all b's); `onion(d, thickness)`, `round(d, radius)`,
`exclusive(a, b)`.

**Operators on the point** (sdfcore: `translate(p, offset)`,
`rotate(p, q)` (the point for a shape turned by q), `scalePoint(p, s)` with
`scaleDistance(d, s)` (both are needed, or the field is s-Lipschitz),
`Placement` (position, rotation, uniform scale: `toLocal(p)`,
`distance(d)`), `mirrorX`/`Y`/`Z`, `mirror(p, n, offset)`,
`repeat(p, period)`, `repeatLimited(p, period, limit)` (exact for shapes
symmetric in their cells), `elongate(p, extent)`.

**Fillets** (sdfcore: `unionRound`,
`intersectRound`, `subtractRound`, `unionChamfer`,
`unionColumns(a, b, r, n)`, `unionStairs(a, b, r, n)`, `intersectStairs`,
`subtractStairs`, `pipe`, `groove`, `tongue`, `engrave`.

**Repetition along a curve** (sdfcore

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
`horn.fluted(count, depth, twist)` grooves `count` flutes along the tube
between the ribs, each `depth` of the radius deep, turning `twist` radians
about the tube from base to tip: the groove is `depth` times the distance
from the chord (up to the radius) times |sin| of half `count` times the
angle about the chord, so the ridges are sharp creases and the grooves
round. It is only added to each cell's tube, so the union's pruning by
bounding balls still holds, and the horn's distance is divided by 1 plus
the groove's steepest slope, so it stays a bound (a conservative one: the
fine flutes' ratio measures 0.75, so a tracer steps short there). Each
evaluation of a fluted horn takes an atan2 and a sin per cell.

**Gradient** (sdfcore
(a `&fn(p Vec3) f32` as a Shape), `gradient(shape, p, h)` (central
differences, six evaluations), `normal(shape, p, h)` (Quilez's tetrahedron,
four).

**Noise detail** (sdfcore:
`displace(d, p, seed, octaves, frequency, amplitude)` adds fBm and is not a
distance; `fbmDetail(d, p, seed, octaves, size)` is Quilez's fBm of
spheres, clipped to a band at the surface and smooth-unioned on, which stays
a bound; `latticeSpheres(p, seed)`, its octave.

In Slang the constructors are `makeArc`, `makeArcCells`, `makeHorn`,
`fluted(horn, count, depth, twist)`; the
traits are the interfaces `IShape`, `ICurve`, `IRepeated`.

## Distances: what holds, measured

`tests/lipschitz.cone` samples 4096 pairs of points 1/64 apart for each
shape and checks |d(p) - d(q)| / |p - q|:

- Every primitive but the ellipsoid, every hard and smooth operator, onion,
  round, placement (turned, moved, scaled), mirror, limited repetition and
  elongation: at most 0.99999.
- **The horns** (curled 2 and 4.5 radians, and straight): at most 0.99991;
  the gradient's length at most 1.007 (central differences at the creases).
- **Fluted horns** (40 fine flutes twisted 0.8, 12 deep ones twisted -2;
  also sampled within the horn's bounds, 16384 pairs 1/256 apart): 0.73 to
  0.75 and 0.55 to 0.64, the gradient 0.77.
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

## Meshing: surface nets

`mesher.cone` makes a shape into triangles on the CPU, on one thread.

| Name | What it is |
|---|---|
| `surfaceNet(shape, box, cellSize, relax, sparse) SurfaceNet` | the shape sampled over `box` (grown by a cell each way) in cubic cells; one vertex per piece of surface in each cell it crosses, the average of the piece's edge crossings, moved `relax` times along the gradient by the distance (kept in its cell); normals the unit gradient (central differences, a tenth of a cell); a quad across each grid edge the surface crosses |
| `SurfaceNet` | `positions`, `normals`, `quads` (4 indices each, counterclockwise from outside), `cellSize`, `cellsX`/`Y`/`Z`, `evaluations` (distances asked in all), `sampled` (of them at the grid), `blocks`, `blocksSkipped`; `toMesh()` (each quad cut along its shorter diagonal), `toPolyMesh(problems)` |
| `netLevels(shape, box, finest, count, relax) List[SurfaceNet]` | levels of detail, level 0 in cells `finest`, each next twice as coarse, each meshed from the field |
| `Horn.bounds() Aabb3` | a box holding the horn (every cell's ball), for the mesher |
| `meshHash(&mesh.Mesh) u32` | the counts, positions' and normals' f32 bits and indices folded by `noise.pcg`, in order |

What holds, and why:

- **A manifold, closed mesh for a closed shape.** Naive surface nets gives
  a cell one vertex even where the surface passes through it twice (two
  parts closer than a cell), pinching the mesh: an edge four quads share.
  Here a cell has a vertex per piece of surface, the pieces marching cubes
  would draw in it (Nielson's dual marching cubes), with each face's
  ambiguous case (inside corners diagonally opposite) always separating
  them, so neighbouring cells agree. `tests/meshing.cone` builds the two
  ambiguous cases by hand (16 vertices, two separate closed nets, where
  naive nets gives 14 and a pinch) and checks the horn closed, `validate`
  clean and of Euler characteristic 2 in cells from 0.02 to 0.16, relaxed
  or not.
- **Sparse, and exact.** Blocks of 4 cells a side whose centre is farther
  from the surface than half their diagonal (with 1% to spare) are not
  sampled: for a 1-Lipschitz field nothing in them is on the surface, and
  every sample in them has the centre's sign. Every edge the surface
  crosses has both ends evaluated, so the mesh is bit for bit the dense
  one; the test checks it by hash, for a sphere and the horn.
- **Deterministic.** Fixed orders throughout; the same mesh, bit for bit,
  on every run and in debug and release builds (the test pins the horn's
  hashes, and passes in both). The hash does not depend on the GPU: step 5
  of the chitin horn compares the meshes the two machines' CPUs make.
- **Rounded edges.** Surface nets averages crossings, so sharp edges and
  corners are rounded off; dual contouring (Ju et al. 2002; Boris the
  Brave's tutorial) keeps them, and is not built.
- **A tube thinner than a cell** is followed only roughly: at the horn's
  tip (radius under a cell) one to three quads can fold to face against
  the gradient (measured at cells of 0.02 to 0.08). Everywhere else every
  quad faces the way its vertices' normals do.

Measured on the horn (`hornmesh`, release build, one thread):

| Level | Cells | Vertices | Triangles | Samples evaluated | Time |
|---|---|---|---|---|---|
| 0 | 0.01 | 75,902 | 151,800 | 460,061 of 7,052,080 (6.5%) | 1.0–1.4 s |
| 1 | 0.02 | 18,822 | 37,640 | 109,527 of 941,460 (12%) | 0.2 s |
| 2 | 0.04 | 4,648 | 9,292 | 27,141 of 132,057 (21%) | 45–52 ms |
| 3 | 0.08 | 1,122 | 2,240 | 7,038 of 19,950 (35%) | 10–12 ms |

Level 3's cells are wider than the ribs are thick, so its ribs alias into
lumps; it is drawn only when the horn is under 120 pixels across. Drawn at
1920 x 1080 (validation on), level 0 takes 0.7–0.9 ms a frame on the RTX
4060 (one run of four, 2.1) and 15.3–15.7 ms on the UHD 770, against
hornmarch's 19.7 and 57.6; the mesh has no shadows or ambient occlusion,
which hornmarch traces. Framed alike, the two examples' frames at frame 90
differ by more than 16/255 in 0.14% of pixels.

Sources: Gibson, "Constrained elastic surface nets", 1998, and Lysenko's
"Smooth voxel terrain, part 2" (0fps.net, 2012), for surface nets;
Nielson, "Dual marching cubes", IEEE Visualization 2004, for one vertex per
piece (from memory, not checked against the paper).

## CPU and GPU agree

`tests/parity.cone` draws 59 quantities at 256 points with `sdf.slang` on
the GPU and compares each with the Cone package (the pattern of `noise`'s
parity test). Measured on the RTX 4060 and the UHD 770:

| Quantities | Agree |
|---|---|
| `plane`, `rotate`, `mirror` (add, subtract, multiply only) | bit for bit |
| every other primitive, operator, domain operator and fillet, fBm detail, displacement (square roots, divisions) | within 7.2e-7 (the ellipsoid's); from half (the ellipsoid) to 95% of samples bit for bit |
| the arc's projection, the horns (fluted too), repetition along the arc (atan2, cos, sin) | within 4.8e-7 |
| the horn's gradient and normal (differences over 2 h = 1/32) | within 1.5e-5 |

The test holds them to 2^-19, 2^-16 and 2^-12: the measured differences
with a margin, not Vulkan's bounds (which allow trigonometry 2^-11), so a
change to the order of operations on one side shows. The unfluted horn
uses no trigonometry per evaluation (its cells step round by rotation),
only when it is made; flutes add an atan2 and a sin per cell. The example's
frames on the two GPUs differ by at most 3/255 in any channel.

`examples/horn.cone` checks the demo's own fluted horn the same way, at
4096 points of a grid over its bounds (148 within 0.02 of its surface):
distance and normal, the GPU's table against the CPU's and the two GPUs'
against each other. Measured (27 Sep 2026):

| | distance, bit for bit | distance, most apart | normal, bit for bit | normal, most apart |
|---|---|---|---|---|
| RTX 4060 against the CPU | 52% | 3.6e-7 | 10–11% | 1.2e-5 |
| UHD 770 against the CPU | 62% | 2.4e-7 | 18–19% | 1.1e-5 |
| RTX 4060 against the UHD 770 | 68% | 2.4e-7 | 20–21% | 1.1e-5 |

The sign of the distance, what the mesher decides by, agrees at every
point. The meshes are made on the CPU, so their hashes are the same
whichever GPU draws them; the demo pins them.

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
