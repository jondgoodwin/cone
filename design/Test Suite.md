# Test suite

Use this when a compiler change needs test coverage added or updated. It answers,
in order: which group to touch, which scenario in it, what to write, what to
assert, and how to update expectations.

Run the full suite before every merge. `python test/run.py` takes a few seconds
and is the default for a reason.

**While working, `--since` narrows it.** It reads the changed paths from git, maps
them through `test/tags.toml`, prints which paths chose which tags and why, and
runs that. An unmapped path widens to the whole suite rather than being skipped,
and so does a change to something like `shared/error.h` that invalidates every
expectation at once.

**It is an inner-loop filter, not a substitute for the full run**, and the reason
is worth understanding. A scenario's tags say what it is *about*, not every phase
it passes through — `typemgmt-success` is tagged `typecheck, genllvm, runtime`
even though its source obviously goes through the parser. So a parser change
selects the scenarios written to exercise parsing, not every scenario a parser
change could conceivably break. Widening the map until it could would select
almost everything and buy nothing. The full run before merge is what closes that
gap; keep it.

## 1. Pick the group

Coverage is organized into groups following the reference manual. **A construct
is covered in depth in exactly one group.** Elsewhere it appears only as deeply
as that group's own subject requires.

| Tier | Group | Owns |
| --- | --- | --- |
| 0 | `lexical` | Literals, identifiers, comments, operators as tokens, statement inference |
| 0 | `core` | Operators and expressions, blocks and statements, functions and function overload, `if`, `while`/`break`/`continue`, number types and enum, void, tuples, local and global vars |
| 1 | `struct` | Structs, method definition, operator methods, initializers and finalizers, delegated inheritance |
| 1 | `union` | Unions, `Option`, `Result`, pattern matching |
| 1 | `array` | Arrays |
| 1 | `closure` | Closures |
| 1 | `ref` | Borrowed references, static permissions, lifetime annotations, function references |
| 1 | `move` | Move types and semantics |
| 2 | `region` | Owning references, region strategies, lock permissions, weak references |
| 2 | `trait` | Traits, trait-based variants, virtual references, closure references |
| 2 | `collection` | Collection types, array references and slices |
| 2 | `each` | `each` and iteration |
| 2 | `typemgmt` | Conversion and coercion, typedef and extend |
| 2 | `generic` | Generics, macros |
| 2 | `module` | Modules, namespaces, `include`/`import`/`extern` |
| 2 | `exception` | Exception handling |
| 2 | `concurrency`, `safety`, `meta` | Concurrency; trust and raw pointers; metaprogramming |

One row is reserved rather than built. **`stream` will own the `<-` append
operator** and whatever iteration protocol arrives with collections. `<-` is
implemented today and reachable only through stdio's `IOStream`, but it is
intended for collections and `corelib/` has none, so covering it now would pin
the operator to the one consumer it is not for. The group appears with the
collections; see [[Collection - Streams & Iteration]].

Three of the rows above have no group directory, and the reasons differ. `exception` and
`concurrency` are unimplemented down to the keyword table — real chapters, real
features, nothing to test yet; their rows stay and the groups appear when the
features do. `meta` is different: the `#` meta-language of `refmeta.html` is
unimplemented down to the token, and the only metaprogramming that *is*
implemented — `macro`, and all of `ir/meta/` — belongs to `generic` by the row
above, which the manual agrees with by nesting `refmacro.html` under
`refgeneric.html`. **The `meta` row is reserved for `refmeta.html`'s `#`
language.** Do not resolve the apparent overlap by moving macros out of
`generic`.

`driver` is a group with no row here at all, and deliberately so: it tests the
compiler's command line rather than the language, so it follows no chapter. It is
the only such exception.

Tier 0 spans several manual chapters because operators, functions and control
flow cannot be tested apart. Tiers 1 and 2 test only their delta over the tiers
below.

**Choosing:** put the change where the manual puts the feature. If a change
affects several groups, the group that owns the feature gets the depth; the
others get only what their own subject needs to exercise.

**Interaction coverage is not replication.** Do not re-test a construct for its
own sake outside its home group. Do test its interaction with your group's
subject — compilers break at intersections. `struct` must not verify `while`
semantics, but must have a struct mutated in a loop and returned from a branch.

A feature with no group means a new group and a new manual chapter. Say so rather
than filing it somewhere convenient.

## 2. Pick the scenario

Each group is a directory. Scenario files carry the group name as a prefix, so
they stay unambiguous when opened by name or compiled by hand into a shared
output directory:

```
test/cases/core/
  cases.toml                    every scenario: category, tags, runs, file-level expectations
  core-success.cone             the group's success program
  core-success.out              expected stdout, for a run scenario
  core-parse-delimiters.cone    parse-stage rejects, expectations annotated inline
  core-parse-decls.cone
  core-typecheck.cone
```

A group holds one success program plus one failure scenario per compiler stage it
needs. Not every group needs every stage.

`cases.toml` lists every scenario and every support module — a file imported by a
scenario and never compiled on its own. A `.cone` file that is neither is an
error, so a forgotten registration fails loudly instead of sitting unrun.

**Extend the success program before adding a file.** Split it only at a natural
seam — one parse error zeroes a whole file's coverage.

**Never mix compiler stages in one failure scenario.** Analysis halts between
phases: parse errors skip semantic analysis, name-resolution errors return before
type checking. A type-check expectation sharing a file with a parse error never
runs, and the scenario silently covers half of what it claims. Same-stage errors
accumulate, so one file carries several distinct codes of one stage.

**Same-stage is not always enough**, because one pass still gates itself on the
global error count rather than on its own:

- **`module.c` type checks a declaration's *body* only if `errors == 0`.** Every
  signature in the module is type-checked first, so a signature-phase diagnostic
  silently suppresses every body-phase diagnostic in the same file — both of them
  type-check stage.

**`fndcl.c` no longer does.** `fnDclTypeCheck` compares the error count against
the one it saw on entry, so data-flow analysis runs for any function whose own
signature and body type checked, whatever failed elsewhere. Flow diagnostics —
`ErrorMove`, the lifetime checks in `assign.c` and `return.c`, and `ErrorNoMut`
on an assignment, which is a flow diagnostic and not a type-check one despite
appearances — may now appear in several functions of one file, and after an
earlier function has failed. `core-flow-gate` is the scenario that pins this.

Existing flow scenarios were written under the old rule and are still one
function each; that is now a simplification rather than a requirement. The
module-level gate above is the reason a flow file still cannot open with a
signature error.

Four groups hit these while being written. If a scenario reports fewer
diagnostics than it should and the missing ones are all late in the file, this is
why.

**Split a stage into several files when any of these applies:**

- **Recovery interference.** The parser resynchronizes by skipping forward, so
  recovery from one error can swallow the construct holding the next. If two
  conditions cannot be spaced far enough apart to recover independently, separate
  them. Known offenders: `ErrorNoRParen` and "Unknown struct statement" each eat
  the following declaration, and the parser abandons an entire `match` at the
  first arm that is neither `case` nor `else`. Each must be last in its file.
- **Mutually exclusive structure.** `ErrorNoEof` needs the file to end wrongly.
  There is one end of file, so it is one per file and it must be last.
- **Aborting diagnostics.** `errorExit` terminates immediately instead of
  accumulating — `ExitNF`, `ExitMem`, `ExitIndent`. Nothing after one of these
  runs, so it gets its own file and never shares with accumulating errors.
- **More than three to six diagnostics.** Past that, split by sub-family.

Name the split for what it covers — `core-parse-delimiters`, not `core-parse-1`.

### What cannot be a scenario at all

**A construct that crashes the compiler.** An access violation fails no
assertion, so it cannot even be `xfail`. Exclude it with a written reason in its
group's `cases.toml` rather than leaving it to be rediscovered — and file the
crash, because the exclusion is a placeholder for a fix and not a verdict. The
last set of these is in `workitems/done/`, under [[Diagnose instead of crash]];
the corpus currently has none.

**A construct that silently parses as something else.** `xfail` asserts that a
case fails; a construct the lexer or parser quietly reads as something valid
fails nothing. Exclude it and say why.

**A construct that hangs.** The runner's timeout survives it, but a scenario
whose assertion is "this takes twenty seconds" asserts nothing worth having.

## 3. Write the source

**Braces only.** Never rely on indentation or `:` for block structure — the
language is moving to free-form only.

**Use no construct you are not testing.** Simplest syntax that exercises the
feature. Incidental scaffolding is what a language change has to be dragged
through.

**Name a scenario for what breaks when it fails.**

Runtime programs print one `name = value` line per fact established.

A runnable program cannot span modules until separate compilation lands, so every
`run` scenario is a single file.

## 4. Assert

Every scenario declares a **category** in `cases.toml`. It tells the runner what
to do with the source and what to assert.

Never hand-assert what a category already covers.

| Category | Runner does | Runner asserts |
| --- | --- | --- |
| `compile` | Compiles | Exit 0, no diagnostics, zero warnings, object emitted |
| `run` | Compiles, links against `conestd`, executes | The above, plus stdout matches the `.out` file |
| `warn` | Compiles | Exit 0, every annotated warning matched, no unannotated ones, no errors |
| `reject` | Compiles | Exit exactly 1, every annotated diagnostic matched by code and location, and no unannotated ones |
| `recover` | Compiles | Exit exactly 1, the expected diagnostic count, no crash and no hang |
| `driver` | Invokes `conec` without valid Cone source | The exact exit code — bad option, missing file, `--version` |

`compile` is for valid code that cannot run — no `main`, library-shaped, or no
codegen path yet. Never add a synthetic `main` to satisfy a category.

Mark anything not yet implemented `xfail`: it reports as an expected failure and
**fails the suite if it starts passing**.

### Located diagnostics: annotate the line

```cone
fn scale(v f32) f32 { v * 2. }

fn main() {
  imm a = scale(1, 2)      //~ ErrorManyArgs:17
  imm b = add(pt, 3)       //~ ErrorNoCandidate:11 "accepts the call's arguments"
                           //~^ ErrorInvType:11 follow-on
}
```

- `//~` applies to its own line; `//~^` to the line above, `//~^^` two lines
  above, and so on. Carets count lines, and an annotation-only line is a line, so
  successive annotations for one code line each take one more caret.
- Name the code symbolically. Never write the number.
- The column after `:` and any quoted substring are written by bless. You write
  the code name — and you decide whether either field is there at all, since
  bless corrects the fields you wrote and never adds one you left off.
- Mark `follow-on` for a diagnostic that exists only as a consequence of another.
  Without it, an unrelated change to error recovery breaks the scenario.

Two constraints follow from annotations living on lines:

- **Where two diagnostics share a code, a line and a column**, the quoted
  substring is the only thing that tells them apart, so it is required rather
  than decorative. The lexer does this: a bad hex digit reports both the escape
  and the literal it could not finish, at one position.
- **A diagnostic reported at end-of-file has no line to carry it.** Some are
  reported at the token that should have followed, which is the next
  declaration's first token — so arrange for that declaration to exist rather
  than ending the file there. Where the position really is the end of file, it is
  a file-level expectation.

### File-level expectations: put them in `cases.toml`

Some facts belong to the file rather than a line:

- **Diagnostics with no location.** `ErrorNoLoop` and the `ErrorGenErr` family
  are raised through `errorMsg`, which prints no source line. There is nothing to
  annotate.
- **Total diagnostic count**, which `recover` asserts.
- **Expected exit status**, where it is not the category's default.

### Two choices, for lowering and codegen defects

**Assert what the program prints.** Use LLVM IR only for what runtime cannot see:
whether a symbol was emitted at all, what a vtable slot points at, whether a
private candidate reachable through a public overload name got a `declare`.
Symbol names change when separate compilation lands.

**Pin the fragment that identifies a diagnostic, not the sentence.**

### Generated-artifact assertions: named checks

An assertion against LLVM IR or a run's stdout has no source line to attach to.
Write it as a named check in `cases.toml`:

```toml
[[scenario.core-success.check]]
name = "overload-lowers-to-concrete"
target = "llvmir"
contains = ["@Point_addValue", "@Point_addRef"]
excludes = ["@Point_add"]
```

The name is what failure output reports and what selection matches.

A bug fix lands with a scenario that fails without the fix.

### `cases.toml` keys

One table per scenario, keyed by the source's basename, plus a `support` list of
modules that are imported and never compiled on their own.

```toml
support = []

[scenario.core-overload]
category    = "run"          # required; one of the six categories
description = "..."          # one line, for failure output
tags        = ["typecheck", "genllvm", "runtime"]
diagnostics = 0              # total count; required for 'recover'
exit        = 0              # only where it is not the category's default
xfail       = false          # omit unless true

[scenario.driver-bad-option]
category    = "driver"       # a driver scenario has no .cone file
argv        = ["--bogus"]    # the whole invocation; nothing is appended
exit        = 4              # required: asserting it is the whole category

[[scenario.core-overload.run]]   # omit entirely for a single default run
name    = "debug"                # declare all of them once you declare any
options = ["--debug"]

[[scenario.core-overload.unlocated]]   # diagnostics errorMsg prints with no line
code    = "ErrorNoLoop"
message = "may not be used as an expression"

[[scenario.core-overload.check]]
name     = "overload-lowers-to-concrete"
target   = "llvmir"
contains = ["@scaleInt"]
excludes = ["@scale("]
```

**Several runs of one source** is how an option matrix avoids duplicating a
`.cone` file. Every run is compared against the same expectations, so what a
second run buys is the assertion that those expectations do not depend on the
option. `core-success` declares `release` and `debug`, the second passing
`--debug` to turn optimization off — which is the only way the corpus can catch
code generation that is wrong in a way the optimizer happens to repair. Declaring
one run means declaring all of them; the default run disappears.

`argv` belongs to a `driver` scenario and to nothing else. Such a scenario is the
one kind with no `.cone` file, so the rule that a listed scenario without a
source is an error does not reach it, and it is the one kind whose group —
`driver` — follows no manual chapter, because the command line is not a language
feature.

The group directory supplies the feature tag, so `tags` carries only pipeline
phases. A scenario with no annotations and no checks still needs its table: a
`.cone` file that is neither a listed scenario nor a listed support module is an
error, which is what keeps a forgotten registration from sitting unrun.

## 5. Update expectations

Blessing records what the compiler actually produced as the new expectation.

```bash
python test/run.py --bless core
```

It rewrites exactly two things: the tail of each `//~` annotation in place —
column and quoted substring — and the `.out` file of a `run` scenario. Your
source, your code names, your caret runs, your `follow-on` flags and your
`cases.toml` are left alone, and so is any field that is still right, so the
diff is the size of the change. A substring it does have to rewrite becomes the
whole message; trim it back to the fragment that identifies the diagnostic if
you want one.

Read the failures first, bless second, then review the diff. **Bless checks
nothing; the review is the safety mechanism.**

Bless never adds an annotation and never deletes one. It cannot know the code
name of a diagnostic that appeared, and an annotation nothing produced may be a
regression rather than a stale expectation, so it reports both and writes
neither. It never adjusts a `diagnostics` count either: a changed set of
diagnostics is a decision, not a bless.

It refuses a scenario outright — recording nothing in it, while the rest of the
suite still blesses — in four cases:

- **The exit status changed.** Something behavioral broke; do not work around it.
- **The run crashed**, timed out, or was killed by the output budget.
- **Two annotations on one line share a code and cannot be told apart.** Bless
  pairs an annotation with a diagnostic by code and line, and falls back on the
  quoted substring when a line carries two of one code. Where that still leaves
  a choice it refuses rather than guess, and names the group. Give the
  annotations substrings that distinguish them.
- **The scenario is marked `xfail`.** Its expectations record a defect rather
  than claim to be current.

After a syntax change: rewrite sources, bless the whole suite, review. A correct
rewrite comes back green.

## Not doing

Deliberate exclusions, each with its reason. Reopen one on purpose, not by
accident — and update this list when you do.

- **No unit tests.** Everything reachable through the CLI; `--checktree` and
  `--verify` cover internal invariants. `--checktree` runs on every compile of
  any category, because what it looks for — a node an error path left with no
  type or no body — only appears where a diagnostic was reported. `--verify`
  runs on `compile` and `run`, where there is a generated module to verify.
- **No AST-dump assertions.** `--ir` output has no stability contract.
- **No WebAssembly tier** until there is a runtime to run against.
- **No performance or memory regression tests.**
- **No multi-module runtime scenarios** until separate compilation lands.
