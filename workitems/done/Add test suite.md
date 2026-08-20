# Add test suite

> **Completed 2026-08-16.** Every requirement is met. `python test/run.py` runs
> **111 scenarios across 15 groups** — 97 passing, 14 expected failures — in
> under eight seconds, and `--bless` records nothing, which is the check that
> every expectation came from the compiler rather than being fitted to it.
>
> **One requirement is met in part.** R1.6 asks for Windows and Linux/WSL. The
> suite is verified on Windows from both PowerShell and Git Bash. The POSIX
> branches are written — `cc`/`gcc` rather than `link.exe`, `.o` rather than
> `.obj`, `shlex.join` rather than `list2cmdline` — and have **never executed**,
> because no Linux environment was available. Two Windows-specific linker faults
> turned up here, both found by running and neither by reading, so the first
> person to run this on Linux should expect to fix something and should look
> there first.
>
> The survey found **29 compiler defects**, three of which were fixed here
> because their fixes were unambiguous one-liners. The rest need language
> decisions and are owned by [[Ownership memory safety]],
> [[Diagnose instead of crash]], [[Unenforced language rules]] and
> [[Compiler defect backlog]]. Fourteen are pinned by `xfail` scenarios that will
> fail the suite the day they are fixed; eight are compiler crashes, which no
> scenario can hold, and that is the gap those items exist to close.
>
> Two constructs left the staging file without a group and were settled rather
> than filed: float intrinsics went to [[Intrinsics]] as future capability, and
> `<-` has a reserved `stream` row awaiting collections, see
> [[Collection - Streams & Iteration]].

The compiler has no test runner. Every check is run by hand, which means a clean
compile is the only routine evidence a change is correct, and a clean compile is
not evidence of correct runtime behavior. This work item builds the missing
infrastructure.

`design/test-suite.md` holds the design, the rationale, and the authoring
guidance for test scenarios. This note is the work: what to build, and what
"done" means.

## Decisions taken

| Decision | Choice |
| --- | --- |
| Runner | A single Python 3 script, no third-party dependencies. Python 3.13 is present; `tomllib` is available. |
| Expectations | Located diagnostics annotated inline in the `.cone` source as `//~` comments; file-level facts in a per-group `cases.toml`. See R2.1. This reverses an earlier sidecar decision — annotations remove line numbers as a maintained artifact, so an inserted line no longer churns every expectation below it. |
| WebAssembly | Out of scope for now. |
| AST-dump assertions | Out of scope. `--ir` output has no stability contract and [[IR refactor]] would invalidate any golden written against it. |
| Unit tests | Out of scope. Everything worth covering is reachable through the CLI; `--checktree` and `--verify` supply internal invariant checking. |
| Preserved overload fixtures | Restructured into the new layout when the suite reaches them. They are **not** canonical — treat them as a worked example of what the format must express, not as content to restore verbatim. |
| Continuous integration | Out of scope, but nothing may block it later. |

## Requirements

### R1 — Runner and invocation

- **R1.1** A run either builds `conec` first or verifies the binary is newer than
  every tracked compiler source, and refuses to run otherwise. A stale binary is
  indistinguishable from a language regression: the binary checked in at
  `build/x64-release/` predated the overload work by a week and failed
  `test/test.cone` with 17 errors that looked exactly like a broken master.
- **R1.2** Exit status is matched exactly against the taxonomy in
  `src/c-compiler/shared/error.h` — 0 success, 1 compile errors, 2 source not
  found, 3 out of memory, 4 bad options, 5 indent overflow. A rejection case
  requires exactly 1. Accepting "nonzero" would score a mistyped fixture path and
  a Windows access violation as correct rejections.
- **R1.3** Every case runs with stdin redirected from null and under a wall-clock
  timeout. `conec.c` and `error.c` both end with `getchar()` under `_DEBUG`, so a
  Debug build hangs at exit without this.
- **R1.4** Every run writes to its own output directory. Output filenames derive
  from the source basename and the IR dump from the program root, so two cases
  sharing a basename collide in a shared directory.
- **R1.5** Cases are independent and safe to run in parallel, with no ordering
  dependency and no reliance on another case's leftovers.
- **R1.6** Runs on Windows and Linux/WSL with no installation step.
- **R1.7** Runs the full suite by default. Selection is opt-in.

### R2 — Test model

- **R2.1** A scenario is a `.cone` source carrying its located-diagnostic
  expectations inline as `//~` annotations. One `cases.toml` per group directory
  holds category, tags, runs, and the file-level expectations that cannot attach
  to a line. A `compile` scenario needs no expectations at all, since its category
  fully describes the result.
- **R2.2** One source may declare several runs (differing option sets). This is
  how option matrices avoid duplicating source files.
- **R2.3** A **check** is a named assertion against a generated artifact — LLVM
  IR, or a run's stdout — declared in `cases.toml`. Located diagnostics are
  annotations (R2.9) and are not checks; a check exists precisely for what has no
  source line to attach to. Its name appears in failure output and is what
  selection matches.
- **R2.4** Cases carry pipeline-phase tags (`parse`, `nameres`, `typecheck`,
  `flow`, `genllvm`, `runtime`). The group directory supplies the feature tag.
- **R2.5** The runner derives a selection from a diff — mapping changed source
  paths to tags through a checked-in table — and reports which tags it selected
  and why.
- **R2.6** A `--list` mode prints what would run without running it.
- **R2.7** The runner's vocabulary is limited to command line, exit code, stderr,
  stdout, and files produced. It contains no knowledge of compiler internals.
- **R2.8** Results are reported in tier order, tier 0 first. Groups are
  deliberately dependent — tier 1 and 2 scenarios assume the foundation works —
  so a foundation break turns every group red, and tier ordering puts the root
  cause at the top instead of burying it among downstream failures.
- **R2.9** Annotation syntax: `//~ Code[:col] ["substring"] [follow-on]` applies
  to its own line, `//~^` to the line above and repeatable. Codes are named
  symbolically (R5.1). Bless writes the column and message substring; the author
  writes the code name.
- **R2.10** File-level expectations in `cases.toml` cover diagnostics with no
  source location (`errorMsg` prints none — `ErrorNoLoop` at
  `parsefnflow.c:240` and `:253`, and the `ErrorGenErr` family in `genllvm.c`),
  total diagnostic count for `recover`, and any non-default exit status.
- **R2.11** Scenario files are prefixed with their group name
  (`core-success.cone`). Output filenames derive from the source basename, so
  unprefixed names collide when scenarios from two groups are compiled by hand
  into one scratch directory — which is exactly when the runner's per-run
  directories (R1.4) are not in play.
- **R2.12** `cases.toml` lists every scenario and every support module. A `.cone`
  file in a group directory that is neither is an error. A support module is
  imported by a scenario and never compiled on its own — `imports.cone` with
  `importsub.cone` is the standing example, and it is also the access-violation
  regression test, so the `module` group cannot be built without this.

### R3 — Categories and pass criteria

- **R3.1** Six categories, defined in the design note: `compile`, `run`, `warn`,
  `reject`, `recover`, `driver`.
- **R3.2** `compile` requires exit 0, no diagnostics, and **explicitly zero
  warnings**. Warnings do not fail a compile on their own, so an unasserted
  warning count is silently ignorable.
- **R3.3** `compile` runs default to `--checktree` and `--verify` enabled, so
  malformed IR and invalid LLVM fail the case rather than reaching an object
  file.
- **R3.4** `reject` matches, for each expected diagnostic, the code and the
  source `line:column`; message text is matched as a required substring by
  default, with exact text available where the wording is the point.
- **R3.5** A case separates primary diagnostics from follow-on diagnostics that
  exist only as consequences.
- **R3.6** Expected-output parsing tolerates unprefixed continuation lines
  (`Uncounted` prints with neither an `Error` nor a `Warning` prefix and is not
  counted).
- **R3.7** `run` compiles, links against `conestd`, executes, and compares
  stdout. Where no linker is available the tier reports **skipped**, visibly and
  distinctly from passed.
- **R3.8** Any case may be marked `xfail`: it reports as an expected failure and
  **fails the suite if it starts passing**.

### R4 — Expected output management

- **R4.1** Comparison is normalized: path separators in the diagnostic location
  (which carries the path exactly as passed on the command line), the elapsed
  time and memory figures in the success line, and line endings. The repository
  has no `.gitattributes` line-ending rules, so a Windows working tree holds CRLF
  while the repository holds LF; comparison and bless must both be
  ending-agnostic.
- **R4.2** A bless mode regenerates expected output in bulk. It must refuse to
  bless a change in exit status or a crash, and its output must be plain text
  that diffs small and reviewably.
- **R4.4** Bless rewrites only the tail of each `//~` annotation — column and
  message substring — leaving surrounding source and the author-written code name
  untouched. It is a line-oriented edit, never a parse-and-reserialize of the
  source file. A scenario whose annotations bless cannot place unambiguously is
  reported, not guessed at.
- **R4.3** A failure prints the case name, the exact command line, and the
  expected-versus-actual delta.

### R5 — Diagnostic identity stability

- **R5.1** Cases name diagnostics symbolically (`ErrorNoCandidate`), never
  numerically (`1056`).
- **R5.2** `ErrorCode` is given explicit values, **and** the suite asserts a
  checked-in name-to-number table. The enum is currently unnamed positions, so
  inserting a code renumbers everything below it and silently invalidates every
  expectation at once; this makes that failure loud and local.

### R6 — Coverage and organization

- **R6.1** Cases are organized into the 17 groups defined in
  `design/test-suite.md`, following the reference manual's chapters. A construct
  is covered in depth in exactly one group. A feature with no group means a new
  group and a new manual chapter.
- **R6.2** Superseded by R2.12, which specifies how entry points and support
  modules are declared.
- **R6.3** Every crash or miscompile fix lands with a case that fails without it.
- **R6.4** The suite reports which `ErrorCode` values have no `reject` case. The
  enum is a closed list of roughly sixty codes, which makes this a cheap and
  honest coverage metric.
- **R6.5** No coverage in `test/test.cone` is lost. Its content is decomposed
  across the groups that own it (sequencing step 4), not discarded, and the
  staging file draining to empty is what marks that complete.
- **R6.6** All test sources use curly-brace blocks. None may depend on
  indentation or `:` for block structure, since the language is moving to
  free-form only. Free-form already parses today, so this costs nothing now and
  avoids rewriting the whole corpus later. `WarnIndent` and `ExitIndent` become
  dead on that switch and are excluded from the R6.4 coverage report rather than
  chased.
- **R6.7** Every new `ErrorCode` lands with a `reject` scenario provoking it, in
  the same change that adds the code. R6.4 reports the backlog; this keeps it
  from growing.

## Repository changes required

Everything outside `test/` that this work has to touch. Each is small, but each
is a place where the repository currently contradicts what the suite needs.

### Compiler source

**Status: the first three are done. The fourth was dropped on purpose** — its
only purpose was a byte-stable success line, and nothing compares one: the runner
counts `Error` and `Warning` prefixes and asserts exit status. Reopen it if
something ever wants to diff successful output literally.

| Change | Why | Requirement |
| --- | --- | --- |
| Give `ErrorCode` explicit values in `src/c-compiler/shared/error.h` | The values are unnamed enum positions, so inserting a code renumbers everything below it and silently invalidates every expectation at once | R5.2 |
| Name the exit code at `src/c-compiler/genllvm/genlstmt.c:95` | `errorExit(100, "Unrecoverable error!")` uses a bare 100 that is not in the `ErrorCode` taxonomy at all, so R1.2's exact-status matching has nothing to match it against | R1.2 |
| Remove or gate the `_DEBUG` `getchar()` in `src/c-compiler/conec.c` and `src/c-compiler/shared/error.c` | A Debug build blocks on stdin at exit, so every case hangs until its timeout | R1.3 |
| Add an option suppressing the timing and memory figures in the success line | `errorSummary` prints elapsed seconds and kilobytes used, so successful output is not byte-stable and cannot be compared literally | R4.1 |

### Documentation

**Status: done.** `CLAUDE.md`'s "Validating a change" now leads with
`python test/run.py`, names the stale-binary hazard, and points at
`design/test-suite.md` for authoring. Its repository-layout section lists the
runner, the group directories and the staging file. The reference-manual line is
added. What follows is the original statement of the problem.

`CLAUDE.md` is the main one, and it is wrong in more than the obvious place:

- **"Validating a change"** describes the by-hand procedure this work replaces —
  "Build `conec` and compile `test/test.cone`", "Run any fixture suite covering
  the affected feature. There is no committed fixture suite yet". The whole
  section needs rewriting around the runner once it exists.
- **`test/test.cone` is referenced by path** and moves under `test/cases/` in the
  layout below.
- **The stale-binary hazard is undocumented.** `CLAUDE.md` tells you to build and
  compile, but nothing warns that a binary left over from an earlier session
  produces failures indistinguishable from a language regression — which is
  exactly what the checked-in `build/x64-release/conec.exe` did during this work
  item's research, failing `test/test.cone` with 17 errors purely because it
  predated the overload merge by a week. R1.1 makes the runner enforce this;
  `CLAUDE.md` should say it too, since not every check goes through the runner.
- **The link-and-run recipe** stays accurate, but the suite becomes its primary
  consumer and should be cross-referenced from it.

`design/_index.md` gains its entry for `design/test-suite.md` (done as part of
writing that note).

The reference manual at `conesite/public/coneref/index.html` becomes the spine of
the test organization (R6.1). Its chapter list is now load-bearing: adding a
chapter implies asking whether a group is needed, and a feature with no chapter
has nowhere to be tested. Worth a line in `CLAUDE.md` alongside the existing
`conesite/` guidance.

### Build and ignore files

- `.gitignore` needs whatever directory the runner writes case output to, unless
  that lands under the already-ignored `build/`. Preferring `build/` avoids the
  change entirely and is the recommendation.
- `CMakeLists.txt`, `Cone.vcxproj` and `Conestd.vcxproj` need no change: the
  compiler changes above modify existing files without adding, removing or
  renaming any. This holds only while the runner stays outside CMake — see the
  CTest recommendation below.

### Noticed in passing, out of scope

`--width` / `ir_print_width` is parsed in `coneopts.c` and never read by
anything. Dead option; worth removing or implementing, but not here.

## Proposed layout

```
test/
  run.py                     the runner
  tags.toml                  source path -> tag map for diff-driven selection
  codes.toml                 pinned ErrorCode name -> number table
  cases/
    <group>/                 one of the 17 groups
      cases.toml             every scenario: category, tags, runs, file-level expectations
      <group>-<name>.cone    one source per scenario, expectations annotated inline
      <group>-<name>.out     expected stdout, for run scenarios
```

One `.cone` per scenario is irreducible — the compiler compiles files. Everything
else collapses into it: located expectations are `//~` comments in the source, so
the file still compiles by hand and still highlights, and the only per-group
metadata is one `cases.toml`.

`test/test.cone` and `test/submod.cone` move under `cases/`, which is the source
of the `CLAUDE.md` path change noted above.

## Anticipated scale

Steady state, after backfill: roughly **45–65 scenario files** carrying **300–600
named checks**, across 17 groups — about 65–85 files once the per-group
`cases.toml` and the `run` scenarios' `.out` files are counted.

| Kind | Estimate | Basis |
| --- | --- | --- |
| `compile` and `run` | 17–25 files | One success program per group, split in two where a group earns it |
| `reject` | 25–35 files | One per compiler stage per group that needs it, each carrying 3–6 codes; ~60 `ErrorCode` values total, minus the indent codes R6.6 retires. Stage files split further for recovery interference, mutually exclusive file structure, or an aborting diagnostic |
| `warn`, `recover`, `driver` | 5–8 files | Three live warning codes; a handful of recovery and invocation cases |

Non-replication is what keeps this smaller than a per-feature corpus: tier 1 and
2 groups test only their delta over the foundation, so most groups need one
modest success program rather than a broad one.

First delivery is far smaller — around 12 files: the `core` group built from the
relocated smoke case, the restructured overload fixtures folded into `core` and
`trait`, and the driver cases.

At ~30ms per compile this is a few seconds serially and about a second in
parallel, which is why R1.7 makes running everything the default.

## Sequencing

**All eight steps are done**, in a different order than planned. Step 5 came
early, because the tier 0 corpus already contained `run` scenarios and leaving
them unrunnable would have wasted the linking work. The coverage report from step
8 came second rather than last, because it turned out to cost almost nothing once
the runner parsed `error.h`, and it is far more useful as an input to the group
work than as a summary of it. And step 4 stopped being a step: draining the
staging file *is* building the tier 1 and 2 groups, so each group claimed its own
content as it was written. Each departure is recorded in the "Found while..."
section for the phase that made it.

1. **Prove the annotation format by hand, before writing any runner.** Transcribe
   two or three scenarios from the `overload-fixtures` branch `README.md` into
   `//~` annotations — `bad-overload-as-value` above all, which has one primary
   diagnostic at three positions plus two distinct follow-ons at other lines.
   That is the hardest thing the syntax must express, and it is real recorded
   output rather than an invented example. The same README also exercises support
   modules (R2.12) and IR checks (R2.3). If the format cannot express it cleanly,
   fix the format now rather than after a runner is built around it.
2. Runner skeleton with `compile`, `reject`, and `driver` categories, plus
   selection, parallelism, tier-ordered reporting, and bless mode.
3. The compiler changes above.
4. **Decompose `test/test.cone`.** It currently covers structs, traits, regions
   and virtual references alongside core content — material that belongs to five
   or six other groups under non-replication. This is substantially more than a
   relocation, and it is the step that establishes whether non-replication holds
   up in practice.

   Extract the core-owned content into `core-success.cone`, converting to braces
   (R6.6). Leave the remainder in place as a staging file that later groups draw
   from as they are built. Decomposition is complete when the staging file is
   empty, which makes the progress visible rather than a matter of judgment.
5. `run` category and the link-and-run path.
6. `warn`, `recover`, and `xfail`.
7. Restructure the overload fixtures into the layout as its first substantial
   consumer — function overload into `core`, method and trait overload into
   `struct` and `trait` — including the single-file runtime check of overloaded
   functions, methods, defaults and operators that was written and verified
   during the overload refactor but never committed.
8. Remaining groups in tier order, then the coverage report (R6.4) and backfill.

## Preserved fixtures

The [[Overload Refactor]] work built a fixture suite exercising much of this by
hand, preserved on branch **`overload-fixtures`**, whose tip is the head of the
overload refactor PR. Under `test/overload/` there: a `README.md` recording, for
each negative fixture, the expected diagnostic code, exact message text,
`line:column`, and which follow-on diagnostics to expect; positive fixtures
`methods.cone`, `operators.cone`, `globals.cone`; `imports.cone` with
`importsub.cone`, which is the regression test for the access violation fixed by
commit `34ca637` and crashes the compiler without it; and eleven `bad-*.cone`
covering diagnostics 1052–1058.

That README is the most complete statement of what a negative case must express,
and is worth reading before designing the expectations format. The fixtures themselves
are not canonical. Several are redundant under the packing rule — `bad-two-exact`,
`bad-exact-plus-coercible` and `bad-two-coercible` all provoke
`ErrorAmbigCandidate` at type-check time and should become one file with three
call sites — and the AST-dump assertions in the README are deliberately not
carried over.

## Found while building the tier 0 groups

Sequencing step 1 is done, and `lexical` and `core` are built:
`test/cases/lexical/` and `test/cases/core/` hold fifteen scenarios and a
`cases.toml` each, every one compiled by hand, and the four `run` scenarios
linked against `conestd` and executed against their `.out` files. Step 4 is
partly done — the core-owned content is out of `test/test.cone`, which now
carries a header saying what is left and for whom.

### The annotation format holds, with three things now written down

`bad-overload-as-value`, `bad-malformed-overload` and `bad-duplicate-concrete`
all transcribe cleanly, including one primary diagnostic at three positions,
follow-ons at other lines, and a paired diagnostic pointing at a line *earlier*
than the one that provoked it. Nothing in the overload README needed a format
change. Three points the syntax left open are now specified in the design note:

- **Carets count lines**, and an annotation-only line is a line, so successive
  annotations for one code line each take one more caret (`//~^`, `//~^^`). The
  alternative reading — every `//~^` in a run pointing at the nearest code line —
  would have been ambiguous in `core-parse-decls`, which needs three diagnostics
  on one line.
- **The quoted substring is load-bearing, not decorative,** wherever two
  diagnostics share a code, a line and a column. `lexical-reject-tokens` has
  exactly that: a bad hex digit reports both the escape and the unfinished
  literal at one position.
- **A diagnostic positioned at end-of-file has no line to carry it.** Several are
  reported at the token that should have followed, so the fix is to keep a
  further declaration in the file rather than to reach for a file-level
  expectation; `core-parse-decls` does this for `ErrorNoInit`.

### Repository change made

`.gitignore` ignored `*.out` as an executable, which would have silently dropped
every `run` scenario's expected stdout. Now negated for `test/cases/**/*.out`.
This was not on the list above; nothing else in the ignore file conflicts.

### Compiler defects found by writing the cases

None blocks tier 0 — each was routed around — but each is a bug the suite exists
to catch, and each wants a fix plus the scenario that fails without it (R6.3).

| Defect | Where | Effect |
| --- | --- | --- |
| `::name` at the start of a statement hangs the compiler | `parseNameUse`, `src/c-compiler/parser/parseexpr.c:25` sets the base module for a leading `::` but never consumes the token, so the statement parser re-enters at the same token forever | Unbounded output, no termination. One-line fix: `lexNextToken()` after `baseset = 1`. R1.3's timeout is what would keep this from wedging a suite run |
| `>>=` does not lex | `src/c-compiler/parser/lexer.c:851` tests `*(srcp + 1)` where it means `*(srcp + 2)` | `ShrEqToken` is unreachable; `n >>= 1` lexes as `>>` then `=` and fails to parse. `<<=` is correct, so this is asymmetric |
| An integer literal wider than i32 is truncated silently | An unsuffixed literal is i32, and nothing checks range | `mut n i64 = 9223372036854775807` yields -1, with no diagnostic. There is no range `ErrorCode` to name; adding one would be an R6.7 case |
| `\0` is not an escape sequence | `lexScanEscape`, `src/c-compiler/parser/lexer.c:298`, matches the null *terminator*, not the character `0` | The reference manual lists `\0`; `'\0'` reports "Invalid escape sequence '0'" |
| A non-ASCII character literal does not lex | `lexScanChar` reads one byte before expecting the closing quote | The manual says any code point 0x0020 or higher may be written directly; `'π'` fails. `'é'` works, so the escape is the only route |

The last two are documentation-versus-implementation gaps rather than crashes:
either the lexer gains the behavior or `reftoken.html` stops claiming it. The
manual also documents raw (`r"`, `r\``) and triple-quoted string literals,
multi-line string literal indentation stripping, and `$` in identifiers, none of
which the lexer implements. Those are excluded from `lexical` rather than marked
`xfail`, because none of them fails cleanly — each silently lexes as something
else, so an `xfail` would assert nothing.

### Uncovered in `core`, and why

- **`enum` is not implemented yet, so `core` has no coverage for it.**
  `parseEnum` builds an empty node and is called only from `parseFieldDcl`;
  there is no standalone `enum` declaration, and a bare `enum` field is accepted
  only as the tag discriminant of a base trait, which must be named `_`. Nothing
  here is worth writing a scenario against. Where it lands once it exists is an
  open question rather than settled by the `core` row of the group table — the
  discriminant behavior clearly belongs with the tagged trait, and a full enum
  type may or may not be core.
- **Static functions declared inside a type** — `Maker::make(...)` in the
  preserved `globals.cone` — are functions rather than methods, and overloading
  them follows the same rules `core-overload` establishes. They are left for
  `struct` only because they cannot be declared without one, under the "use no
  construct you are not testing" rule. This is the *only* part of function
  overload `core` does not cover: `core-overload` is entirely module-level
  global functions, exercising selection by argument type, by argument count, a
  defaulted parameter on one candidate, direct calls to each concrete name, and
  a private candidate selected through a public overload name.

One boundary call made rather than surfaced: `ErrorGenericOverload` is in
`core-parse-overload`, with a generic parameter list as scaffolding, because the
diagnostic is an overload-declaration rule and would be surprising to find in
`generic`.

### Still owed

- `test/test.cone` and `test/submod.cone` have **not** moved under
  `test/cases/`. The staging file was left in place, so the `CLAUDE.md` path
  references are still correct; the move belongs with the `CLAUDE.md` rewrite.
  *(Since done — the staging file is at `test/staging/test.cone` and
  `submod.cone` is superseded by the `module` group. See "The staging file is in
  `test/staging/`" below.)*
- `test/cases/core/core-typecheck.cone` carries eight diagnostics and
  `lexical-reject-tokens` seven, both past the three-to-six guidance. Both are
  five or six primaries plus follow-ons, and neither shows recovery
  interference, so they were left whole.

## Found while building the runner

Sequencing step 2 is done. `test/run.py` is a single Python 3.11+ script with no
third-party dependencies. `python test/run.py` runs all fifteen tier 0 scenarios
green, in tier order, in under two seconds.

### What it implements

Discovery and validation (R2.12), exact exit status (R1.2), stdin from null with
a wall-clock timeout (R1.3), per-run output directories under `build/testrun/`
(R1.4), parallelism (R1.5), Windows and Linux invocation (R1.6), the whole suite
by default (R1.7), `//~` parsing and matching (R2.9) with `cases.toml`
file-level expectations (R2.10), named checks against LLVM IR and stdout (R2.3),
`--list` (R2.6), tier-ordered reporting (R2.8), the `compile`, `reject` and
`run` categories with `--checktree --verify` and zero-warning enforcement
(R3.2, R3.3, R3.7), normalization (R4.1), and failure output carrying the case,
the exact command line and the delta (R4.3).

R1.1 is enforced by comparing the binary's timestamp against every `.c` and `.h`
under `src/` and against `CMakeLists.txt`; `--build` builds first instead, and
`--allow-stale` downgrades the refusal to a warning for the case where a
timestamp is misleading rather than the binary.

Two behaviors the requirements imply but do not spell out:

- **An output budget alongside the timeout.** The `::name` parser hang emits
  unbounded output, and it reaches a megabyte in about a third of a second, so a
  20-second timeout alone would mean gigabytes on disk per wedged case. A run is
  killed at 8 MB as well as at 20 seconds, and the failure says which.
- **A `llvmir` check reads the post-optimization dump.** `--llvmir` writes both
  `<srcname>.preir` and `<srcname>.ir`; the latter is what reaches the object
  file, so that is what a symbol assertion is about. `core-overload`'s three
  checks pass against either.

### Deferred, and why

Nothing is deferred any longer — every row this table held has since been built.
See "Found while building the remaining categories", "Found while building
bless", and "Diff-driven selection (R2.5)" below. Nothing was silently ignored on
the way: an unimplemented `cases.toml` key failed discovery by name for as long
as it was unimplemented.

### The staging file is in `test/staging/`, not under `test/cases/`

The layout above puts `test/test.cone` and `test/submod.cone` under `cases/`.
They are in `test/staging/` instead. The runner walks only the *directories*
under `test/cases/`, so a loose `.cone` file directly beneath it would be neither
an error nor run — exactly the "sitting unrun" state R2.12 exists to prevent —
and it cannot go inside a group directory, where an unregistered `.cone` file is
an error by that same rule. `test/staging/` makes it unambiguously not a case.

Draining it is not a step of its own. Its remaining content is structs and
operator methods, unions and variant matching, arrays and slices, borrowed
references and permissions, regions and rc references, pointers, closures,
generics and macros, `each`, and imports — all still in `:`-block form needing
brace conversion (R6.6). Every line of it belongs to a tier 1 or tier 2 group, so
each group claims its own content as it is built, and the file empties as
sequencing step 8 proceeds rather than in a pass before it.

### The CRLF hazard is worse than R4.1 records

R4.1 anticipated CRLF in `.out` files. It also changes **what the compiler
prints**. `core.autocrlf` is `true` on the development machine and the
repository has no `.gitattributes` eol rules, so a checkout gives CRLF sources;
`errorOutCode`'s echo loop copies up to `\n` and so carries the `\r` with it,
and stderr's text mode then turns its own `\n` into a second line ending. The
caret line holding `line:column` therefore lands two lines below the header on a
CRLF checkout and one line below on an LF one.

The runner skips blank lines — and only blank lines, never the echoed source,
which contains the scenario's own `//~` text — between the echo and the caret.
The suite was run green with the whole corpus rewritten to CRLF and again with
it rewritten to LF.

This is worth a `.gitattributes` deciding the question rather than a runner that
tolerates both, but that is a repository-wide change affecting every file and
belongs on its own, not inside the runner's commit.

### Repository changes made

- `.gitignore` gains `__pycache__/`. Nothing writes it today, but importing
  `test/run.py` does, and that is how its parsing was checked by hand.
- `CLAUDE.md`'s "Validating a change" said "There is no automated test runner",
  which is now false and would send a reader back to the by-hand procedure. It
  is updated to lead with the runner and to name the stale-binary hazard. The
  full rewrite listed under "Documentation" above is still owed: it belongs with
  the `test/test.cone` move, which has not happened.

### A sixth compiler defect

Found by feeding the runner a warning case, not by writing a scenario.

| Defect | Where | Effect |
| --- | --- | --- |
| A `while` with an empty body crashes the compiler | Reproduces as `fn main() i32 { while { } 0i32 }` | Access violation, exit `0xC0000005`. A non-empty infinite loop is fine and warns `WarnLoop`, and a conditional `while` is fine, so it is the empty block specifically |

This is what R1.2's exact-status matching is for: the runner reported "exit
status 3221225477 (not in the ErrorCode taxonomy)". Accepting "nonzero" would
have scored the access violation as a correct rejection. No scenario is added
for it yet — R6.3 wants the case to land with the fix.

## Found while building the remaining categories

Sequencing step 6 is done, and with it `codes.toml` (R5.2), the coverage report
(R6.4), the `warn`, `recover` and `driver` categories (R3.1) and `xfail` (R3.8).
`python test/run.py` runs twenty scenarios green. Only bless — since built, see
below — and diff-driven selection were left.

`test/codes.toml` is the pinned name-to-number table, regenerated with
`--bless-codes` and compared against `error.h` before any case runs; a mismatch
is a `SuiteError` naming exactly which codes moved, added or vanished.
`--coverage` prints the R6.4 report and runs nothing.

### `driver` is the one group with no manual chapter

R6.1 says a group follows a reference-manual chapter. `driver` cannot: it tests
the compiler's command line, not the language. It is tier 0, has no `.cone`
files, and takes an `argv` key that no other category may use. The four statuses
were measured rather than assumed — `--version` 0, unrecognized option
`ExitOpts`, missing source `ExitNF`, no arguments `ExitOpts`.

### `warn` asks exactly what `reject` asks

The category table originally asked a `warn` scenario for exit 0, the named
warnings present, and no errors — and, unlike `reject`, not for the absence of
unnamed ones. **That was wrong and the table is corrected.** `warn` is the only
category that permits a warning at all, since `compile` and `run` require
explicitly zero (R3.2), so a `warn` scenario is the single place in the whole
suite where a newly introduced spurious warning could sit unnoticed. Leaving that
covered only by an author remembering to write a `diagnostics` count is precisely
the silently-ignorable failure R3.2 exists to close. A `warn` scenario now
requires every warning annotated, the same as `reject`.

Discovery also refuses a `warn` scenario that names no warning at all, on the same
ground: a category assertion nothing exercises is not an assertion.

### `recover` is implemented and unexercised

Nothing in `core`'s reach demonstrates recovery beyond what the existing `reject`
scenarios already show. `core-parse-stmts` carries five parse diagnostics and
`core-typecheck` eight, each accumulated and each annotated; a `recover` scenario
over the same ground would assert strictly less. Recovery interference is real —
the parser resynchronizes by skipping forward, and a file with several broken
declarations yields one diagnostic rather than several, because parse errors stop
the pipeline before analysis — but that is a reason `reject` files are split, not
a scenario. The category is built and waits for a group whose diagnostics are
recovery artifacts whose positions are not worth pinning.

### Warning coverage is one code of three

Of the three warnings in `error.h`, only `WarnLoop` is `core`'s to provoke.

- **`WarnCopy` is emitted nowhere in the compiler.** It is declared in `error.h`
  and has no `errorMsgNode` call anywhere in `src/`, so no scenario in any group
  can reach it. Either the check it belongs to is unwritten or the code is dead.
- **`WarnName` needs a function-valued expression.** `parseFn` reports it where
  the parser expected an anonymous function and got a named one, which is
  `parseexpr.c`'s `fn` term. It cannot be written without one, so it belongs to
  `closure`.
- **`WarnIndent` is excluded** from the coverage denominator by R6.6 along with
  `ExitIndent`, rather than chased.

`WarnLoop` is reported at the loop's *block*, not at the `while`, so the column
is the `{`. A labeled `break` counts for the loop it names and not for the one it
sits in, so a nested loop whose only exit is `break 'outer` warns while the outer
one does not. `core-warn-loops` pins both.

### Where coverage stands

28 of 61 diagnostic codes are covered by tier 0. The 33 uncovered are almost
entirely tier 1 and 2 subject matter — references, permissions, moves, arrays,
slices, methods, allocation — which is the expected shape at this point and is
what R6.4 exists to make visible rather than to complain about.

## Found while building bless

`--bless` is built (R4.2, R4.4), so diff-driven selection is the only deferred
item left. It re-runs the selected scenarios and records what the compiler
actually produced, rewriting two things and nothing else: the tail of each `//~`
annotation — the column after `:` and the quoted substring — and the `.out` file
of a `run` scenario. The edit is line-oriented, as R4.4 requires. The file's
lines are read, the two fields are replaced by their spans within the line the
author wrote, and the lines are written back, so indentation, the position of
the `//~`, the caret run, the code name, the `follow-on` flag and the spacing
between fields all survive byte for byte whether or not the runner would have
written them that way. Nothing is reserialized from a parsed representation, and
each line keeps the ending it had, which is what makes R4.1's
ending-agnostic requirement hold in the writing direction as well as the reading
one.

### A field is rewritten only where what it asserts no longer holds

This falls out of the guidance to pin the fragment that identifies a diagnostic
rather than the whole sentence. A fragment that is still contained in the
message still identifies it, so there is nothing to bless and the line is left
untouched; blessing a green corpus is therefore a no-op, which is the property
that makes `--bless` safe to run over a whole suite. Where a substring does have
to be rewritten it becomes the entire message, because bless has no way to know
which fragment of a new sentence the author would have chosen — trimming it back
is part of reviewing the diff. By the same reasoning a field the author left off
stays off: adding a column to an annotation deliberately written without one
would tighten an expectation, which is the same overreach as adding an
annotation outright.

### The pairing problem, and how it is resolved

To rewrite a column, bless has to know which produced diagnostic belongs to
which annotation — but the column is the thing that may be wrong, so it cannot
also be part of the key. `match_diagnostics`, which the assertion path uses, keys
on code, line *and* column and is deliberately left alone; bless has its own,
looser pairing that keys on code and line only. Where one line carries two
annotations for one code it falls back on the quoted substring, and requires that
there be exactly one way to give every annotation in that group its own
compatible diagnostic. Where there is none, or more than one, the scenario is
refused and the group is printed with both sides side by side, as R4.4's "reported,
not guessed at" requires. Pairing the two sides in the order they happen to arrive
would work today and is exactly the guess that rules out: nothing says an author
writes carets in emission order.

`lexical-reject-tokens` is the case this exists for — two diagnostics sharing a
code, a line and a column, separated only by their substrings — and
`core-parse-decls` is the other, with three diagnostics on one line reached by
`//~`, `//~^` and `//~^^`. Both bless correctly, and both refuse correctly once
their substrings stop distinguishing anything.

### What it refuses, and what it merely reports

Refusal is per scenario rather than per suite, so one case whose behavior changed
does not hold up recording the rest. A scenario is refused when its exit status
differs from what its category or `cases.toml` expects, when the run crashed —
a status outside the `ErrorCode` taxonomy, a timeout, or the output-budget kill —
when its annotations cannot be placed unambiguously, or when it is marked
`xfail`. The last was not anticipated: an `xfail` scenario's expectations record
a defect rather than claim to be current, so blessing them to the buggy output
would make the case pass, which R3.8 then reports as an XPASS, and would erase
what the mark records in the same stroke.

Reported but never written are a diagnostic that no annotation claims and an
annotation that nothing produced. Bless cannot write the first because it does
not know the code name, which R2.9 gives the author, and must not delete the
second because it may be a regression rather than a stale expectation. Both are
printed and the rest of the scenario is still recorded, since neither says the
placeable annotations are wrong. `cases.toml` is never touched at all: a
`diagnostics` count that no longer matches means the set of diagnostics changed,
which is an authoring decision.

### It was tested by breaking the corpus mechanically

The corpus is green, so there was nothing genuinely wrong to bless, and a
rewriter with no baseline to check against was the reason bless was deferred in
the first place. The baseline was manufactured instead: a script perturbs every
expectation in `test/cases/` — all fifty-three annotation columns, every quoted
substring, and all four `.out` files — and bless is required to put them back
byte-identical to a copy taken beforehand.

Columns and `.out` files round-trip exactly, both with the corpus in LF and with
the whole of it rewritten to CRLF, which is what checks that endings are
preserved rather than normalized. Substrings round-trip exactly where the author
had pinned the whole message, and where the author had pinned a fragment they
come back as the whole message, which is correct rather than identical — the
suite runs green afterwards either way, and blessing a second time changes
nothing, so the result is a fixed point. Blessing every substring in
`lexical-reject-tokens` into garbage is refused, and that file is left exactly as
it was perturbed, which is the direct evidence that a refusal writes nothing in
the scenario rather than most of it.

The refusals were checked by hand: repairing every line of a `reject` scenario so
that it compiles clean, appending a syntax error to a `compile` scenario so that
it does not, and appending the known `::name` parser hang so the run is killed by
the output budget. Each is refused for its own reason while the other scenarios
selected alongside it still bless. `--bless` with a selector was checked against a
fully perturbed corpus and restored only what it selected.

### R1.6 was quietly broken from a Git Bash shell

Found while testing bless, and pre-existing. Git for Windows ships a coreutils
`link.exe` in its `/usr/bin`, so under Git Bash the runner found a `link.exe` on
PATH, took it on faith, and skipped sourcing vcvars entirely. Every `run`
scenario then failed to link, and failed in a way that read as a compiler problem
rather than a shell one — which is the same class of misleading failure R1.1
exists to prevent, arriving by a different route.

vcvars is now tried first, and an inherited `link.exe` is trusted only after it
identifies itself as Microsoft's. The suite runs green from PowerShell and from
Git Bash. R1.6 asks for Windows and Linux/WSL with no installation step; it did
not anticipate that one Windows shell would supply a decoy.

## Found while building the groups

Sequencing steps 4, 7 and 8 are done. The suite runs **107 scenarios across 14
groups**: 93 pass and 14 are expected failures, each recording a defect that
fails cleanly and will fail the suite the day it is fixed.

| Tier | Groups |
| --- | --- |
| 0 | `driver`, `lexical`, `core` |
| 1 | `struct`, `union`, `array`, `closure`, `ref`, `move` |
| 2 | `region`, `trait`, `collection`, `each`, `typemgmt`, `generic`, `module`, `safety` |

`exception`, `concurrency` and `meta` have no directory; see the design note's
group table for why, and the "unimplemented" list below for the evidence.

Every group was written against the compiler rather than against the manual, and
every group's `--bless` records zero changes — which is the check that says an
expectation came from real output rather than being fitted to it.

### Coverage

`python test/run.py --coverage`: **48 of 61 diagnostic codes covered, 2
genuinely uncovered, 11 raised by nothing.**

That third bucket is new and is the more useful finding. Eleven codes are
declared in `error.h` with no call site anywhere in `src/`: `ErrorDupImpl`,
`ErrorNotFn`, `ErrorNoElse`, `ErrorNoVtype`, `ErrorNotLval`, `ErrorAddr`,
`ErrorNoFlds`, `ErrorBadAlloc`, `ErrorNoDbl`, `ErrorBadSlice` and `WarnCopy`.
Each was found independently by whichever group would have owned it, and the
runner now derives the same list by scanning `src/`, so it cannot go stale. Either
the check each belongs to is unwritten, or the code should be deleted from the
enum — deletion is cheap now that values are explicit, since it leaves a gap
rather than renumbering neighbours.

`ErrorNoElse` is the instructive one: the exhaustiveness diagnostic everyone
would expect it to be is actually `ErrorInvType`, "if requires an 'else' clause
(or exhaustive matches) to return a value" (`ir/exp/if.c:181`).

The two genuinely uncovered are both blocked rather than unwritten. `ErrorGenErr`
is provoked by a nested allocation just before the compiler dies on it.
`ErrorNoEof` is reported **only inside an included or imported file**
(`parsemod.c:71` and `:274`), and annotation matching requires a diagnostic's
path to equal the scenario's own source, so no inline annotation can reach it.
The `module` group asserts the behavior as a `recover` scenario instead. Letting
an annotation in a *support module* match diagnostics reported in that file is
what R2.12's support-module model implies should already work, and is the fix.

### The compiler defects, by shape

Building the corpus found **29 defects**. The count matters less than the fact
that they fall into four repeating shapes, because each shape suggests a
different remedy.

**These are now owned by four follow-up work items**, split by the decision each
defect needs rather than by subsystem, because that is what determines who can
pick one up: [[Ownership memory safety]] for the miscompiles and the soundness
hole, [[Diagnose instead of crash]] for the error paths that dereference their
own NULL, [[Unenforced language rules]] for the rules nothing checks, and
[[Compiler defect backlog]] for the residue. What follows stays here as the
evidence — this is where each was found and how — and those items carry the work.

**A. Memory-unsafe code generation — the serious ones, all in `region`.**

| Defect | Where |
| --- | --- |
| A moved `+so` reference double-frees | `genlDealiasNodes` frees every owning-reference variable with no notion of one whose value was moved out |
| A first assignment to an uninitialized `rc` variable releases garbage | `genlStore` dealiases an `rc` lval's previous value without consulting `VarInitialized` |
| A region-allocated struct owning a reference corrupts the heap on release | `genlDealiasFlds` reaches the field with a struct GEP and hands that *address* to a routine wanting the reference the field *holds*. A stack struct with such a field is safe only because its fields are never released at all — they leak |
| A nested allocation fails LLVM module verification | |
| Fallible allocation `?+rc-mut v` dies with an access violation | |

**Region coercion is backwards, and it is a soundness hole.** `refMatches` calls
`regionMatches(from->region, to->region)` — the arguments are swapped against the
parameter names. One direction rejects a valid coercion, which
`region-borrow-coerce` pins as an `xfail`. The other **silently accepts a
borrowed reference where an owning one is wanted**, handing a stack address to a
parameter typed `+rc-mut`; it compiles, it runs, and there is no diagnostic to
assert against. The suite cannot hold the dangerous half.

**B. Crash instead of diagnose.** In each of these the compiler works out that
something is wrong, reports it or returns an error, and then dereferences the
NULL that the error path left behind: `struct {` with no name; `trait {` with no
name; `each x in <anything not a range>`; a two-dimension array literal
(`[2, 3; 0]`); any error in a written-out generic type argument list; an empty
generic type parameter list `fn f[]()`; narrowing to a *structurally* conforming
target; and cross-module private access.

**None of these can be a scenario.** A process death fails no assertion, so they
cannot even be `xfail`ed — this is the category the suite structurally cannot
hold, and it is the strongest argument for fixing them rather than recording
them. Each is excluded with a written reason in its group's `cases.toml`.

**C. One-line gaps.** A missing tag in a condition, a missing `break` in a
switch, a signature built from `unknownType`, a permission read from the wrong
node:

- `itypeGetTypeDcl` (`ir/itype.c`) lacks a `break` after `case TypedefTag:` and
  returns the typedef's unresolved `typeval`, so **any use of a typedef as a type
  crashes**. This is the one defect whose fix looks genuinely mechanical.
- `assign.c:175` gates the borrow-escape check on `RefTag` on both sides, so
  `ArrayRefTag` skips it and **a slice may outlive the array it borrows**.
- `newArrayRefTypeMethods` (`corelib/corenumber.c:274`) declares `==` and `!=`
  for slices with a signature whose region, permission and element type are all
  `unknownType`, which no real slice matches, so **slices cannot be compared**.
- `iexpGetLvalInfo`'s `ArrIndexTag` case takes the permission from the reference
  only for `ArrayRefTag` and `PtrTag`, so **writing through `&mut [N; T]`
  requires an explicit deref**.
- `parseAmper` re-applies suffixes to the borrow, so `&mut p.x` type-checks as
  the field's type while codegen returns the field pointer — **LLVM verification
  fails**. The array-index path has the `FlagBorrow` fixup the field path lacks.
- `genlTypeMeta`'s nullable-pointer optimization gives base and variants a bare
  pointer type while the variant initializer still stores through the variant's
  struct type; **matching on one segfaults**.

**D. Reported in the wrong place.** `each`'s synthesized increment reports at the
function's closing brace, because `parseEach` builds those nodes with the lexer's
position at the time the block finished. A name-fold clash between two wildcard
imports names the wrong file entirely. And `ErrorFewArgs` is emitted with the
message "Too many arguments provided for generic function" — code and message
disagree, and `generic-typecheck-infer` asserts it as-is so that fixing either
forces the choice.

### Documented features that do not exist

Each was verified against the compiler, not inferred from absence.

| Feature | Evidence |
| --- | --- |
| Exception handling | No `try`/`catch`/`throw`/`throws`/`panic`/`assert` in the lexer's 43-entry keyword table. `\|\|` does not lex at all; `?` is *prefix* Option sugar, never the postfix form the manual shows |
| Concurrency | No `spawn`/`thread`/`async`/`await`/`actor`/`qfn`/`atomic` |
| The `#` meta-language | `lexer.c:608` produces `MetaIdentToken` and **no parser site consumes it** |
| `trust` | No keyword, no parse rule |
| Weak references, lock permissions | `weak` appears nowhere in `src/` in any case |
| Closure capture | `parseexpr.c:296` lifts the `FnDclNode` to module scope while parsing, so an anonymous function never sees its enclosing locals. It is a lifted function with a reference type, not a closure |
| Closure references | The literal `\|{w}\| {…}` does not lex; the type `+<so \|\|` does not parse |
| Delegated inheritance | `engine Engine use fuel, thrust` — `use` terminates the field statement |
| Initializers and `clone` | Both need `self &new`; there is no `new` permission |
| `extend` | Not lexed. Only `extends`, which is trait inheritance |
| Selective import | `import mod::name` — `parseImport` consumes `::` only to look for `*` |
| `enum` | As already recorded for tier 0 |
| **`imm` enforcement** | `imm n = 3; n = 5` compiles clean, as does writing through an `imm` reference or to an `imm` struct's field. There is no immutability check on a variable anywhere |
| Module-level privacy | `importNameRes` folds every named node including private ones, and `mod::_privateName` resolves without complaint |
| Lifetime rules beyond one | The only enforced rule is the assignment check at `assign.c:176-179`. Returning `&local` from a function is not diagnosed |

**These are features, not defects, and most already have a work item.** The
survey's contribution is that each is now *verified against the compiler* rather
than assumed, so whoever picks one up knows exactly what exists. Routing:
exception handling to [[Error Handling]]; concurrency and the sendability flags
to [[Concurrency Threads]] and [[Concurrency Primitive Types]]; the `#`
meta-language to [[Metaprogramming]]; weak references and lock permissions to
[[Regions]] and [[Permissions]]; closure capture and closure references to
[[Types. Function and Closure]]; initializers and `clone` to [[Init and Final]];
delegated inheritance to [[Types. Struct and Union]]; selective import to
[[Using and Module Name-folding]]; `enum` to [[Types. Number and Enum]]; and
`typedef`/`extend` to [[Type Inference and Coercion]]. `imm` enforcement, module
privacy and the lifetime gaps are rules rather than features and go to
[[Unenforced language rules]].

Two dead permission flags belong with these: `RaceSafe` and `IsLockless` are
declared in `permission.h`, populated correctly across all six permissions in
`corelib.c` — `RaceSafe` matching the manual's sendability prose exactly — and
**read nowhere**. The concurrency design is one flag check away from being
partially assertable, and that check does not exist.

**None of `throw`, `catch`, `panic`, `assert`, `yield`, `spawn` or `actor` is a
reserved word.** A program declaring all seven as functions compiles clean.
Implementing any of those chapters is therefore a breaking change to existing
user code that no current test would catch. Reserving the words ahead of the
features is cheap insurance, and cheapest now.

### Published documentation that is wrong

The manual was treated as a claim to verify throughout, and it needed to be.

- `refmove.html` opens "None of this has been implemented." Nearly all of it is
  implemented and enforced. It also says moving out of an indexed array element
  is prohibited; the compiler permits it and deactivates the whole array.
- `refperm.html` says dereferencing an `&opaq` reference is an error. It compiles
  clean.
- `refinitdrop.html` calls finalizers unimplemented. Finalizers work; it is
  initializers that do not.
- `refeach.html` admits only the range operators are implemented, but is silent
  that the others *crash*.
- The anonymous variable `_` for early destruction is documented and fails name
  lookup.

By contrast `refexcept.html`, `refconc.html`, `refmeta.html`, `reftrust.html`,
`refweakref.html`, `refpermlock.html`, `refinherit.html` and `reflifefn.html` all
open by disclaiming themselves, and all are accurate. The pattern is that
*aspirational* chapters are honest and *partially built* ones drift.

### Facts the corpus now pins, which nothing recorded before

- **The coercion rule is one line.** `nbrMatches` (`ir/types/number.c:57`): same
  tag, and strictly more bits. It widens, never narrows, never crosses
  signed/unsigned, never crosses integer/float.
- **An unsuffixed integer literal is i32; an unsuffixed float literal is f32.**
  The second silently costs precision — `mut x f64 = 16777217.0` yields
  16777216.
- **Operator widening is asymmetric.** `wide + small` compiles, `small + wide`
  does not: the candidate set comes from the left operand's type.
- **`each` is purely a parser rewrite** into `{ mut v = lower; while cmp { body;
  v += step } }`, which explains all of its surprising behavior.
- **`match` is sugar for an `if` chain**, so its diagnostics say "if" and "else".
- **Macro arguments are substituted, not evaluated**, and macro bodies are
  name-resolved at the declaration site.
- **Generic instantiation is memoized** and shared across spellings; instance
  functions are name-mangled with their type arguments, instance structs are not.
  The struct half of that is right and stays: an LLVM struct type name carries no
  linker identity, so `%Holder` and `%Holder.1` may share a Cone name with no
  consequence. An instance's **methods** are functions and did need one — they are
  clones sharing the generic's `genname`, and their signatures may name no type
  parameter at all, so both instances' copies landed on one `linkonce` symbol.
  `itypeMangle` now appends a generic instance's type arguments wherever a named
  type appears in a mangled name, which distinguishes the methods without renaming
  the type.
- **Structural conformance suffices for `&<Trait` coercion and dispatch; only
  narrowing needs nominal `extends`.**
- **A region is not a privileged set of two** — any struct with an `_alloc` of
  the right shape is one, and the checks fire when an allocation names it, not
  when it is declared.
- **Flow analysis tracks whole values only**, never a field or element.
- **Receivers are not auto-borrowed**; `&` binds tighter than `.`; `by` and `in`
  are reserved; a `&fn(...)` parameter must come last.
- **Bounds checks are emitted for fixed arrays and array references alike.** What
  differs is that an array's length is a compile-time constant, so a constant
  index folds the trap away, while a slice's comes from the value and does not.

### Still owed

- The defects above, now owned by [[Ownership memory safety]],
  [[Diagnose instead of crash]], [[Unenforced language rules]] and
  [[Compiler defect backlog]]. Three with unambiguous one-line fixes — the
  `::name` parser hang, the `>>=` lexer typo, and the `typedef` missing `break` —
  were fixed here with their scenarios, since leaving a known typo unfixed to
  file a work item about it helps nobody.
- R6.4 will not reach full coverage while eleven codes are unraisable and two are
  unreachable by annotation.

### Diff-driven selection (R2.5)

`test/tags.toml` maps repository paths to selectors — 24 rules, 44 patterns,
each carrying a `why` the runner prints verbatim, because "reports which tags it
selected and why" is half the requirement. A path ending in `/` is a prefix and
the longest match wins, so `shared/error.h` overrides `shared/` without the table
depending on the order its rows are written in. Every rule names exactly one
outcome: phase tags, groups, `everything`, an explicit "nothing observes this",
or `group-from-path` for `test/cases/` where the directory *is* the selector.

`--since [REV]` derives the selection and hands it to the same `select()` a typed
selector goes through; there is no parallel mechanism. It defaults to the merge
base with `@{upstream}`, or `HEAD` — the working tree, untracked files included —
where the branch has none, and says which it took.

**Unmapped widens rather than narrows.** A path no rule covers is named and the
whole suite runs. So does a change to `error.h`, which invalidates every
expectation at once. A diff-driven mode that silently skips is a trap; one that
widens loudly is useful. All 471 tracked files map to a rule today, and adding a
source directory without adding a row is caught the first time a diff touches it.

**The limit worth knowing.** Tags say what a scenario is *about*, not every phase
it traverses, so a parser change selects the 43 scenarios written to exercise
parsing rather than every scenario a parser change could break. Widening until it
did would select almost everything. The full run before merge is what closes
that; `design/test-suite.md` says so where someone will read it.

Two mappings are judgement calls a later reader may want to revisit: `ir/`
selects everything but `parse` (89 of 111, so barely a filter) because five
ownership-lowering scenarios are tagged `genllvm` without `typecheck`; and
`shared/timer.c` claims nothing observes it, which is true only while nothing
compares successful output literally.

### Decomposition is complete (R6.5)

`test/staging/` is gone. Every construct of the old smoke input went to the group
that owns it, and the last two — which had no owning group — were settled rather
than filed somewhere convenient:

- **`.sqrt()` and the other float functions are future capability**, and belong
  with float intrinsics rather than with any current group. Recorded in
  [[Intrinsics]]. Coverage follows the capability; when it is built, R6.1's
  question is whether it earns its own group and manual chapter.
- **`<-` gets its own coverage group, once there are collections.** It is
  implemented and reachable today only through stdio's `IOStream`, but it is an
  append operator intended for collections, and `corelib/` has none — so covering
  it now would pin the operator to the one consumer it is not for.
  `design/test-suite.md` carries the reserved `stream` row; see
  [[Collection - Streams & Iteration]].

### R1.6's Linux half is written but unverified

The suite runs on Windows from both PowerShell and Git Bash. The POSIX paths
exist — `cc`/`gcc` rather than `link.exe`, `.o` rather than `.obj`, `shlex.join`
rather than `list2cmdline` — but **none of them has ever executed**, because no
WSL or Linux environment was available while this was built.

That is worth stating rather than assuming, because two Windows-specific linker
faults turned up during this work: the runner sourced vcvars for `link.exe`
correctly but then handed the command to `subprocess` as a list, which escaped
the quotes around the batch path; and it later trusted Git for Windows'
coreutils `link.exe` because it only checked that *a* `link.exe` existed. Both
were found by running, neither by reading. The POSIX path deserves the same
treatment before R1.6 is claimed outright.

## Recommendations on the two open questions

### `codes.toml`: generated, checked in, and verified against the source

Neither pure alternative works. Hand-maintaining sixty name-to-number pairs is
error-prone, but generating the table at build time defeats its entire purpose —
a renumber would silently regenerate a matching table and nothing would fail.

The recommendation is the pattern that keeps both properties: **the runner parses
`error.h` itself, compares the result against the checked-in `codes.toml`, and
fails on any mismatch.** A renumber then produces one loud failure naming exactly
which codes moved, and the fix is to re-bless and review the diff. No build step,
no CMake change, no generated-file staleness, and it works identically on both
platforms because it is just Python reading a header.

Note this is defense in depth rather than the primary guard. Explicit `ErrorCode`
values (R5.2) remove the hazard at the source; the table catches the case where
someone adds a code without following the convention.

### Invocation: run the script directly

CTest is the wrong fit here, for a specific reason. Registering each case as a
CTest test would require the case list at CMake configure time, so adding a case
would mean re-running CMake — and diff-driven selection (R2.5) cannot be
expressed in CTest at all, since `-R` matches test-name regexes and knows nothing
about tags. Registering the whole suite as a single CTest test avoids that but
then contributes nothing beyond a conventional entry point, while still requiring
a configured build tree just to run tests.

The runner already handles parallelism and selection better than CTest would, so
**invoke it directly** and let it own both. If CI arrives later and wants a
conventional hook, a three-line `add_test` registering the suite as one test can
be added then, at no cost to anything designed now.
