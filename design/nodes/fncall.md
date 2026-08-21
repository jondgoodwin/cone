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
| `methfld` | the member after `.`, or the operator's interned name — `MbrNameUseTag`, or a `ULitTag` for a tuple element index, or NULL |
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

`fnCallNameRes` resolves `objfn` and each argument. That is all.

**It never resolves `methfld`** — its own comment says so, and `inodeNameRes`
lists `MbrNameUseTag` in the do-nothing arm. Selecting a member needs the
receiver's *type*, which does not exist yet. Resolving `objfn` first is what
lets `itypeIsGenericType` recognize an unlowered `Box[i64]` as a type, which the
type-versus-value retags elsewhere depend on.

## Type check

`fnCallTypeCheck` in three stages. This is the map worth carrying.

**Stage 1 — syntax, before the callee is known.**
Macro call (only when `methfld` is NULL — with it set, the name is a receiver
and expands like any other value); `<-` on a value tuple, which becomes a block
of applications; then every argument is checked; then generic substitution,
which may finish the node entirely.

**Stage 2 — make the callee knowable.**
Check `objfn`, *unless* it names an overload set — that one path deliberately
skips the ordinary name-use check, which leaves `nameUseTypeCheck` free to
reject an overload name everywhere else. Bail if `objfn` is already marked
`errorType`. Then rewrite the shapes that are not yet calls:

- **A type**, with `FlagIndex` → retag `TypeLitTag` and hand to
  `typeLitTypeCheck`.
- **A type**, without → rewrite the name to the type's `init` method.
- **A bare method or field name** (`FlagMethFld`, unqualified) → rewrite to
  `self.method`, synthesizing a resolved `self` from parameter 0.
- **An overload set** → `fnCallLowerOverloadFn` picks the concrete candidate.
- **`FlagLvalOp`** → borrow the receiver as `&mut`, or hand an operator-assign
  on a method type to `fnCallOpAssgn`.

**Stage 3 — dispatch on the receiver's type tag.**

| Receiver type | Goes to |
| --- | --- |
| `FnSigTag` | `fnCallFnSigTypeCheck` — a plain call |
| struct, number | fill in `()`/`[]`/`&[]` as `methfld` if absent, then `fnCallLowerMethod` |
| `TTupleTag` | `fnCallLowerIntField` — element by literal index |
| `ArrayTag` | `fnCallArrIndex`, only under `FlagIndex` |
| `ArrayRefTag` | index, or `fnCallLowerPtrMethod` against `arrayRefType` |
| `RefTag` | function-by-ref, array index, or `fnCallLowerPtrMethod` then `fnCallLowerMethod` |
| `VirtRefTag` | `fnCallLowerPtrMethod`, else set `FlagVDisp` and `fnCallLowerMethod` |
| `PtrTag` | the pointer's own operators first, then the value's fields and named methods |

### Selecting a candidate

`fnCallLowerMethod`: look the name up in the receiver's namespace, check
visibility against **the spelling the caller used**, then `iNsTypeFindMethod`,
which tests every candidate with `fnSigViableCall` and **alters nothing**. One
viable candidate is a match; two are `OverloadAmbiguous`. There is no ranking.

Then the node is rewritten: the receiver is inserted at `args[0]`, `methfld`'s
name-use node is repurposed into `objfn` pointing at the selected function,
`methfld` becomes NULL, `vtype` becomes the signature's return type, and
`fnCallFinalizeArgs` coerces each argument to its parameter and appends
defaults. **Coercion happens once, after selection** — which is what lets
selection be a pure filter.

A field, rather than a method, retags the node `FldAccessTag` and injects a
deref on the receiver if needed.

Two asymmetries that are deliberate:

- **The deref retry.** A receiver held through a reference still satisfies a
  method declaring `self` by value: `derefInject`, then select again. It runs
  *only* when no candidate matched at all, so a real ambiguity is still an
  ambiguity. Nothing is borrowed on the receiver's behalf — `self &mut` stays
  out of reach of a value.
- **An operator on a pointer does not reach through.** `p + 2` offsets the
  pointer; `p * 2` is an error rather than becoming `(*p) * 2`. `FlagOperator`
  on a pointer receiver is what skips the retry.

### What the node becomes

`FnCallTag` (a real call), `FldAccessTag`, `ArrIndexTag`, `TypeLitTag`, a
generic instance, a macro expansion, or a block of applications. Anything after
type check that still sees an un-lowered `FnCallTag` with `methfld` set is
looking at a bug.

## Flow

Three entry points, by what the node became:

- `fnCallFlow` — for each argument: `flowLoadValue`, then
  `flowHandleMoveOrCopy`. Arguments are moved or copied into the callee.
- `fnCallArrIndexFlow` — the receiver and the index.
- `fnCallFldAccessFlow` — the receiver only.

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
