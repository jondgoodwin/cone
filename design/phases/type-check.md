Type check has two halves. **This note covers *when* a declaration is checked**:
what demand means, the two marks a declaration carries, what a declaration still
under check can answer about its type and its size, and the order each
declaration kind resolves in.
[Type Check Reasoning](type-check-reasoning.md) covers *what* the checks decide.

Read this when a declaration is analyzed in an order you did not expect, when a
type reports no size, when something is called circular, or before changing
anything that walks declarations.

*Provenance: measured. Every claim here was produced by running the compiler
rather than reading it — see [Measuring](../diagnostics/measuring.md).*

## 1. Where this phase sits

```
parse  ->  name resolution  ->  type check  ->  generation
                                    |
                                    +--> flow analysis, per function
```

**Name resolution is one eager pass over the whole program**, with a global gate:
if it reports anything, `conec.c` returns before type check begins. So type check
never meets an unbound name and nothing has to reason about a partly-bound
declaration.

It stays a single source-order pass deliberately. Binding a name needs the
declaration to *exist*, not to be analyzed, and the parser guarantees that — so
name resolution has no use for demand. But it also could not have it cheaply:
**it binds through a global slot**, and jumping out of a function body into an
unrelated declaration would leave that function's locals still plugged in, so a
matching name in the declaration jumped to would bind to one of them. Demand
would mean saving and restoring the whole hook stack at every jump. The
mechanism is [Name Resolution](name-resolution.md) section 2.

**Type check has no such constraint**, so it can be suspended anywhere and
re-entered with nothing to save. The one place it hooks at all is generic
substitution — `clonePushState` hooks the arguments, `cloneNode` reads them,
`clonePopState` unhooks — and that is self-contained within one instantiation,
which is not a suspension point.

**What the eager pass costs** is the global gate: one unresolved name anywhere
stops the compile before any type checking, so a file cannot report a name error
and an unrelated type error in one run. Removing it needs a per-node
unresolved/resolving/resolved state, and every site that reads `dclnode`
handling an unbound one.

**Type check is demand-driven and it interleaves.** A declaration reaches names
belonging to other declarations, so its type check suspends, the named
declaration is analyzed, and only then does the first resume — and that nests. At
any moment several declarations are part-way through their own type check at
different depths, which is what puts the work in dependency order rather than
source order.

**Flow analysis is terminal and per function.** It runs at the close of a
function's own type check, on that function's body, and nothing ever demands a
flow result from elsewhere.

**Lowering belongs to type check**, all of it. Lowering is what establishes a
node's type — `fnCallLowerMethod` sets a call's `vtype` from the method it
selects, and the parent expression needs that type — so it can be neither earlier
than types nor later.

## 2. The rules

1. **Reaching a name analyzes its declaration** before continuing.
2. **A finished declaration answers from what it recorded.** — `TypeChecked`
3. **A declaration establishes its own type before analyzing anything that could
   refer back to it.** — `TypeChecking`
4. **A field asks its type for a size.** A type still being laid out has none,
   and the field that asked reports it. — `TypeChecked` means *laid out*
5. **"No known size" names its cause and the chain that led there.**
6. **A constant or inferred declaration re-entered before its type exists is
   circular**, and says so.
7. **Instantiation depth is bounded by a limit**, checked where instances are
   created.
8. **Entering a declaration saves and resets the walk context.**

## 3. The two marks

Every declaration carries two bits in its node `flags`:

| Flag | Value | Means |
| --- | --- | --- |
| `TypeChecking` | `0x4000` | Type check of this declaration has begun and not finished |
| `TypeChecked` | `0x8000` | Type check of this declaration finished |

**They are type check's marks, and only type check's.** `inodeTypeCheck` sets and
tests them, in two branches: one for type nodes and modules, one for every other
declaration (`inodeIsDcl`). `inodeNameRes` neither sets nor reads them — it is a
bare dispatch with no guard — and neither does flow analysis. `TypeChecking` means
*this declaration's type check has begun*, not that anything about name
resolution is in progress.

That is safe rather than an oversight, and the global gate is what makes it so:
name resolution finishes over the whole program before type check starts, so
every declaration is fully bound before any mark is set. There is no cross-phase
state to keep consistent because there is no overlap. Were the two walks ever
merged (section 1), these marks would not survive the merge unchanged — a
declaration could then be name-resolved but not type checked, which is a third
state neither bit can express.

`TypeChecked` returns at once — type check lowers and replaces nodes, so a second
walk of a declaration corrupts it, which makes the mark a correctness requirement
rather than an optimization.

**Read `TypeChecked` as "this declaration was type checked", never as "the node
carrying it is a type".** Most things carrying the mark *are* types, which is
what makes the reading tempting and wrong. The name states which phase owns the
mark; it says nothing about what the marked node is.

**For a type, `TypeChecked` means laid out**, not "everything about me is done". It
is set in `structTypeCheck` at the point the layout settles — fields indexed,
size known, method set complete — and *before* the methods themselves are
analyzed. The placement is load-bearing: a method may use its own type by value,
`fn twin(self) Self`, so the size has to be available before methods run.
Nothing needs a state stronger than that, and nothing should be added that
does — see "Settled deliberately" below.

**The marks live on the node, not on the name.** Generic and macro instantiation
clones declarations, and each clone is legitimately unanalyzed, so every clone
function clears them. `cloneStructNode`, `cloneFnDclNode`, `cloneVarDclNode` and
`cloneFieldDclNode` all do. **Anything added to a node's `flags` needs its clone
audited** — three separate defects in this codebase have been a clone carrying
state it should have cleared.

## 4. Demand

A name use analyzes the declaration it names, then reads what it needs:

| Site | Reaches |
| --- | --- |
| `nameUseTypeCheckType` | a type declaration |
| `nameUseTypeCheck` | a value declaration — variable, function, field, constant |
| `fnCallTypeCheck` | its callee, arguments and receiver |
| `itypeTypeCheck` | any type named in a signature or a declared type |

Because the declaration is analyzed at the moment a use has to decide anything
about it, each decision is locally justified. A namespace asked for a member is
complete — mixins expanded, trait methods inherited. A type read for its size has
one, or says why not.

**Source order does not decide what is analyzed, only when.** The driver still
visits every declaration; one already analyzed by demand returns at once. So an
unreferenced declaration with an error still fails the compile, and a function
may name a global declared below it — including one whose type comes from its own
initial value, which is the case nothing else can serve.

**Locals are not part of this.** They are hooked and unhooked during the walk, in
source order, and `mut a = a` inside a function fails at name resolution because
the name is not yet in scope.

## 5. Re-entry

Reaching a declaration that is `TypeChecking` and not `TypeChecked` is normal, not an
error. What it can answer depends on what was asked.

**Asked for a type, it answers.** Rule 3 says a declaration establishes its own
type before analyzing anything that could refer back to it, so the type is
already on the node. This is what makes mutual recursion work:

```cone
fn even(n i64) i64 { if n == 0 { 1 } else { odd(n - 1) } }
fn odd(n i64) i64 { if n == 0 { 0 } else { even(n - 1) } }
```

`even` reaches `odd`, which reaches `even` — whose signature is filled in, and a
call needs its callee's signature, never its body.

**Asked for a size, it has none.** That is section 6.

**Asked for a type it does not have yet, it is circular.** That is section 7.

## 6. Size

A field or a variable holds its type by value, so that type has to say how large
it is. `itypeNoSizeCause` answers, and there are five ways the answer is no:

| Cause | Where it comes from | Remedy the message names |
| --- | --- | --- |
| declared `@opaque` | the `@opaque` marker, `parsetype.c` | hold it through a reference |
| a trait that is not `SameSize` | `structTypeCheck` | use a virtual reference, `&<Trait>` |
| a function signature | `fnSigTypeCheck` | use a reference to a function |
| a struct with an unsized field | `structTypeCheck`, infectiously | fix that field — the cause is further down |
| still being laid out | rule 4 | break the cycle with a reference |

All five are one `ErrorCode`, `ErrorNoSize`. Five codes would be
indistinguishable to everything except the message, and the message is what the
author needs, so the cause lives in the text.

**A reference answers its own size and never consults its target.** A pointer is
a pointer whatever it points at. That single rule settles every recursion case
without the field having to know which case it is in:

```cone
struct S { v i64; next &S }      // legal — S never asks itself
struct A { b B }  struct B { a &A }   // legal — B completes, so A gets its size
struct A { b &B } struct B { a &A }   // legal — neither asks the other
struct A { b B }  struct B { a A }    // error — A asks B, which asks A mid-layout
```

There is deliberately no recursion check. An unfinished struct has no size, a
finished one does, and the diagnostic belongs to the field that needed one, which
is also the better error: it names what to change.

**A union has a size only when every variant does.** Its own mark says only that
its tag is settled, so a variant holding its own union by value asks a question
the mark would wrongly answer yes to. See 10.2.

**An array's size is its length times its element's**, so an array of a type with
no size has none either — and then it is not a type at all, not even behind a
reference, because there is nothing to allocate. `arrayTypeCheck` says so where
the array type is written, which is the only site that can: a reference to such
an array asks it nothing.

**Opacity is infectious**, so the type a field names is usually unsized only
because of something several levels below it. The diagnostic names the
declaration that actually lacks the size, and uncounted lines name the hops:

```
Error 1069: Variable o cannot be held by value: Handle is declared @opaque, ...
... Outer has no size because its field w has type Wrapper
... Wrapper has no size because its field h has type Handle
```

The path is recovered by walking fields where it is reported, so it needs no
stored demand stack. That walk is depth-bounded — the type graph it crosses may
be cyclic, and it runs on error paths, where the tree is the shape nothing
checked.

**Repeated diagnostics are not suppressed.** One mistake several levels down
still reports once per level that holds it. Fixing that needs a `Failed` state
per declaration and a test at every reporting site; see section 13.

## 7. Circularity

Rule 3 says a declaration establishes its type before anything can recurse. Two
kinds cannot: a constant, and a variable or field whose type is inferred. Both
take their type *from* the very thing that might recurse, so there is no earlier
answer to give.

```cone
const A = B
const B = A       // error: A is defined in terms of itself
mut a = a         // the same shape, for an inferred global
```

Detected without a new flag, since it is exactly "asked for a type, and the type
is still `unknownType` while under analysis". `ErrorCircular` is its code, and it
is not a variant of `ErrorNoSize`: "this type cannot tell you its size, hold it by
reference" and "this definition depends on itself" are not the same advice.

## 8. Generics and macros

A generic is not analyzed; it is a template. `structTypeCheck` and
`fnDclTypeCheck` both return early when `genericinfo` is set. Only *instances*
are analyzed — each a fresh clone that `genericInstantiate` checks as it builds,
memoized on `genericinfo->memonodes`. Macros expand by the same clone-and-check
path. So instantiation is demand-driven, and rules 1 to 6 apply to instances
rather than templates.

**The marks cannot police this.** They find a cycle by returning to the same
node, and a generic instantiating itself at ever-larger arguments never does —
every instance is a new node with new arguments:

```cone
fn recur[T](x T) T { recur[Box[T]](Box[T][x]).v }
```

Depth is the only thing that distinguishes an expansion that terminates from one
that does not, so `genericInstantiateEnter` counts it and refuses past
`TypeCheckLoopMax` (256) with `ErrorInstDepth`. Both macro expansion paths — a
parameterless name standing for its body, and a call substituting arguments — are
bounded the same way. Past the limit it is the C stack that gives out, with no
diagnostic at all: measured, the generic form reaches depth ~702 and the macro
form ~2282 before it does, while the deepest legitimate expansion in the test
corpus is 1.

## 9. The walk context

**Each walk carries its own state**, `NameResState` and `TypeCheckState`. They
overlap on two fields and differ on three:

| Field | Meaning | In | Saved and reset by |
| --- | --- | --- | --- |
| `mod` | current module | name resolution | `modNameRes` |
| `loopblock` | innermost loop block | name resolution | `blockNameRes` |
| `fn` | current function | type check | `fnDclTypeCheck` |
| `typenode` | current type | both | `structNameRes`, `structTypeCheck` |
| `scope` | 0 global, 1 signature, 2+ blocks | both | `fnDclNameRes`, `fnDclTypeCheck`, `blockTypeCheck`, `fnSigTypeCheck` |

Demand means jumping from the middle of one function's body into an unrelated
declaration, so everything above describes somewhere else at that moment.
**Entering a declaration saves and resets what it changes**, which is what makes
analyzing a declaration independent of where it was analyzed from.

**The two are separate deliberately**, so that `pstate->fn` inside a name
resolution function fails to build rather than reading a field it must not.
Measured, no function takes both. One merged struct would compile perfectly
well — and would hand every `*NameRes` function an `fn` it must never read, and
every `*TypeCheck` function a `mod` and `loopblock` it must never read. Two
structs make that a compile error instead of a convention.

`loopblock` is read only during name resolution, which is still one eager
source-order pass, so demand cannot reach it. `scope` is consumed during type
check by `clonePushState`, which gives a cloned lifetime node its `life`, and by
the temporary `fnCallTypeCheck` injects for an append.

## 10. Order of resolution within a declaration

Steps marked **→** are where a demand can leave and re-enter.

### 10.1 Struct and trait

1. If it is a generic template, stop. Only instances are analyzed.
2. **→** Analyze the base trait.
3. Propagate the base trait's closed-type flags (`SameSize`, `HasTagField`). A
   derived type of a closed trait must be declared in the same module.
4. Insert a mixin field for the base trait at position 0.
5. Walk fields **backwards**: expand mixins in place — splicing in the trait's
   fields and inheriting its methods — and **→** analyze each ordinary field.
   Backwards so that splicing does not invalidate the position.
6. Index the fields. Compute infectious flags from them: `ThreadBound`,
   `MoveType`, `OpaqueType`, `ZeroSizeType`. Identify the tag field.
7. `final` forces `MoveType`; `clone` clears it. Propagate infection up to base
   traits.
8. **Size is now known**, and `TypeChecked` is set here — meaning laid out.
9. **→** Analyze the methods.

Steps 2 and 5 are where recursion arrives; step 8 is why a method at step 9 may
use its own type by value.

### 10.2 Closed traits, unions and variants

A union is not a node kind. It is a trait carrying `HasTagField` or `SameSize`,
whose variants are structs written inside its body.

- **The variant list is complete at parse time.** `parsetype.c` adds each nested
  struct to the trait's `derived` list and assigns its tag number as it parses.
- **Analysis never computes a union's size.** `genlSameSizeTrait` sizes each
  variant and pads to the largest at *generation* time.

The rule that a derived type lives in the same module as its closed trait keeps
both true.

**A closed trait's `TypeChecked` mark does not mean it has a size.** It means its
*own* fields are laid out, which for a union is the tag. Each variant is reached
separately and pulls the trait in as its base trait, finishing it before walking
its own fields — so the trait is marked long before the variants that determine
its size are done. Anything asking a union for a size has to ask whether every
variant is laid out as well; `itypeVariantPending` is that question, and section
6 is where it is asked. Without it a variant could hold its own union by value,
which compiled clean and generated a layout with the field dropped.

### 10.3 Function and method

1. If it is a generic template, stop.
2. **→** Analyze the signature: parameters, then return type. **The type is now
   established** — this is rule 3, and it is why mutual recursion works.
3. If the signature raised anything, stop. A body checked against a signature
   that failed reports again at every use of the types that check was meant to
   establish.
4. If there is no body, or it is a trait's default method, stop.
5. Check that a method's `self` parameter matches its enclosing type.
6. Turn an implicit final-expression return into an explicit one.
7. **→** Analyze the body, with `fn` and `scope` saved and reset.
8. Run flow analysis on the body — escape, permission, lifetime, move — and skip
   it if this function raised anything.

Steps 3 and 8 both compare the error count against the one this call entered
with, so each is about this declaration alone and not about whatever failed
elsewhere, whichever walk arrived at it.

### 10.4 Variable, field and constant

1. **→** Analyze the permission, then the declared type.
2. If there is no initializer, the type must have been declared.
3. **→** Analyze the initializer, coercing it to the declared type; if no type was
   declared, **the type becomes the initializer's**. For this shape, steps 1 and
   3 are the same step, which is what section 7 is about.
4. A global or parameter requires a literal initializer; so does a field default.
5. The type must have a size — rule 4's report site.

### 10.5 Module and program

1. **→** Analyze imports first. Includes are not modules and are not visited
   separately.
2. Iterate the declarations and analyze each. Demand pulls forward whatever a
   forward reference needs; one already analyzed returns at once.

## 11. Diagnostics type check owns

| Code | Raised when |
| --- | --- |
| `ErrorNoSize` (1069) | a value's type cannot say how large it is — five causes, named in the message |
| `ErrorCircular` (1068) | a constant or inferred declaration is defined in terms of itself |
| `ErrorInstDepth` (1067) | generic or macro expansion nests past `TypeCheckLoopMax` |

**`ErrorRecurse` (1049) is retired and its number must not be reused.** It
refused every re-entry, which rule 4 replaced: a reference answers its own size,
so a type reached again through one is not an error.

A diagnostic reported on a node that came from an instantiation names what
expanded it, and what expanded that, outward. That trace is capped at four
frames: ordinary code nests one or two deep, and a runaway expansion caught by
`ErrorInstDepth` would otherwise bury the diagnostic under hundreds of identical
frames.

## 12. Settled deliberately

Kept so that reopening one is a decision rather than a rediscovery.

| Question | Answer |
| --- | --- |
| Does the name-resolution gate change? | No. One eager pass, global gate. |
| Is name binding tracked as its own state? | No. It is the first thing analyzing a declaration does, and nothing else asks. |
| What state does demand need? | None beyond the two marks, read rather than refused. |
| Does anything need "complete" beyond "laid out"? | No consumer exists. A `SizeKnown` field on `ITypeNodeHdr` would separate "laid out" from "methods checked", but `TypeChecked` at the layout point already says the first and nothing asks for the second. Do not add one. |
| Should a size question have five codes? | No. One code, cause in the message. |
| Should a repeated diagnostic be suppressed? | Not for now. It needs a `Failed` state per declaration plus a test at every reporting site. |

## 13. Hazards

- **Diagnostics come out in dependency order, not source order.** Per-line
  annotations in the test suite are unaffected; anything asserting a count or a
  sequence is not.
- **The suite cannot assert an absent check.** Where a rule is unenforced the
  corpus records it by establishing the opposite, so a scenario that starts
  failing may be one a change correctly invalidated.
- **Node `flags` bits are not one namespace.** Check every declaration family
  before claiming a bit, not just the type block. A collision has no
  diagnostic: `0x0040` overlapping `HasTagField` stops type checking every
  tagged union and reports nothing.
- **An ordinary `assert` is a no-op** in the release build: it compiles to
  nothing under `/DNDEBUG`. The sites that meant *unreachable* no longer are
  asserts — they call `errorUnreachable`, which reports `ErrorUnreachable` and
  exits. Do not write a new `assert(0)` expecting it to catch anything shipped.
- **A node built during analysis takes the lexer's position**, which by then is
  the end of the file. `newNode` reads `lex->tokp`, so an injected node points at
  nothing unless `inodeLexCopy` is called on it.
- **The eight type properties crowded into `flags`** — `MoveType`, `ThreadBound`,
  `OpaqueType`, `ZeroSizeType`, `TraitType`, `SameSize`, `HasTagField`,
  `NullablePtr` — would be better in `ITypeNodeHdr`. Analysis neither needs that
  nor makes it worse.

## 14. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `conec.c` | `doAnalysis` | runs name resolution, gates on errors, then walks the program for type check |
| `ir/inode.c` | `inodeTypeCheck` | the dispatch switch, and where both marks are set and tested |
| | `inodeTypeCheckAny` | the same with no expected type |
| `ir/itype.c` | `itypeTypeCheck` | check a node expected to be a type |
| | `itypeNoSizeCause`, `itypeNoSizeExplain` | the five causes of section 6, and the hop-by-hop trace |
| | `itypeVariantPending` | whether every variant of a union is laid out — section 10.2 |
| `ir/iexp.c` | `iexpTypeCheckAny` | check a node expected to be an expression |
| `ir/types/struct.c` | `structTypeCheck` | the nine steps of section 10.1; sets `TypeChecked` at the layout point |
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the eight steps of section 10.3, including both error-delta gates |
| `ir/stmt/vardcl.c` | `varDclTypeCheck` | section 10.4 |
| `ir/stmt/module.c` | `modTypeCheck` | imports first, then declarations — section 10.5 |
| `ir/meta/generic.c` | `genericInstantiate`, `genericInstantiateEnter` | instantiation, memoization, and the depth bound |
| `ir/clone.c` | `clonePushState`, `clonePopState` | the one place type check hooks a name |
| `ir/ir.h` | (`TypeCheckState`) | the walk context of section 9 |

## 15. What lives elsewhere

| Question | Note |
| --- | --- |
| What a check actually decides — coercion, overloads, casts, borrows | [Type Check Reasoning](type-check-reasoning.md) |
| What may be assumed already bound, and why the gate exists | [Name Resolution](name-resolution.md) |
| Moves, aliasing, drops — what runs at step 8 of section 10.3 | [Flow Analysis](flow.md) |
| Node headers, marks as flag bits, the sentinels | [IR Nodes](../nodes/_index.md) |
| How to re-measure any claim here | [Measuring](../diagnostics/measuring.md) |
