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
declaration. The exception is a pattern's bare root, whose meaning depends on the
matched value's type: the `is` test and the conversion that hold it bind it
first (`castPatternBind`, see [cast](../nodes/cast.md)), so nothing else meets it
unbound either.

It stays a source-order pass deliberately, with one use of demand. Binding a
name needs the declaration to *exist*, not to be analyzed, and the parser
guarantees that — so a name never needs demand. What does is a type's
dictionary: the members it inherits are copies of a trait's, so the trait must
be complete first, and `structNameRes` resolves it when it is first needed.
That is affordable only because it is confined to a type reached from a type:
**name resolution binds through a global slot**, and jumping out of a function
body into an unrelated declaration would leave that function's locals still
plugged in, so a matching name in the declaration jumped to would bind to one
of them. A type reached from a type has only module names and the demanding
type's generic parameters plugged in, and the target's own scope is hooked over
them, or in place of them where it is another module's. Demand from anywhere else would mean saving and restoring the whole hook
stack at every jump. The mechanism is [Name Resolution](name-resolution.md)
section 3.

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

## 2. Principles — the rules

⚠ **Measured, not read** — every claim in this note was produced by running the
compiler. **Unchecked with the author is the claim that these are ruling
positions rather than the present arrangement.**

▸ **What the eight collectively forbid is any dependence on source order.**
Reaching a name pulls its declaration forward, so the order declarations appear
in decides *when* each is analyzed and never *whether*. A rule that only holds
when declarations are written in a particular sequence contradicts the set.

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
state to keep consistent because there is no overlap. Name resolution keeps two
marks of its own for the one place it leaves walk order — `NameResolving` and
`NameResolved`, on a module and a struct only, distinct bits that type check
never reads — so were the two walks ever merged (section 1), the four would
still not meet.

`TypeChecked` returns at once — type check lowers and replaces nodes, so a second
walk of a declaration corrupts it, which makes the mark a correctness requirement
rather than an optimization.

**Read `TypeChecked` as "this declaration was type checked", never as "the node
carrying it is a type".** Most things carrying the mark *are* types, which is
what makes the reading tempting and wrong. The name states which phase owns the
mark; it says nothing about what the marked node is.

**For a type, `TypeChecked` means laid out**, not "everything about me is done". It
is set in `structTypeCheck` at the point the layout settles — fields indexed,
size known, method set complete — and the type's members are not analyzed by
that check at all: they wait until no layout is in flight anywhere (section 4,
"Layout before members"). So a method may use its own type by value,
`fn twin(self) Self`, or any other type, and find its size settled. Nothing
needs a state stronger than "laid out", and nothing should be added that does —
see "Settled deliberately" below.

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
| `fnCallLowerMethod` | every candidate a member name declares, before selection compares signatures (`fnCallDemandCandidates`) |
| `structCheckTraitReqs` | each method a mixed-in trait requires, before the type's own is compared with it |
| `itypeTypeCheck` | any type named in a signature or a declared type |

**A member name is a use too.** A type checks its methods in order, so from
inside one of them a method declared later — or spliced in after the type's own,
as an enum's are into each variant — is not yet analyzed, and an unchecked
signature's reference parameter matches no receiver. `self.later()` was
`ErrorNoCandidate` while the bare `later()`, which reaches it through
`nameUseTypeCheck`, worked. Selection now demands each candidate first, under a
walk state of the candidate's own type rather than the caller's (Rule 8:
`fnDclTypeCheck` compares a method's `self` with `pstate->typenode`). A number
type's methods are skipped: corenumber builds them typed and nothing checks them.
**A bare name is demanded the same way** (`nameUseTypeCheck`): a member of a type
not yet begun is analyzed under its own type's walk state, not the use's. A bare
call in a generic enum's variant is bound to the enum instance's own method, and
checked under the variant's state that method was refused as not taking the
variant for `self`; it was reached only when a variant's members happened to be
checked before its enum's.

Because the declaration is analyzed at the moment a use has to decide anything
about it, each decision is locally justified. A namespace asked for a member is
complete — the traits taken in by name resolution, or, for an instance of a
generic trait, at step 4 below. A type read for its size has one, or says why
not.

**Source order does not decide what is analyzed, only when.** The driver still
visits every declaration; one already analyzed by demand returns at once. So an
unreferenced declaration with an error still fails the compile, and a function
may name a global declared below it — including one whose type comes from its own
initial value, which is the case nothing else can serve.

**Locals are not part of this.** They are hooked and unhooked during the walk, in
source order, and `mut a = a` inside a function fails at name resolution because
the name is not yet in scope.

### Layout before members

**Every type is laid out before any type's members are checked.** A type's
layout — its fields, its own size, for an enum its variants, and the move and
thread-bound properties that follow from them — is what a size question reads,
and what a move or thread question reads. A member checked while some layout is
still in flight could see a type with no size yet, or one that does not yet know
it moves, and which of those it saw would follow the order the declarations are
written in. So demanding a type lays it out and nothing more; its members wait.

**The mechanism is two queues and a count**, in `struct.c`:

- `structLayoutEnter` and `structLayoutExit` count the layouts in flight. They
  are called by `inodeTypeCheck` around the check of every type that holds
  values by value — a struct, an array, a tuple. A reference answers its own
  size and a function signature has none, so neither counts there; a
  function declaration's signature is held as one by `fnDclTypeCheck`, for
  another reason (below).
- `structTypeCheck` lays the type out, sets `TypeChecked`, settles its drop
  function, and puts the type in the **members queue**. An enum then lays out
  each of its variants not yet begun, an extension's copies of its base's
  variants among them (section 10.2) — unless one of them is in flight, which
  means the enum was demanded from inside that variant's layout. A sibling laid
  out there could hold that variant by value and find it unfinished, which would
  be a cycle only by the order of the walk, so the enum goes in the **variants
  queue** instead.
- When the count returns to zero, `structLayoutExit` works the queues: every
  waiting enum's remaining variants first, counted as in flight so that nothing
  is checked until all are laid out, then each waiting type's members
  (`structCheckMembers`: every member in its `nodelist`, then its overload sets,
  then its traits' requirements), first laid out first, under a walk state of the
  type's own.

**Checking a member may begin new layouts**, from a signature or a body, and
when the last of those finishes the queues are worked from there, nested. A
function's signature is held as a layout in flight while it is checked
(`fnDclTypeCheck`), so a layout its signature begins works the queues only once
the whole signature is in place: the queue may hold a type whose method calls
this function, and that call reads the return type (rule 3). Before, a method of
a generic type's instance returning `Option[T]`, where that signature was the
first to name `Option[i64]`, had its caller checked mid-signature; the call's
type was still the unchecked `Option[T]`, and a `match` on it refused `Some`
as a bare generic — unless a field had named `Option[i64]` first
(`generic_signature_first`). So
when a use that demanded a type from a function body gets control back, that
type's members are checked — unless they were already waiting behind the ones
being checked, which is the same state a type in flight was always in. The
module walk itself needs nothing of this: whatever a declaration reaches is laid
out on demand, and its members are checked before the walk moves on.

**What it fixed.** Before, `structTypeCheck` laid a type out and then checked its
members in the same call, so a type demanded from inside another's layout had its
members checked while the demander was in flight. Measured on `2f3225cb`:
`struct S { t T; }` above `struct T { fn g(self &, s S) }` was `ErrorNoSize` on
`s`, while the other order compiled; and an enum, which the module walk reaches
first through its first variant, had its own members checked inside that
variant's layout, so a by-value `self`, a static function taking the enum, a
function below it that a static function calls, and a struct field naming it
that a member's signature names were all refused as a cycle. The same window
made the enum look copyable to its members before its variants' move properties
reached it, and a struct or function written above the enum laid it out ahead of
any variant, so a value of an enum with a variant holding a move field could be copied twice
or moved out through a borrowed reference twice, clean. Laying out every
variant with its enum closes both (`move_flow_infection`).

**What it does not change.** A real by-value cycle is still refused, by the same
size question (section 6): one of its types is always asked while in flight. And
a reference still type checks its target, which lays it out (`refTypeCheck`, which
also reads the target's thread-bound flag) — so a variant's field `&W`, where `W`
holds the enum by value, lays `W` out while the variant is in flight, and `W`'s
field is refused as though it closed a cycle. Written with `W` above the enum it
compiles. That is the one source-order dependence measured to remain; see
section 13.

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

"Unfinished" is read only of a type whose size depends on what it holds. A
reference, pointer or array reference can itself be in flight — `typedef QRef
&Quad` written above `Quad` checks the reference first, which demands `Quad`
from inside it, and `fn get(self QRef)` in `Quad` then reaches the same
reference mid-check. `itypeNoSizeOwnCause` answers a reference before the
in-flight test, as the rule above says it must; reading the mark first made a
typedef above its type refused as a cycle and the same typedef below it accepted,
which is the source-order dependence section 2 forbids.

**An enum has a size only when every variant does.** Its own mark says only that
its tag is settled, so a variant holding its own enum by value asks a question
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
`TypeCheckLoopMax` (256) with `ErrorInstDepth`. All three macro expansion paths
— a parameterless name standing for its body, a call substituting arguments, and
a macro method called on a receiver — go through `macroExpand` and are bounded
the same way. Past the limit it is the C stack that gives out, with no
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
   variant must be declared in the same module as its enum, and a type outside an
   enum may not join its variant set. If name resolution did not take the base trait
   in — it was an instance of a generic, which exists only now — insert a
   placeholder for it at position 0.
4. **→** Analyze every trait name resolution took in, then walk fields
   **backwards**: expand any placeholder still standing — splicing in the
   trait's fields and inheriting its methods, as name resolution does, or refusing
   an instance of a generic enum that an `is` list named — and
   **→** analyze each ordinary field. Backwards so that splicing does not
   invalidate the position. Then expand any fold clause name resolution left
   (a field whose type was an instance of a generic), and refresh every folded
   copy from the field it stands for, **→** demanding that field's check.
5. Index the fields. Compute infectious flags from them: `ThreadBound`,
   `MoveType`, `OpaqueType`, `ZeroSizeType`. Validate the tag field marked at
   parse; any other enum-typed field is refused.
6. `final` forces `MoveType`; `clone` does not clear it, since no copy calls
   `clone` and a bitwise copy of a finalizing value is finalized twice. Propagate
   infection up to base traits, but not into a built-in one (`Move`, `Copy`,
   `RegionRef`). A declared `is Move` was marked at name resolution; a declared
   `is Copy` is checked against the result with the members (`structCheckCopy`,
   `ErrorCopyMove`), once every variant is laid out.
7. **Size is now known**, and `TypeChecked` is set here — meaning laid out.
8. Settle the drop fn: validate `final`, and generate a `drop` if a field needs
   finalizing — but not on a trait or an enum, whose methods are its
   implementers' and would make that `drop` a requirement on each of them.
   Part of the layout, because each method's flow pass finalizes a
   by-value `self`, or a local of this type, only if the type has one by then.
   The generated `drop` is built lowered and carries `TypeChecked` from birth.
8a. Put the type in the members queue. **→** An enum lays out each variant not
   yet begun, or waits in the variants queue if one is in flight (section 4,
   "Layout before members"). The layout ends here.

Steps 9 and 10 run from the members queue (`structCheckMembers`), once no
layout is in flight:

9. **→** Analyze every member — methods, static functions, statics — then each
   overload set the type declares.
10. **→** Verify each mixed-in trait's method requirements against the signatures
   now known: a name the type declares itself must have the one candidate of the
   trait's signature, and a requirement with no body is unmet in a struct — an
   `extern` method has no body here and meets it, being defined elsewhere. Each
   requirement is analyzed first, under the trait's walk state. This type may have
   been demanded from inside the trait's own step 9 — a static function written
   above the requirement names it in its signature — and an unchecked requirement
   matched nothing, so whether a variant or an implementer conformed followed
   source order.

Steps 2, 4 and 8a are where recursion arrives; the members queue is why a method
at step 9 may use its own type, or any type, by value, and step 8 why it
finalizes one. The members themselves — which fields and which
default methods a type inherits — were settled by name resolution, which builds
the dictionary whole before any body is resolved; see [struct](../nodes/struct.md).

### 10.2 Enums and variants

An enum is not a node kind. It is a `StructNode` carrying `EnumType`, with
`HasTagField` and — unless it declined the padding — `SameSize`, whose variants are
written inside its body.

- **The variant list is complete at parse time.** `parsetype.c` adds each variant
  to the enum's `derived` list and settles its tag number as it parses, whether the
  author wrote the value or it was assigned in sequence. **An enum that extends
  another is the exception**: name resolution puts its copies of the base's variants
  at the front of its list and numbers its own from there, since the values the
  base's variants hold are not known until the base is resolved.
- **An extension's copies are laid out by the extension's layout**, with its own
  variants (step 8a), since they are no module's nodes; their members wait in the
  queue like any variant's, so a copy's method body that builds an added variant by
  value finds it finished however the extension was reached. A generic extension's
  copies are templates, so nothing reaches them, and each instance of the
  extension instantiates its copies with its own variants (`genericMemoize`) and
  lays them out with them. **A generic base is an
  instance**, written with its arguments, so type check is where it exists: the
  extension's `extendsbase` is checked into it, and the placeholder standing for it
  is expanded into the extension's fields, and into each copy's through its enum, as
  a generic enum's are into its variants.
- **A base enum and an extension of it never substitute for each other**, in either
  direction, and type check is what enforces it: two `EnumType` declarations are
  refused in `structMatches` ahead of its structural test and in
  `structVirtRefMatches` ahead of its mapping. Both would otherwise say yes, because
  an extension's fields are clones of its base's and its members are the base's. What
  the refusal buys is the exhaustive match: a base-typed value can then only hold one
  of the base's own variants. **Membership is one enum per variant**: `structMatches`
  answers a variant against an enum by its `basetrait` chain alone, padded or not, so
  neither a base variant nor an extension's copy passes as the other enum by the
  structural test the shared discriminant would otherwise satisfy.
  [struct](../nodes/struct.md), "An enum extending an enum".
- **Analysis never computes an enum's size.** `genlSameSizeTrait` sizes each
  variant and pads to the largest at *generation* time.
- **Analysis does settle the discriminant's width**, because that follows the
  largest tag value rather than the variant count and generation cannot see a
  pinned value. `structSetTagWidth` is where, in step 5 of the struct sequence,
  after the variants' numbers are known and the enum's declared integer type is
  checked — or, for an instance of a generic enum, from `genericMemoize` instead,
  once per generic, after the instance and its variants are checked. A value too large for a declared type is `ErrorTagWidth`, and so is one
  too large for the width an extension's base already settled: they share the node
  the width is written on.

The rule that a derived type lives in the same module as its enum keeps the first
two true.

**An enum's `TypeChecked` mark does not mean it has a size.** It means its
*own* fields are laid out, which begins with the tag. Its variants are laid out
after that, each taking the enum as its base — so the enum is marked before the
variants that determine its size are done. The module walk usually reaches the
enum through its first variant, which demands it as its base; then that variant
is in flight while the enum is laid out, and the enum's other variants wait until
it is done (step 8a). Anything asking an enum for a size inside that window has
to ask whether a variant is still being laid out as well; `itypeVariantPending`
is that question, and section 6 is where it is asked. Without it a variant could
hold its own enum by value, which compiled clean and generated a layout with the
field dropped. No member is ever checked inside the window: members wait until
every layout is done, variants included.

The question is whether a variant is *in flight* — `TypeChecking` without
`TypeChecked` — not whether every variant has finished. A variant not yet begun is
not on the demand stack, so it cannot close a cycle with whatever asked, and if
it does hold the enum by value its own field asks again once it is in flight, and
is refused there. Counting it as pending made the answer depend on source order,
which the rules of section 2 together forbid.

### 10.3 Function and method

1. If it is a generic template, stop.
2. **→** Analyze the signature: parameters, then return type. **The type is now
   established** — this is rule 3, and it is why mutual recursion works.
3. If the signature raised anything, stop. A body checked against a signature
   that failed reports again at every use of the types that check was meant to
   establish.
4. If it is an intrinsic declared in core, with its meaning from the registry,
   **→** analyze the type it acts on (its instance's type argument), which must
   have a size (`ErrorIntrinsicType`, reported at the call that instantiated it),
   and stop: it has no body ([intrinsic](../nodes/intrinsic.md)).
5. If there is no body, or it is a trait's default method, stop.
6. Check that a method's `self` parameter matches its enclosing type.
7. Turn an implicit final-expression return into an explicit one.
8. **→** Analyze the body, with `fn` and `scope` saved and reset.
9. Run flow analysis on the body — escape, permission, lifetime, move — and skip
   it if this function raised anything.

Steps 3 and 9 both compare the error count against the one this call entered
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

1. **→** Analyze imports first. A module's swept files are not modules and are
   not visited separately.
2. Iterate the declarations and analyze each. Demand pulls forward whatever a
   forward reference needs; one already analyzed returns at once. A type reached
   here is laid out, and its members are checked before the walk moves on, once
   no layout is in flight (section 4, "Layout before members").

## 11. Diagnostics type check owns

| Code | Raised when |
| --- | --- |
| `ErrorNoRefType` (1074) | a reference or slice type never says what it refers to — `refTypeCheck` and `arrayRefTypeCheck` are its only two sites |
| `ErrorNoSize` (1069) | a value's type cannot say how large it is — five causes, named in the message |
| `ErrorCircular` (1068) | a constant or inferred declaration is defined in terms of itself. Name resolution raises the same code for two types that each extend or name the other in an `is` |
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
| Is name binding tracked as its own state? | For a module and a type only, by name resolution's own two marks, so that a type may be resolved ahead of the walk when another type names it as a base. Nothing else asks, and type check never reads them. |
| What state does demand need? | None beyond the two marks, read rather than refused. |
| Does anything need "complete" beyond "laid out"? | No consumer exists. A `SizeKnown` field on `ITypeNodeHdr` would separate "laid out" from "methods checked", but `TypeChecked` at the layout point already says the first and nothing asks for the second. Do not add one. The members queue needs no mark either: a type joins it once, where its layout settles. |
| Is layout-before-members two passes over the module? | No. A type is laid out on demand, as before, and its members wait in a queue that is worked whenever no layout is in flight. Two passes would still need demand for a layout reached across modules or from a generic instance, and would check a type's members far from the declaration that reached it. |
| Should a size question have five codes? | No. One code, cause in the message. |
| Should a repeated diagnostic be suppressed? | Not for now. It needs a `Failed` state per declaration plus a test at every reporting site. |

## 13. Hazards

- **Diagnostics come out in dependency order, not source order.** Per-line
  annotations in the test suite are unaffected; anything asserting a count or a
  sequence is not.
- **The suite cannot assert an absent check.** Where a rule is unenforced the
  corpus records it by establishing the opposite, so a scenario that starts
  failing may be one a change correctly invalidated.
- **A reference inside a variant's layout can still refuse a non-cycle.** A
  reference type checks its target, so a variant's `&W`, where `W` holds the enum
  by value and is written below the enum, lays `W` out while the variant is in
  flight, and `W`'s field is `ErrorNoSize` "a variant still being laid out".
  Written above the enum, `W` is laid out first and it compiles. Closing it needs
  a reference that does not demand its target's layout, or a thread-bound flag
  settled after the layouts; measured, not built.
- **One declaration's error can silence another's body.** `fnDclTypeCheck` skips a
  body when the error count moved during its signature's check, and demand can
  run other declarations inside that check — a signature naming an enum lays the
  enum out, and the queues then check its members there. An error in one of them
  skips the function's own body. Measured the same before and after layout-before-
  members; the gate would have to count only errors reported on the signature.
- **Node `flags` bits are not one namespace.** Check every declaration family
  before claiming a bit, not just the type block. A collision has no
  diagnostic: `0x0040` overlapping `HasTagField` stops type checking every
  enum and reports nothing.
- **An ordinary `assert` is a no-op** in the release build: it compiles to
  nothing under `/DNDEBUG`. A site that means *unreachable* calls
  `errorUnreachable`, which reports `ErrorUnreachable` and exits. Do not write a
  new `assert(0)` expecting it to catch anything shipped.
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
| `conec.c` | `doAnalysis` | runs name resolution, gates on errors, then walks the program for type check, then judges where traced references are held (`regionTracedCheckAll`), which needs every type laid out |
| `ir/inode.c` | `inodeTypeCheck` | the dispatch switch, where both marks are set and tested, and where a struct, array or tuple layout is counted in flight |
| | `inodeTypeCheckAny` | the same with no expected type |
| `ir/itype.c` | `itypeTypeCheck` | check a node expected to be a type |
| | `itypeNoSizeCause`, `itypeNoSizeExplain` | the five causes of section 6, and the hop-by-hop trace |
| | `itypeVariantPending` | whether a variant of an enum is still being laid out — section 10.2 |
| `ir/iexp.c` | `iexpTypeCheckAny` | check a node expected to be an expression |
| `ir/types/struct.c` | `structTypeCheck` | the layout, steps 1 to 8a of section 10.1; sets `TypeChecked` at the layout point; `structSetDropFn` is step 8 |
| | `structCheckMembers` | steps 9 and 10, run from the members queue; `structCheckTraitReqs` is step 10 |
| | `structLayoutEnter`, `structLayoutExit` | the count of layouts in flight, and the queues worked when it returns to zero — section 4, "Layout before members" |
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the nine steps of section 10.3, including both error-delta gates |
| `ir/stmt/intrinsic.c` | `intrinsicDclTypeCheck` | step 4 of section 10.3: a declared intrinsic's type argument must have a size |
| `ir/stmt/vardcl.c` | `varDclTypeCheck` | section 10.4 |
| `ir/stmt/module.c` | `modTypeCheck` | imports first, then the module-trait check, then declarations — section 10.5 — and last `modLifecycle` |
| `ir/stmt/module.c` | `modLifecycle`, `modGiveDrop` | once every global's type is settled: the module's `init` and `final` checked as `fn @initpure init()` and `fn final()` (`ErrorModLifecycle`), each global without a value reported where the module has no `init` (`ErrorGlobalUninit`), and the module given a pre-lowered `drop` — its `final`, then each finalizing global's drop — where a global needs it. [module](../nodes/module.md), "Init and final" |
| `ir/stmt/modtrait.c` | `modTraitCheck`, `modTraitTypeCheck` | what a conforming module has for each member of its module trait against the member's shape — one candidate of exactly the signature, or a global of the type and permission — each difference `ErrorModTraitMismatch` at the `is`; the trait's own members' signatures and globals, never a default's body, which each copy checks. [module](../nodes/module.md), "Module traits" |
| `ir/meta/generic.c` | `genericInstantiate`, `genericInstantiateEnter` | instantiation, memoization, and the depth bound |
| `ir/clone.c` | `clonePushState`, `clonePopState` | the one place type check hooks a name |
| `ir/ir.h` | (`TypeCheckState`) | the walk context of section 9 |

## 15. What lives elsewhere

| Question | Note |
| --- | --- |
| What a check actually decides — coercion, overloads, casts, borrows | [Type Check Reasoning](type-check-reasoning.md) |
| What may be assumed already bound, and why the gate exists | [Name Resolution](name-resolution.md) |
| Moves, aliasing, drops — what runs at step 9 of section 10.3 | [Flow Analysis](flow.md) |
| Node headers, marks as flag bits, the sentinels | [IR Nodes](../nodes/_index.md) |
| How to re-measure any claim here | [Measuring](../diagnostics/measuring.md) |
