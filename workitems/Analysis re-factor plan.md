Sequenced implementation plan for [[Analysis re-factor]]. The design is
`design/Analysis.md`; this is the order it gets built in and how each step is
proved. Read the design first — this plan names mechanisms without re-explaining
them, and its rule numbers are that note's.

## How this plan is used

One branch, one commit per stage. Not pull requests: the branch is read before
merge either way, and a stack of dependent PRs needs rebasing every time a stage
changes. Each commit is green and revertable on its own, so a stage that turns
out wrong costs one commit rather than the sequence.

**A stage is done when all four hold:**

1. The build is clean.
2. `python test/run.py` is green — 119 scenarios / 120 runs at the start, plus
   whatever the stage adds.
3. `python test/run.py --bless` records no drift, *or* the drift is named and
   defended in the commit message.
4. `python test/run.py --coverage` is no worse — 57 of 60 at the start.

**A stale `conec` fails good sources in ways indistinguishable from a
regression.** Build before believing any failure; `--build` builds first.

## Progress

Branch `analysis-refactor-design`. Stages 1-3 are done and each was green on all
four checks with no expectation changes.

| Stage | Commit | Notes |
| --- | --- | --- |
| 1 Fold the analysis state | `752935e` | 87 files. Also fixed `tstate.scope` never being initialised. |
| 2 Rename the flags | `4f28e78` | 4 files. |
| 3 Mark every declaration | `ee26cb7` | The probe fired; see hazard 1 below. |
| 4 Bound instantiation depth | `adeb75a` | Macros needed the bound too, and two diagnostic defects surfaced with it. See hazards 5 and 6. |
| 5 Demand from value name uses | `6d7de87` | Held green only by moving the signature-failure skip into the declaration. See hazards 7 and 8. |
| 6-8 | | not started |

## Standing hazards, found in flight

These were discovered while doing the stages and are not in the design note.

1. **Clone functions silently duplicate analysis state.** `cloneFnDclNode`,
   `cloneVarDclNode` and `cloneFieldDclNode` `memcpy` the whole node, flags
   included, and never cleared the analysis marks — only `cloneStructNode` did.
   Nothing had noticed because only types carried those marks. Stage 3 fixed all
   three, but the *class* is what matters: this is the third defect of this shape
   in this codebase, after `cloneStructNode` carrying a list's `used` count into
   every generic instance method. **Anything added to a node's flags from here
   needs its clone audited in the same commit.**
2. **A declaration under analysis returns silently; it does not raise
   `ErrorRecurse`.** Stage 3 deviated from what this plan said, deliberately.
   Refusing would fire on mutual recursion the moment stage 5 lands. Rule 3 says
   a declaration's own type is established before anything can refer back to it,
   so the caller reads that from the node and there is nothing to do. Stage 5
   depends on this; stage 7 gives type nodes the equivalent treatment for size.
3. **The probe technique works, and reading the code does not.** Instrument the
   thing under test to *report instead of act*, compile all 120 corpus sources,
   and confirm silence:

   ```bash
   for f in $(find test/cases -name "*.cone"); do
     ./build/x64-release/conec.exe -o build/probe "$f" 2>&1 | grep "^PROBE"
   done
   ```

   Stage 3's probe found hazard 1, which no amount of reading would have. Stages
   5 and 7 both have "signal of a bad reading" lines that want the same
   treatment before the change is written.
4. **The runner's staleness guard fires after any git operation that touches
   source mtimes** — `stash`, `checkout`, `stash pop`. It refuses to run rather
   than reporting failures against a stale binary. Rebuild; do not debug.
5. **A node built during analysis takes the lexer's position, which by then is
   the end of the file.** `newNode` reads `lex->tokp`, so an injected node points
   at nothing unless `inodeLexCopy` is called on it. `genericSubstitute`'s
   `inferredgencall` was one, found because stage 4's diagnostic landed on it and
   pointed one line past the end of the source. Any diagnostic reported on an
   injected node is suspect until its position is checked against a real program.
6. **A block whose only value expression was reported bad infers `unknownType`,
   not `errorType`.** `iexpMultiInfer` returns `EqMatch` for an `errorType`
   branch *before* recording it, so `inferredType` is left unknown, the block's
   `vtype` becomes `unknownType`, and the enclosing return reports a type
   mismatch that says nothing. Measured on a runaway macro in a function's value
   position: one real diagnostic, two follow-ons. Nothing was done about it --
   it is the deferred "suppressing repeated diagnostics" work, recorded here
   because it is where that work should start.

7. **Demand runs ahead of `FlagSigError`.** The module's signature pre-pass sets
   the flag and its body pass skips those declarations, but a use can now demand
   a declaration before that loop reaches it, arriving at `fnDclTypeCheck`
   without passing the skip. Measured: one scenario, `closure-typecheck-sig`,
   gained two cascading diagnostics from a closure body that had never been type
   checked. Stage 5 moved the skip into `fnDclTypeCheck`, which is where stage 6
   needs it anyway -- **the skip now belongs to the declaration, and stage 6 has
   only to swap the flag test for `errors != errorsOnEntry` once the pre-pass is
   gone.** The first attempt tested both, but the error-count half fires nowhere
   in the corpus and would have widened the gate `core-flow-gate` pins, so it was
   left for stage 6 to add when it becomes reachable.
8. **`blockTypeCheck` leaked `pstate->scope` on two of its three exits.** The
   `--pstate->scope` sat after two early returns, so the counter climbed for the
   rest of the compile: successive module-level functions were entered at scope
   0, 1, 2, 3, 4, 5. Harmless while scope was only read by `clonePushState` and
   one injected temporary, and not harmless once a declaration can be analyzed
   from the middle of a body. Fixed in stage 5 along with `fnDclTypeCheck` saving
   and resetting scope, which is rule 8's half of that stage.

## Sequencing principles

1. **Mechanical before semantic.** Changes that cannot alter behaviour go first,
   so the suite proves them against a known-good base. If a rename breaks
   something, that is a fact about the rename, not about the design.
2. **Add capability before anything relies on it.** The flags are extended as a
   no-op commit, then the demand that needs them arrives. If the marking is
   wrong, it surfaces where nothing else changed.
3. **Fix the hazard before removing the guard.** The instantiation limit lands
   before rule 4, because rule 4 removes the blanket refusal that currently
   catches recursive generics by accident.
4. **Remove scaffolding last and alone**, so a dependency nobody saw costs one
   revert.
5. **Every fix commit carries the scenario that proves it.** The five defects in
   design section 13.4 have no coverage. Each arrives with its own case; that is
   what makes "no worse than before" checkable rather than asserted.
6. **Changed expectations are defended in the commit message.** Rewriting a
   scenario because behaviour genuinely changed is correct. Rewriting one to
   silence a failure is not, and the message is where the difference is argued.
7. **Plan for being wrong.** Re-read the code against this plan at each stage
   boundary rather than trusting it end to end. The design note was built by
   measuring rather than reading, and several confident readings still turned out
   wrong; this plan has had no such scrutiny.

## Scope check

The only new algorithm in the plan is a depth counter on instantiation. Nothing
else adds a data structure, an IR field, or a traversal. If a stage seems to need
one, that is a signal the design mis-read something — stop and raise it rather
than inventing storage.

---

## Stage 1 — Fold the analysis state

**Goal.** One `AnalysisState` replacing `NameResState` and `TypeCheckState`.
Item 1 of the work item.

**Changes.** `ir/ir.h`: merge the two structs — the union is `mod`, `typenode`,
`loopblock`, `fn`, `scope`, `flags`. `conec.c`: one state, and **initialise
`scope`** — today `tstate.scope` is never set, and `blockTypeCheck` increments it
while `clonePushState` consumes it. Then the signatures: 87 files, 201
occurrences, 189 parameter declarations.

The two dispatchers stay separate. Only the state merges; `inodeNameRes` and
`inodeTypeCheck` are untouched.

**Why safe here.** Pure rename and merge. No behaviour depends on it.

**Expectation changes.** None.

**Signal of a bad reading.** Any scenario changing behaviour. A merge of two
structs cannot legitimately do that, so a diff in `--bless` here means a field
collided or `scope` was being read as garbage in a way something depended on.

---

## Stage 2 — Rename the flags

**Goal.** `TypeChecking` → `Analyzing`, `TypeChecked` → `Analyzed`. The names
assume the marked thing is a type; after stage 3 most marked things are not.

**Changes.** 9 sites: `ir/inode.h` (definitions), `inode.c` (the guard and the
set), `exp/nameuse.c`, `types/struct.c` (the clone clear, and the set at the
layout point).

While here: the comment at `structTypeCheck`'s set says "we know enough about the
type at this point". Make it say enough for *what* — laid out, method set
complete, methods themselves not yet analyzed. The whole scheme rests on that
placement.

**Why safe here.** Rename only.

**Expectation changes.** None.

---

## Stage 3 — Mark every declaration

**Goal.** `Analyzing`/`Analyzed` set on `FnDcl`, `VarDcl`, `FieldDcl` and
`ConstDcl` as well as type nodes and modules, with an early return when
`Analyzed`. Rule 2, made general.

**Changes.** `inode.c`, the guard at the top of `inodeTypeCheck` — currently
conditioned on `isTypeNode(*node) || ModuleTag`. Extend the condition; keep the
in-progress branch refusing exactly as it does now.

**Done — `ee26cb7`.** Green, no expectation changes, but not the no-op it was
predicted to be.

The probe fired on four scenarios, two generic and two trait. The cause was not
a second walk: the three declaration clone functions carried the marks into
their copies, so a clone was born already analyzed and the guard would have
skipped it entirely. Fixing those three made the probe silent across all 120
sources, confirming that nothing is genuinely visited twice today. See standing
hazard 1.

The in-progress branch returns silently rather than refusing, which is a
deliberate departure from what this plan said. See standing hazard 2.

---

## Stage 4 — Bound instantiation depth

**Goal.** Rule 7. Fixes a live compiler crash, and lands before the guard that
currently hides it is removed in stage 7.

**Changes.** `ir/meta/generic.c` around `genericInstantiate`, and the macro
expansion path in `ir/meta/macro.c`: count depth, refuse past `TypeCheckLoopMax`
(declared in `ir.h`, currently with no call site anywhere). A new `ErrorCode` is
likely — decide against design section 13.5's reasoning, and register it in
**both** `shared/error.h` and `test/codes.toml`; next free is 1067, never
renumber. `--bless-codes` regenerates the table.

**Scenario.** `test/cases/generic/` — the recursive generic function that today
dies with a stack overflow and no diagnostic:

```cone
fn recur[T](x T) T { recur[Box[T]](Box[T][x]).v }
struct Box[T] { v T }
fn f() i64 { recur[i64](3i64) }
```

**Why safe here.** Independent of everything above and below. On its own it turns
a crash into a diagnostic.

**Expectation changes.** None; one case added.

**Done.** Green on all four checks, no expectation changes, one scenario added:
120 scenarios / 121 runs and `--coverage` 58 of 61. Four things differed from
what is written above.

- **The macro path needed the bound as much as the generic one, and its own
  scenario.** The plan named `macro.c` but the design's example was a generic.
  Both macro expansion paths -- a parameterless name standing for its body, and
  a call substituting arguments -- crash identically today, measured at depth
  ~2282 against the generic form's ~702. `TypeCheckLoopMax` (256) sits well under
  both. The corpus's deepest legitimate expansion is **1**, generic and macro
  alike, so the limit has three orders of magnitude of headroom over real code.
- **One runaway generic yields more than one diagnostic.** Refusing at the limit
  and unwinding puts the *next* instantiation out at the limit as well, so the
  canonical example reports three times, once per instantiation site on the line.
  Nothing suppresses that, and the scenario is written so each site sits on its
  own line, since bless cannot place two annotations sharing a line and a code
  unless their messages differ.
- **The diagnostic arrived buried under 256 frames of instantiation trace.**
  `errorMsgNode` follows `instnode` to the end, which is one or two frames for
  ordinary code and 1546 lines of output here. It now shows four and counts the
  rest. This is behaviour every diagnostic shares, and no corpus scenario nests
  deep enough to notice.
- **It also pointed one line past the end of the file**, because
  `genericSubstitute` injects a call node without copying the position of the
  call it stands for. Fixed in the same commit; see hazard 5.

The probe was worth running twice: once over the corpus to learn the real depth,
and once on the crashing programs to learn how far each gets before the stack
gives out. Neither number is derivable from the code.

---

## Stage 5 — Demand from value name uses

**Goal.** Rule 1 for the declarations that lack it. This is the ordering defect
the work item names.

**Changes.** `exp/nameuse.c`, `nameUseTypeCheck`: analyze the declaration rather
than reading whatever is in its `vtype`, as `nameUseTypeCheckType` already does.

Rule 6 becomes reachable in the same commit — a constant demanded while it is
`Analyzing` and its type is still `unknownType` is circular. That is
`ErrorCircular`, the second new code, in `error.h` and `codes.toml`.

**Also audit the walk context** (rule 8). Demand now leaves from many more
places. `structTypeCheck` saves and restores `typenode`, `fnDclTypeCheck` saves
and restores `fn`; check every declaration entry saves what it changes, and that
`loopblock` and `scope` are right across a jump.

**Scenarios.** Two: the forward reference to an inferred global (design section
1), and the circular constant (section 7).

**Why safe here.** Stage 3 supplies the flags this depends on, so a global
referenced twice is analyzed once.

**Expectation changes.** Watch for scenarios that depended on a global being
untyped at the moment a function body read it. None is known.

**Done.** Green on all four checks, no expectation changes, two scenarios added
and one success program extended: 121 scenarios / 122 runs, `--coverage` 59 of
62. What differed:

- **The audit found two real defects, not the one it was looking for.** Rule 8's
  concern was `loopblock` and `scope` being wrong across a demand jump.
  `loopblock` turned out to be read only during name resolution, which is still
  one eager source-order pass, so demand cannot reach it at all. `scope` was
  worse than "not reset": `blockTypeCheck` was leaking it. See hazard 8.
- **`typenode` is not reachable stale.** Probing every demand in the corpus for a
  non-NULL `typenode` found exactly one, `self` inside its own type's method,
  where pointing at that type is correct. No reset was added, because none could
  be provoked or tested.
- **The one scenario that moved was not a global at all** but a closure, and the
  cause was the signature-failure skip rather than anything about types. See
  hazard 7. Preserving the skip put the suite back to no drift, which is what
  this stage predicted.
- **Two of the three probes disproved the reading that prompted them.** The
  guess that `errors != errorsOnEntry` inside `fnDclTypeCheck` would catch the
  signature failure measured zero hits corpus-wide, because the pre-pass had
  already marked the signature analyzed and the re-check reported nothing.

---

## Stage 6 — Remove the signature pre-pass

**Goal.** Delete `modTypeCheck`'s signature pre-pass and the `FlagSigError` flag.
Stage 5 replaced what they were for.

**Changes.** `ir/stmt/module.c`, `modTypeCheck`: both loops collapse to one that
analyzes each declaration. `inode.h`: `FlagSigError` (`0x0100`) goes.

**Keep the behaviour the flag carried.** It stops a declaration whose signature
failed from having its body checked. That is no longer cross-pass state — it is
local control flow inside one declaration's analysis, the same shape
`fnDclTypeCheck` already uses to skip flow after an error. **The flag goes; the
skip stays.** Without it this stage produces extra cascading diagnostics.

**Why safe here.** Last of the structural changes that can be reverted in
isolation, and only after demand demonstrably replaces what it removes.

**Expectation changes.** Watch `--bless` closely. Drift here almost certainly
means the skip above was not preserved.

---

## Stage 7 — Read the in-progress state

**Goal.** Rules 4 and 5. Recursive data structures become expressible.

**Changes.** `inode.c`, the in-progress branch of `inodeTypeCheck`: instead of
`ErrorRecurse` whatever was asked, answer — the type for a type question, "no
known size" for a size question. `ErrorNoSize` is the new code, its message
naming which of the five causes applies (design section 6): `@opaque`, a
non-`SameSize` trait, a function signature, infection from an unsized field, or
still being laid out. Retire `ErrorRecurse` from the layout path, and move
`types/struct.c`'s field check and `stmt/vardcl.c`'s variable check off
`ErrorInvType`'s "must be concrete and instantiable".

**Scenarios.** The three forms that are legal and currently refused — the linked
list, and both mixed and pure mutual-reference pairs (design section 5) — as a
`run` scenario if a list can be built and traversed, otherwise `compile`.

**Expectation changes, all three known:**

- `test/cases/struct/struct-typecheck-decl.cone:46` asserts
  `ErrorRecurse:9 "Recursive types are not supported"` on `inner SelfHolding`.
  That is a by-value self-hold, still an error; the code and message change.
- `test/cases/ref/ref-typecheck-borrow.cone` and
  `test/cases/trait/trait-typecheck-vref.cone` assert "concrete and
  instantiable"; the code changes.

**Why safe here.** Stage 4 already bounds instantiation. Without it this stage
turns a spurious error into a stack overflow, because the blanket refusal being
removed is what catches a recursive generic type today.

**Signal of a bad reading.** A scenario failing that this list does not name. The
corpus records unenforced rules by establishing the opposite, so a case may exist
that asserts something this stage correctly invalidates — rewrite it and say why,
per principle 6.

---

## Stage 8 — Lowering out of name resolution

**Goal.** Item 2.6, rewritten on the work item to say what it asks.

**Changes.** `exp/fncall.c`, `fnCallNameRes`: the `<-` value-tuple expansion moves
to type check, where its `borrowMutRef` gets a real type instead of the
`unknownType` it is handed today. `exp/nameuse.c`, `nameUseNameRes`: the
bare-name-to-`self.member` rewrite moves to type check, where it merges with the
copy `fnCallTypeCheck` already performs.

**Why safe here.** Independent of the rest. It is last because the second half is
a merge, and merging into `fnCallTypeCheck` is easier once stage 7 has made its
preconditions explicit.

**Expectation changes.** None expected. The `<-` case has coverage; check which
group before starting.

---

## After the stages

Not part of this work, recorded so they are not lost:

- **Suppressing repeated diagnostics** — the second half of rule 5. Needs a
  `Failed` state per declaration and a test at every reporting site. Design
  section 13.2 says what it would buy.
- **Migrating the eight type properties out of `flags`** into `ITypeNodeHdr`.
  [[IR refactor]]'s call; this work neither needs it nor makes it worse.
- **Giving the parser distinct node shapes for distinct syntaxes**, which is what
  would actually shrink `fnCallTypeCheck`. [[Lexer and Parser]] and
  [[IR refactor]].
- **Item 3's sub-steps 3.4 to 3.8** prescribe a manual topological order within a
  module. Demand makes that unnecessary. Confirm with Jon that they are struck
  rather than deferred.
- **`assert(0)` is a no-op** in the release build — 23 sites fall through into
  the next switch case. [[Compiler]] owns what replaces them. Do not convert them
  as a side effect of this work.

## When the stages are done

- **Rewrite `design/Analysis.md` as an as-is description.** It is a before/after
  argument, built to justify a change. Once the change has landed, the "today"
  halves are history and the note should describe how analysis works, full stop:
  the rules, the resolution order per declaration kind, and what each flag means.
  The measured defects in 13.4 become scenarios in the corpus and can go; 13.5's
  settled questions are worth keeping, since they say what was decided against.
- **This plan is disposable.** Discard it, or append whatever it turned out to
  teach to [[Analysis re-factor]]. It exists to sequence the work, not to
  describe the result.
- **Delete `handoff-analysis-refactor.md`** from the repository root.
