# Test suite

Use this when a compiler change needs test coverage added or updated. It answers,
in order: which group to touch, which scenario in it, what to write, what to
assert, and how to update expectations.

Run the full suite before every merge.

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

**Split a stage into several files when any of these applies:**

- **Recovery interference.** The parser resynchronizes by skipping forward, so
  recovery from one error can swallow the construct holding the next. If two
  conditions cannot be spaced far enough apart to recover independently, separate
  them.
- **Mutually exclusive structure.** `ErrorNoEof` needs the file to end wrongly.
  There is one end of file, so it is one per file and it must be last.
- **Aborting diagnostics.** `errorExit` terminates immediately instead of
  accumulating — `ExitNF`, `ExitMem`, `ExitIndent`. Nothing after one of these
  runs, so it gets its own file and never shares with accumulating errors.
- **More than three to six diagnostics.** Past that, split by sub-family.

Name the split for what it covers — `core-parse-delimiters`, not `core-parse-1`.

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
| `warn` | Compiles | Exit 0, the named warnings present, no errors |
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

- `//~` applies to its own line; `//~^` to the line above, repeatable for
  further.
- Name the code symbolically. Never write the number.
- The column after `:` and any quoted substring are written by bless. You write
  the code name.
- Mark `follow-on` for a diagnostic that exists only as a consequence of another.
  Without it, an unrelated change to error recovery breaks the scenario.

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

## 5. Update expectations

Blessing records what the compiler actually produced as the new expectation. It
rewrites the tail of each `//~` annotation in place — column and message text —
leaving your code and your code names alone.

```bash
python test/run.py --bless core
```

Read the failures first, bless second, then review the diff. **Bless checks
nothing; the review is the safety mechanism.**

Bless refuses to change an exit status or absorb a crash. If it refuses,
something behavioral broke — do not work around it.

After a syntax change: rewrite sources, bless the whole suite, review. A correct
rewrite comes back green.

## Not doing

Deliberate exclusions, each with its reason. Reopen one on purpose, not by
accident — and update this list when you do.

- **No unit tests.** Everything reachable through the CLI; `--checktree` and
  `--verify` cover internal invariants and run on every `compile`.
- **No AST-dump assertions.** `--ir` output has no stability contract.
- **No WebAssembly tier** until there is a runtime to run against.
- **No performance or memory regression tests.**
- **No multi-module runtime scenarios** until separate compilation lands.
