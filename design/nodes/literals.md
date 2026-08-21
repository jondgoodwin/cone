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
concretely.

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
`elems[0]` is **not** a type node. Decided by the first element alone.

`namedValNameRes` resolves the *value* only — the name is deliberately not
bound, because it is matched against a field by symbol later.

## Type check

**`litTypeCheck` accepts `expectType` and ignores it.** Its whole body is
`itypeTypeCheck(&node->vtype)`. A literal is not context-typed here.

**`FlagUnkType` is consumed in exactly one place** — `iexpMatches`, which
returns `ConvSubtype` for an untyped integer literal against any number type.
So the literal keeps its default `i32` and the *coercion* machinery adapts it by
wrapping it in a conversion; the node is never rewritten in place. Note the test
is on the flag only: **it never asks whether the value fits the target type**,
and it deliberately ignores subtype direction "for user convenience".

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
  of their union, while `[1, 2u8]` is still refused — signed and unsigned have
  no type in common.

Every diagnostic path sets `errorType`, so the literal never leaves the pass
untyped.

**Type literal** — `typeLitTypeCheck` requires a concrete type, then dispatches
to a struct or a number check. `typeLitStructReorder` walks the struct's fields
in declaration order and rewrites `args` to match: a `NamedValNode` is moved
into position; a missing field takes its default; a field flagged `IsTagField`
gets the variant's `tagnbr` **inserted** — which is how a union variant's
discriminant is materialized. A `_`-prefixed field may not be given a value from
outside the type. Then a positional pass requires **exact** type equality per
field, again with no coercion.

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

Scalars are LLVM constants; `nil` is `undef` of the empty struct.

**An array literal is emitted as a constant when every element is constant**,
and otherwise as an `undef` plus a chain of `insertvalue`. The constant form is
kept where possible because it is cheaper and it is **the only form usable
outside a function body**.

A type literal is the same `insertvalue` chain, with one special case: a
**nullable-pointer** union has no struct at all, so the literal is either a null
pointer or the payload alone, with the tag discarded.

**A string literal emits a fresh global on every occurrence** — there is no
interning, and constant merging is not in the pass list.

## Hazards

- **`litTypeCheck` ignores `expectType`**, so the parameter reads as though
  literals are context-typed. They are not — coercion adapts them afterward.
- **`FlagUnkType` is a permission to convert, not a range check.** Nothing asks
  whether the literal's value fits.
- **An array literal is not given the expected type.** `inodeTypeCheck`
  dispatches `arrayLitTypeCheck` without `expectType`, where the `BlockTag` and
  `IfTag` arms beside it pass it through. So the elements fold among themselves
  and the result is matched against the declared type afterward rather than
  coerced to it — which is why `imm a [4; u8] = [4, 10, 12, 40]` needs the `u8`
  suffix on every element, and `imm a [3; i64] = [1, 2, 3]` is refused.
- **`ArrayLitTag` has no arm in `inodePrintNode`**, so an array literal
  serializes as `**** UNKNOWN NODE ****` in an `--ir` dump.
- **`TypeLitTag` has no arm in `inodeTypeCheck`**, so it falls to the `assert(0)`
  default — a silent no-op under `NDEBUG`. `typeLitNameRes` *is* dispatched, so
  an already-retagged literal in a cloned generic body can be name-resolved but
  not re-checked.
- **`cloneArrayNode` clones `elems` but shares `dimens`**, so a cloned fill
  literal shares its dimension node with the original.
- **A fill dimension that cannot be resolved silently becomes 0**, so a
  run-time-sized allocation's type claims a zero-length array. That path
  generates through the run-time fill loop, not the constant path.
- **`typeLitStructReorder`'s error recovery inserts fake zero values** typed as
  the field's type, so a later exact-type check can pass on a value that is not
  real.
- **`newFakeULitNode` is dead code.**

## What lives elsewhere

- How an untyped literal is adapted: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- The literal-initializer rules for globals, parameters and field defaults: [vardcl](vardcl.md)
- What a fill literal's alias count means: [Flow Analysis](../phases/flow.md), "Moves and counting"
- The nullable-pointer union: [struct](struct.md) and [Generation](../phases/generation.md)
- Where a type literal is retagged from a call: [fncall](fncall.md)
