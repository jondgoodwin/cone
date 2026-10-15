# humanbody

People of game quality, as data: a person is a set of measures from a real survey, a look and an outfit, and the
body is made the way RDR2's, GTA's and EverQuest II's are, with the artist's part done by rule. One base mesh per
sex, of game topology (quads in rings, extra loops at the joints, rows that follow the lids and the mouth); people
as blends of that one mesh; skinned to bonepose's 19-bone humanoid; clothes as fitted layers made from each
person's own body; a face that moves by facerig's FACS units; skin in render's subsurface material. `src/humanbody.cone`
has the overview and the file map; each file's header says its method and its sources.

```cone
import humanbody use *;

imm men = buildBlendSet(MALE, 1u32);                 // the base man and his 29 blends
imm people = cast();                                 // the main avatar and six, as ANSUR II measures
mut body = personBody(&men, &people[0usize].measures);   // a person: the base plus the blends at their weights
imm face = buildFace(&mut body);                     // FACS targets on the mesh; the jaw and eyes as bones
imm wardrobe = dressPerson(&body, people[0usize], false, 4u32);   // shells, a skirt, the cape
imm pose = sitPose(&body.skeleton, &Seat.chair(point, 0., 0.45), 1., true);
```

`examples/peoplebench.cone` is the test bench: the cast side by side, under day and dusk light, with ray tracing
off, in underwear against their measures, beside today's pegasus3d mannequin, their faces cycling expressions,
walking, sitting down and standing up, hailing; and the sculpting step by step (`--stages`). It writes
2560 x 1440 PNG sheets.

## How a body is made

1. **The measures** (`measures.cone`). ANSUR II (the 2012 US Army survey, 4,082 men and 1,986 women): each person is
   the median, measure by measure, of the 40 subjects nearest a stature percentile and a body-mass-index percentile;
   the landmark heights (crotch, trochanter, navel, nipples, armpit, acromion, neck) are the survey's means as
   fractions of stature. The face's character (brow, nose, lips, chin, cheekbones, jaw) is set by hand.
2. **The skeleton** (`skeleton.cone`): bonepose's humanoid of the person's height (Drillis and Contini's lengths, which
   walkgait and ragdollrig are built on) with the hip and shoulder joints moved to where the measures put them (a
   quarter of the hip breadth out; 25 mm in and 35 mm down from the acromion). The humanoid's own hips, 0.0955 H out,
   are half its hip WIDTH, which is what made the mannequin's hips 0.28 H across.
3. **The forms** (`forms.cone`): the masses an artist blocks in (ribcage, pelvis, belly, buttocks, pectorals or
   breasts, shoulder blades, latissimus, trapezius, deltoids, biceps, triceps, forearm, the mitten hand, thigh
   muscles, knee cap, calf, heel, foot) as ellipsoids and tapered capsules in smooth union: a signed distance field.
   Small blend radii: the quadratic smooth union swells by up to k where forms meet, and many forms meeting swell by
   many k.
4. **The cage** (`cage.cone`): the topology at its coarsest, a few hundred faces: a trunk of rings of 16, each arm
   grown from a 2 x 2 patch of the trunk's side (its outline is the arm's first ring of 8), each leg from half the
   bottom ring and a crotch vertex both legs share (rings of 10), three loops at each knee and elbow, mitten and foot
   closed by fans, the neck's ring open. Skin weights painted on the cage's vertices.
5. **The surface** (`sculpt.cone`): Catmull-Clark (sculpt's) once, the weights subdivided with it and smoothed; then
   each vertex projected onto the forms (Newton's steps on the field) and relaxed along the surface. Retopology the
   other way round: the clean layout first, the sculpt it lies on second. No marching cubes in the shipped mesh.
6. **The fit** (`measure.cone`, `body.cone`'s `fitGains`): the mesh is measured as a surveyor measures (a breadth or
   depth is a horizontal slice's extent, a girth a slice's length round one limb), and the forms' radii corrected by
   gains until the mesh's measures are the survey's.
7. **The head** (`head.cone`): a grid laid over a second field of forms (cranium, forehead, brow ridge, cheekbones,
   upper and lower jaw, chin, mouth's mound, lips with a cupid's bow, nose, eye sockets, neck). Each grid point is
   found by a ray from inside the head. Face rows are level lines, and the rows near each eye are drawn onto the lids'
   edges, so the lid margins and the mouth's line ARE rows. A smooth brush (Taubin's, along the normals only) takes out
   the creases where forms meet, sparing the lids and the mouth. The lids and the mouth's slot are facerig's test
   head's operations; the bottom row is zipped to the body's neck ring and their normals welded.
8. **The blends** (`blends.cone`): for each of 29 axes (the trunk's and limbs' measures, the head's, the face's
   character, the build), the same vertices sculpted again with one thing changed; the target is the difference per
   unit, with the joints, the eyes, the jaw's hinge and the mouth moved too. A person's weights for the measured blends
   are solved from a matrix of how each blend moves each measure; the rest are weighted directly. All seven people are
   the same 8,834 vertices.
9. **The face** (`face.cone`): facerig's spec read off the head (eyes and lids from the head's numbers, other landmarks
   the grid points nearest each feature), its 50 FACS targets built on the whole mesh, the jaw and eyes added to the
   skeleton; eyeballs, teeth, tongue, the mouth's inside and the ears as rigid parts.
10. **The clothes** (`outfits.cone`): garmentdrape's shells (the body's surface over a region of bones, pushed out by
    ease and the layers beneath, smoothed, skinned as the body), a lofted skirt that hangs from the waist, and the main
    avatar's draped cape; the body under opaque cloth is not drawn.
11. **The look** (`look.cone`): an albedo painted from the head's own shape (lips, brows, lash lines, cropped hair to a
    hairline, beard shadow, warmer cheeks, nose and ears) and a roughness map, under render's skin material.
12. **The poses** (`poses.cone`): standing at ease, sitting down on and standing up from any Seat (a sit point, a
    facing, a height, its own rock or swing), a hail; walking is walkgait's.

## What this example teaches

- **A cheap sculpt is a field, a game mesh is a layout; keep them apart.** The forms are quick to write and to change,
  and they say nothing about topology; the cage says everything about topology and nothing about shape. Projecting the
  one onto the other gives a mesh an animator can skin and a garment can be made from.
- **Measure, then correct.** Every form's size is a guess until the mesh is sliced and measured; four rounds of
  multiplying each measure's gain by its error bring breadths, depths and girths within a few per cent of the survey.
  The proportions sheet draws the survey's numbers behind the bodies so a reader can check them by eye.
- **Smooth unions accumulate.** Chaining the quadratic smooth minimum over many forms swells the shape by the blend
  radius at every meeting; the head uses the exponential one (associative, so it swells by k ln n where n meet).
- **Put features on rows.** A lid's margin or a mouth's line cut across a grid's squares is a staircase; drawn onto a
  row it is a curve, and facerig's targets act on it cleanly.
- **Blends must move the skeleton.** A morph target that moves the arms must move the shoulder joint with them, or
  the person skins against a skeleton that is not theirs; here every target carries the joints and the face's points.
- **A cone of cloth is a stable shape.** garmentdrape wraps cloth at the radius its flat width asks; a half-circle cape
  becomes a bell its strain-limited, stiff cloth cannot buckle out of. A hem a little wider than the back hangs.

## What it does not do yet

- Articulated hands (mittens, by the agreed scope), hair (cropped scalp painting), user configurability.
- No normal maps: the muscle detail is what 17,500 triangles carry. No corrective shapes at the joints yet.
- The thighs are 5 to 10 per cent under their survey girth (each thigh is held inside the hip breadth).
- The cape hangs with few folds: garmentdrape's cloth settles into the shape it was wrapped in.

## Tests

`tests/people.cone`: the cage closes, the surface lies on the forms, the fitted measures are the survey's, the body is
one mesh whose weights sum to one, the face's spec and targets are built, a blended person meets their measures, a
seated pose sits and a hail raises the hand. `congo test` in this folder.
