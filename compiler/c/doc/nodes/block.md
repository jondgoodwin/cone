`BlockNode` is a statement list that is also an expression. A **loop** block is
the same node with `FlagLoop` — there is no loop node, and `while` and `each`
are lowered into one, by the parser, or for an `each` over a source that is not
a numeric range, by type check once it knows the source.

**At a glance.** Built by `parseExprBlock`. Name resolution pushes a scope,
binds a lifetime label, enforces jump placement, and repairs `each`'s
`continue`. Type check builds the loop of an `each` over a source that is not
a range (`FlagEach`), then folds the block's value paths into one type. Flow
brackets the scope, injects `blockret`, and builds the release list. Generation
creates basic blocks only when it has to.

*Provenance: read from source; the double `blockret`, and the double drop it
caused, were measured.*

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
| `FlagKeepTemps` | an operator's rewrite (`fnCallOpAssgn`, `contentsLower`): its statements' temporaries die at its end, not each statement's ([Flow](../phases/flow.md), "Temporaries") |
| `FlagEach` | the outer block of an `each` over a source that is not a numeric range: `blockTypeCheck` checks its first statement, the hidden variable holding the source, then builds the loop (`eachLower`, `ir/exp/each.c`) and clears the flag |

## Parse

`parseExprBlock(parse, isloop)` is the single builder. Loop blocks come from
`while` and `each`; everything else — `{…}`, `if` arms, `with`, a function body,
a macro body, and the wrapper blocks pattern-matching builds — is regular.

**`while` and `each` are lowered here, not later.** `while cond {…}` gets
`if not cond { break }` inserted at index 0. `each x in a < b by s` becomes an
outer block holding the loop's **counter** (a hidden variable, no name of the
reader's) plus a loop block that begins with the guard and the pass's variable,
`imm x = counter`, and whose **last statement is the synthesized step**, flagged
`FlagLoopStep`. The variable is a new one on every pass and cannot be written;
the loop steps its counter, whose uses are bound as the parser builds them, so a
copy of the step carried into an inner loop by a labelled `continue` reaches this
loop's counter whatever the inner loop names. Where stepping past the bound
could wrap the counter around the type's extreme, and so satisfy the
comparison again, that step is itself a block. For an inclusive range without
`by` it is `{ if c == b {break}; c++ }`. With `by` it is `{ imm prev = c; c += s;
if c < prev {break} }`, `>` in place of `<` for a range counting down: a step of
more than one need never land on the bound, and neither the distance to the bound
nor the step's sign may be computed ahead of the step, so the wrap is recognized
afterwards instead — as the counter having moved against the range's direction.
The statements are one block so that the `continue` repair below carries the
guard too, and `prev` is a phantom variable, resolved as the parser builds it, so
that a copy reads its own. The synthesized `break` names the loop's lifetime when
it has one, since a copy of it can land inside an inner loop.

**`each` over anything else is finished by type check.** The parser cannot say
how a loop walks its source, which is the source's type to say, so it builds an
outer block flagged `FlagEach` holding `mut hidden = src` and the loop block,
whose leading statements declare the reader's variables with no value and whose
body follows them. Name resolution sees an ordinary pair of nested blocks, and
the variables, the body and every `break`/`continue` in it resolve as for a
`while`. The scopes it counts are the ones the finished loop has, which is why
the loop keeps the body's statements where they are and adds only to its head.

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
known. (The step names the loop's counter by a pre-resolved use, so the copy
needs no hooked name.)

## Type check

**`FlagEach`: the loop is built here, from the source's type.** Before any
statement is checked, `blockTypeCheck` checks the block's first statement, the
hidden variable `mut hidden = src` (`blockStmtTypeCheck`, so the source is
lowered once and a temporary it extends is hoisted as any initializer's is),
and `eachLower` reads what the checked source is and rewrites the block's
statements; the loop below then checks them as built. By the source's type:

- **an array, or a reference to one, a slice, or a type that lends an array it
  holds** (a list, `structLentBody`): a counted loop over the slice, `mut i = 0`
  and `loop { if i >= s.len {break}; imm x = &s[i]; i++; ...body }`, the slice
  being the array lent as one, the source itself, or the lending method's
  result (`list.view()`). No cursor is made, and the loop is the one a `while` over the
  list's indexes optimizes to. A list's `iter()` is not called: its cursor gives
  an Option each pass, whose null test on a pointer the optimizer cannot know is
  not null blocks the loop's vectorization. A slice that is mutable (what
  `mutItems()` answers) lends each element as `&mut s[i]`; any other as `&s[i]`.
- **core's `ArrayIter` or `ArrayIndexed`, a value made for the loop** (not a
  place, not behind a reference; `iter()` with one variable, `indexed()` with
  two, `eachIsCore`): the same counted loop, over the slice the cursor holds and
  from the position it holds (`mut i = cursor.pos`), so a cursor made by the loop
  is no cursor in the code. With `indexed()` the first variable is a copy of `i`
  and the second the element's borrow. A cursor kept in a variable, or taken
  whole by one variable of `indexed()`, is walked through its `next`.
- **a type with `next`**: the cursor is the source as it is. A place named
  again without evaluating anything (`eachStablePlace`: a variable, a field of
  one, a dereference of one) is advanced where it stands, so a cursor left part
  way by a `break` is finished by the next loop; another place is borrowed once;
  a value is held in the hidden variable.
- **a type with `iter` and no `next`**: `mut cursor = src.iter()`, the source
  held first unless it is a place.

Each pass of a cursor loop declares its variable from the item:
`imm x = match cursor.next() { case imm s Some { s.value; } case is None {
break; } }`, built as `match` is desugared (`eachNextItem`), the `break`
joined to the loop as it is built. Two or more variables take the item through
a variable of the pass's own, `imm -item = ...; imm k = -item.0; imm v =
-item.1`. The pass's binding of the `Some` has a name no reader can write
(`-some`), so that a move out of it is marked at the move as a named binding's
is. An item that moves is taken whole by one variable, `s.value` moving the
Option the loop holds with it (`flowTakesSoleField`, [Flow](../phases/flow.md),
"Moves and counting"); with several variables it would move the elements of a
tuple out one by one, which is refused before the loop is built
(`ErrorEachItem`).

A source is checked once, as the initializer of the hidden variable. Checking an
expression a second time is not idempotent for a call, which type check lowers,
so the lowering uses the node the check left: where the source is a place a
second check leaves alone (`eachRecheckable`), the variable is dropped and the
place is used in its stead; otherwise the variable holds it.

Every statement but the last is checked with `noCareType`. A nested plain block
may not end in `break`/`continue` (`blockNoBreak`) — `if` arms are exempt,
because they are reached through `ifTypeCheck`.

**A statement declaring a local may grow hidden locals before it.** Each
declaration is checked between `varDclExtendBegin` and `varDclExtendEnd`
(`blockStmtTypeCheck`): a temporary its initializer extends, Rust's way,
becomes a hidden local of this block ([VarDcl](vardcl.md), "Temporaries an
initializer extends"), handed back in the order they run. They are inserted
just before the statement once the whole block is checked (`blockHoist`), the
last thing before either return, since the checks after the statement loop
still hold pointers into the statement list.

The last statement splits:

- **Loop block**: may not end in `break`/`continue`/`return`; checked with
  `noCareType`; no breaks at all is `WarnLoop`. **Nothing is injected.**
- **Regular block ending in an expression**: that expression is checked
  against `expectType`, then is a value path. **Nothing is injected** —
  unless it is a call returning `Never` (`fnCallIsNever`), which does not
  return and so gives the block no value: it becomes the expression of a
  `return` put in its place, joined to the function (`returnJoinFn`) without
  being checked again, and the block ends in a jump. Every later pass already
  knows a path ends at a `return`, and generation ends this one with
  `unreachable` ([return](return.md)).
- **Regular block ending in a jump**: checked, nothing injected.
- **Anything else** (empty, or ending in a declaration): `blockret nil` is
  appended, and it is a value path.

Then every registered break is folded in — except for the function's own body
block, where `returnTypeCheck` already coerced returns against the signature.
The fold loop uses a manual index because `breaks` can grow while iterating.

`vtype` is `expectType` when one was given, else the inferred type. **A function
body is checked with `noCareType`**, so its `vtype` is `unknownType` — except an
**inline** function's, which `fnDclTypeCheck` then sets to the signature's return
type. That body is generated in each caller as a block whose returns are its
breaks, so with more than one return it is a phi block, and the phi takes its
type from `vtype`: at `unknownType` `genlBlock` would build no phi, and the call
would have no value.

## Flow

`blockFlow` brackets the scope and, on entering scope 2 — the function body —
adds the signature's parameters to the variable stack.

**`blockret` is injected here too, and this is the second of two sites.** If the
last node is neither a jump nor a `blockret`, flow wraps it (if it is an
expression) or appends `blockret nil`. So: a loop block gets its `blockret`
**only** from flow, and so does a regular block ending in an expression.
`blockTypeCheck` handles only the third case, and flow keeps the `blockret` it
made. Looking in one place misses two.

The final node's value expression is walked, and its `dealias` built after —
`return` unwinds from position 0, the whole function; `blockret` unwinds this
block; a `break` or `continue` unwinds from where it stands down to its target
block's `flowmark`. **That order is what makes a tail call correct**: the walk
is what marks a variable moved, so a local handed to a call in final position is
known to have left the scope before the list that would release it is built.

**A block that throws its value away hands nothing back** (`blockDiscards`): a
loop block, whose final expression loops back, and a block typed with no value
— a statement's, a branch of an `if` that is a statement — but not a
function's own block, typed with none whatever it returns, unless the function
returns nothing. Its final expression is a statement's: a temporary there is
wrapped as one thrown away, and a local it names is no result, so the release
list does not exempt it (`flowresult` is NULL). `if c { imm x = mk(); x; }`
finalizes `x` at the branch's end.

**Each statement that made a temporary is walked again** once flow has walked
it (`blockTempEscape`), so that a temporary a raw pointer going out of it may
point into is kept: a variable's initializer, and a `return`'s, a `break`'s or
a used block's value, go out ([Flow](../phases/flow.md), "Temporaries"). A
borrow of one going out is the loan walk's to check.

## Generation

**Basic blocks are created only when needed**: `isPhiBlk = isLoop || breaks > 1`.
A regular block with at most one break emits its statements straight into the
current block. A loop gets `loopbeg` and `loopend` and a back-edge; a phi block
gets `blockend` and a `GenBlockState` on a fixed 256-deep stack.

**A phi is built only where the block converges on a value**, and one condition
answers that — `vtype` is neither `VoidTag` nor `UnknownTag` — read once to
allocate the arrays and once to build the phi. `genlBreak` guards on those arrays
existing, while still generating its value for the effects. **Where the value is
a small struct or array, the "phi" is a slot** (`GenBlockState.slot`, from
`genlMergeSlot`): `genlBreak` stores the value into it and the block's end loads
it, because LLVM never splits a phi of an aggregate and a loop carrying one stays
scalar ([Generation](../phases/generation.md), "A small aggregate is merged
through a slot").

A `terminated` flag stops emission after a jump, because an instruction after a
terminator is invalid IR — reachable only in an `each` block, where the step
sits behind the jump the reader wrote last.

**Each statement's temporaries die at its end**, newest first: after its value
and, for a `blockret`, before the `dealias` list (`genlTempsEnd`). A jump
finalizes them, with those of the statements it leaves, before it goes, and the
statement's entries are then dropped ([Generation](../phases/generation.md),
"Temporaries").

## Hazards

- **A regular block must not get two `blockret`s.** `blockTypeCheck` appends one
  for a block not ending in an expression or a jump -- `{}`, `{ imm f = mk(); }`
  -- so `blockFlow` treats a final `blockret` as present. It once appended a
  second: the main walk built the first one's `dealias` empty, but the path
  walk records a release at every exit it meets, and the drop walk
  (`dropApplyExit`) rebuilt both lists, so in any function it walks -- one with
  a drop flag, or a behaviour holding an `await` -- a droppable declared last in
  a block was finalized twice, a counted one freed twice (`move_drop_flags`,
  `concurrency_await_split`).
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
