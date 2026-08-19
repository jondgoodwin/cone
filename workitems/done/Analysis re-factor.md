**Done.** `design/Analysis.md` describes the result: what demand means, the two
marks a declaration carries, what one under analysis can answer about its type
and its size, and the resolution order within each declaration kind. Read that
for how analysis works; read this for what was done and what it taught.

The work landed as eight commits, each green on the whole suite and revertable on
its own. The sequencing plan they were built from has been absorbed here and
discarded.

| Stage | Commit | What it taught |
| --- | --- | --- |
| 1 Fold the analysis state | `752935e` | 87 files. Also fixed `tstate.scope` never being initialised. |
| 2 Rename the flags | `4f28e78` | 4 files. |
| 3 Mark every declaration | `ee26cb7` | Three clone functions were carrying the marks into their copies, so a clone was born already analyzed. |
| 4 Bound instantiation depth | `adeb75a` | Macros needed the bound as much as generics. Corpus depth is 1; the crash is at ~702 and ~2282. |
| 5 Demand from value name uses | `6d7de87` | `blockTypeCheck` was leaking `pstate->scope` on two of three exits. Demand runs ahead of `FlagSigError`. |
| 6 Remove the signature pre-pass | `8ef2fb2` | A net deletion, green first try, because stage 5 had already carried the risk. |
| 7 Read the in-progress state | `acdacbe` | Exactly the three predicted expectations moved. An array of self was a crash nothing named. |
| 8 Lowering out of name resolution | `968355c` | The two lowering sites are disjoint, not duplicates, and neither had coverage. |

Five measured defects are closed: a forward reference to an inferred global, a
linked list and both mutual-reference forms, an unsized root reporting no cause,
a circular constant compiling clean, and a recursive generic crashing the
compiler. Three new codes carry them — `ErrorInstDepth`, `ErrorCircular`,
`ErrorNoSize` — and `ErrorRecurse` is retired.

## What this taught that the design note does not say

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

1. Create AnalysisState from folding TypeCheckState into NameResState
	1. Rename NameResState to AnalysisState
	2. Rename nState to aState
	3. Add `FnDclNode *fn` to AnalysisState
	4. Rename TypeCheckState to AnalysisState
	5. Rename tState to aState
2. Create analyze method for nodes, beginning with pgm, that combines nameres and typecheck. 
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

