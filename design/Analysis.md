
Analysis is the work between parsing and generation: binding names to
declarations, establishing types, and checking flow rules for ownership,
permissions and lifetimes.

This note builds the design one example at a time. Each section starts from a
program, shows what the compiler does with it today, and derives the one
mechanism that program forces. Sections 10 to 13 are reference: resolution order
per declaration kind, a per-node summary of what changes, the rules collected,
and what was measured.

Claims about current behaviour were measured against the compiler, not read from
it. Section 13.2 says how to re-measure. Everything else is design, and is the
subject of [[Analysis re-factor]].

## 1. The problem, in three lines

```cone
fn early() i64 { later }    // Error 1013: return expression type does not match
mut later = 5i64
```

Reverse the two lines and it compiles. Give `later` an explicit type and it
compiles either way.

Nothing is wrong with the program. `early` is checked before `later` has been
looked at, so it is checked against a type that does not exist yet.

## 2. The core idea

When analysis reaches a name it does not yet know, it analyzes that name's
declaration to completion, then carries on where it left off.

Applied to section 1: checking `early`'s body reaches `later`, so `later` is
analyzed — its initializer is a literal, so its type becomes `i64` — and then
`early`'s body continues against a type that is now known. Source order stops
mattering.

**The compiler already does this.** Not for globals, but for types. Given:

```cone
fn main() i64 {
  mut p = Point[1i64, 2i64]
  (&p).sum() + gLimit
}

struct Point {
  x i64
  y i64
  fn sum(self &) i64 { self.x + self.y }
}

mut gLimit i64 = 10i64
```

tracing what analysis enters, and in what order, produces:

```
typecheck fn     main
typecheck var    p
typecheck struct Point      <-- demanded from inside main's body
typecheck field  y          <-- fields run backwards
typecheck field  x
typecheck fn     sum
typecheck var    self
cached    struct Point
flow      fn     sum        <-- sum's flow analysis, inside main's type check
flow      fn     main
cached    struct Point
typecheck var    gLimit     <-- never demanded; arrives in source order
```

`Point` is analyzed *completely* — fields, methods, and the flow analysis of
those methods — from the middle of `main`'s body, at the moment `main` first
needs it. That is this design, working, for one kind of declaration.

`gLimit` is not. It is reached only when the source-order walk arrives, after
`main` has been type checked *and* flow analyzed.

**The whole difference is one line.** A name use of a type lands in
`nameUseTypeCheckType`, a name use of a value in `nameUseTypeCheck`:

| Function | What it does with the declaration it names |
| --- | --- |
| `nameUseTypeCheckType` | `inodeTypeCheckAny(pstate, dclnode)` — analyzes it now |
| `nameUseTypeCheck` | `name->vtype = ((IExpNode*)name->dclnode)->vtype` — reads whatever is there |

So this is not new machinery. It is machinery that reaches one node family.

**What props up the other family today.** `modTypeCheck` runs a signature
pre-pass over every declaration so forward references have *something* to read,
then a body pass, marking any declaration whose signature failed with
`FlagSigError` so the body pass skips it. The pre-pass cannot infer:

```c
if (varnode->vtype != unknownType)
    inodeTypeCheckAny(pstate, &varnode->vtype);
```

A global whose type comes from its initializer is deliberately left
`unknownType`. Hence section 1. The pre-pass, `FlagSigError`, and the
`unknownType` checks that cope with a half-analyzed declaration all exist to
support source order, and all become unnecessary.

> **Rule 1.** Reaching a name analyzes its declaration before continuing.

## 3. Broadening: do not analyze twice

```cone
fn a() i64 { shared() }
fn b() i64 { shared() }
fn shared() i64 { 7 }
```

`a` demands `shared`. Then `b` demands `shared` again. Analyzing it twice is not
merely wasted work — analysis lowers and replaces nodes, so a declaration
analyzed twice is corrupted.

So each declaration records that it is done, and a demand for a finished
declaration returns immediately. The trace in section 2 shows this already
working: `cached struct Point` is the second and third demands returning without
work.

> **Rule 2.** A finished declaration answers from what it recorded. — flag
> `Analyzed`

## 4. Broadening: what if it is already being analyzed?

```cone
fn even(n i64) i64 { if n == 0 { 1 } else { odd(n - 1) } }
fn odd(n i64) i64 { if n == 0 { 0 } else { even(n - 1) } }
```

Analyzing `even` reaches `odd`, which reaches `even` — which is not finished.
Rule 2 does not apply. Yet this compiles today and should.

It works because of *where* the recursion lands. A function's analysis fills in
its signature before it starts its body, so when the demand comes back around,
`even`'s signature is already there. The body is not, and nobody needs it: a
call needs the callee's signature, never its body.

That generalises into the invariant the whole design rests on:

> **Rule 3.** A declaration establishes its own type before analyzing anything
> that could refer back to it. — flag `Analyzing`

`fnDclTypeCheck` already works this way. What is missing is that nothing *states*
it, so a node method that filled in a type only after checking a body would break
demand everywhere with no local sign of it.

Note what this means for the two flags: `Analyzing` does not mean "refuse". It
means "ask me for my type, not my body". Today the compiler treats it as a trap,
and section 5 is what that costs.

## 5. Broadening: when re-entry *is* fatal

```cone
struct A { b B }
struct B { a A }
```

`A` needs `B`'s size to lay out its field. `B` needs `A`'s size. Neither can
answer. This must be an error — the two are infinitely large.

But this is a linked list, and must not be:

```cone
struct S {
  v i64
  next &S
}
```

**Today both are refused**, with `ErrorRecurse`, because re-entering an
unfinished declaration is treated as failure regardless of what was asked. Cone
cannot express a linked list or a tree.

The distinction is not about recursion. It is about size, and it belongs to the
type being asked rather than to the field asking:

| Type | Answers its size from |
| --- | --- |
| a number, permission, or other primitive | itself |
| a reference or pointer, `&T` / `*T` | itself — pointer size, whatever `T` is |
| an array `[n; T]` | `n`, and `T`'s size |
| a nominal type | its own settled layout |
| a nominal type still being laid out | it cannot |

A reference answers on its own behalf and never consults its target. That one
rule settles every case without the field having to know which case it is in:

- `A { b B }`, `B { a A }` — `A` asks `B`, which asks `A` mid-layout. Error,
  reported at `B`'s field `a`.
- `A { b B }`, `B { a &A }` — `B`'s field is a reference, so `B` completes and
  `A` gets its size. **Legal.**
- `A { b &B }`, `B { a &A }` — neither asks the other. **Legal.**
- `S { v i64; next &S }` — `S` never asks itself. **Legal.** A linked list.

The last three are refused today.

There is deliberately no recursion check here. An unfinished struct has no size,
a finished one does, and the diagnostic belongs to the field that needed one.
That is also the better error: it names what is wrong and points at the field
the author can change.

> **Rule 4.** A field asks its type for a size. A type still being laid out has
> none, and the field that asked reports it.

**This needs no new state.** `Analyzed` set means laid out, so the size is
available; `Analyzing` without `Analyzed` means still being laid out, so it is
not. What changes is only how that state is *read*: today every re-entry is
refused; instead it answers "here is my type" or "I have no size yet" according
to what was asked.

`Analyzed` therefore means *laid out*, not *everything about me is done* — a
type's methods are checked after it is set. That is deliberate, and it is what
`structTypeCheck` already does, so that a method may use its own type by value:
`fn twin(self) Self`. Nothing needs a "methods are checked too" state, and
measured, a method may call one declared after it in either order.

## 6. Broadening: the other four ways a size is missing

```cone
struct @opaque Handle { }
struct Wrapper { h Handle }   // Error 1013: Field's type must be concrete and instantiable
struct Outer { w Wrapper }    // Error 1013: Field's type must be concrete and instantiable
fn f() { mut o Outer }        // Error 1013: Variable's type must be concrete and instantiable
```

One mistake, three diagnostics, and none of them mentions `Handle` or says what
is wrong with it.

Recursion is only one of five reasons a type cannot report a size, and the remedy
differs for each:

| Cause | Where it comes from | What the author does |
| --- | --- | --- |
| declared `@opaque` | `parsetype.c`, the `@opaque` marker | hold it only by reference |
| a trait that is not `SameSize` | `structTypeCheck` | use a virtual reference, `&<Trait>` |
| a function signature | `fnSigTypeCheck` | use a reference or a closure |
| a struct with any unsized field | `structTypeCheck`, infectiously | fix the field — the cause is further down |
| still being laid out | rule 4 | break the cycle with a reference |

The fourth is why the cause must be *chased*. Opacity is infectious, so the type
a field names is usually unsized only because of something several levels below
it, and naming the proximate type tells the author nothing.

Analyzing on demand supplies the chase for free: **the demand stack is the
explanation.** At the moment a field cannot get a size, the stack from that field
to the root is the cycle for recursion and the infection path for opacity, so the
compiler can say:

```
Field 'w' has no known size, so it cannot be held by value.
  'Outer.w' has type 'Wrapper'
  'Wrapper' has no size because its field 'h' has type 'Handle'
  'Handle' is declared @opaque: its layout is not known to this program
  Hold it by reference instead
```

And once `Wrapper` is recorded as unsized-and-reported, `Outer` consumes that
silently: one mistake, one diagnostic. No phase holds that chain today at the
moment it would need to print it.

> **Rule 5.** "No known size" names which of the five causes it is, and the chain
> that led there.

Naming the cause costs no storage: where a field cannot get a size, the compiler
can test `OpaqueType`, trait-without-`SameSize`, `FnSig`, or in-progress, and say
which.

**This is one new `ErrorCode`, `ErrorNoSize`**, carrying all five causes in its
message. It replaces `ErrorRecurse` on the layout path — after rule 4 that code's
text is simply false, since recursive types *are* supported through a reference —
and it replaces `ErrorInvType`'s two "must be concrete and instantiable" uses,
which today say nothing about what is wrong. Five separate codes would be
indistinguishable to everything except the message, so the cause belongs in the
text rather than in the number.

*Suppressing the repeat* is a separate thing and is **not** part of this work. It
needs a declaration to record that it was already reported — new state, and a new
test at every site that reports. The cascade above therefore stays as it is,
three diagnostics for one mistake, until that is taken up on its own.

## 7. Broadening: when there is no earlier answer at all

```cone
const A = B
const B = A     // compiles clean today
fn f() i64 { A }
```

Accepted in silence. The only complaint arrives at the *use*, as a type mismatch
naming neither constant.

Rule 3 says a declaration establishes its type before anything can recurse. Two
kinds of declaration cannot: a constant, and a variable or field whose type is
inferred, both take their type *from* the very thing that might recurse. There is
no earlier answer to give.

```cone
mut a = a       // the same shape, for an inferred variable
```

So for these, re-entry while `Analyzing` is an error naming the circularity —
detectable without a new flag, since it is exactly "asked for a type, and the
type is still `unknownType` while under analysis".

> **Rule 6.** A constant or inferred declaration re-entered before its type
> exists is circular, and says so.

This is a second new `ErrorCode`, `ErrorCircular`, and not a variant of
`ErrorNoSize`: the condition differs and so does the remedy. "This type cannot
tell you its size, hold it by reference" and "this definition depends on itself"
are not the same advice, and one message covering both would have to hedge about
which it meant.

## 8. Broadening: generics and macros

```cone
fn recur[T](x T) T { recur[Box[T]](Box[T][x]).v }
struct Box[T] { v T }
fn f() i64 { recur[i64](3i64) }
```

This crashes the compiler today — stack overflow, `0xC00000FD`, no diagnostic.

A generic is not analyzed; it is a template, and `structTypeCheck` and
`fnDclTypeCheck` both return early when `genericinfo` is set. Only *instances*
are analyzed, each a fresh clone that `genericInstantiate` checks as it builds,
memoized on `genericinfo->memonodes`. So instantiation is already demand-driven,
and rules 1 to 7 apply to instances rather than templates.

**But the flags cannot police this.** They find a cycle by returning to the *same
node*, and a generic instantiating itself at ever-larger arguments never does —
every instance is a new node with new arguments. Macros expand by the same
clone-and-check path and are exposed identically.

The type form is caught today, but only by accident:

```cone
struct Box[T] {
  v T
  next &Box[Box[T]]   // Error 1049 today
}
```

It is refused because `ErrorRecurse` refuses every re-entry — the same blanket
rule that refuses the linked list in section 5. **So rule 4 removes the only
thing standing between this program and the crash above.** The two must land
together, or a spurious error becomes a stack overflow.

> **Rule 7.** Instantiation depth is bounded by a limit, checked where instances
> are created. This is in addition to rules 2 and 4, not instead of them.

`TypeCheckLoopMax` (256) and `TypeCheckBlockMax` (1024) are declared in `ir.h`
for exactly this and have no call site anywhere.

## 9. Broadening: the walk context

Demand means jumping from the middle of a function body into an unrelated struct
declaration. The state carried along the walk — `fn`, `loopblock`, `scope`,
`typenode` — all describes somewhere else at that moment.

So entering a declaration saves and resets the walk context rather than
inheriting it. Merging `NameResState` and `TypeCheckState` into one
`AnalysisState` is item 1 of [[Analysis re-factor]] and is a safe mechanical
change on its own, but the merge alone is not sufficient; the reset is the part
that matters here.

The merge also removes an existing defect by construction: `conec.c` initializes
`tstate.fn` and `tstate.typenode` but not `tstate.scope`, which
`blockTypeCheck` increments and `clonePushState` consumes.

> **Rule 8.** Entering a declaration saves and resets the walk context.

## 10. Order of resolution within a declaration

**Name resolution, type check and flow analysis stay separate.** Each remains its
own node method, reached through its own dispatch, exactly as today. What changes
is when they run.

**Name resolution stays one eager pass over the whole program, and its gate stays
global.** It does not need to change, and deliberately does not. Binding a name
needs the declaration to *exist*, not to be analyzed — the parser guarantees that
— so name resolution never enters another declaration's analysis and gains
nothing from running on demand. Keeping it means the compiler still stops before
type checking if any name failed to resolve, so type checking never meets an
unbound name and nothing anywhere has to reason about a partly-failed
declaration.

None of the defects in 13.4 is a name-binding ordering bug. They are all in type
and size analysis, which is what changes.

**Type checking is what becomes demand-driven, and it interleaves.** A
declaration reaches names belonging to other declarations, so its type check
suspends, the named declaration is analyzed, and only then does the first resume
— and that nests. At any moment several declarations are part-way through their
own type check at different depths of incursion, which is what puts the work in
dependency order instead of source order.

**Flow stays where it is:** run on a function's body at the close of that
function's type check, and skipped if the function raised anything.

**Lowering belongs to type check, and two sites have to move there.** Name
resolution lowers in two places today, both without types to work from:

- `fnCallNameRes` expands `<-` on a value tuple into a whole block, and injects
  its borrow as `borrowMutRef(&lval, unknownType, mutPerm)` — passing
  `unknownType` because no type exists yet.
- `nameUseNameRes` rewrites a bare `a` into `self.a` when the name resolves to a
  method or field, building an `FnCallNode` to do it. `fnCallTypeCheck` performs
  the same rewrite for a different case, so the logic exists twice, in two
  phases.

Both belong in type check, where the types they need exist and where the second
can merge with the copy already there. This is what item 2.6 of the work item
asks for; it does not depend on the rest and lands last.

The rest of this section is the type-check order for each kind, which is where
the interesting sequencing lives. Steps marked **→** are where a demand can leave
and re-enter.

### 10.1 Struct and trait

1. If it is a generic template, stop. Only instances are analyzed.
2. **→** Analyze the base trait.
3. Propagate the base trait's closed-type flags (`SameSize`, `HasTagField`).
   A derived type of a closed trait must be declared in the same module.
4. Insert a mixin field for the base trait at position 0.
5. Walk fields **backwards**: expand mixins in place — splicing in the trait's
   fields and inheriting its methods — and **→** analyze each ordinary field.
   Backwards so that splicing does not invalidate the position.
6. Index the fields. Compute infectious flags from them: `ThreadBound`,
   `MoveType`, `OpaqueType`, `ZeroSizeType`. Identify the tag field.
7. `final` forces `MoveType`; `clone` clears it. Propagate infection up to base
   traits.
8. **Size is now known**, and `Analyzed` is set here — meaning *laid out*, not
   *finished*. Unchanged from today, where it sets `TypeChecked`.
9. **→** Analyze the methods.

Nothing needs a state stronger than step 8's. Everything a consumer can ask a
type for is established by then, including the method set: mixins are expanded
and trait methods inherited in step 5, so the namespace is complete before the
mark is set. What happens at step 9 is each method being analyzed individually,
which is demanded per method and is never a property of the type. The mark should
say so where it is set — today's comment there, "we know enough about the type at
this point", is true but does not say enough for what.

Steps 2 and 5 are where recursion arrives, and step 8 is why a method at step 9
may use its own type by value.

### 10.2 Closed traits, unions and variants

A union is not a node kind. It is a trait carrying `HasTagField` or `SameSize`,
whose variants are structs written inside its body.

Two things follow, and both are simpler than they look:

- **The variant list is complete at parse time.** `parsetype.c` adds each nested
  struct to the trait's `derived` list and assigns its tag number as it parses.
  Nothing has to discover variants during analysis.
- **Analysis never computes a union's size.** `genlSameSizeTrait` determines each
  variant's size and pads to the largest at *generation* time. So "size known" for
  a closed trait means every variant has been analyzed and is itself sized — not
  that a byte count exists.

The rule that a derived type must live in the same module as its closed trait
(step 3 above) is what keeps both of these true.

### 10.3 Function and method

1. If it is a generic template, stop.
2. **→** Analyze the signature: parameters, then return type. **The type is now
   established** — this is rule 3, and it is why mutual recursion works.
3. If there is no body, or it is a trait's default method, stop.
4. Check that a method's `self` parameter matches its enclosing type.
5. Turn an implicit final-expression return into an explicit one.
6. **→** Analyze the body.
7. Run flow analysis on the body — escape, permission, lifetime, move — and skip
   it if this function raised anything, since flow relies on the types that check
   was meant to establish.

### 10.4 Variable, field and constant

1. **→** Analyze the permission, then the declared type.
2. If there is no initializer, the type must have been declared.
3. **→** Analyze the initializer, coercing it to the declared type; if no type
   was declared, **the type becomes the initializer's**. This is rule 6's
   exception: for this shape, steps 1 and 3 are the same step.
4. A global or parameter requires a literal initializer; so does a field default.
5. The type must be concrete and instantiable — rule 5's report site.

### 10.5 Module and program

1. **→** Analyze imports first. Includes are not modules and are not visited
   separately.
2. *Today:* a signature pre-pass over every declaration, then a body pass
   skipping anything marked `FlagSigError`. *Design:* neither. Iterate the
   declarations and analyze each; demand pulls forward whatever it needs.

The driver still visits everything. Demand changes *when* a declaration is
analyzed, never *whether*, so the set analyzed is order-independent even though
the sequence is not, and an unreferenced declaration with an error still fails
the compile.

## 11. What does not change

- **Flow stays terminal, per function.** It already runs at the close of a
  function's type check, and nothing ever demands a flow result from elsewhere.
- **Lowering stays with type check.** The concern it raises is doing it twice,
  and rules 2 and 3 already prevent that: lowering happens inside a declaration's
  body, each body is analyzed once, and nothing outside demands a body. Lowering
  also *cannot* become a pass after a declaration is checked, because it is the
  typing — `fnCallLowerMethod` sets the call's `vtype` from the method it
  selects, and the parent expression needs that type. What does move is the two
  lowerings currently done during name resolution; see section 10.
- **Locals stay source-ordered.** Everything above concerns declarations in a
  namespace. Locals are hooked and unhooked during the walk, and `mut a = a`
  fails at name resolution because the name is not yet in scope.
- **`fnCallTypeCheck` does not get smaller.** See 12.1.

## 12. Flow change by node

### 12.1 Summary

| Node | Today | Under the design |
| --- | --- | --- |
| Program | Name resolution over everything, gate, then type check | Unchanged; the second pass becomes demand-driven |
| Module | Signature pre-pass, then body pass, `FlagSigError` between them | Iterate declarations; demand orders them |
| Struct / trait | Demand-driven already; every re-entry an error | Same flags, same placement; re-entry answered per rule 4 rather than refused |
| Closed trait | Same | Unchanged — variants come from the parser, size from generation |
| FnDcl | Signature then body; recursion works by walk order | Same order, now stated as rule 3 |
| FnSig | Parameters then return type | Unchanged |
| VarDcl (global) | Type checked in source order; inferred type left `unknownType` by the pre-pass | Analyzed on demand; inferred type resolved when first asked |
| VarDcl (local) | Source-ordered within its block | Unchanged |
| FieldDcl | Analyzed during its struct's field walk | Same, and asks its type for a size per rule 4 |
| ConstDcl | Type comes from its value; circularity undetected | Circularity reported per rule 6 |
| NameUse → type | `nameUseTypeCheckType` analyzes the declaration | Unchanged — this is the model |
| NameUse → value | `nameUseTypeCheck` reads `vtype`, whatever is there | Analyzes the declaration, like the type case |
| FnCall | A chain: each test askable only once an earlier step established enough to ask it | Each decision made against a complete declaration, so the chain flattens into a dispatch. See 12.2 |
| Generic / macro | Instance cloned and checked on demand, no depth bound | Same, with rule 7's limit |

### 12.2 What `fnCallTypeCheck` gains

Its front matter is the hardest code in the compiler to read, and the reason is
that it is a *chain*, not a set of branches. Each test can only be asked once an
earlier step has established enough to ask it:

```
is the callee a macro?          -- must be asked first: a macro does not want
                                   its arguments checked before substitution
  analyze the arguments         -- must happen next: generic argument inference
                                   reads their types
    substitute the generic      -- must happen before the callee is analyzed,
                                   because it replaces the callee
      is the callee an overload name?
                                -- must be asked before analyzing the callee,
                                   because analyzing an overload name is an error
        analyze the callee      -- only now is there a type to look at
          is it a type?         -- constructor, or 'init' lookup
            is it a method name? -- rewrite to self.method
              what shape is the receiver?  -- the eight-case dispatch
```

Nothing here is locally justified. Every branch is correct only because of what
ran before it, and none of that is written down. Reordering any two steps breaks
something silently, which is why the sequence has to be read end to end before
any part of it can be changed — and why a reader cannot start in the middle.

**Analyzing on demand makes each decision locally justified**, because the
declaration a name refers to is completely analyzed at the moment the name use
has to decide anything about it. Concretely, in this method:

- The explicit `inodeTypeCheckAny(pstate, &node->objfn)` disappears. Reading the
  callee's type *is* the demand, so "analyze the callee" stops being a step that
  a path can be written without.
- `nameuse->vtype = ((FnDclNode*)nameuse->dclnode)->vtype` on the initializer
  path, and every other direct read of a declaration's type, becomes correct by
  construction rather than because the walk happened to get there first.
- `namespaceFind` for `init`, and the method lookups in `fnCallLowerMethod`, ask
  a namespace that is guaranteed complete — mixins expanded, trait methods
  inherited. Today that guarantee is supplied by nothing but ordering.
- The hand-written `inodeIsError` test and `errorType` assignment become rule 5's
  uniform answer, so a bad callee is handled the same way here as everywhere.
- `genericSubstitute`'s "quit if that finished processing" protocol goes, once an
  instantiation is just another declaration analyzed on demand.

The chain then flattens into a dispatch. Each receiver shape can be read, and
changed, on its own, because it no longer depends on setup performed by the
branches above it. That is what the defect record from this file argues for:
`((StarNode*)fncall)->vtexp` read `methfld` for years because two node structs
overlap and nothing declared what had to hold at that point.

What this does not address is that one node shape means six things — `objfn` plus
`methfld` serves a call, a method call, an operator, an index, a field access and
a type constructor, with `FlagIndex`, `FlagBorrow`, `FlagOperator`, `FlagLvalOp`
and `FlagOpAssgn` to tell them apart afterwards. That is why the macro test has
to ask whether `methfld` is NULL to distinguish `TWO + 1` from a macro call. The
parser knows which syntax it saw and discards the distinction; recovering it is
[[Lexer and Parser]] and [[IR refactor]] work, and it is independent of this.

## 13. Reference

### 13.1 The rules

1. Reaching a name analyzes its declaration before continuing.
2. A finished declaration answers from what it recorded. — `Analyzed`
3. A declaration establishes its own type before analyzing anything that could
   refer back to it. — `Analyzing`
4. A field asks its type for a size; a type still being laid out has none, and
   the field that asked reports it. — `Analyzed` means laid out
5. "No known size" names its cause and its chain.
6. A constant or inferred declaration re-entered before its type exists is
   circular.
7. Instantiation depth is bounded by a limit.
8. Entering a declaration saves and resets the walk context.

### 13.2 What has to be stored

**Nothing new.** `TypeChecking` (`0x4000`) and `TypeChecked` (`0x8000`) already
carry rules 2, 3 and 4, and their placement is already right: `structTypeCheck`
sets `TypeChecked` where the layout settles, which is what makes it answer "size
known".

Two things change, neither of them storage.

**They are read rather than refused.** Today re-entering a declaration that is
`TypeChecking` and not `TypeChecked` reports `ErrorRecurse` whatever was asked.
Instead: asked for a type, answer with it; asked for a size, answer that there is
none, and let the field that asked report rule 5. Rule 6 needs no flag either —
it is "asked for a type, and the type is still `unknownType` while under
analysis".

**They are set on every declaration**, not on type nodes and modules only. The
bits carry no other meaning in node `flags` for any declaration family, so this
costs nothing.

Their names should change with the work. `TypeChecking` and `TypeChecked` assume
the marked thing is a type, and most declarations are not — `Analyzing` and
`Analyzed`, or any pair that says *analysis*. That belongs with item 1, which is
already the change that stops calling this work type checking.

The flags live on the node, not the name: generic and macro instantiation clones
declarations and each clone is legitimately unanalyzed, which is why
`cloneStructNode` already clears `TypeChecked | TypeChecking`.

Deliberately not added, and why:

| Considered | Would give | Why not now |
| --- | --- | --- |
| a `SizeKnown` field on `ITypeNodeHdr` | "laid out" separate from "methods checked" | `TypeChecked` at the layout point already says it. Nothing asks for "methods checked". Costs eight bytes on every type node. |
| a `Failed` state per declaration | suppressing a repeated diagnostic (the second half of rule 5) | New state plus a new test at every site that reports. The cascade it fixes is no worse than today's. |

The eight type properties crowded into `flags` next to per-family bits —
`MoveType`, `ThreadBound`, `OpaqueType`, `ZeroSizeType`, `TraitType`, `SameSize`,
`HasTagField`, `NullablePtr` — would be better in `ITypeNodeHdr`, and their being
in `flags` is why claiming a bit is hazardous at all: `0x0040` once collided with
`HasTagField` and silently stopped type checking every tagged union. That is
[[IR refactor]]'s call, and this work neither needs it nor makes it worse.

### 13.3 What this deletes

- `modTypeCheck`'s signature pre-pass, and with it `FlagSigError`.
- The `unknownType` checks that cope with a half-analyzed declaration, as
  distinct from those that mean genuine inference.
- `ErrorRecurse` as a blanket refusal on the layout path, replaced by
  `ErrorNoSize`.
- `ErrorInvType`'s "must be concrete and instantiable" for a field and for a
  variable, also replaced by `ErrorNoSize`. This narrows the compiler's most
  overloaded code by two uses.
- Nothing about name resolution or its gate. Both stay exactly as they are —
  see section 10.

### 13.4 What was measured, and how to re-measure

Every claim about current behaviour above was produced by running the compiler.
The trace in section 2 came from temporarily printing declaration-level entry to
`inodeNameRes`, `inodeTypeCheck` (including its early return when the work is
already done) and the `blockFlow` call in `fnDclTypeCheck`, then reverting. It
takes a few minutes and is worth repeating rather than trusting this note after
the code has moved.

Measured defects this design closes, none of which has a test scenario yet:

| Defect | Section |
| --- | --- |
| Forward reference to an inferred global fails | 1 |
| A linked list, and both mutual-reference forms, are refused | 5 |
| One unsized root reports once per level, naming no cause | 6 |
| A circular constant compiles clean and misreports at the use | 7 |
| A recursive generic crashes the compiler | 8 |

### 13.5 Settled, so that they are not reopened by accident

| Question | Answer | Where |
| --- | --- | --- |
| Does the name-resolution gate change? | No. One eager pass, global gate, as today. | 10 |
| Is name binding tracked as its own state? | No. It is the first thing analyzing a declaration does, and nothing else asks. | 10 |
| What new state does this need? | None. Two existing flags, renamed, read rather than refused. | 13.2 |
| What diagnostic carries rule 5? | One new code, `ErrorNoSize`, cause in the message. Rule 6 gets `ErrorCircular`. | 6, 7 |
| Does anything need "complete" beyond "laid out"? | No consumer exists. Do not add one. | 10.1 |
| What is item 2.6 for? | No lowering during name resolution; two sites move to type check. | 10 |

Two of these were settled *against* adding machinery — a `SizeKnown` field and a
`Failed` state — and 13.2 records what each would have bought, so that reopening
them is a decision rather than a rediscovery.

### 13.6 Companion changes outside the compiler

Decisions land in this note; consequences land in the guides and the corpus in
the commit that changes the behaviour, not before — writing them up early would
leave the authoring guide describing a compiler that does not exist.

- **The comment at `structTypeCheck`'s `TypeChecked` assignment** should say what
  the mark guarantees — laid out, method set complete, methods themselves not yet
  analyzed — since the whole scheme rests on its placement.
- **Two new `ErrorCode`s**, `ErrorNoSize` and `ErrorCircular`, in *both*
  `src/c-compiler/shared/error.h` and `test/codes.toml` — the runner validates
  one against the other before any case runs. Next free is 1067; never renumber.
  `python test/run.py --bless-codes` regenerates the table.
- **Scenarios asserting `ErrorInvType` for "must be concrete and instantiable"**
  move to `ErrorNoSize`. `--bless` shows exactly which.
- **The five defects in 13.4 need scenarios.** None has one. Each lands with the
  change that fixes it, and two of the five are what prove the new codes.
- **[Test Suite](Test%20Suite.md) needs no change to its stage rule.** Name
  resolution and its gate are unchanged (section 10), so a scenario still holds
  errors from one stage only.

### 13.7 Hazards for the implementation

- **Diagnostics come out in dependency order**, not source order. Per-line
  annotations in the test suite are unaffected; anything asserting a count or a
  sequence is not.
- **The suite cannot assert an absent check.** Where a rule is unenforced the
  corpus records it by establishing the opposite, so a scenario that starts
  failing may be one this work correctly invalidated.
- **`assert(0)` is a no-op** in the release build: 23 sites compile to nothing
  under `/DNDEBUG` and fall through into the next switch case. Converting error
  paths touches the same code; what should replace them is [[Compiler]]'s.
- **Node `flags` bits are not one namespace.** Check every declaration family
  before claiming a bit, not just the type block.
- **Confirm no *expression* node is reachable twice within one declaration's
  walk.** A declaration-level flag would not see it. `inodeTypeCheck` documents a
  *type* node in that position — a match pattern and the variable it declares
  share one — so the question is whether an expression equivalent exists.
- **Clone correctness stays its own concern.** The defect that type checked every
  generic instance method twice was a bad `memcpy` in `cloneStructNode`, which no
  analysis order would have caught.
