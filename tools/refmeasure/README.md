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
  maximising silhouette IoU. Starts from the starship's hero camera and from its mirror below.
  Writes `fitcamera.json`.
- `render <prefix> --camera=ARG [--size WxH] [--fine]`: one shot and its mask from the starship example.

Camera arguments are `ex,ey,ez,tx,ty,tz,ux,uy,uz,fov` (eye, target, up, vertical field of view in
degrees). Pass them as `--camera=...`, with the equals sign, because they start with a minus sign.

## The starship example's flags

`packages/sdf/examples/starship.cone` takes `--camera ARG` (repeatable: one shot per camera, meshing once),
`--mask` (a silhouette beside each shot: the ship and its lights flat white on black, with no sky, planet,
stars, bloom or tone mapping), `--size WxH` (W a multiple of 64) and `--coarse` (mesh and draw only the
coarser level). The tool finds the built example under `~\.congo\lone\starship-*`, or takes `--exe`.
It puts `C:\libs\SDL3-3.4.16\lib\x64` (or `SDL3_DIR`) on PATH for it.

## A hint

When automation is not enough, a small JSON in the cropped picture's pixels:
`{"box": [x0,y0,x1,y1], "fg": [[x,y,r],...], "bg": [[x,y,r],...], "head": [x,y], "tail": [x,y]}`.
The box and discs steer the mask; head and tail choose the spine's ends. The sheet records the hint.

## The modules

- `mask.py`: per-pixel evidence in Lab (lightness, warmth a*+b*, pinkness a*, local detail) seeds GrabCut,
  which runs on a three-channel image of that evidence rather than on colour; then morphology.
- `outline.py`: contour, RDP, corners, Schneider's cubic Bezier fit with a per-segment error bound.
- `skeleton.py`: medial axis, spur pruning, spine, side branches, wings, regions, image moments.
- `colour.py`: Lab k-means palettes, ramps along paths, CIEDE2000.
- `texture.py`: spectral slope and Hurst exponent, structure-tensor grain and coherence, Gabor hump
  wavelength, edge density; its docstring says which `noise` or `sculpt` dial each one maps to.
- `camera.py`: the render package's camera in numpy, the search's parameters, the starship's landmarks.
