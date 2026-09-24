`FnCallNode` is the compiler's busiest node. One shape — `objfn`, `methfld`,
`args` — serves function calls, method calls, operator applications, field
access, array indexing, type constructors, initializers, generic
instantiation and macro calls. Type check is where they separate.

**At a glance.** Built by `parseexpr.c` from several unrelated syntaxes. Name
resolution binds `objfn` and the arguments and **deliberately leaves `methfld`
alone**. Type check dispatches on the receiver's type, selects a candidate, and
retags the node into what it actually is. Flow moves or copies each argument.
Generation emits a call, a GEP, an `extractvalue`, or an intrinsic.

Read this before touching `fnCallTypeCheck`, which is the largest function in
the compiler — and the one most often proposed for splitting, because giving
these syntaxes distinct node shapes would remove most of what stage 3 has to
re-derive.

*Provenance: read from source, end to end through every dispatch arm.*

## Shape

| Field | Meaning |
| --- | --- |
| `objfn` | the callee, or the receiver of a method/field/index |
| `methfld` | the member after `.`, or the operator's interned name — a `NameUseNode` bound to nothing until the member is selected, or a `ULitTag` for a tuple element index, or NULL |
| `args` | argument list, or NULL. **The receiver is inserted at index 0** when a method is selected |
| `vtype` | the call's result type, established by lowering |

**`methfld` is what tells a call from an operator or member access.** `TWO + 1`
is objfn `TWO`, methfld `+`, one argument — the same shape `p.sum()` builds. The
flags carry the rest of what the source said:

| Flag | Means |
| --- | --- |
| `FlagIndex` | arguments were written in `[]` |
| `FlagBorrow` | this link is part of a borrow chain |
| `FlagVDisp` | virtual dispatch |
| `FlagLvalOp` | the operator needs an lval receiver (`++`, `--`, `<-`, op-assign) |
| `FlagOpAssgn` | an operator-assignment such as `+=` |
| `FlagOperator` | **the source wrote an operator, not a named member access** |

`FlagOperator` exists solely because the two are otherwise indistinguishable
after parsing, and one dispatch decision depends on knowing which — see Hazards.

## Constructors

| Function | For |
| --- | --- |
| `newFnCallNode` | a plain call |
| `newFnCallOpname`, `newFnCallOp` | an operator application; **both set `FlagOperator`** |
| `newFnCallOpnameLower` | an operator application positioned on an existing node rather than on wherever the lexer has reached; also sets `FlagOperator` |
| `newFnCallLower` | a plain call, positioned the same way. **Does not set `FlagOperator`** |
| `cloneFnCallNode` | instantiation |

Use a `*Lower` form for anything synthesized after its construct was parsed —
the lexer has moved on, and a node built with the plain constructors points at
end of file.

## Parse

Built from: a call `f(a)`, an index `a[i]`, a member access `a.b`, every binary
and unary operator, a generic instantiation `Box[i64]`, `?T` for `Option[T]`,
and a type constructor `Point[1,2]`. `parseDotCall`, `parseSuffix`, `parseArgs`
and the whole precedence cascade all build this node.

Nothing about which of those it is has been decided yet.

## Name resolution

`fnCallNameRes` resolves `objfn`, collapses the node if that turned out to be a
path, and resolves each argument. Resolving `objfn` first is what lets
`itypeIsGenericType` recognize an unlowered `Box[i64]` as a type, which the
type-versus-value decisions elsewhere depend on.

**It never resolves `methfld` as a member** — selecting a member needs the
receiver's *type*, which does not exist yet — so a member name is a
`NameUseNode` name resolution never meets, since nothing else hands one to
`inodeNameRes`.

### The path collapse

**A period whose left side names a namespace is a path through it, not an
access to a value**: `math3d.Point3`, `Tally.make(1)`, `Tally.made`. The parser
cannot tell the two apart, so `fnCallNameResPath` decides as soon as `objfn` is
bound, and **what it binds to is the whole test** — a `ModuleTag` or a
`StructTag` is a namespace, anything else is a receiver. An operator, a tuple
index and a call with no member name are never paths.

The member is looked up with `namespaceFind` in that namespace, bound, stamped
`FlagQualified`, and the hop disappears:

| | Becomes |
| --- | --- |
| no arguments — `f32.pi`, `mymod.Gadget` | the bound name use, replacing the node outright |
| arguments — `Tally.make(1)` | a plain call: `methfld` moves to `objfn` and becomes NULL |

**Either way the shape handed on is the one an unqualified name of the same
declaration produces**, so nothing downstream learns a path was written. A
chain falls out of that for free: `mymod.Gadget.make(2)` parses innermost
first, so the inner hop has left a resolved type name in `objfn` before the
outer hop looks at it.

**Privacy is checked here**, against `dclInfoGetModule` of the base — so
`modulesyms.Gadget.make` is judged against `modulesyms`, one hop back, which is
the module that owns the type.

**So is a method of an abstraction.** Through a trait or an enum, a member that
is a method — or an overload name with a method among its candidates — is
reported `ErrorAbstractMeth`: the implementers and variants own the only
generated copies ([struct](struct.md), `nodelist`), so the path would name code
that does not exist. A static function through the same path is the
abstraction's own and passes, and the same name through a struct, a variant or
an implementer names that type's copy. `trait-nameres-method-path` pins it.

⚠ **This cannot wait for type check.** Name resolution itself asks `isTypeNode`
of an operand: `&mut mymod.Gadget` and `(mymod.A, mymod.B)` are settled by
`refNameRes` and `ttupleNameRes`, which run after this and need a resolved type
name to look at.

**What it does not reach** is a base that is not a namespace *yet* — an alias,
a number type, a generic instance, a generic parameter. Those arrive at type
check as member accesses and are refused there by stage 2's type receiver.

## Type check

`fnCallTypeCheck` in three stages. This is the map worth carrying.

**Stage 1 — syntax, before the callee is known.**
Macro call (only when `methfld` is NULL — with it set, the name is a receiver
and expands like any other value; a macro *method* named bare is first rewritten
to `self.name`); `<-` on a value tuple, which becomes a block of applications.
Then, for a member access by name that is not an operator, the **receiver is
checked ahead of the arguments** and its type asked what the name binds — an
alias, for a macro method the type holds by folding, is resolved first, and the
receiver shifted to the field it was folded through (`structFoldReceiver`): a
macro method expands here, through `macroMethodTypeCheck`, with its arguments
still unchecked, as a macro's must be. Then every argument is checked; then
generic substitution, which may finish the node entirely.

**Stage 2 — make the callee knowable.**
Check `objfn`, *unless* it names an overload set — that one path deliberately
skips the ordinary name-use check, which leaves `nameUseTypeCheck` free to
reject an overload name everywhere else. Bail if `objfn` is already marked
`errorType`. Then rewrite the shapes that are not yet calls:

- **A type**, with `FlagIndex` → retag `TypeLitTag` and hand to
  `typeLitTypeCheck`.
- **A type**, with a member name → a path the collapse could not take, because
  the base is not a namespace until later: an alias, a number type, a generic
  instance, a generic parameter. `ErrorUnkName`, naming what a path may pass
  through.
- **A type**, with neither → rewrite the name to the type's `init` method.
- **A bare method or field name** (`FlagMethFld`, not `FlagQualified`) →
  rewrite to `self.method`, synthesizing a resolved `self` from parameter 0.
- **An overload set** → `fnCallLowerOverloadFn` picks the concrete candidate.
- **`FlagLvalOp`** → borrow the receiver as `&mut`, or hand an operator-assign
  on a method type to `fnCallOpAssgn`. A receiver that is already a reference
  (`fnCallIsRefReceiver`) is passed as it is, exactly as the reference arm of
  stage 3 takes a named method's receiver: its own permission is what candidate
  selection checks, not the permission of the binding that holds it. The `<-`
  tuple lowering holds such a receiver in its temporary unborrowed for the same
  reason.

  **An operator-assign is routed by what the receiver's type or its referent
  declares** (`fnCallOpAssgnMethodType`), so a reference to a method type goes
  to `fnCallOpAssgn` too, and stage 3's reference arm never sees it. That is
  what makes the derivation an operator-assign is entitled to — `a += b`
  rewritten to `a = a + b` where the type declares no `+=` — reachable through
  a reference as it is by value. Stage 3's arm loses nothing by not seeing it:
  of the operator names, `refType` declares only the identity comparisons,
  `===` and `!==`.
- **`===` and `!==` on a receiver that is neither a reference, a slice, a
  virtual reference nor a pointer** → `ErrorSameNotRef`: identity asks about
  places, and it is not an operator a type declares, so the type's own
  namespace is never asked. `!==` is never derived from anything.
- **`!=` on a type that declares `==` and no `!=`** (`fnCallNeFromEq`) → the
  node is renamed to `==` and a `NotLogicTag` node takes its place in the
  tree, wrapping it; stage 3 then lowers the `==` like any other operator, and
  its answer is coerced to `Bool` (through `isTrue` if need be). The type asked
  is a struct receiver's own, or the struct a reference refers to, through any
  number of references — `!=` on references compares the values, so
  `fnCallLowerRefCompare` lowers the renamed `==` exactly as it would a written
  one, and its diagnostics name `==`. A pointer declares its own `!=`, on the
  pointer, and a slice or a virtual reference refuses it, so none of them asks
  a referent. A type declaring its own `!=` keeps it, an enum's intrinsic pair
  is declared together, and a type declaring neither is reported missing its
  `!=`. A `==` that selects nothing is reported once, under `==`, and the `not`
  carries `errorType` on.

**Stage 3 — dispatch on the receiver's type tag.**

| Receiver type | Goes to |
| --- | --- |
| `FnSigTag` | `fnCallFnSigTypeCheck` — a plain call |
| struct, number | fill in `()`/`[]`/`&[]` as `methfld` if absent, then `fnCallLowerMethod` |
| `TTupleTag` | `fnCallLowerIntField` — element by literal index |
| `ArrayTag` | `fnCallArrIndex`, only under `FlagIndex` |
| `ArrayRefTag` | index; `==`, `!=` or an ordering is `ErrorRefNoCompare`; else `fnCallLowerPtrMethod` against `arrayRefType` |
| `RefTag` | function-by-ref, array index, a comparison to `fnCallLowerRefCompare`, or `fnCallLowerPtrMethod`, then `fnCallLowerTraitMethod` and failing that `fnCallLowerMethod` |
| `VirtRefTag` | `==`, `!=` or an ordering is `ErrorRefNoCompare`; else `fnCallLowerPtrMethod`, else set `FlagVDisp` and `fnCallLowerMethod` |
| `PtrTag` | the pointer's own operators first, then the value's fields and named methods |

**A comparison on a reference compares what it refers to.** A reference reads
as its value everywhere else — `r.x`, `r.method()` — so `==`, `!=` and the four
orderings do too, and `===`/`!==` are what ask whether two references point to
the same place; `refType` and `arrayRefType` declare only those two, as
`EqIntrinsic` and `NeIntrinsic` on the address (on both words, for a slice).
Identity is selected by `iNsTypeFindPtrMethod`, which wants the two operands of
the same type, permission included, so `&i32 === &mut i32` is refused as no
candidate. `fnCallLowerRefCompare`:

- **Both operands must be references.** One side a reference and the other a
  value is `ErrorRefCompareMixed`, rather than read through on one side only, so
  `r == v` never says something `*r == v` does not. The mirror, a value on the
  left and a reference on the right, reaches the value's own operator and is
  refused there as no candidate.
- **A referent that is a pointer or a reference is read through** on both sides,
  and the result compared as it would be by value: a pointer by its own
  operators, a reference by this same function again.
- **A referent whose type declares the operator** is asked first with the
  operands as written, so a method declared for references (`self &`,
  `other &T`) takes them unchanged. Only when no candidate matches are both
  operands dereferenced (`derefInject`, positioned on the comparison) and the
  value's operator selected by `fnCallLowerMethod`, as for `*a == *b`. An
  enum's compiler-declared `==` is reached that way, and so is its refusal,
  `ErrorEnumEquality`, for one whose variants carry fields.
- **Anything else is `ErrorRefNoCompare`**, whose message names `===` for `==`
  and `!=`: a referent with no such operator, a referent with no methods at all
  (an array, a function), and a trait other than an enum, whose comparison would
  be dispatched on the variant and is not built. A slice (whose `==` would
  compare elements) and a virtual reference are refused the same way in their
  own arms.

The permission a reference carries is enforced on the dereference, by flow, so
`==` through an `opaq` reference is `ErrorNoRead` while `===` on it is allowed.

**A raw pointer is the exception**: its operators are on the pointer (see the
deref retry below), so its `==` already asks about places, and `ptrType`
declares `===` and `!==` as synonyms for its `==` and `!=`.

**A method called on a plain reference to a trait dispatches on the variant**,
and `fnCallLowerTraitMethod` is what routes it there. Neither of the trait's own
declarations is callable — an abstract method has no body, and one with a body is
a default that was cloned into each variant — so selecting either left the call
naming a declaration with no symbol, which generation dereferenced as a null.
The route is the one [reftraitvar](../../../../doc/reference/reftraitvar.html)
describes: the tag says which variant, that selects its vtable, and the vtable
holds the method. It is built by coercing the receiver to `&<Trait`, which
already exists and already does the tag lookup, and then dispatching as any
virtual reference does — the compiler writing what a caller could write by hand.

**An open trait has no tag**, so `refvirtMatches` refuses that coercion, and the
refusal is reported rather than left to fail later. The same page states the rule
and the remedy: obtain a virtual reference first. **A field takes none of this**
— it lives in the trait's own layout, a prefix of every implementer, so
`fnCallLowerMethod` reaches it directly.

### Selecting a candidate

`fnCallLowerMethod`: look the name up in the receiver's namespace, check
the visibility of **the binding the name reaches** (`inodeIsPrivate`), resolve
an alias to the method it stands for (`aliasDclResolve` — the binding for a
method the type holds by folding), **type check every candidate not yet
analyzed** (`fnCallDemandCandidates` — a later method of the caller's own type
is not, and its unchecked signature matched no reference receiver; see
[type check](../phases/type-check.md), "Demand"), then `iNsTypeFindMethod`,
which tests every candidate with `fnSigViableCall` and **alters nothing**. One
viable candidate is a match; two are `OverloadAmbiguous`. There is no ranking.
For a folded method the receiver is rewritten before any candidate is tried:
`structFoldReceiver` makes it the access to the field the name was folded
through, reborrowed with a reference receiver's permission, so selection,
borrowing and the permission checks see the receiver the method was declared
for. [struct](struct.md), "Name folding", has the rule.

Then the node is rewritten: the receiver is inserted at `args[0]`, `methfld`'s
name-use node is repurposed into `objfn` pointing at the selected function,
`methfld` becomes NULL, `vtype` becomes the signature's return type, and
`fnCallFinalizeArgs` coerces each argument to its parameter and appends
defaults. **Coercion happens once, after selection** — which is what lets
selection be a pure filter.

A field, rather than a method, retags the node `FldAccessTag` and injects a
deref on the receiver if needed. A folded copy of a field is reached through
the field it was folded through: `fnCallFieldAccess` makes the receiver the
access to that field — an access per hop, root first — and this node the
access to the copy on it, the nesting `p.left.fuel` written out produces. The
copy carries the index, type and permission of the field it stands for, so
generation and the permission read treat the last access as one to that field.

A private member (one not declared `pub`) is granted to a receiver that is the enclosing
method's own `self`, and to an access that a macro method's body wrote on *its*
`self` — the clone carries `FlagSelfRecv`, stamped by `cloneFnCallNode` at
expansion, since by then the receiver is the use site's expression. Every other
receiver gets `ErrorNotPublic`.

Two asymmetries that are deliberate:

- **The deref retry.** A receiver held through a reference still satisfies a
  method declaring `self` by value: `derefInject`, then select again. It runs
  *only* when no candidate matched at all, so a real ambiguity is still an
  ambiguity. Nothing is borrowed on the receiver's behalf — `self &mut` stays
  out of reach of a value.
- **An operator on a pointer does not reach through.** `p + 2` offsets the
  pointer; `p * 2` is an error rather than becoming `(*p) * 2`. `FlagOperator`
  on a pointer receiver is what skips the retry. A reference's comparison is
  the other way round, reading through both operands, which the retry cannot
  do because it dereferences only the receiver — `fnCallLowerRefCompare` does
  it before `fnCallLowerMethod` is reached.

### What the node becomes

`FnCallTag` (a real call), `FldAccessTag`, `ArrIndexTag`, `TypeLitTag`, a
generic instance, a macro expansion, a block of applications, or — for a
derived `!=` — a call wrapped in a `not` that replaces it in the tree. Anything after
type check that still sees an un-lowered `FnCallTag` with `methfld` set is
looking at a bug.

## Flow

Three entry points, by what the node became:

- `fnCallFlow` — for each argument: `flowLoadValue`, then
  `flowHandleMoveOrCopy`. Arguments are moved or copied into the callee.
- `fnCallArrIndexFlow` — the receiver, through `flowLoadThroughRef` because a
  reference to a fixed-size array and a slice are indexed with no dereference
  injected, and the index.
- `fnCallFldAccessFlow` — the receiver only, also through `flowLoadThroughRef`:
  a plain reference's field access had a dereference injected and `derefFlow`
  reads through that, but a virtual reference's did not, so this is where its
  `MayRead` is asked.

**`fnCallFlow` does not flow `objfn`**, so a call through an uninitialized
function-reference variable goes unreported. See Hazards.

## Generation

`genlFnCall` evaluates every argument, then `genlFnCallInternal` picks: a call
through a deref, an indirect call through a reference or pointer value, virtual
dispatch (extract the object and vtable from the fat pointer, `structgep` the
slot, load, call), generator-level inlining for `FlagInline`, an ordinary call,
or an intrinsic.

**The intrinsic switch dispatches on the LLVM type kind of argument 0**, not on
the Cone type — so a mutating intrinsic's receiver, which arrives as an lvalue
pointer, and a non-mutating one's, which arrives as a value, land in the same
branch and are told apart only by which intrinsic it is.

`FldAccessTag` splits on `FlagBorrow`: with it, `StructGEP` the receiver's
address; without it, load the **whole aggregate** and `extractvalue`. Getting
the flag wrong is not a type error.

## Hazards

- **`methfld` is NULL after lowering.** Code written against the parsed shape
  breaks on the lowered one, and both exist during type check.
- **The node's tag is not stable.** It may become `FldAccessTag`, `ArrIndexTag`
  or `TypeLitTag` — all still `FnCallNode` structurally.
- **`args[0]` is the receiver after selection, and was not before.** Anything
  iterating arguments has to know which side of lowering it is on.
- **`FlagOperator` is the only record of what the source wrote.** Lose it and
  `p * 2` silently starts dereferencing.
- **One dispatch arm skips resolving the callee** — the overload-set path — so
  the invariant "objfn is type checked by stage 3" holds in most of the function
  and not all of it.
- **`fnCallFlow` ignores `objfn`.** An uninitialized `&fn` variable called
  through is not diagnosed.
- **`fnCallLowerMethod` returns three values** — 1 lowered, 0 receiver has no
  methods so try another way, −1 already reported. Treating it as a boolean
  produces a duplicate diagnostic.
- **`fnCallArrIndex` must resolve the receiver's type the way its caller did.**
  It is reached only from the array, slice, reference and pointer arms above,
  and its own switch has to agree with that decision. Reading a reference's
  pointee tag raw rather than through `itypeGetTypeDcl` was the bug: `&Alias` to
  a typedef of an array matched neither the array nor the slice arm, so a valid
  index kept `unknownType` and surfaced as a return-type mismatch elsewhere.

## What lives elsewhere

- Overload selection, coercion, and the verdict vocabulary: [Type Check Reasoning](../phases/type-check-reasoning.md), "Calls, methods and overloads"
- Why `methfld` cannot be resolved earlier: [Name Resolution](../phases/name-resolution.md), "Where it stops, and why"
- How the parser builds all these shapes: [Parse](../phases/parse.md)
