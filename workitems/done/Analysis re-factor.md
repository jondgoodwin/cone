**Done.** `design/Analysis.md` describes the result: what demand means, the two
marks a declaration carries, what one under analysis can answer about its type
and its size, and the resolution order within each declaration kind. Read that
for how analysis works; read this for what was done and what it taught.

The work landed as eight commits, each green on the whole suite and revertable on
its own. The sequencing plan they were built from has been absorbed here and
discarded. **Stages 1 and 2 were later reverted**; see the note below the table.

| Stage | Commit | What it taught |
| --- | --- | --- |
| 1 Fold the analysis state | `752935e` | 87 files. **Reverted.** Its one real fix, `tstate.scope` never being initialised, was kept. |
| 2 Rename the flags | `4f28e78` | 4 files. **Reverted.** |
| 3 Mark every declaration | `ee26cb7` | Three clone functions were carrying the marks into their copies, so a clone was born already analyzed. |
| 4 Bound instantiation depth | `adeb75a` | Macros needed the bound as much as generics. Corpus depth is 1; the crash is at ~702 and ~2282. |
| 5 Demand from value name uses | `6d7de87` | `blockTypeCheck` was leaking `pstate->scope` on two of three exits. Demand runs ahead of `FlagSigError`. |
| 6 Remove the signature pre-pass | `8ef2fb2` | A net deletion, green first try, because stage 5 had already carried the risk. |
| 7 Read the in-progress state | `acdacbe` | Exactly the three predicted expectations moved. An array of self was a crash nothing named. |
| 8 Lowering out of name resolution | `968355c` | The two lowering sites are disjoint, not duplicates, and neither had coverage. |

### Stages 1 and 2 were reverted

Both were graded "no behaviour change, suite green", which is true and is not a
reason to make a change. Nothing after them depended on either: stages 3 to 8
need the marks to exist and to be set on every declaration, not to be named or
housed any particular way. Measured afterwards, by instrumenting the compiler and
compiling all 122 corpus sources:

- **The merge had no instance to justify it.** No function takes both states --
  37 take the name resolution state, 66 the type check state -- and neither walk
  ever enters the other. Three of the six merged fields are read by one walk and
  never the other: `mod` 424 reads in name resolution and 0 in type check,
  `loopblock` 1773 and 0, `fn` 0 and 6406. A seventh field, `flags`, was read by
  neither, before the merge or after. What the merge cost is a compile error:
  `pstate->fn` inside a `*NameRes` function used to fail to build, and after it
  merely returned NULL.
- **The rename's premise was false.** It held that most marked things would not
  be types once stage 3 landed. Of first marks, 12,336 land on type nodes and
  5,304 on non-types. And every one of ~66,000 mark touches is in the type check
  walk, so `Analyzed` claimed coverage by all three phases that the code has
  never had -- which misled a reader after `design/Analysis.md` had documented
  the true scope.

The revert kept both commits' genuine gains: the `scope` initialisation fix and
the comments explaining what the marks mean and why `structTypeCheck` sets one
where it does.

Five measured defects are closed: a forward reference to an inferred global, a
linked list and both mutual-reference forms, an unsized root reporting no cause,
a circular constant compiling clean, and a recursive generic crashing the
compiler. Three new codes carry them — `ErrorInstDepth`, `ErrorCircular`,
`ErrorNoSize` — and `ErrorRecurse` is retired.

## What this taught that the design note does not say

- **Check that a change is *needed*, not only that it is *safe*.** Stages 1 and 2
  were verified green and never questioned, and both were reverted. "The suite
  still passes" is evidence a change did no harm, not evidence it was worth
  making. Both were inherited from a plan, and a plan item is a proposal, not a
  finding.
- **Measure; do not read.** Every confident reading of this compiler that was not
  measured turned out wrong at least once, including several during design. The
  technique that kept working is in `design/Analysis.md` section 12. Two crashes
  and two silent defects were found by probing programs the corpus did not have;
  none would have been found by reading.
- **Sequencing carried real information.** Stage 6 was a clean deletion only
  because stage 5 had already been forced to move the signature-failure skip. Had
  both landed together, the cascade stage 5 exposed would have looked like a
  consequence of removing the pre-pass rather than of demand overtaking the flag.
- **Cover before you move.** Stage 8 moved two lowerings that had no coverage at
  all — `<-` on a value tuple is named nowhere else in the corpus, and
  `fnCallTypeCheck`'s bare-method rewrite fires in no scenario. Both were covered
  in a separate commit first, so the move could claim no expectation change
  against a corpus that already tested it.

## Left undone, deliberately

- **A block whose only value expression was reported bad infers `unknownType`,
  not `errorType`.** `iexpMultiInfer` returns `EqMatch` for an `errorType` branch
  *before* recording it, so `inferredType` is left unknown and the enclosing
  return reports a mismatch that says nothing. Measured on a runaway macro in a
  function's value position: one real diagnostic, two follow-ons. This is where
  the deferred "suppress repeated diagnostics" work should start.
- **Suppressing repeated diagnostics** more generally — one mistake several
  levels down still reports once per level that holds it. Needs a `Failed` state
  per declaration and a test at every reporting site.
- **A union variant may hold its own union by value**, and the field is silently
  dropped from the layout. Carried by [[Types. Struct and Union]], which has the
  measurement.
- **Giving the parser distinct node shapes for distinct syntaxes**, which is what
  would actually shrink `fnCallTypeCheck`. [[Lexer and Parser]] and
  [[IR refactor]].
- **Migrating the eight type properties out of `flags`** into `ITypeNodeHdr`.
  [[IR refactor]]'s call; this work neither needs it nor makes it worse.
- **`assert(0)` is a no-op** in the release build. [[Compiler]] owns what
  replaces them; they were deliberately not converted as a side effect here.

---

The original plan follows, with its outcome recorded against each item.

1. ~~Create AnalysisState from folding TypeCheckState into NameResState~~

	**Done and reverted.** `NameResState` and `TypeCheckState` are separate
	again, because the walks are: no function takes both, and half the merged
	fields belong to exactly one walk. The merge's one real gain, initialising
	`tstate.scope`, is kept. The dead `flags` field is gone.
2. ~~Create analyze method for nodes, beginning with pgm, that combines nameres and typecheck.~~

	**The combining was decided against, and the two walks are still separate.**
	`doAnalysis` runs `inodeNameRes` over the whole program, gates on the error
	count, then runs `inodeTypeCheckAny` over it. Binding a name needs the
	declaration to *exist*, not to be analyzed, and the parser guarantees that --
	so name resolution never enters another declaration's analysis and gains
	nothing from running on demand. Keeping it separate is also what lets type
	check assume every name is bound, so nothing downstream reasons about a
	partly-bound declaration. The *state* was merged for a while and then split
	back apart, for the same reason. See `design/Analysis.md` sections 1, 9
	and 13.

	The sub-items below are what actually landed.
	1. Be sure to confirm nameres did not error before running typecheck.
	2. Check for recursion and place limits on infinite recursion
	3. nameres: dictionary is complete and all names are resolved
	4. typecheck: conforms when constraint exists, inferred when it does not
	5. flow for escape, permission, lifetime, read-only-when-populated analysis
	6. No lowering during name resolution: move the '<-' value-tuple
	   expansion in fnCallNameRes, and the bare-name-to-self.member rewrite
	   in nameUseNameRes, into type check where the types they need exist.
	   (Lowering cannot move any later than that -- it is what establishes
	   the node's type. See design/Analysis.md section 10.)
3. Switch to just-in-time type check to fix ordering bugs. For pgm, mod, struct, union, fnsig nodes
	2. Visit all imports first, but not includes! (module-only) -- **done**, `modTypeCheck`
	3. Build this level's dictionary of all declared names (do this in parser), including fields/var, methods/fn/macros/behaviors, typedcls, constants, etc. -- **done**, and name resolution stays one eager pass over it (design section 10)
	4. Analyze base type of a type, and perform folding of fields/names -- **done**, `structTypeCheck` steps 2-4 (design 10.1)
	5. ~~Fully analyze all isolated named-type / constant (non value nodes) in order~~
	6. ~~Fully analyze all fields/vars, and do folding as needed~~
	7. ~~Fully analyze all functions/methods/macros/behaviors~~
	8. ~~Fully analyze all variants (and do folding as part of that)~~

	**5 to 8 are struck rather than deferred.** They prescribe a manual
	topological order within a module -- types, then constants, then fields and
	variables, then functions, then variants. Demand produces that order itself:
	reaching a name analyzes the declaration it names, so `modTypeCheck` is one
	loop over the declarations with nothing to sequence, and the two-pass shape
	that used to stand in for it is gone. Their sub-point, "handle recursion and
	ensure if 'len' is unknown because of unfinished analysis, this is an error",
	is what rules 4 to 6 do, with `ErrorNoSize` and `ErrorCircular`.

Verification:
- Fix type checking to do vars before function bodies
- Bug when function is declared before global variable it accesses… type is unknown. Probably means we should type check initialized global vars before defs for functions

