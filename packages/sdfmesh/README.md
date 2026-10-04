# sdfmesh

A signed distance field made into a mesh: any `sdf.Shape` (anything with a
`distance(p)`) sampled on a grid and made into a manifold mesh of quads,
then mesh's `Mesh` or `PolyMesh`. One mesher runs on the CPU, on one
thread (`surfaceNet`); the same surface nets run on the GPU, of a grid a
part has already sampled there (`GridMesher`, below). It is a tool
package, not maths: what it makes is a mesh of a size known only once it
is made, held in Lists or GPU buffers. The field maths is the `sdf`
package's.

The package is in two halves: `src/`, the root module (the CPU mesher, and
the CPU side of the GPU one), and `gpu/gridnet.cone`, the kernels, which
Congo compiles into sdfmesh for the CPU (they are their own twins) and into
`sdfmesh.spv` for the GPU. Because `src/` runs the kernels through
`gpuwork`, **importing sdfmesh brings `gpuwork`, `gpu` and `sdl` with
it**: every program that imports sdfmesh (or `sculpt`, which imports it)
links SDL3.lib, which must be on `LIB`, and gets SDL3.dll beside it, whether
it meshes on the GPU or not.

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

## Surface nets on the GPU

`gridmesher.cone` (the CPU side) and `gpu/gridnet.cone` (the kernels) mesh
a grid that has already been sampled: one f32 distance a sample of an
`sdf.SampleGrid`, in a buffer. A part samples its own field on the GPU with
a kernel of its own (one invocation a sample, at `grid.pointOf(i)`), and the
samples go from that job to the mesher without leaving the GPU.
`packages/meshedpart` is the sample of the whole pipeline.

```cone
mut mesher = GridMesher.make(&device);              // reads sdfmesh.spv beside the program
imm grid = SampleGrid.covering(box, 0.03);           // surfaceNet's grid for this box and cell
// ... a part's kernel writes grid.sampleCount() distances into 'samples' ...
mut net = mesher.mesh(&mut device, grid, samples);   // a GridNet: buffers left on the GPU
imm cpu = net.readBack(&mut device);                 // a SurfaceNet, if wanted
imm tris = net.readBackMesh(&mut device);            // or a mesh.Mesh, the GPU's own triangles
```

| Name | What it is |
|---|---|
| `GridMesher.make(device)`, `isValid()` | the seven kernels, from `sdfmesh.spv`, each with its CPU twin |
| `mesher.mesh(device, grid, samples Buffer) GridNet` | the samples meshed on the GPU; waits for the GPU twice (the counts, then the mesh) |
| `mesher.meshSamples(device, grid, &[]f32) GridNet` | the same of samples on the CPU, uploaded first |
| `mesher.meshOnTwin(grid, &[]f32) SurfaceNet` | the same kernels run on the CPU as their twins; slow, for tests |
| `GridNet` | `ok`, `grid`, `vertexCount`, `triangleCount`, and on the GPU `positions` and `normals` (a Vec3 each, 12 bytes; usable as vertex buffers), `indices` (three u32 a triangle; usable as an index buffer), `vertexCells` (each vertex's cell's number), `samples` (handed back) |
| `net.readBack(device) SurfaceNet`, `net.readBackMesh(device) mesh.Mesh` | read back; the buffers stay on the GPU |
| `divExact(a, b)`, `sqrtExact(x)` | IEEE's correctly rounded division and square root, from integer steps (`gpu/gridnet.cone`) |

**The same mesh as `surfaceNet`'s.** Given the samples `surfaceNet` takes
(`SampleGrid.covering` of the same box and cell size, which computes each
point as `surfaceNet` does), the GPU makes `surfaceNet(shape, box, cell, 0,
false)`'s net bit for bit: the same vertices in the same order at the same
f32 bits, and the same quads, whose triangles in the index buffer are
`toMesh`'s, index for index. `tests/gpumesh.cone` checks it for a sphere, a
box, a torus, a plate thinner than a cell and the horn, and the twin
against the GPU, every word; `meshedpart` checks it again on a grid of
3.5 million samples.

How it is made the same:

- **Order without atomics.** Each vertex's slot is its cell's place in an
  exclusive prefix sum of the cells' vertex counts, and each quad's its
  grid edge's in a prefix sum of the samples' quad counts, so both come out
  in `surfaceNet`'s z, y, x order. The sums are integer and made by one
  invocation looping over a block of 256 counts in index order, level upon
  level (`scanTotals`, `scanApply`): no barriers, which the twin cannot run
  yet, and no order that depends on timing, as the repository's determinism
  rules for compute ask. An atomic counter handing out slots would make a
  different order every run.
- **The CPU's arithmetic.** Additions, subtractions and multiplications in
  the CPU's order (conec never fuses them on the GPU); the crossing's
  `d0 / (d0 - d1)` and the average's `1 / count` by `divExact`, the
  normal's length by `sqrtExact`: Vulkan lets a GPU's division be 2.5 ulp
  off and its square root 1, and both are made here from integer long
  division and the digit-by-digit root, correctly rounded, subnormals
  included. The test checks each against the CPU's on 1,000,000 arguments.
- **The same tables.** The GPU reads `surfaceNet`'s own tables of a cell's
  pieces (`netTables`), packed two words a config, so its cells split into
  the same pieces.

**Where it differs from `surfaceNet`**, each a choice:

- **Normals from the grid.** The mesher never sees the shape, so a vertex's
  normal is the gradient the samples give: the central difference on each
  axis at each corner of its cell (one-sided, doubled, at the grid's
  faces), blended trilinearly at the vertex, made a unit vector (marching
  cubes' normal, Lorensen and Cline 1987). `surfaceNet`'s is the shape's
  own gradient at the vertex, a tenth of a cell either side. On smooth
  shapes they agree within a degree (the sphere's and the torus's least
  cosine 0.9998); at a box's edges and where detail is finer than a cell
  (the horn's ribs, a plate thinner than a cell) the grid's is smoother:
  the horn at cells of 0.06 has 1,150 of 2,042 normals more than 8 degrees
  from the gradient's.
- **No relaxation.** `surfaceNet` moves each vertex `relax` times along the
  gradient by the distance, which asks the shape; the GPU's vertices are
  the unrelaxed ones (relax 0). A part that wants them relaxed, or normals
  from its own field, can run a kernel of its own over `positions`,
  `normals` and `vertexCells` (which cell each vertex may move within)
  with its shape and sdf's `gradient`; nothing here does yet.
- **No sparse sampling.** Skipping blocks far from the surface is the
  sampler's business, not the mesher's: the mesher reads every sample it
  is given. `surfaceNet`'s sparse path gives its dense mesh anyway.
- **Statistics.** A read-back SurfaceNet's `evaluations` is 0, `sampled`
  the grid's samples, `blocks` and `blocksSkipped` 0.

**Limits.** A buffer is at most 128 MiB (WebGPU's storage binding): the
samples at most 33.5 million, the triangles' indices 5.6 million quads.
`mesh` refuses a grid past that (`ok` false). The output buffers are sized
from the counts, which is why `mesh` waits for the GPU in the middle.

**Measured**, release, RTX 4060 Laptop (`meshedpart`, its part a ball and a
ring smoothly joined and a rod through both, 205 x 100 x 173 = 3,546,500
samples in cells of 0.01242, 86,290 vertices and 172,576 triangles; five
runs, the least and the median, wall clock from the CPU):

| Step | Time |
|---|---|
| sampling on the GPU, one job | 6.1–6.3 ms |
| meshing on the GPU, both jobs and the counts' round trip | 21.7–22.0 ms |
| reading the mesh back (positions, normals, indices: 3.1 MB) | 13.8–14.1 ms |
| `surfaceNet` of the same part on the CPU, dense, unrelaxed, one thread | 375 ms |

Of the meshing's 22 ms, measured once by instrumenting it: making the
first job's buffers 2–3 ms, recording and submitting its 13 dispatches
3–5 ms, waiting for it 6.5 ms; the second job about 4.5 ms in all.
`mesh` makes every buffer anew each call; keeping them between calls
would save most of the first two.

**Compiler defects worked round.** conec today miscompiles, for the GPU, a
`while` loop that carries an f32 (or a struct of them) and indexes a slice
in its body, when the loop is inside a branch: the loop runs once, and
what follows it is skipped (the loop's back edge becomes a constant exit).
`place` therefore reads every sample and table word it needs before its
loops, and blends the corner gradients without a loop. A loop carrying
only integers is compiled correctly (the scans, `classify`, `emit`).

Sources: Gibson, "Constrained elastic surface nets", 1998, and Lysenko's
"Smooth voxel terrain, part 2" (0fps.net, 2012), for surface nets;
Nielson, "Dual marching cubes", IEEE Visualization 2004, for one vertex per
piece (from memory, not checked against the paper); Lorensen and Cline,
"Marching cubes", SIGGRAPH 1987, for normals from the grid's central
differences.
