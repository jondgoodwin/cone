# sdfmesh

A signed distance field made into a mesh: any `sdf.Shape` (anything with a
`distance(p)`) sampled on a grid and made into a manifold mesh of quads,
then mesh's `Mesh` or `PolyMesh`. The mesher here runs on the CPU, on one
thread; a GPU mesher is to join it. It is a tool package, not maths: what
it makes is a mesh of a size known only once it is made, held in Lists.
The field maths is the `sdf` package's.

```cone
import sdf use *;
import sdfmesh use *;
import mesh;

imm net = surfaceNet(&horn, horn.bounds(), 0.01, 1u32, true);   // cells of 0.01, relaxed once, sparse
imm m = net.toMesh();                                             // a mesh.Mesh, gradient normals
imm h = mesh.meshHash(&m);                                        // the same on every run and build
```

`sdf/examples/hornmesh.cone` meshes the horn at four levels of detail and
draws it; `sdf/examples/horn.cone` and `sdf/examples/starship.cone` mesh
and pin their shapes the same way.

## Surface nets

`mesher.cone` makes a shape into triangles.

| Name | What it is |
|---|---|
| `surfaceNet(shape, box, cellSize, relax, sparse) SurfaceNet` | the shape sampled over `box` (grown by a cell each way) in cubic cells; one vertex per piece of surface in each cell it crosses, the average of the piece's edge crossings, moved `relax` times along the gradient by the distance (kept in its cell); normals the unit gradient (central differences, a tenth of a cell); a quad across each grid edge the surface crosses |
| `SurfaceNet` | `positions`, `normals`, `quads` (4 indices each, counterclockwise from outside), `cellSize`, `cellsX`/`Y`/`Z`, `evaluations` (distances asked in all), `sampled` (of them at the grid), `blocks`, `blocksSkipped`; `toMesh()` (each quad cut along its shorter diagonal), `toPolyMesh(problems)` |
| `netLevels(shape, box, finest, count, relax) List[SurfaceNet]` | levels of detail, level 0 in cells `finest`, each next twice as coarse, each meshed from the field |

A box to mesh over comes from the shape (sdf's `Horn.bounds()`, a box
holding the horn, every cell's ball); a mesh's bits are pinned by mesh's
`meshHash(&Mesh) u32` (the counts, positions' and normals' f32 bits and
indices folded by `noise.pcg`, in order).

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
