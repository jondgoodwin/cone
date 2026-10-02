# refmeasure

Authoring-time tooling, not shipped: it measures a reference picture with classic image analysis, and
scores a render of our model against it. Nothing in it is learned; every number comes from a stated
algorithm, and every measurement is drawn on the picture so it can be checked.

It needs Python with numpy, scipy, opencv-python, scikit-image and cma (the `C:\src\imgtools\.venv` venv
has them). Run it as `python tools/refmeasure/refmeasure.py <command> ...`.

## Commands

- `sheet <image> <out> [--hint hint.json]`: the measurement sheet. It crops a screenshot's page border away,
  separates the subject from the background, and writes `sheet.json` and the overlays: `overlay-mask`,
  `overlay-outline` (Bezier segments, their handles and corners), `overlay-skeleton` (the spine, inscribed
  discs and branches, labelled), `overlay-regions`, `overlay-palette` (palettes and ramps) and
  `overlay-orientation` (the grain field).
- `compare <sheet.json> <render> <render-mask> <out> [--camera=ARG | --hint hint.json]`: scores a render.
  The reference is resized to the render's size and both are measured the same way. It writes
  `score.json` (silhouette IoU, outline and interior-edge chamfer, the spine's radius profile, wing lengths
  and angles, palette and mean-colour CIEDE2000 per region, texture statistics per region, tile SSIM with
  the worst tiles), `compare-outlines.png` (reference green, ours magenta), `compare-ssim-heat.png`,
  `compare-worst-tiles.png` (each worst tile enlarged, the reference above ours), and both skeletons.
- `fitcamera <sheet.json> <out> [--minutes 20] [--width 384] [--popsize 16]`: CMA-ES over the camera (target,
  distance, azimuth, elevation, roll, field of view), rendering the starship for every candidate and
  maximising silhouette IoU. Starts from the starship's hero camera and from its mirror below, or, with
  `--start=ARG` (repeatable), from the cameras given and each one's mirror below (its elevation negated),
  the time split evenly between the starts. Writes `fitcamera.json`.
- `render <prefix> --camera=ARG [--size WxH] [--fine]`: one shot and its mask from the starship example.

Zooming in (`zoom.py`), for finding a part's sub-structure in a crop. Each command draws what it found:

- `crop <image> x0,y0,x1,y1 <out.png> [--scale S]`: a part, enlarged to look at.
- `period <image> <out> --line x0,y0,x1,y1 [--width W]`: the repetition along a strip, by autocorrelation (the
  Fourier peak beside it as a cross-check): period, count, peak strength, ticks on the picture. With
  `--spine --camera=ARG [--t0 --t1 --lift]` it samples instead along the starship's spine projected
  through the camera, at equal **true arc length** (rectified), and gives the period in model units;
  `--raw` samples at equal image steps for comparison.
- `repeats <image> <out> --box B --template T [--thresh]`: copies of one element, by normalised template
  matching over a few angles and scales, with non-maximum suppression; their pitch.
- `elements <image> <out> --box B [--pct --min-len]`: elongated bright elements (tubes, ribs, seams): a Sato
  ridge filter, thresholded and skeletonised; each piece's length, radius (distance transform) and
  direction, and the crop's structure-tensor grain. On a glossy tube it traces the rim highlights, so
  its radius is the highlight's, not the tube's.
- `rectify <image> <out> --camera=ARG --plane P0 P1 P2`: the picture warped onto the plane through three
  3-D points (a wing's joints), so periods and sizes come out in true units.
- `profile <image> <out> --at x,y,axis_deg,half [--at ...]`: brightness across a part, perpendicular to its
  axis, read as round (a centred peak, smooth falloff), flat (a plateau) or a step (a ridge or edge).
- `cropscore <sheet.json> <render> <mask> <out> --box B [--line L]`: a render against the reference over one
  box: IoU, edge density, grain, the element summary, the gradient-orientation histograms' L1 distance,
  and the period along the same line in both.

The artist's gates (`gates.py`), for the coarse-to-fine procedure's checks:

- `gates <sheet.json> <render> <mask> <out> [--levels 8,32] [--name gates]`: the render against the
  reference on the subject alone (each picture masked by its own mask): SSIM after blurring to S/8 (the
  squint) and S/32 (the middle forms), S being the reference's spine length; a 3-value notan (background,
  dark mass, light mass, each picture split at its own median after blurring to S/32) and its agreement;
  the busy map (edge density blurred to S/16), its correlation and each one's busy share; and IoU. Writes
  `<name>.json` and `<name>.png` (the blurred pairs and the notan pair).

A box in reference pixels compares whatever our model puts there: unless the macro shape matches part for
part, not just in silhouette, the two crops can hold different anatomy.

Camera arguments are `ex,ey,ez,tx,ty,tz,ux,uy,uz,fov` (eye, target, up, vertical field of view in
degrees). Pass them as `--camera=...`, with the equals sign, because they start with a minus sign.

## The starship example's flags

`packages/sdf/examples/starship.cone` takes `--camera ARG` (repeatable: one shot per camera, meshing once),
`--mask` (a silhouette beside each shot: the ship and its lights flat white on black, with no sky, planet,
stars, bloom or tone mapping), `--size WxH` (W a multiple of 64), `--coarse` (mesh and draw only the
coarser level) and `--without PART` (repeatable: leave a part out, for the procedure's cumulative steps and
the leverage ablation; the parts are listed in the example's header). The tool finds the built example under `~\.congo\lone\starship-*`, or takes `--exe`.
It puts `C:\libs\SDL3-3.4.16\lib\x64` (or `SDL3_DIR`) on PATH for it.

## A hint

When automation is not enough, a small JSON in the cropped picture's pixels:
`{"box": [x0,y0,x1,y1], "fg": [[x,y,r],...], "bg": [[x,y,r],...], "head": [x,y], "tail": [x,y]}`.
The box and discs steer the mask; head and tail choose the spine's ends. The sheet records the hint.

## The modules

- `mask.py`: per-pixel evidence in Lab (lightness, warmth a*+b*, pinkness a*, local detail) seeds GrabCut,
  which runs on a three-channel image of that evidence rather than on colour; then morphology.
- `outline.py`: contour, RDP, corners, Schneider's cubic Bezier fit with a per-segment error bound.
- `skeleton.py`: medial axis, spur pruning, spine, side branches, wings, regions, image moments. With a
  hint, the branches through the head and tail points are never pruned: on a thick, spiky silhouette the
  repeated pruning otherwise ate the main axis from the blunt head, so the spine started at a wing tip.
- `colour.py`: Lab k-means palettes, ramps along paths, CIEDE2000.
- `texture.py`: spectral slope and Hurst exponent, structure-tensor grain and coherence, Gabor hump
  wavelength, edge density; its docstring says which `noise` or `sculpt` dial each one maps to.
- `camera.py`: the render package's camera in numpy, the search's parameters, the starship's landmarks
  and spine (from the example's SPX and SPY control points).
- `gates.py`: the artist's gates (blur pyramid, notan, busy map).
