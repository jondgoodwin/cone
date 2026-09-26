Six literal forms across three source pairs: `nil`, integer, float and string in
`literal.c`; the array literal in `arraylit.c`, which **shares its node with the
array type**; and the type literal in `typelit.c`, which **shares its node with
a call**.

**At a glance.** The parser builds them without deciding types. Name resolution
resolves each literal's type name, and decides whether `[…]` is an array type or
an array literal. Type check sizes a string, checks array elements against each
other, and reorders a type literal's fields. Flow accounts for the values a
composite literal takes ownership of. Generation emits constants where it can.

*Provenance: read from source.*

## Shape

| Node | Tag | Payload |
| --- | --- | --- |
| `NilLitNode` | `NilLitTag` | none; `vtype` is a fresh void node |
| `ULitNode` | `ULitTag` | `uintlit` — also carries `true`/`false` and tuple element indices |
| `FLitNode` | `FLitTag` | `floatlit` |
| `SLitNode` | `StringLitTag` | `strlit` pointer into the lexer's arena, plus `strlen` |
| `ArrayNode` | `ArrayTag` **or** `ArrayLitTag` | `dimens`, `elems` |
| type literal | `TypeLitTag` | an `FnCallNode` — `args` are the field values |

**`FlagUnkType`** is set only by `newULitNode`, only when the lexer gave no type
suffix. **There is no float equivalent** — a suffix-less float defaults to `f32`
concretely, and the lexer refuses a float literal past its type's range
(`ErrorFloatRange`; [Parse](../phases/parse.md)).

**The array node serves both a type and a literal.** `[3; i32]` and `[3; 7]`
have the identical shape — `dimens` `[3]`, `elems` `[i32]` or `[7]`. The list
form `[a,b,c]` has empty `dimens`. The parser deliberately does not decide.

## Parse

Literal tokens map straight to constructors. `parseArrayLit` gathers
comma-separated expressions, and **if a `;` follows, swaps** — what it gathered
becomes `dimens` and a fresh list is gathered into `elems`.

A type literal is not built as one: `parseSuffix` builds an `FnCallNode` with
`FlagIndex`, and `parseArg` wraps `name: value` in a `NamedValNode`. Whether
`Point[1,2]` is an index, an instantiation, or a construction is type check's.

## Name resolution

`litNameRes` resolves the literal's `vtype` name use — turning the `NameUseNode`
naming `i32` or `f64` into a resolved one. A string literal's `vtype` is still
`unknownType`, so this is a no-op for it.

**`arrayNameRes` is the one retag site**: `ArrayTag` becomes `ArrayLitTag` when
`elems[0]` is **not** a type node. Decided by the first element alone. The one
exception is a generic's template, where `[2; T]` is a literal until
`cloneArrayNode` decides again on the substituted element
([generic](generic.md)).

`namedValNameRes` resolves the *value* only — the name is deliberately not
bound, because it is matched against a field by symbol later.

## Type check

**An untyped integer literal takes the number type it is wanted as, wherever it
meets it, and no other literal is context-typed.** `litAdoptNumberType` is the
one rule: a `ULitTag` carrying `FlagUnkType` against an integer type takes that
type and drops the flag; against a float type it is replaced by an `FLitNode`
holding the full 64-bit value, read as signed, rounded once at the target's own
precision. `litTypeCheck` applies it when `expectType` is a number type, and
`iexpCoerce` applies it to a literal that reaches coercion still untyped — a
call's argument, which is type checked before its callee is resolved, and a
struct literal's field. Every other literal is typed by
`itypeTypeCheck(&node->vtype)` alone.

**`Bool` is the one number type it refuses.** `Bool` is a 1-bit unsigned, so it
answers `UintNbrTag` like any other, but its only values are `true` and `false`
and a literal reaches it the way every other number does — through the `isTrue`
coercion in [Type Check Reasoning](../phases/type-check-reasoning.md),
"Coercion", which makes any non-zero value true. `litAdoptNumberType` returns 0
for it, so both of its callers fall through to that coercion and every position
agrees. `typemgmt_success` pins all of them — initializer, assignment, argument,
return value, struct-literal field, `if`, `not`, `and` and `or` — each with a
value whose low bit is 0, because `1` and `-1` read the same either way.
`true` and `false` are built carrying `Bool`, never `FlagUnkType`, so the rule
never reaches them.

**Adopting the type is what builds the constant at the right width.** The
alternative, converting from the `i32` default, materializes the constant at 32
bits first and widens what is left, which silently drops every bit above the low
32 — `i64`'s maximum stored as `-1`, `u64`'s as `4294967295`, `i64`'s minimum as
`0`, and `5000000000` as `705032704` on its way to an `i64` parameter or an
`f64`. `typemgmt_success` pins the cases that tell the two apart, in every
position a literal meets a type: initializer, assignment, argument, return value
and struct field, for `i64`, `u64`, `f32` and `f64`.

**`FlagUnkType` is also read by `iexpMatches`**, which returns `ConvSubtype` for
an untyped integer literal against any number type. `iexpCoerce` adopts before
it asks, so this answers only the callers that ask without coercing: overload
resolution, struct field matching and the branch meet. It deliberately ignores
subtype direction "for user convenience".

**A literal nothing typed keeps its `i32` default, and must fit it.** Some
literals are reached by neither `litTypeCheck`'s `expectType` nor `iexpCoerce`:
the branches of an `if` or a block passed as an argument, which the call checks
before it knows the parameter's type, and every literal whose type comes from
nothing but itself — a variable, global or constant declared without a type, an
array literal's elements. Such a literal still carries `FlagUnkType` when it is
generated, so the `i32` is final, and `litCheckDefaultRange`, called from
`genlExpr`, refuses one whose value does not fit it (`ErrorLitRange`) rather than
materializing it at 32 bits — `wantI64(if v {5000000000;} else {1;})` passed
`705032704`. It is checked there because generation is the one place every such
literal is reached, in a body, a global's initializer and a constant's value
alike, after everything that could have typed it has run.
`typemgmt_genllvm_litrange` pins each position.

⚠ **Nothing asks whether a value fits a type it was given.** `mut n u8 = 300`
and `300u8` store `44` and `mut n i32 = 3000000000` stores `-1294967296`, all
silently [differs]. See the hazard below for why the check is not simply a
comparison.

`slitTypeCheck` sets a string's type to an array of `u8` sized from `strlen`. A
string literal is also an lval.

**Array literal** — two entry points. `arrayLitTypeCheck` is the normal one and
requires the fill dimension to be a literal constant.
`arrayLitTypeCheckDimExp` is called directly by `allocateTypeCheck` and is the
**only** path permitting a run-time element count.

- **Fill form**: one dimension only; a `ULitTag` dimension is forced to `usize`;
  exactly one fill value.
- **List form**: not empty; the elements settle on one type by folding, the same
  way an `if` folds its branches — the first successfully typed element sets the
  type in common, and each later one either matches it or meets it at a
  supertype found by `itypeFindSuper`. An element already reported bad
  contributes nothing. Where a supertype was reached, a second pass coerces
  every element to it, because the array's element type and the values in it
  would otherwise disagree about size. So `[Circle[..], Rect[..]]` is an array
  of their enum, while `[1, 2u8]` is still refused — signed and unsigned have
  no type in common.

Every diagnostic path sets `errorType`, so the literal never leaves the pass
untyped.

**Type literal** — `typeLitTypeCheck` requires a concrete type, then dispatches
to a struct or a number check. `typeLitStructReorder` walks the struct's fields
in declaration order and rewrites `args` to match: a `NamedValNode` is moved
into position; a missing field takes its default; a field flagged `IsTagField`
gets the variant's `tagnbr` **inserted** — which is how a variant's discriminant is
materialized, and why a constructor never writes one. Inserted where the field
sits, so an enum that placed its discriminant itself is served by the same walk. A
private field may not be given a value from outside the type, by position or by
name; leaving it to its default is allowed, because the default is the type's own
value rather than one the literal gives. Then a
positional pass runs each value through `iexpCoerce` against its field's type: **a
field takes a value on the same terms a variable initializer does**, a variant
standing in for its enum included.

`litIsLiteral` is the compile-time-constant predicate the global, parameter and
field-default rules use. It accepts a use resolved to a `ConstDclTag`, which is
what makes `imm g i32 = K` legal.

## Flow

Scalar literals are no-ops. The two composites take ownership of values:

`typeLitFlow` unwraps each `NamedValNode` and move-or-copies its value —
a field initialization is accounted exactly as a call argument would be.

`arrayLitFlow`:

- **List form**: every element gets its own holder, so each is move-or-copied.
- **Fill form**: a **move value may not be repeated** — `ErrorBadFill`,
  unconditionally, without trying to prove the count is 1. A counted reference
  may be, but the count must be a compile-time constant within `int16_t`, else
  `ErrorFillCount`. The amount is **n for an lvalue, n−1 for a temporary** — an
  lvalue still holds its own reference after the read, a temporary hands over
  the one it was born with.

The two codes are deliberately distinct: `ErrorBadFill` is a language rule;
`ErrorFillCount` is an implementation limit that should disappear when a fill
lowers to a loop.

## Generation

Scalars are LLVM constants; `nil` is `undef` of the empty struct. An integer
literal still carrying `FlagUnkType` is range-checked against its `i32` default
first (see Type check).

**An array literal is emitted as a constant when every element is constant**,
and otherwise as an `undef` plus a chain of `insertvalue`. The constant form is
kept where possible because it is cheaper and it is **the only form usable
outside a function body**.

A type literal is the same `insertvalue` chain, with one special case: a
**nullable-pointer** enum has no struct at all, so the literal is either a null
pointer or the payload alone, with the tag discarded.

**A string literal emits a fresh global on every occurrence** — there is no
interning, and constant merging is not in the pass list.

**A string literal's global ends in a NUL its type does not count**, for C
compatibility: `"hello"` is a `[5; u8]` and its global a `[6 x i8]`. The
`StringLitTag` case of `genlAddr` recasts the global's address to a pointer to
the literal's own array type, so a load, a copy and a slice's count all see the
text's bytes only; the terminator is reachable only through a pointer handed to
code that reads to it.

**So does a global variable initialized from a string literal.** It is not a
copy: its storage is the initialized data, so like the literal it gets the
terminating zero after the text, uncounted — `imm g = "hello"` and
`mut g [5; u8] = "hello"` are each a `[5; u8]` stored in a `[6 x i8]`, and a
`static` in a function body the same. `genlGloVarName` creates the longer
global and keeps in `llvmvar` its address recast to a pointer to the
variable's type, which is all any use sees; `genlGloVarGlobal` recovers the
global itself for its initializer, COMDAT, constness and linkage. Every Cone
store to a `mut` one is a store of the whole `[N; u8]` or an index the bounds
check holds below `N`, so the NUL is never overwritten — only a raw pointer,
which reads or writes past `N` at its own risk, reaches it. A global array
initialized from anything but a string literal, and a local copy of either,
is its type exactly and has no terminator.

## Hazards

- **Only an integer literal is context-typed.** Every other literal is still
  adapted by coercion afterward, so `expectType` reads as more general than it is.
- **`FlagUnkType` is a permission to convert, not a range check.** Nothing asks
  whether the literal's value fits the type it is adopted as; only the `i32`
  default is checked, and only at generation.
- **A negated literal cannot be told from a large positive one**, which is what
  makes that range check harder than a comparison. `parsePrefix` folds unary `-`
  into the literal by negating `uintlit` in place, two's complement, and records
  nothing — so `-1` and `18446744073709551615` are the same node. A check can
  decide most cases by sign extension (a value fits a signed *N*-bit type when
  bit *N*-1 repeats to the top), which is how `litCheckDefaultRange` reads the
  `i32` default, but `18446744073709551615` is indistinguishable from `-1` and
  passes it, as `mut n i64 = 18446744073709551615` would have to. Deciding that
  one needs the parser to record that it negated. Its message quotes the
  literal's digits as written for the same reason: printed from the value,
  either a negated literal or one above `i64`'s maximum would read wrong.
- **An array literal is not given the expected type.** `inodeTypeCheck`
  dispatches `arrayLitTypeCheck` without `expectType`, where the `BlockTag` and
  `IfTag` arms beside it pass it through. So the elements fold among themselves
  and the result is matched against the declared type afterward rather than
  coerced to it — which is why `imm a [4; u8] = [4, 10, 12, 40]` needs the `u8`
  suffix on every element, and `imm a [3; i64] = [1, 2, 3]` is refused.
- **`TypeLitTag` has no arm in `inodeTypeCheck`**, so it falls to the default,
  which reports `ErrorUnreachable` and stops. `typeLitNameRes` *is* dispatched,
  so an already-retagged literal in a cloned generic body can be name-resolved
  but not re-checked — and if that path is live, the compile aborts rather than
  skipping the check.
- **`cloneArrayNode` clones `elems` but shares `dimens`**, so a cloned fill
  literal shares its dimension node with the original.
- **A fill dimension that cannot be resolved silently becomes 0**, so a
  run-time-sized allocation's type claims a zero-length array. That path
  generates through the run-time fill loop, not the constant path.
- **`typeLitStructReorder`'s error recovery inserts fake zero values** typed as
  the field's type, so the coercion pass that follows passes on a value that is
  not real.
- **`newFakeULitNode` is dead code.**

## What lives elsewhere

- How an untyped literal is adapted: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- The literal-initializer rules for globals, parameters and field defaults: [vardcl](vardcl.md)
- What a fill literal's alias count means: [Flow Analysis](../phases/flow.md), "Moves and counting"
- The nullable-pointer enum: [struct](struct.md) and [Generation](../phases/generation.md)
- Where a type literal is retagged from a call: [fncall](fncall.md)
