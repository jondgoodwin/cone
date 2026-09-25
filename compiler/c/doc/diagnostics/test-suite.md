# Test suite

Use this when a compiler change needs test coverage added or updated. It answers,
in order: which group to touch, which scenario in it, what to write, what to
assert, and how to update expectations.

Run the full suite before every merge. `python test/run.py` takes a few seconds
and is the default for a reason.

## Principles — [derived]

**An unknown widens the run; it never narrows it.** An unmapped path, or a change
to something like `shared/error.h`, expands `--since` to the whole suite rather
than being skipped. ▸ **Forbids** a filter that can silently omit coverage —
the failure mode of every selective test runner, and the reason this one is safe
to use in the inner loop.

**A scenario asserts a code, so a diagnostic's identity is a test interface.** ▸
**This is why [Error Codes](error-codes.md) forbids reusing a code for an
unrelated condition**: two conditions sharing one cannot be told apart by any
test, whatever their messages say.

**Where a rule is unenforced, a scenario establishes the opposite** — a
violation that compiles clean, pinned deliberately. ▸ **Settles** how to read a
scenario that starts failing: it may be one a fix correctly invalidated, not a
regression. [Safety](../../../../doc/design/safety.md) and [Flow Analysis](../phases/flow.md)
both depend on this convention.

**While working, `--since` narrows it.** It reads the changed paths from git, maps
them through `test/tags.toml`, prints which paths chose which tags and why, and
runs that. An unmapped path widens to the whole suite rather than being skipped,
and so does a change to something like `shared/error.h` that invalidates every
expectation at once.

**It is an inner-loop filter, not a substitute for the full run**, and the reason
is worth understanding. A scenario's tags say what it is *about*, not every phase
it passes through — `typemgmt_success` is tagged `typecheck, genllvm, runtime`
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
| 0 | `lexical` | Literals, identifiers, comments, operators as tokens, statement termination |
| 0 | `core` | Operators and expressions, blocks and statements, functions and function overload, `if`, `while`/`break`/`continue`, number types, void, tuples, local and global vars |
| 1 | `struct` | Structs, method definition, operator methods, initializers and finalizers, delegated inheritance |
| 1 | `enum` | Enums in both size varieties, `Option`, `Result`, pattern matching |
| 1 | `array` | Arrays |
| 1 | `closure` | Closures |
| 1 | `ref` | Borrowed references, static permissions, lifetime annotations, function references |
| 1 | `move` | Move types and semantics |
| 2 | `region` | Owning references, region strategies, lock permissions, weak references |
| 2 | `trait` | Traits, virtual references and the dispatch through them, closure references |
| 2 | `collection` | Collection types, array references and slices |
| 2 | `each` | `each` and iteration |
| 2 | `typemgmt` | Conversion and coercion, typedef and extend |
| 2 | `generic` | Generics, macros |
| 2 | `module` | Modules, namespaces, `import`/`extern` |
| 2 | `exception` | Exception handling |
| 2 | `concurrency`, `safety`, `meta` | Concurrency; trust and raw pointers; metaprogramming |

One row is reserved rather than built. **`stream` will own the `<-` append
operator** and whatever iteration protocol arrives with collections. `<-` is
implemented today and reachable only through stdio's `IOStream`, but it is
intended for collections and `corelib/` has none, so covering it now would pin
the operator to the one consumer it is not for. The group appears with the
collections.

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
output directory. A name is written with underscores, never hyphens, so that a
scenario's file name is also the name of the module it compiles to:

```
test/cases/core/
  cases.toml                    every scenario: category, tags, runs, file-level expectations
  core_success.cone             the group's success program
  core_success.out              expected stdout, for a run scenario
  core_parse_delimiters.cone    parse-stage rejects, expectations annotated inline
  core_parse_decls.cone
  core_typecheck.cone
```

A group holds one success program plus one failure scenario per compiler stage it
needs. Not every group needs every stage.

`cases.toml` lists every scenario and every support module — a file imported by a
scenario and never compiled on its own. A `.cone` file that is neither is an
error, so a forgotten registration fails loudly instead of sitting unrun.

**Extend the success program before adding a file.** Split it only at a natural
seam — one parse error zeroes a whole file's coverage.

### A scenario that is a folder

A module is the files of a folder, so some scenarios are folders. **A name the
suite uses reaches either `<name>.cone` or `<name>/<name>.cone`** — the designated
file of a folder named for it. That is the compiler's own lookup, so a name means
the same thing to the runner and to `conec`, and it holds for a scenario and for a
support module alike:

```
test/cases/module/
  module_folder_sweep/          a folder scenario
    module_folder_sweep.cone      its designated file: what the runner compiles
    module_folder_sweep.out       expected stdout, beside its source as always
    sibling.cone                  swept in by the compiler
    deep/nested.cone              and at any depth
  module_submodule/             a folder scenario holding a module TREE
    module_submodule.cone         the root module
    geometry/geometry.cone        a submodule of it, drawn by its own name
    geometry/scaling/scaling.cone and a submodule of that
  modfolder/                    a folder support module, imported by a scenario
    modfolder.cone
    helper.cone
```

- **The designated file is what the runner compiles**, and the compiler finds the
  rest. Output filenames and the `.out` file derive from it exactly as from a flat
  scenario's source.
- **`cases.toml` registers the folder under its own name** — as a scenario table,
  or in `support` — and registers nothing inside it. The sweep is what finds those
  files; listing them would restate the thing under test. A folder in a group
  directory that is registered under neither is an error, as a stray `.cone` file
  is, and so is a folder with no designated file.
- **Both spellings of one name is an error.** The compiler takes `<name>.cone` and
  never looks in the folder, so the folder's files would sit unread while looking
  like coverage.
- **An annotation may live in any file of the folder**, for the reason it may live
  in a support module: a diagnostic carries the path of the file it was reported
  against, and an annotation matches only a diagnostic reported against its own
  file. A support module that is a folder contributes every file of it the same
  way.
- **A folder scenario may be a `run` scenario**, which a scenario spanning an
  `import` of a LOADED module may not: what a folder holds, module tree and all,
  this compile *defines*, so it links like any single-file program. A module an
  `import` reached by a path is declared and never generated, which is what stops
  those from running — while an import of a SISTER loads nothing and links like
  the rest of the tree, which `module_import_sister` runs.
- **A subfolder holding its own designated file is a submodule**, not more of the
  scenario's own files, and so is a file whose first statement is `mod` — a
  scenario file that opens with `mod` is a one-file module, not one of the
  scenario's files. So a folder scenario may hold a whole module tree. It is
  registered the same way — under the scenario folder's name, with nothing inside
  it listed — and an annotation in a submodule's file works exactly as one in a
  swept file does.

**A folder scenario may instead hold a build description**, `<name>/<name>.conebuild`,
and no designated file — both is an error. The runner hands `conec` the
description, which lists the package's files and modules itself, so nothing in
the folder is swept: a file it does not list is not compiled. Output filenames
and the `.out` file derive from the description's name as from any source.
Annotations live in the description, which the compiler reads with the Cone
lexer and so reports against, and in every `.cone` file beneath the folder — the
ones it lists and the ones its import lines name. `module_build_lib` and
`module_build_exe` are the pair: a library and a program.

**Write a folder scenario when the file layout is the subject** — which files a
module holds, what names them, what collides, what a subfolder draws. Anything
else belongs in a flat scenario, because a folder costs a reader a directory
listing before they can see what the case says.

**Never mix compiler stages in one failure scenario.** Analysis halts between
phases: parse errors skip semantic analysis, name-resolution errors return before
type checking. A type-check expectation sharing a file with a parse error never
runs, and the scenario silently covers half of what it claims. Same-stage errors
accumulate, so one file carries several distinct codes of one stage.

**Both error gates within type check are per declaration**, so neither
constrains how a file is written:

- **`fndcl.c` skips a function's body when that function's own signature did
  not type check.** The skip is local to that function; every other declaration
  in the file is analyzed whole.
- **`fndcl.c` runs `blockFlow` on a function whose own signature and body type
  checked**, whatever failed elsewhere.

So flow diagnostics — `ErrorMove`, the lifetime checks in `assign.c` and
`return.c`, and `ErrorNoMut` on an assignment, which is a flow diagnostic and not
a type-check one despite appearances — may appear in several functions of one
file, and after an earlier declaration has failed at either stage.
`core_flow_gate` pins both gates, and is the one file in the corpus that
deliberately mixes a signature failure, a body failure and flow diagnostics.

Several flow scenarios are one function each, and `closure_typecheck_sig` is
split from `closure_typecheck_use` and `closure_typecheck_call`. Those splits
are simplifications rather than requirements.

**One gate is global**: name resolution returns before type checking begins if it
reported anything (`conec.c`). A file whose subject is a type-check or flow
diagnostic cannot contain a name-resolution error.

If a scenario reports fewer diagnostics than it should and the missing ones are
all late in the file, check that gate first.

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
  accumulating — `ExitNF`, `ExitMem`. Nothing after one of these
  runs, so it gets its own file and never shares with accumulating errors.
- **One diagnostic's whole story.** Where a single `ErrorCode` covers several
  distinct causes with different remedies, the cases belong side by side, so that
  a reader can check each cause names the right advice and a change that reworded
  one is visible against the others. `struct_typecheck_nosize` is that file for
  `ErrorNoSize` and its five causes.

**Length is not a reason to split.** Ten scenarios carry more than six
diagnostics and one carries fifteen, so any threshold would describe nothing the
corpus does. Split for one of the reasons above, or because the file stops being
about one thing; never for a count.

**A scenario whose subject is a crash guard is not a reason to split either**,
though it costs something worth writing down in the file. If that guard
regresses, the scenario does not fail an assertion — the process dies and
every expectation in the file is lost with it, including coverage of unrelated
things. `generic_typecheck_macro` carries both arity and non-termination and says
so at the top. Accepting that cost is the ordinary choice; a separate file buys
only the isolation.

Name the split for what it covers — `core_parse_delimiters`, not `core_parse_1`.

### What cannot be a scenario at all

**A construct that crashes the compiler.** An access violation fails no
assertion, so it cannot even be `xfail`. Exclude it with a written reason in its
group's `cases.toml` rather than leaving it to be rediscovered — and file the
crash, because the exclusion is a placeholder for a fix and not a verdict. The
corpus currently excludes no scenario for a crash.

**A construct that silently parses as something else.** `xfail` asserts that a
case fails; a construct the lexer or parser quietly reads as something valid
fails nothing. Exclude it and say why.

**A construct that hangs.** The runner's timeout survives it, but a scenario
whose assertion is "this takes twenty seconds" asserts nothing worth having.

## 3. Write the source

**Braces and semicolons.** A block is delimited by braces and every statement
that does not end in a block ends with `;`, the last one before a `}`
included. The language has no other way to write either, so a scenario that
omits a `;` is asserting `ErrorNoSemi`.

**The header.** A file the compiler builds as a module — a flat scenario, a
support module, a folder's designated file, a one-file module — opens, after its
comments, with its `mod` line naming the file or the folder, and its imports
come right after that. A scenario that leaves either out is asserting
`ErrorNoModDcl` or `ErrorImportLate`. The other files of a folder module carry
no `mod` line, since one would make the file a module of its own, and so no
import either: every import of the module is in its designated file.

**Use no construct you are not testing.** Simplest syntax that exercises the
feature. Incidental scaffolding is what a language change has to be dragged
through.

**Name a scenario for what breaks when it fails.**

Runtime programs print one `name = value` line per fact established, through
`import stdio`. `stdio`, like `core`, is a package: a folder module in the
repository's `packages/` folder, which a `conec` built by CMake finds by default,
with no option and no setup. The runner passes nothing to find it. A run that
needs another package of the same name adds a `--path` folder in its `options`,
which is searched ahead of `packages/` (`module_package_path`); the
`CONE_PACKAGES` environment variable, which replaces `packages/` itself, is not
something a scenario can set.

A runnable program spans **modules** only where they are compiled into its one
object — the files of one folder, a module tree, a package the search path
finds (so `stdio` links) — or where it is **linked with a package compiled on
its own**. That is what a scenario's `link` key is for: each build description
or source it names, relative to the scenario's folder, is compiled first with
the scenario's options, to an object of its own in the same output folder, and
linked in beside the program's object. A diagnostic or a failing compile there
fails the scenario. It belongs to a folder `run` scenario only, and a linked
source may not share the scenario's own basename, since its object would take
the same name (`module_build_link`). A named check with an `object` key reads
what a linked compile generated (section 4, "`cases.toml` keys").

**The round trip** of a package's include file is the `include` key beside
`link`. A linked package compiled as a library writes its include file,
`<package>.cone`, beside its object; each golden file `include` names — named
for its package, `include/q.cone` for `q/q.conebuild` — must be that file byte
for byte, checked as soon as the package is compiled and before anything else
is, and the scenario's build descriptions name the golden file where they import
the package. So the program, and any later package, compiles against exactly
what the package generated, and a change to the generator shows as a diff of
the golden file. `--bless` records a generated file that differs as its golden
file, and nothing else in that pass (`module_include_roundtrip`).

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

**An annotation may live in a support module.** A diagnostic carries the path of
the file the offending node came from, and an annotation matches only a
diagnostic reported against its own file — so a diagnostic the compiler reports
inside an imported file is annotated *there*, not in the scenario that pulls it
in. The runner works out which support modules a scenario reaches, following
`import` transitively, and reads their annotations as
part of that scenario's expectations. Bless rewrites them in place like any
other, and a failure names the file so it is clear where the expectation lives.

Two consequences worth knowing. A support module carrying annotations imposes
them on **every** scenario that imports it, which is the honest reading: if two
scenarios both pull in a module that fails, both must say so. And a scenario
whose annotations are all in a support module still has none of its own —
`module_typecheck_provenance` is the case, and its whole assertion is which file
gets named.

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

**Assert what the program prints.** Use the generated artifacts only for what
runtime cannot see: whether a symbol was emitted at all and with what linkage
(`symbols`), what a vtable slot points at (`llvmir`), how a type was laid out
or what instruction flow analysis injected (`preir`), whether an imported
overload name's candidates were each declared (`symbols`).

**Pin the fragment that identifies a diagnostic, not the sentence.**

### Generated-artifact assertions: named checks

An assertion against LLVM IR, the symbols it declares, or a run's stdout has no
source line to attach to. Write it as a named check in `cases.toml`:

```toml
[[scenario.struct_methods.check]]
name = "methods-lower-to-concrete-symbols"
target = "symbols"
contains = ["define internal Point.addValue comdat nodeduplicate"]
excludes = ["Point.add "]
```

The name is what failure output reports and what selection matches.

A bug fix lands with a scenario that fails without the fix.

#### `llvmir`

Matches against the **post-optimization** dump, `<name>.ir` — the IR that
reaches the object file. Use it for what must survive the optimizer: a vtable
slot that holds a bitcast rather than `null`, a `%"Meter:Vtable" = type`, the
`$main = comdat` line. **A program's definitions are all internal, so only what
`main` reaches survives**: the optimizer deletes an internal definition nothing
references and folds the rest into `main`, which leaves a `compile` scenario's
dump empty of functions and a `run` scenario's holding little but `main`, its
globals and its constants. A check on anything else belongs on `preir`.
**Never write an encoded symbol's bytes into it** — `@_CNvNt2Pt3get` is a
`symbols` assertion, below, and the reader of a check is owed `Pt.get`. Bare
names (`@main`, a root `fn`, a C name) are their own spelling and may appear
where the check is about a `$name = comdat` line or a call.

#### `preir`

Matches against the **pre-optimization** dump, `<name>.preir` — what
generation wrote, before the optimizer deletes or folds it. Use it for an
instruction, a type or a signature that is the claim: the `icmp` a slice index
emits against its runtime count, the `add i64 %6, 12` flow analysis injects for
a fill literal, `%Node = type { i64, %Node* }`, `@read(i32**`. Register numbers
are the pre-optimization ones — allocas and loads are still there — and are
read out of the dump rather than chosen. The same rule about symbol bytes
applies.

#### `symbols`

Matches against one line per global symbol, derived from the
**pre-optimization** dump, `<name>.preir`. A symbol's spelling and linkage are
generation's facts, so they are read where generation wrote them: the optimizer
deletes an internal definition nothing references, and a check on an uncalled
definition — a generic type's method instance nobody used, say — must still see
it. The runner writes the derived lines beside the dump as `<name>.symbols`, so
a failing check can be read against them.

Each line is, in order, separated by single spaces:

1. the **kind**: `define` or `declare` for a function; `global` or `constant`
   for a variable defined here, `external global` or `external constant` for
   one merely declared;
2. the **linkage, visibility, storage-class and calling-convention words**
   exactly as LLVM prints them, in LLVM's order, and only those it prints —
   so nothing for `external` linkage or `default` visibility: `internal`,
   `dllimport x86_stdcallcc`; a program compiled with no build description
   sets no visibility and asks nothing to merge, so `hidden` and `linkonce` are
   what an `excludes` guards against there. A library's export is external and
   so prints no word — `define q.addOne comdat nodeduplicate` — which is what
   tells it apart from the `define internal` a program gives the same
   definition. In a described build a generic's instance and a vtable are
   `linkonce_odr` — `define linkonce_odr q.larger[i64] comdat any`;
3. the **demangled name**: the symbol read back through the scheme in
   `doc/design/names-and-namespaces.md`, "Symbols" — `sub.SubPt.get`,
   `Holder[i64].tally`, `pick[&so mut i32]`, `Vec.+`, a vtable as
   `Gauge as Meter (vtable)`, a vtable list as `Meter (vtable list)`, a name
   Cone source could only write in backticks in its backticks. A symbol the
   scheme does not spell — `main`, a root `fn`, a C name, `string`, `anon` — is
   its own reading, and LLVM's `.1` uniquifier is kept on whatever it landed on;
4. `comdat <kind>` — `comdat nodeduplicate` or `comdat any` — when the symbol
   leads a COMDAT, which every definition does on the default triple.

```
define main comdat nodeduplicate
define internal Pt._hid comdat nodeduplicate
define internal Holder[i64].tally comdat nodeduplicate
declare sub.subFn
declare dllimport x86_stdcallcc GetTickCount
global internal _privGlobal comdat nodeduplicate
external global sub.subGlobal
constant internal Gauge as Meter (vtable) comdat nodeduplicate
constant internal Meter (vtable list) comdat nodeduplicate
```

Since a definition's name is followed by its COMDAT and a declaration's ends
the line, an `excludes` for a name that is a prefix of another — `Pair.sum`
beside `Pair.sumValue` — writes the trailing space or newline:
`"Pair.sum "`, `"modulesub.scale\n"`.

The demangler lives in `test/run.py`, and every run begins by reading the
scheme's worked examples through it (`--selftest` does only that). A grammar
change that broke a reading therefore stops the run as one fault, and the
scenarios then check conec's encoder against the demangler on the symbols it
actually emits; `struct_methods` is where a punycoded name does so.

### `cases.toml` keys

One table per scenario, keyed by the source's basename — or by the folder's name,
for a folder scenario — plus a `support` list of modules that are imported and
never compiled on their own, each of which may likewise be a file or a folder.

```toml
support = []

[scenario.core_overload]
category    = "run"          # required; one of the six categories
description = "..."          # one line, for failure output
tags        = ["typecheck", "genllvm", "runtime"]
diagnostics = 0              # total count; required for 'recover'
exit        = 0              # only where it is not the category's default
xfail       = false          # omit unless true

[scenario.module_build_link]
category    = "run"
link        = ["q/q.conebuild"]  # compiled alone first, and linked in (section 3)
include     = ["include/q.cone"] # what q's compile must generate, which the program compiles against

[scenario.driver_bad_option]
category    = "driver"       # a driver scenario has no .cone file
argv        = ["--bogus"]    # the whole invocation; nothing is appended
exit        = 4              # required: asserting it is the whole category

[[scenario.core_overload.run]]   # omit entirely for a single default run
name    = "debug"                # declare all of them once you declare any
options = ["--debug"]

[[scenario.core_overload.unlocated]]   # diagnostics errorMsg prints with no line,
code    = "ErrorNoLoop"                # or located in a file the compile wrote under
message = "may not be used as an expression"  # the output folder, which has no source to annotate

[[scenario.core_overload.check]]
name     = "overload-lowers-to-concrete"
target   = "symbols"           # or "llvmir", "preir", or "stdout" for a 'run' scenario
contains = ["define internal scaleInt comdat nodeduplicate"]
excludes = ["scale "]          # a definition's name is followed by its COMDAT

[[scenario.module_build_link.check]]
name     = "the-library-shares-its-own-instances"
target   = "symbols"
object   = "q"                 # read what a 'link' entry's compile generated
contains = ["define linkonce_odr q.larger[i64] comdat any"]
```

`object` names a `link` entry by its stem, and the check reads that package's
dump — `q.preir`, from `q/q.conebuild` — in place of the scenario's own; the
runner then compiles that entry with `--llvmir`. It is how a scenario that
links two objects asserts on both: that a symbol both define has the same
linkage in each, say.

**Several runs of one source** is how an option matrix avoids duplicating a
`.cone` file. Every run is compared against the same expectations, so what a
second run buys is the assertion that those expectations do not depend on the
option. `core_success` declares `release` and `debug`, the second passing
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
- **Multi-object runtime scenarios only through `link`**, which compiles each
  package alone and links its object in (section 3). Nothing else of separate
  compilation is exercised: there is no Congo run in the suite. Congo is
  checked by its own script, `tools/congo/test_congo.py`, which builds and runs
  programs against the packages folder's `core` and `stdio`, each compiled
  alone.
