**Done.** Building out `design/`: which subsystems have a note, which do not,
what the notes that exist are missing, and the conventions they follow.

`design/` went from five notes to thirty, grouped into `northstar/`, `phases/`,
`nodes/`, `compiler/` and `diagnostics/`, with `design/_index.md` routing by aim,
by phase and by task. Every subsystem in the coverage table below now has a note.

**The remainder moved to [[Design notes follow-on]]** — per-node notes not yet
earned, the `names-and-namespaces` split, a link checker, and the northstar
material the author's own writing surfaced that has not been folded in.
[[Vault and repo sync]] carries the duplicate-copies problem.

This item is kept for its reasoning rather than its task list: what a design note
is for, the provenance rule, and the two measured sections at the end that show
how a note's scope was decided.

The gap was not a new observation. `design/nodes/_index.md` carried eight dangling
links — to name resolution, type check, generation, the parser, and four node
notes — written by someone who expected these notes to be there. **All eight now
resolve**, which is what the first pass of this item was for.

## What a design note is for

A design note says **how a subsystem works** — the mechanism, the invariants,
and the reasons a shape was chosen over the alternatives. A work item says
**what to do about it** — open bugs, unfinished features, decisions still owed.
The split matters when both exist for one subject: `design/phases/type-check.md`
should describe how coercion reaches a decision, and
[[Type Inference and Coercion]] should keep the measured cases where that
decision is wrong. Neither should restate the other.

**Read the code carefully; measure where you are uncertain; say which you did.**
Every claim in `design/phases/type-check.md` was produced by instrumenting the
compiler to report instead of act and compiling every corpus source;
`design/diagnostics/measuring.md` is that technique, and it remains the standard for any
claim that matters enough to be sure of. But that bar cannot hold uniformly across a dozen
more notes, and pretending otherwise would produce notes that *claim* to be
measured and are not — which is worse than either honest option.

So: **every note states its provenance near the top**, and a claim that a reader
would act on destructively gets measured rather than read. During
[[Analysis re-factor]], every confident reading of this compiler that was not
measured turned out wrong at least once, and writing the five phase notes
repeated that lesson three times — a claim that borrow lifetimes were enforced
nowhere, a claim that three sites read `Name.node`, and a `continue` that turned
out to leak. Each was caught by checking rather than by rereading. A design note
that is merely plausible is worse than no note, because it gets trusted; a note
that says which of its claims were checked lets the reader calibrate.

## Coverage today

`design/` is grouped into five folders; `design/_index.md` routes by aim, by
phase and by task.

| Subsystem | Note | State |
| --- | --- | --- |
| Parse (lexer and parser) | `phases/parse.md` | present |
| Name resolution | `phases/name-resolution.md` | present |
| Type check scheduling | `phases/type-check.md` | thorough |
| Type check reasoning | `phases/type-check-reasoning.md` | present |
| Flow analysis | `phases/flow.md` | present |
| LLVM generation | `phases/generation.md` | present |
| Naming rules | `phases/names-and-namespaces.md` | present; stale rows corrected |
| IR nodes | `nodes/_index.md` | present; also the per-node manifest |
| Per-node notes | `nodes/*.md` | twelve written; the rest below |
| Generics and macros | `nodes/generic.md` | present — was the weakest coverage, now closed |
| Measuring | `diagnostics/measuring.md` | present |
| Error codes | `diagnostics/error-codes.md` | present |
| Test suite | `diagnostics/test-suite.md` | operational guide, complete |
| References and regions | `northstar/references-and-regions.md` | present; corrected against the author's writing |
| Safety | `northstar/safety.md` | present — a scorecard of promise against enforcement |
| Performance (language) | `northstar/performance.md` | present; corrected against the author's writing |
| Modularity | `northstar/modularity.md` | present; corrected against the author's writing |
| Compiler architecture | `compiler/architecture.md` | present |
| Compiler performance | `compiler/performance.md` | present |

**Sections 1 and 2 below are closed** — `phases/type-check-reasoning.md` and
`phases/flow.md` are what closed them. They are kept unedited because the
reasoning about *what belongs in a phase note*, and the function-size
measurements behind it, still apply to the notes not yet written. Read them as
worked examples of how to scope a note, not as outstanding work.

## What is still owed

Moved to [[Design notes follow-on]].

## 1. The type check phase covers *when*, not *what*

`design/phases/type-check.md` is 480 lines. Sections 2 to 8, 10 and 11 — about
81% — describe how type check is scheduled: demand, the two marks, re-entry,
size, circularity, and the resolution order within each declaration kind. That
part is measured and solid.

What is absent is the reasoning the phase actually performs. Mentions in the
whole note: coercion 1, overload 0, cast 0, borrow 0, tuple 0, type literal 0,
region 0.

The gap lines up exactly with where the complexity is. Largest type check
functions by line count and branch count, against whether the note describes
them:

| Function | Lines | Branches | Described |
| --- | --- | --- | --- |
| `fnCallTypeCheck` | 275 | 59 | only its lowering |
| `structTypeCheck` | 180 | 37 | yes, 10.1 |
| `inodeTypeCheck` | 161 | 68 | yes, 3 and 4 |
| `blockTypeCheck` | 118 | 25 | partly |
| `borrowTypeCheck` | 110 | 18 | **no** |
| `genericSubstitute` | 83 | 18 | **no** |
| `ifTypeCheck` | 82 | 20 | **no** |
| `arrayLitTypeCheckDimExp` | 76 | 16 | **no** |
| `castIsTypeCheck` | 63 | 14 | **no** |
| `castTypeCheck` | 60 | 20 | **no** |
| `allocateTypeCheck` | 42 | 10 | **no** |
| `returnTypeCheck` | 41 | 10 | **no** |

52 of 62 type check functions are never named in the note. Most of that is
fine and should stay that way — section 10.4 covers variables, fields and
constants by behaviour without naming a function, which is the right altitude,
and the simple nodes all do the same primitive thing. The real hole is the
expression side.

Sections worth adding, in the order their absence costs most:

1. **Coercion and inference.** `iexpTypeCheckCoerce` and `iexpMultiCoerceInfer`:
   how an expected type flows down and an inferred type flows up, what
   `TypeCompare` and `OverloadMatch` distinguish, and how a block or an `if`
   unifies its branches. This is the contract every other type check
   implements, and it is the single biggest omission. Point at
   [[Type Inference and Coercion]] for the cases where it decides wrongly
   rather than repeating them.
2. **Calls and overload resolution.** `fnCallTypeCheck` is the largest function
   in the phase by a wide margin and serves several distinct syntaxes at once,
   which is also why [[IR refactor]] and [[Lexer and Parser]] want to split it.
   What the note most needs to say is *where the callee becomes knowable*: the
   dispatch leads with syntax, resolves the callee partway down, and has one
   path that deliberately skips resolving it. Until that is restructured, the
   note has to describe an invariant that holds in most of the function and not
   all of it. The measurements are in [[Namedef Refactor]]; the argument-order
   half is in [[Type Inference and Coercion]]; the one arm nothing exercises is
   in [[Init and Final]].
3. **Casts and conversions.** What `castTypeCheck` permits, and what `is`
   decides in `castIsTypeCheck`.
4. **References and borrows.** `borrowTypeCheck`, and precisely where it stops
   and hands off to flow analysis.
5. **Tuples and multi-value assignment.** The four `assign*Check` helpers, and
   why they are four.

## 2. Flow analysis has no note at all

`ir/flow.c` is 274 lines with its own `FlowState`, plus 559 lines in
`parser/parsefnflow.c`. It does alias accounting, move handling, and scope
dealiasing. `design/phases/type-check.md` gives it three sentences, all about
*when* it runs, and says so explicitly rather than pretending otherwise.

It should have its own note. Escape, permission, lifetime and move analysis are
the parts of Cone least like other languages and the least guessable from the
source. Related work: [[Use, escape analysis and de-aliasing]], [[Permissions]],
[[Ownership memory safety]], [[Regions]].

## 3. Filename conventions

**`design/` is kebab-case, no spaces.** Settled. Spaces in a path are not an OS
portability problem — Linux, macOS and Windows all handle them — but they are a
scripting hazard on all three: unquoted shell expansion, `find | xargs` without
`-print0`, and `make`, which cannot escape a space in a prerequisite at all.
`design/` is the folder most likely to be automated over later — a link checker,
a docs build, publication to conesite — and the only one cited from C comments.

**`workitems/` keeps its spaces.** Deliberate, not an oversight. It holds 43 of
the repository's 46 spaced paths and roughly 206 of its 212 wiki-link
references, no script or build file reads it, and no source file cites it. The
vault convention earns its keep there: `[[Types. Struct and Union]]` reads as
prose in a way a kebab-cased target would not.

Revisit that decision if any of these becomes true:

- something automates over `workitems/` — a link checker, an index generator, a
  publication step;
- a work item path starts appearing in source comments or in `test/run.py`;
- the vault stops being the primary way these notes are edited.

If it is revisited, rename in one pass with the wiki-links rewritten to the
aliased form the repo already uses, a kebab-cased target piped to a display
name.

**Wiki-links to `design/` notes use the aliased form** for this reason:
`[[names-and-namespaces|Names and Namespaces]]`.

**Renames go through `git mv`** so `git log --follow` reaches past them.
`design/phases/type-check.md` was `Analysis.md` until this work.

## Why the rename happened, so it is not undone

`Analysis.md` claimed three phases and described one. That is the same defect
[[Analysis re-factor]] had just removed from the `Analyzed` mark, which claimed
coverage by all three phases when every one of its ~66,000 measured touches was
in type check. A name that overstates its scope misleads exactly the reader who
trusts it, and it defeats a correct document sitting underneath.
