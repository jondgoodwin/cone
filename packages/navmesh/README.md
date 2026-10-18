# navmesh

Finding the way: a navigation mesh built from a 3-D world's walkable ground
and solids, A* over it, a funnel path, a raycast along it, off-mesh links, and
the steering helpers to follow a path; and a grid A* with jump point search for
tile worlds. After Recast and Detour (Mikko Mononen); written again in Cone,
not ported from the C++. `src/navmesh.cone` has the overview and the file map;
`Design\navmesh.md` the design and where it differs.

```cone
import navmesh use *;

var input = NavInput.empty();
input.addHeightfield(0., 0., 2., 101, 101, heights.view(), AreaGround);   // ground
input.addCircleObstacle(40., 60., 1.5, -10., 20., AreaNull);             // a pillar
let door = input.addBoxObstacle(12., 8., 0.2, 1., 0., 0., 2.2, AreaNull); // a shut door

let params = NavParams.standard();     // cells 0.3 x 0.2 m; agent 0.4 m radius, 1.8 m tall, climbs 0.5 m, 45 degrees
var nav = buildNavMesh(&mut input, &params);

let filter = QueryFilter.standard();
let route = findRoute(&nav, start, goal, new Vec3(2., 4., 2.), &filter);   // the corners, start to end

input.setObstacleEnabled(door, false);     // the door opens
rebuildDirty(&mut nav, &mut input);        // only the tiles it touches are rebuilt
```

A grid:

```cone
let g = Grid.fromRows(&["S....", "####.", ".....", ".####", "....G"]);
let p = gridAStar(&g, 0, 0, 4, 4, true);   // or gridJps for the uniform case
```

## What was measured

Release build, one thread, Jon's laptop (HP Victus 16, i7-13700HX), 12 Oct 2026,
`examples/bench.cone`: a 200 m by 200 m heightfield (20,000 triangles),
132 obstacles, 0.3 m cells, 64-cell tiles (121 tiles): the whole mesh in about
0.2 to 0.25 s (1,910 polygons); 20 routes across the area 6 ms in all; one
obstacle added and its tile rebuilt, 1.7 ms.

## Tests

`congo test` in the package: `grid` (hand-worked mazes and 40 random boards
against a Bellman-Ford brute force, A* with and without diagonals and weights,
and jump point search), `mesh` (clearance round a pillar and a turned box,
the world's edge, steps, slopes, ceilings, a heightfield, convex polygons whose
neighbours agree), `query` (the funnel path round a corner against the mesh's
own corner vertices, raycast, moving along the surface, area costs and filters,
a door that rebuilds one tile of three, off-mesh links one way and two,
determinism), `steer` (arrival, a follower walking a path, a corridor going stale).
`examples/scene.cone` writes two BMPs of a small scene with the door shut and
open.
