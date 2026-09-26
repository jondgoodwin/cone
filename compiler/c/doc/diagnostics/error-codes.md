How the compiler says something is wrong: the code, the message, the position,
and the rules for adding one.

Read this before adding a diagnostic, and before reusing an existing code.

*Provenance: read from source, cross-checked against `test/codes.toml`.*

## Principles — [derived]

⚠ **Read from source and cross-checked against `test/codes.toml`.** **Unchecked
is the claim that these rule rather than describe.**

1. **A diagnostic gets its own code.** Do not reuse an unrelated one for a new
   condition. Lookup failures, visibility errors and no-match errors must stay
   distinguishable — a scenario asserts a code, so two conditions sharing one
   cannot be told apart by any test. ▸ **Forbids** the convenience of the nearest
   existing code, and **settles** that the test suite is the constraint: a code
   is an assertion target before it is a message.
2. **The numbers are a published interface.** The compiler prints the number, so
   a renumber invalidates every expectation at once and none of them loudly. ▸
   **Forbids** tidying the ranges. **Retirement leaves a hole**; holes are
   cheaper than renumbering.
3. **The cause goes in the message, not in a second code** — but only where the
   codes would be indistinguishable except by wording. Section 4 is where that
   line sits.

## The ranges

| Range | Kind | Counted as |
| --- | --- | --- |
| 0–99 | process exit codes | not a diagnostic |
| 1000–2999 | errors | `errors` |
| 3000+ | warnings, `WarnCode` upward | `warnings` |

`code < WarnCode` is the whole of the error/warning distinction. The two
counters are separate, which matters because **warnings never gate**: the flow
analysis gate and the two `fnDclTypeCheck` early returns all compare `errors`.

Exit codes are their own thing and they abort immediately rather than being
counted: `ExitError` (1), `ExitNF` (2, source file not found), `ExitOpts` (4),
`ExitGen` (6); 5 is a retired code and stays a hole. `ExitGen` covers both
unrecoverable internal failures: code generation that could not proceed, and a
compiler invariant that did not hold.

## Reporting

| Function | Position from | Use for |
| --- | --- | --- |
| `errorMsgLex` | the lexer's current token | the parser — this is its workhorse |
| `errorMsgLexAfter` | the end of the token before the lexer's current one | something that should have followed a token: the `;` ending a statement, reported at the end of the statement rather than on the next line's first token |
| `errorMsgNode` | a node's stored position, **plus the instantiation trace** | every phase after parsing |
| `errorMsg` | nothing | when there is no position to give, which should be rare |
| `errorExit` | prints and exits | only where continuing is impossible |
| `errorUnreachable` | a node's position, then exits `ExitGen` | a state the compiler had ruled out — never a bad program |

**`errorMsgNode` walks `instnode`** to print "as instantiated by…" outward from
the reported node, capped at `ErrorInstTraceMax` (4). Ordinary code nests one or
two deep; the cap exists so a runaway expansion does not bury the diagnostic
under hundreds of identical frames.

**A node built after parsing takes the lexer's current position**, which is end
of file. Call `inodeLexCopy` on anything injected, or the diagnostic points at
nothing. This is the single most common way a new diagnostic comes out wrong.

## Adding one

1. Add the code to `shared/error.h` **with an explicit value**, at the end of
   its block, in declaration order. **The next free number is the highest in use
   plus one, not the next number after the block you are writing in** — a block
   sits where it reads best, and its neighbours' numbers say nothing about what
   is taken.
2. Add the same name and number to `test/codes.toml`. The runner compares that
   table against the header **before any case runs** and fails naming exactly
   which codes moved. Regenerate with `python test/run.py --bless-codes` and
   read the diff rather than trusting it.
3. Add a scenario that provokes it, in the owning group. A new `ErrorCode` lands
   with the scenario that produces it — see [Test Suite](test-suite.md).

Explicit values in the header are what remove the renumber hazard at the source;
the pinned table is defence in depth, catching a code added without following
the header's convention.

**The runner also refuses two names for one number**, which is principle 1 made
mechanical. The pinned table compares names to numbers and so cannot see a
collision — both names are present and both agree with the header — but the
corpus matches a diagnostic on the number alone, so two conditions sharing one
are indistinguishable to every scenario that asserts either. Where a collision
has to be resolved, the later-added code takes the next free number and the
older one keeps what it published.

## When one code carries several causes

`ErrorNoSize` is the worked example, and the reasoning generalizes. A type can
fail to have a size five ways — `@opaque`, a trait or an `@unsized` enum, a
function signature, a struct with an unsized field, and a type still being laid
out. All five report `ErrorNoSize`, and the cause is named in the message.

**Five codes would be indistinguishable to everything except the wording**, and
the wording is what the author actually needs — each cause has a different
remedy. So the test is not "is this a different condition?" but "would a caller
ever branch on which?" If nothing would, one code and a specific message is
right.

`ErrorEnumExtends` is the same shape, over what an enum extending another may name
and declare: a base that is not an enum, a base that is itself, a discriminant of
its own, a member its copies of the base's variants could not be given — a
requirement, a common field, a macro or an `is` — and no variant at all. A generic
base written with the wrong number of arguments, none included, is not among them:
it is `ErrorArgCount`, the code every instantiation's arity wears. Nor is a member
declared under a name the base has: that is `ErrorExtendsOverride`, the code a
struct enrichment wears for the same mistake. One question — is this
relationship declared correctly — with the cause in the
message.

`ErrorBuildDesc` is a third: a build description that cannot be read, whether a
setting is unknown, set twice, badly valued or written after the module, a line
is malformed, a child module, an import or a package line is named twice, a
package line is written after the module, a module's import line names a
different file from the package line of the same name, or a module lists no
file. The description is written by a tool, and every cause has the one remedy
of fixing the line reported, so nothing would branch on which. What the
description says about the *source* is not among them: a `mod` line naming
another module is `ErrorBuildModName`, and an import it provides nothing for is
`ErrorBuildImport`, each its own condition with its own remedy. The first is kept
apart from `ErrorModName`, the same mistake against a folder's or a file's name,
because what is wrong differs — the description or the file — and so does who
fixes it.

`ErrorCAttr` and `ErrorBadExtern` are the same shape over the two C-interop
words. `ErrorCAttr` is a `@c` that is malformed (an argument other than a
string and `system`, an empty function symbol) or written where there is no one
symbol for it to name (a type, an anonymous, generic or `inline` function, a
trait's or a generic type's method), and the retired `extern system`; the one
remedy is to move or drop the marker. `ErrorBadExtern` is `extern` on what its
user needs the body of (an `inline` or generic function, a trait's or a generic
type's method) or on what is not a function or global; the remedy is to write
the body, or the declaration, without it. A `@c` that is well formed and says
nothing — a bare one on a function its C-named module already names — is kept
apart as `ErrorCNameTwice`: the marker is right in itself, and what is wrong is
that the module already said it.

One C name declared by two declarations of one compile is one symbol, and what
can be wrong with that takes two codes, because the remedies differ.
`ErrorCNameConflict` is two declarations that do not agree — functions of
different signatures or calling conventions, globals of different types or
permissions, a function and a global, or a C name the compiler already uses for
a symbol of its own — and the remedy is to make them say the same thing, C's
"conflicting types". `ErrorCNameDefTwice` is two that agree and both have a
body (or a value) the linker would see; the remedy is to delete one body. Both
are reported by generation, at the declaration generated second, naming the
first.

A module trait and a module's conformance to one take four codes, split where
the remedy is: the author of the `mod` line, of the trait, or of the module's
declarations. `ErrorModIs` is a `mod` line's `is` that does not name one module
trait by one name — a struct's trait, a module or anything else not a module
trait, a path, a list — or is written before `extends`; the remedy is on that
line. `ErrorModTraitBody` is a trait's member that is not a function or a
global, or is a generic function or declares an overload name; the remedy is in
the trait. What conformance finds is two conditions a reader would tell apart:
`ErrorModTraitMissing`, a requirement the module declares nothing for (declare
it), and `ErrorModTraitMismatch`, a declaration under a member's name without
the member's shape — not a function or not a global, another signature, type or
permission (change it). A name nothing binds is `ErrorUnkName`, a path through a
module trait `ErrorAbstractMeth` and `extends` naming one `ErrorModExtends`, each
the code the same mistake wears elsewhere, and `use` written before `is` is
`ErrorBadFold`, as it is before `extends`.

`ErrorImportLoop` is one code for every loop in the module order, whatever its
edges: two sisters importing each other, a ring of three, a child importing a
name of its parent (a loop through containment), a module importing one that
extends it, and a chain of `extends` alone. The remedy is always the same —
break the loop, usually by moving what the modules share into a sister they
all import — and the message names the modules round the loop and each step,
which is what tells the cases apart. A chain of `extends` alone is a loop like
the others, so it wears this code, and `ErrorModExtends` keeps what an
`extends` may not name. Its message has the
shape Congo's has for the same loop, so a direct `conec` run and a Congo build
say one thing.

A module's `init` and `final` take two codes, split where the remedy is.
`ErrorModLifecycle` is a declaration under either name that is not the one the
program's stitched init and final call — `fn @initpure init()` or
`fn final()`: one taking parameters, returning a value, generic, `inline` or
declaring an overload name, an `init` without `@initpure`, anything but a
function under either name at module scope — and a module that needs the `drop`
it is given while declaring a `drop` of its own; the remedy is on that
declaration, and the message names the cause. `ErrorGlobalUninit` is a global
declared without a value that no `init` of its module assigns — its module
declares none, or its `init` never does; the remedy is to assign it there, or to
give it a value. What `init` does wrong with such a global wears the code the
same mistake wears for a local: a read before it is assigned is `ErrorMove`, a
second assignment of an `imm` one `ErrorNoMut`.

A generic module takes three codes, split where the remedy is.
`ErrorGenModBare` is the generic named without type arguments where only an
instance has members — a path through it, a standalone `use`, an import's `use`
clause, `extends`, a default fold on its own `mod` line; the remedy is to name
an instance, `stack[i64].push`, or to stop folding. `ErrorGenModBody` is what a
generic module holds that an instance is not yet built for — a generic function
or type, a trait or an enum, a macro, a module trait, a global's `use` clause,
a submodule; the remedy is in the generic module, and the code retires piece
by piece as the clone learns each. `ErrorGenModRoot` is an executable's root
declared generic; the remedy is to move the generic module under a root. What an
instance is given that is not a type, or the wrong number of arguments, wears
`ErrorNotType` and `ErrorArgCount`, as for a generic type, and a member an
instance lacks or keeps private `ErrorUnkName` and `ErrorNotPublic`, as through
any module; `@c` on a generic module is `ErrorCAttr`, as on a generic function.

A file's header takes two codes, one per rule, since each remedy is a different
edit. `ErrorNoModDcl` is a module's first file — its designated file, a lone
file, the first a build description lists — whose first statement is not its
`mod` line, `mod trait` included; the remedy is to write `mod name;` there, and
the message names the name. `ErrorImportLate` is an `import` that is not right
after the `mod` line: after any other declaration of the designated file, or
anywhere in a file of the module that has no `mod` line [Jon 23 Sep]. The
remedy is the same edit either way, to move it up next to the module's other
imports, and the second message names the designated file they are in. A `mod` line in the wrong place stays `ErrorModDcl`: that is the
declaration the module already has, or one a file may not make.

That is a narrow licence, and the tell that it has been stretched is the
scenarios: **when a scenario needs a message substring to tell two uses of one
code apart, the substring is doing the code's job.** Wrong arity, a non-type
generic argument, a call that expects arguments and gets none, and arguments
given to a field access are four conditions a reader would absolutely
distinguish — so they are four codes, `ErrorArgCount`, `ErrorNotType`,
`ErrorNoArgs` and `ErrorFldArgs`, and `ErrorManyArgs` means only what it says.
A `name: value` argument anywhere but a type literal is `ErrorNamedArg`, one
code for a call, an index and a macro use, because the remedy is the same for
each: pass the value by position.

## The one code with no scenario

`ErrorUnreachable` is reported by `errorUnreachable` and by nothing else. It
means the compiler reached a state it had established cannot happen: not a bad
program, a compiler defect. A source that provoked it would be a bug report
rather than a test case, so it is the one code that lands without a scenario —
deliberately, not by oversight.

Everything about it follows from that. It goes through `errorMsgNode` so a
report carries a source position **and the instantiation trace**, which for a
defect that only appears inside a generic expansion is the only actionable part.
It then exits `ExitGen` — the compiler has just established that its own
invariants do not hold, so anything further it emitted would be guesswork. A
separate exit code was considered and rejected: it would enlarge the `driver`
category's contract for a status nothing can produce.

**Never call it for a condition a source can reach.** That is a missing
diagnostic, and it gets a code and a message of its own, upstream where the rule
lives.

Having no scenario has a cost worth naming: nothing exercises this reporting
path, so a regression in it — the position, the trace, the exit code — would
leave the suite green. [Measuring](measuring.md) carries the recipe for
provoking it by hand, which is the only way to check.

## Suppressing a cascade

One mistake should produce one diagnostic. Two mechanisms:

- **`errorType`**, installed on a node whose error was already reported.
  `itypeMatches` returns `EqMatch` when either side carries it and
  `iexpMultiInfer` drops such a branch, so everything derived from it stays
  quiet. `inodeIsError` is the predicate. See the sentinel section of
  [IR Nodes](../nodes/_index.md).
- **An error-count delta.** `fnDclTypeCheck` records `errors` on entry and
  compares, so "did *this* declaration fail?" is answerable without knowing what
  failed elsewhere. That is what gates the body check and flow analysis.

`iexpTypeCheckCoerce` returning success on an untyped operand is the same idea
by a third route: a deliberate lie so one bad subexpression does not provoke a
complaint from every enclosing node.

## Hazards

- **An `assert` is a no-op in the release build.** `NDEBUG` compiles it to
  nothing, and the release build is the only one the test runner uses, so an
  `assert` has never caught anything in a tested configuration. Do not add one
  expecting it to. The twenty-two sites that meant *unreachable* have been
  converted to `errorUnreachable`; the ordinary value asserts have not.
- **Reusing a code makes a scenario unable to tell two conditions apart.** If
  you find yourself picking an existing code because the new condition is
  "close enough", that is the smell.
- **A diagnostic on an injected node without `inodeLexCopy`** points at end of
  file, and the test suite's per-line annotations will not match.
- **`--checktree` reports `ErrorBadTree` on the error path too**, deliberately —
  a phase that reports and returns early is exactly what leaves a node without
  a `vtype`.

## What lives elsewhere

- Asserting a diagnostic in a scenario, and the annotation format: [Test Suite](test-suite.md)
- Finding out what the compiler actually reports: [Measuring](measuring.md)
- The three type sentinels and `newErrorNode`: [IR Nodes](../nodes/_index.md)
- Which codes each phase owns: the "Diagnostics" section of each phase note
