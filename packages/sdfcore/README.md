# sdfcore

The GPU-safe half of signed distance fields: shapes as functions from a
point to its distance from the surface, negative inside, in plain f32
arithmetic over `geomath`'s `Vec3` and `Quat`. The `sdf` package is its CPU
side (the mesher, `PathCurve`, `CurveCells`, `FnShape`, the horn) and
re-exports every public name here, so most programs write `import sdf use *`
and never name this package. Code meant for the GPU imports `sdfcore` alone.

```cone
import sdfcore use *;

fn scene(p Vec3) f32 {
  imm body = roundBox(p, new Vec3(1., 0.5, 0.5), 0.1);
  imm hole = cylinder(rotate(p, turn), 1., 0.3);
  smoothSubtract(body, hole, 0.05);
}
```

What it holds, file by file (`src/sdfcore.cone` lists every name):

| File | What |
|---|---|
| `scalar.cone` | private helpers; `Blend`, `CurveFrame` |
| `primitive.cone` | `sphere`, `box`, `roundBox`, `capsule`, `torus`, `cylinder`, `cone`, `roundCone`, `taperedCapsule`, `triangle`, `membrane`, `plane`, `ellipsoid` |
| `operator.cone` | `union`, `subtract`, `intersect`, their smooth forms, `smoothUnionBlend`, `onion`, `round`, `exclusive` |
| `domain.cone` | `translate`, `rotate`, `scalePoint`, `scaleDistance`, `Placement`, the mirrors, `repeat`, `repeatLimited`, `elongate` |
| `fillet.cone` | hg_sdf's fillets (MIT) |
| `curve.cone` | the `Curve` and `Repeated` traits, `repeatAlong`, `Arc`, `ArcProjection`, `ArcCells`, `arcCellFrame`, `repeatAlongArc` |
| `shape.cone` | the `Shape` trait, `gradient`, `normal` |
| `detail.cone` | `displace`, `latticeSpheres`, `fbmDetail`, over `noise` |

What each function promises (exact, a bound, or neither), what was
measured, the sources and the licences are in `sdf`'s README, as are the
Slang twins (`sdf/src/sdf.slang`) and the parity test that checks them on a
GPU. This package's own test, `tests/distances.cone`, checks the primitives,
operators, domain operators, fillets and membranes at points whose distances
are known by hand; the Lipschitz, curve and parity tests are `sdf`'s, since
they take in the horn too.

## Why it is GPU-safe

Nothing here allocates, holds a list or an owning reference, makes a function
or virtual reference, recurses, chooses a reference at run time, or uses a
number wider or narrower than 32 bits. The functions that take a shape or a
curve (`gradient`, `normal`, `repeatAlong`, `repeatAlongArc`) are generic
over the `Shape`, `Curve` and `Repeated` traits, so each use is its own copy
and no trait is reached through a virtual reference; a shape is a struct of
its numbers with a `distance` method.

What still stands between it and running on the GPU:

- `absf` and `floorf` (`scalar.cone`) call the C library's `fabsf` and
  `floorf`, and `Arc.project` calls `geomath`'s `atan2`, the C library's
  `atan2f`. The compiler gives f32 a built-in `sqrt`, `sin` and `cos`; GPU
  code needs `abs`, `floor` and `atan2` the same way.
- It imports `geomath` and `noise`, which are not split: `geomath` holds
  lists (its polygons), and both call the C library.
- `Arc.make`, `Arc.frameAt` and `Arc.cells` take `sin` and `cos`, and
  `Arc.project` `atan2`; on a GPU those differ from the CPU in the last bits.
  `repeatAlongArc` takes no trigonometry per evaluation.
- No f64 anywhere.
