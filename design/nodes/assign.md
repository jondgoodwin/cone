`AssignNode` is where most of Cone's mutability and ownership enforcement
actually lands — not in type check, but in **flow**. Assignment is also an
expression: its value is the rval's.

**At a glance.** Built by `parseAssign`. Name resolution walks both sides. Type
check dispatches on whether each side is a tuple — a two-by-two, hence four
helpers — and coerces. Flow does the real work: write permission, initialization
tracking, move-or-copy, borrow lifetime, and the `FlagFirstAssign` marker
generation depends on. Generation stores, releasing the previous value first.

*Provenance: read from source; the tuple-return defect in Flow was measured
against emitted LLVM IR.*

## Shape

| Field | Meaning |
| --- | --- |
| `lval` | assignment target |
| `rval` | value assigned |
| `assignType` | `NormalAssign` or `LeftAssign` |
| `vtype` | **the rval's type** — an assignment is usable as an expression |

`LeftAssign` (`:=`) differs only at generation, where the node's value is the
target's content *before* the store rather than after.

Operator-assignment (`+=`) is **not** this node. `parseOpEq` builds an
`FnCallNode` with `FlagOpAssgn`; `fnCallOpAssgn` and `fnCallOpEqMethod` lower it
to the base operator's method. `<=>` is a `SwapNode`, its own thing.

## Parse

`parseAssign` is the loosest binding level and does **not** loop — it recurses
into `parseAnyExpr` for the right-hand side, so assignment is
**right-associative**. It handles `=`, `:=`, `<=>` and every op-assign form.

## Name resolution

`assignNameRes` walks `lval` and `rval`. Nothing else.

## Type check

`assignTypeCheck` checks the lval, then dispatches on whether each side is a
`VTupleTag`. **That two-by-two is why there are exactly four helpers:**

| lval | rval | Helper | Means |
| --- | --- | --- | --- |
| single | single | `assignSingleCheck` | ordinary assignment |
| tuple | tuple | `assignParaCheck` | parallel assignment, element by element |
| tuple | single | `assignMultRetCheck` | a call returning several values, destructured |
| single | tuple | `assignToOneCheck` | one target, several values |

Then `vtype` becomes the rval's type.

**Type check does not check mutability.** It coerces and it types. Everything
about whether the write is *allowed* is flow's.

## Flow

`assignFlow` mirrors the same two-by-two, and the work is in `assignlvalrtype`,
which every path calls. Per assignment:

1. **The `_` placeholder short-circuits.** An anonymous lval swallows the value
   and returns early.
2. **Write permission.** `iexpGetLvalInfo` walks the lval to its variable,
   computing the effective permission — through derefs, indexes and field
   accesses, taking the reference's permission where one is crossed. Without
   `MayWrite`, `ErrorNoMut`. The exception is a variable that holds nothing yet,
   which is how an `imm` local gets initialized once.
3. **Initialization tracking.** Set `VarInitialized`, clear `VarMoved`.
4. **`FlagFirstAssign`** on the lval's name-use node when the variable held
   nothing before. Generation reads this to *skip* releasing a previous value
   that never existed. It has to be a per-site flag because flow state is a
   running summary over the whole function — only the assignment site itself can
   carry it.
5. **Borrow lifetime.** When both sides are references and the lval is a borrow,
   `lvalscope < rvaltype->scope` is `ErrorInvType`, "lval outlives the borrowed
   reference you are storing". A slice carries the same scope as a single
   reference, so both tags are subject to it.

Then `assignSingleFlow` calls `flowHandleMoveOrCopy` on the rval: a move type
deactivates its source; an lvalue read of a counted reference gets a `+1`.

**`assignMultRetFlow` calls `assignlvalrtype` only** — no move-or-copy at all,
so the holders a destructuring creates are never counted. `flowScopeDealias`
walks a `VTupleTag` return element by element and exempts each from release, so
the return side does not free what it is handing back. The caller-side hole is
what remains.

## Generation

`genlExpr`'s `AssignTag` case handles `LeftAssign` (evaluate the target's
content first, that is the node's value) and tuple destructuring on either side
via `extractvalue`, then `genlStore`.

`genlStore` **skips a store to the anonymous name entirely**, and otherwise
releases the lval's previous value before overwriting — but only for an
`rc`-region reference, and only when `FlagFirstAssign` is absent.

## Hazards

- **Mutability is enforced in flow, not type check.** Looking for `ErrorNoMut`
  in `*TypeCheck` will not find it.
- **A function that fails its flow gate gets no mutability checking at all**,
  and no `FlagFirstAssign` — so generation then decrements an uninitialized
  count on first assignment. Safe only because generation does not run when
  errors were reported.
- **`assignMultRetFlow` does no move-or-copy.** Ownership is unaccounted for on
  that one path; the other three call it.
- **`+=` is not an `AssignNode`.** It is an `FnCallNode`. Code matching on
  `AssignTag` to find "all writes" misses every op-assign and every swap.
- **`iexpGetLvalInfo` and `iexpIsLval` are different questions.** The first
  computes permission and scope; the second only answers whether something can
  be a target. Flow uses a third, `flowIsLvalRead`.
- **Assignment is right-associative** — `a = b = c` is `a = (b = c)`, and works
  because assignment is an expression.

## What lives elsewhere

- The four type check helpers and why four: [Type Check Reasoning](../phases/type-check-reasoning.md), "Tuples and multi-value assignment"
- Move-or-copy, and what the count counts: [Flow Analysis](../phases/flow.md), "Moves and counting"
- Where else borrow lifetimes are enforced: [Flow Analysis](../phases/flow.md)
- Operator lowering: [fncall](fncall.md)
