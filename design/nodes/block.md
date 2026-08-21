`BlockNode` is a statement list that is also an expression. A **loop** block is
the same node with `FlagLoop` — there is no loop node, and `while` and `each`
are lowered into one by the parser.

**At a glance.** Built by `parseExprBlock`. Name resolution pushes a scope,
binds a lifetime label, enforces jump placement, and repairs `each`'s
`continue`. Type check folds the block's value paths into one type. Flow
brackets the scope, injects `blockret`, and builds the release list. Generation
creates basic blocks only when it has to.

*Provenance: read from source; the double `blockret` and the malformed phi were
measured.*

## Shape

| Field | Meaning |
| --- | --- |
| `stmts` | ordered statements. **Never NULL** — `--checktree` reports `ErrorBadTree` if it is |
| `lifesym` | the `Name` of a `'label:` annotation, else NULL. The *declaration* side; `BreakRetNode.life` is the use side |
| `breaks` | the `BreakRetNode`s targeting this block. NULL for a regular block; allocated for a loop; created lazily for an **inline** function's body |
| `vtype` | `unknownType` until the end of type check |
| `flowmark` | where this block's scope starts on the flow stack. Set by `blockFlow` on entry and **valid only while flow is inside the block**; read by a `break` or `continue` naming it, to know how many scopes it is leaving |

| Flag | Means |
| --- | --- |
| `FlagLoop` | a loop block. **Set only by `newLoopBlockNode`** |
| `FlagLoopStep` | the last statement is `each`'s synthesized step |

## Parse

`parseExprBlock(parse, isloop)` is the single builder. Loop blocks come from
`while` and `each`; everything else — `{…}`, `if` arms, `with`, a function body,
a macro body, and the wrapper blocks pattern-matching builds — is regular.

**`while` and `each` are lowered here, not later.** `while cond {…}` gets
`if not cond { break }` inserted at index 0. `each x in a < b by s` becomes an
outer block holding the loop variable plus a loop block whose **last statement
is the synthesized step**, flagged `FlagLoopStep`.

## Name resolution

`blockNameRes`: save `loopblock` and make this block the innermost loop target
if it is one — that is what an unlabelled `break`/`continue` binds to. Push a
scope and a hook table. Hook `lifesym` if it is free; **if it is already bound,
report the duplicate and do not hook**, so an inner `break 'x` silently reaches
the outer block.

**Placement rule.** `return` may only be last; `break` and `continue` may be
last, or one before last when `FlagLoopStep` allows for the step. `return` gets
no such allowance — it would leave the step as unreachable code. `ErrorRetNotLast`.

**`blockContinueStep` repairs `each`.** After the statement walk, if a
`continue` targets a `FlagLoopStep` loop, the loop's trailing step is **cloned**
and inserted ahead of the jump, then re-resolved. Cloned rather than shared,
because a node reachable twice is type checked twice and lowering is not
idempotent. Timing is load-bearing: after the walk so `continue` targets are
known, before the hook pop so the copy can still see the loop variable.

## Type check

Every statement but the last is checked with `noCareType`. A nested plain block
may not end in `break`/`continue` (`blockNoBreak`) — `if` arms are exempt,
because they are reached through `ifTypeCheck`.

The last statement splits:

- **Loop block**: may not end in `break`/`continue`/`return`; checked with
  `noCareType`; no breaks at all is `WarnLoop`. **Nothing is injected.**
- **Regular block ending in an expression**: that expression is a value path.
  **Nothing is injected.**
- **Regular block ending in a jump**: checked, nothing injected.
- **Anything else** (empty, or ending in a declaration): `blockret nil` is
  appended, and it is a value path.

Then every registered break is folded in — except for the function's own body
block, where `returnTypeCheck` already coerced returns against the signature.
The fold loop uses a manual index because `breaks` can grow while iterating.

`vtype` is `expectType` when one was given, else the inferred type. **A function
body is checked with `noCareType`**, so its `vtype` is always `unknownType`.

## Flow

`blockFlow` brackets the scope and, on entering scope 2 — the function body —
adds the signature's parameters to the variable stack.

**`blockret` is injected here too, and this is the second of two sites.** If the
last node is not a jump, flow wraps it (if it is an expression) or appends
`blockret nil`. So: a loop block gets its `blockret` **only** from flow, and so
does a regular block ending in an expression. `blockTypeCheck` handles only the
third case. Looking in one place misses two.

The final node's `dealias` is then built — `return` unwinds from position 0, the
whole function; `blockret` unwinds this block; a `break` or `continue` unwinds
from where it stands down to its target block's `flowmark`.

## Generation

**Basic blocks are created only when needed**: `isPhiBlk = isLoop || breaks > 1`.
A regular block with at most one break emits its statements straight into the
current block. A loop gets `loopbeg` and `loopend` and a back-edge; a phi block
gets `blockend` and a `GenBlockState` on a fixed 256-deep stack.

A `terminated` flag stops emission after a jump, because an instruction after a
terminator is invalid IR — reachable only in an `each` block, where the step
sits behind the jump the reader wrote last.

## Hazards

- **A regular block can get two `blockret`s.** `blockTypeCheck` appends one for
  a block not ending in an expression or a jump; that node is then not a jump
  and not an expression, so `blockFlow`'s default arm appends **another**.
  Measured: `{}` and `{ mut z = n }` each show two `blockret nil` in an `--ir`
  dump. Harmless at generation — both emit nothing — but the first one's
  `dealias` is never populated.
- **`genlBlock`'s two phi guards disagree**, and the result is invalid LLVM IR.
  Storage is allocated under `vtype != VoidTag` but the phi is built under
  `vtype != UnknownTag`. Measured: a loop-as-expression whose breaks all carry
  `nil` emits `%phival = phi %void` **with no incoming entries**, and
  `--verify` reports "PHINode should have one entry for each predecessor". The
  default build does not run the verifier, so it is emitted silently. `phiCnt`
  is an uninitialized arena read on that path.
- **A colliding lifetime label is not hooked** after its diagnostic, so an inner
  `break 'x` binds to the outer block.
- **`breakNameRes` accepts any labelled block; `continueNameRes` requires a
  loop** — and they share one message string, so `continue`'s failure says
  "break's lifetime not found".
- **A loop body is walked once by flow**, so anything true only on a second
  iteration is invisible to move and alias accounting.
- **`cloneBlockNode` copies `breaks` shallowly.** A clone made after breaks were
  registered would share break nodes with the original.
- **The block stack is a fixed 256 entries**, and overflow is a hard exit.

## What lives elsewhere

- The four block-ending statements and their `dealias`: [return](return.md)
- Folding value paths into one type, and the re-coercion pass: [Type Check Reasoning](../phases/type-check-reasoning.md), "Unifying branches"
- Scope release lists, and how far a jump unwinds: [Flow Analysis](../phases/flow.md)
- Basic blocks and phis: [Generation](../phases/generation.md)
