# Add test suite

The compiler has no test runner. Every check is run by hand, which means a clean
compile is the only routine evidence a change is correct, and a clean compile is
not evidence of correct runtime behavior. This work item builds the missing
infrastructure.

`design/Test Suite.md` holds the design, the rationale, and the authoring
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
  `design/Test Suite.md`, following the reference manual's chapters. A construct
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

| Change | Why | Requirement |
| --- | --- | --- |
| Give `ErrorCode` explicit values in `src/c-compiler/shared/error.h` | The values are unnamed enum positions, so inserting a code renumbers everything below it and silently invalidates every expectation at once | R5.2 |
| Name the exit code at `src/c-compiler/genllvm/genlstmt.c:95` | `errorExit(100, "Unrecoverable error!")` uses a bare 100 that is not in the `ErrorCode` taxonomy at all, so R1.2's exact-status matching has nothing to match it against | R1.2 |
| Remove or gate the `_DEBUG` `getchar()` in `src/c-compiler/conec.c` and `src/c-compiler/shared/error.c` | A Debug build blocks on stdin at exit, so every case hangs until its timeout | R1.3 |
| Add an option suppressing the timing and memory figures in the success line | `errorSummary` prints elapsed seconds and kilobytes used, so successful output is not byte-stable and cannot be compared literally | R4.1 |

### Documentation

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

`design/_index.md` gains its entry for `design/Test Suite.md` (done as part of
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

| Deferred | Requirement | Why |
| --- | --- | --- |
| Bless | R4.2, R4.4 | Needs a known-good baseline to test a rewriter against. Every expectation is correct right now, so there is nothing to bless and nothing to check a rewriter's output against. Build it once an expectation is genuinely wrong |
| `codes.toml` | R5.2 | Wants the explicit-`ErrorCode`-values compiler change first, which is sequencing step 3. The runner already parses `error.h` for name-to-number, which is the half that has to exist either way |
| Diff-driven selection | R2.5 | Needs `tags.toml`, which does not exist. Selection by group, scenario, check name and `tag:<phase>` is implemented, so the vocabulary a diff would map onto is in place |
| `warn`, `recover`, `driver` | R3.1, R3.8 | No scenarios exercise them. `driver` also needs an `argv` key that the `cases.toml` schema does not have, and adding schema without content to write against it is the wrong order |
| `xfail` | R3.8 | Same. A scenario setting `xfail` is a hard configuration error rather than a silently ignored key, so this cannot be forgotten |
| `ErrorCode` coverage report | R6.4 | Two groups is too small a sample to report a backlog against; it wants the tier 1 groups first |

Nothing deferred is silently ignored. A `cases.toml` naming a deferred category
reports the scenario as **skipped** with the reason; any other unimplemented key
fails discovery by name.

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
