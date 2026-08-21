`return`, `break`, `continue` and the injected `blockret` are **one struct**,
`BreakRetNode` (`ir/stmt/break.h`), across four source pairs. This note covers
all four, because most of what matters about them is what they do *differently*
with fields they all share.

**At a glance.** Built by `parsefnflow.c`, or injected by type check and flow.
Bound to a target block by name resolution. Coerced against the function
signature or folded into the block's type by type check. Given a release list by
flow. Read by generation for the terminator and that list.

They share a struct deliberately: it lets one be substituted for another in
place — type check rewrites a `return` inside an inlined function into a break —
and it puts every field where the code that reads it expects, even for the tags
that never populate it.

*Provenance: read from source. The `continue` defect in Hazards was measured.*

## Shape

| Field | Meaning | `return` | `blockret` | `break` | `continue` |
| --- | --- | --- | --- | --- | --- |
| `exp` | the value carried out | yes | yes, `nil` if none | yes, `nil` if none | **never** |
| `life` | named lifetime of the block being escaped | **uninitialized** | **uninitialized** | yes | yes |
| `block` | the block this leaves — set by **name resolution** for break/continue, by **type check** for return | yes | no | yes | yes |
| `dealias` | what to release on the way out — filled by **flow** | yes | yes | yes | yes, but see Hazards |

Constructors: `newReturnNode` (retag to `BlockRetTag` for a block return),
`newReturnNodeExp` (wraps an expression **and copies its source position** — use
this for anything injected), `newBreakNode`, `newContinueNode`, and a
`clone*Node` per tag that re-points `block` through `cloneDclFix`.

## Parse

`parseReturn` records `exp` only. `block` is NULL and placement is not enforced.

**`newReturnNode` does not initialize `life`**, unlike `newBreakNode`. `newNode`
and `memAllocBlk` do not zero, so `life` on a `ReturnTag` or `BlockRetTag` node
holds arena garbage rather than NULL. Nothing reads it for those two tags today,
which is the only reason this is latent rather than a crash — do not be the code
that reads it.

`blockret` is never written. It is injected twice, by two different phases, for
two different reasons — see Hazards.

## Name resolution

`breakNameRes` and `continueNameRes` **bind the target block**: to the
lifetime-named block if `life` was given, else to `pstate->loopblock`, else
`ErrorNoLoop`. `continue` additionally requires the named block to be a loop.
`returnNameRes` walks `exp` and does nothing else — a `return` needs no target.

**`blockNameRes` enforces placement**, not type check: `ErrorRetNotLast` for a
`return`, `break` or `continue` that is not a block's last statement. The
allowance is `FlagLoopStep` — an `each` loop's synthesized step sits behind the
jump the reader wrote last.

## Type check

`returnTypeCheck`:

1. If the returned expression is an `if`, `ifRemoveReturns` strips the redundant
   `return` from each of its paths, recursively.
2. Coerce to the function's declared return type. **An explicit value tuple
   against a tuple return type is coerced element by element**, so each element
   gets its own conversion and diagnostic; the tuple's type is then taken from
   the signature. A mismatch reports twice — at the expression and at the
   declared return type — so the reader sees both ends.
3. Point `block` at the function's body block.
4. **For an inline function only**, add this node to that block's `breaks` list,
   which generation uses to splice the inlined body's exits.

`breakTypeCheck` does one thing: add itself to its target block's `breaks` list.
It deliberately does **not** check its expression's type — `blockTypeCheck` does
that later, folding every registered break together so they agree with each
other and with what the block's context expects. See
[Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches".

`continueTypeCheck` is empty.

`fnDclTypeCheck` turns a function body's final expression into an explicit
`return` before checking the body.

## Flow

Two jobs, both in `blockFlow` and `flowScopeDealias`:

- **Fill `dealias`** with what leaves scope on this path. `return` passes start
  position 0 — the whole function, parameters included. The other three pass the
  mark for the block being left.
- **Inject `blockret` into loop blocks.** Regular blocks got theirs from
  `blockTypeCheck`; a loop block gets one only here, and it is where the loop
  body's per-iteration release list hangs.

`returnFlow` → `returnFlowEscape` enforces the one rule `return` owns: **a
returned borrowed reference may not point at a local.** Scope 0 is a global, 1 a
parameter, 2+ a local, so the test is `region == borrowRef && scope > 1`,
reported as `ErrorEscape`. A returned value tuple is checked element by element.
[Flow Analysis](../phases/flow.md) owns the borrow-lifetime rule these two sites
implement.

## Generation

`genlReturn` compares `block` against the function's own body block: equal means
a real `ret`, unequal means this is a return inside an **inlined** function body
and it becomes a break. Either way `dealias` is replayed by `genlDealiasNodes`,
and the return value is computed **before** the release. `genlBreak` records the
phi value and predecessor, releases, and branches. `genlBlockRet` is an empty
stub — `BlockRetTag` is handled inline in `genlBlock`.

## Hazards

- **`continue` releases nothing.** `blockFlow` passes a NULL expression to
  `flowScopeDealias`, where that same parameter gates whether anything is added
  to the list at all, so every owning reference in scope leaks. Measured.
- **`break` and `continue` release only their innermost block**, not the scopes
  between them and the block they target. Only `return` passes 0.
- **`block` is NULL until name resolution** for break and continue, and until
  *type check* for return. Anything walking these nodes earlier must not read
  it.
- **`blockret` is injected by two phases** — `blockTypeCheck` for regular
  blocks, `blockFlow` for loop blocks. Looking in only one is how the loop case
  gets missed.
- **`dealias` is NULL when the function failed its flow gate**, and generation
  then silently releases nothing. That is safe only because generation does not
  run when errors were reported.
- **`continue` never has an `exp`.** `newContinueNode` does not even initialize
  it. Do not read it.

## What lives elsewhere

- The order a function's own check runs in: [Type Check Phase](../phases/type-check.md), "Function and method"
- Folding breaks into a block's type: [Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches"
- Borrow lifetimes, and why enforcement sits at the consumers: [Flow Analysis](../phases/flow.md)
- Basic blocks, phis, and how `dealias` is replayed: [Generation](../phases/generation.md)
- Node headers, injection hazards, the sentinels: [IR Nodes](_index.md)
