Data flow analysis enforces Cone's ownership rules: what moves, what is
counted, what is released, and where a borrow may not reach. It is the part of
the compiler least like other languages and the least guessable from the source.

Read this before changing anything about ownership, moves, aliasing, drops, or
borrow lifetimes — and read "What a reader from Rust will get wrong" before
assuming it does anything a borrow checker does. It does not.

*Provenance: read from source; the defects in Hazards were measured by reading
emitted LLVM IR, and the claims about what is **not** enforced are corroborated
by test scenarios asserting the absence. See
[Measuring](../diagnostics/measuring.md).*

## 1. It is a fifth phase that is not a fifth pass

`doAnalysis` runs exactly two whole-program walks: name resolution, then type
check. Flow rides on the second. `fnDclTypeCheck` is the only caller of
`blockFlow` from outside flow's own recursion, at the close of type checking
each function's body — so it is the single entry point to the whole pass.

It is scheduled per function rather than globally because flow needs types, and
Cone infers types bottom-up by demand: there is no point at which "type checking
is done" globally before any body could be flowed.

**The gate is a delta, not a total.** `fnDclTypeCheck` records `errors` on
entry and runs flow only if the count is unchanged:

```c
int errorsOnEntry = errors;
...
if (errors != errorsOnEntry) return;
blockFlow(&fstate, (BlockNode **)&fnnode->value);
```

Because type check is demand-driven and re-entrant, a nested `fnDclTypeCheck`
captures its own baseline, so each function's gate is about that function alone.
An earlier failure elsewhere must not silence move and permission checking for
every function that follows it. Warnings never gate.

**A function that fails the gate gets nothing** — no diagnostics, and no
injected nodes. That is safe only because generation does not run when
`errors != 0`.

> `test/cases/core/core-flow-gate.cone` is the scenario that pins this. The
> delta is easy to misread as a global `errors == 0` check — trust the scenario
> over any comment that says so.

## 2. What a reader from Rust will get wrong

Put these first, because every one of them is load-bearing.

1. **There is no borrow checker.** Nothing tracks aliasing of borrows. A `&mut`
   and a `&` to the same variable coexist freely, and the source of a borrow
   stays fully usable and mutable while the borrow is alive. `borrowFlow` is an
   empty function body with a comment describing the deactivation that was never
   written.
2. **A lifetime is a `uint16_t` block-nesting depth on the borrow expression's
   type node.** Not a constraint variable, not region inference. 0 is global, 1
   is a parameter, 2+ is a local. The rule is a numeric comparison at two sites.
   `lifeMatches` exists and is called from nowhere.
3. **Lifetime tracking does not survive a variable.**
   `mut r &i32; r = &local; return r` compiles clean: assignment does not carry
   the borrow's scope onto the variable's declared type. `ref-flow-return.cone`
   asserts this absence deliberately.
4. **Flow is path-insensitive and does not iterate.** No CFG, no lattice, no
   join, no fixed point. `ifFlow` walks both arms against one shared mutable
   state, so a move in the `then` arm marks the source moved for the `else` arm
   and for everything after. A loop body is walked once. **Not building a CFG is
   a deliberate design choice**, not a simplification to be outgrown — the
   block-structured IR is held to be easy enough to follow directly.
5. **Ownership is not one model.** `so` is unique-owner-frees; `rc` is counted;
   `borrowRef` is a sentinel node, not a struct, and is a reference's default
   region.
6. **Permission belongs to the reference, not the binding.**
   `imm fixed = &mut target` is a writable target through an unrebindable name.
7. **`&uni` is not `&mut` with extra rules.** `uni` lacks `MayAlias`, which is
   exactly what makes a reference carrying it a move type.

## 3. State

`FlowState` has two fields, and each is read in exactly one place:

| Field | Read by |
| --- | --- |
| `fnsig` | `blockFlow`, to `flowAddVar` each parameter on entering the function's main block |
| `scope` | `blockFlow`, only as `if (++fstate->scope == 2)` — the test for "this is the main block" |

**Flow computes no lifetimes of its own.** `VarDclNode.scope` is set during name
resolution; `RefNode.scope` during type check by `borrowTypeCheck`. Flow only
compares them.

The live state is on the declarations, in `VarDclNode.flowtempflags`:

| Flag | Set by | Cleared by |
| --- | --- | --- |
| `VarInitialized` | `varDclFlow`, `assignlvalrtype`; pre-set at parse for globals, fields and parameters | never |
| `VarMoved` | `flowHandleMove` | `assignlvalrtype` on reassignment |

Because these live on the declaration and are never saved or restored, **they
are a running summary over the whole function, not per-program-point state.**
Every imprecision below follows from that one fact.

A file-static variable stack (`gVarFlowStackp`) records which declarations are
in scope. It is global mutable state, safe only because flow never runs
re-entrantly — it never descends into a callee. `VarFlowInfo.flags` and
`VarDclNode.flowflags` are both dead.

## 4. Moves and counting

**Move-ness is a type property, derived not declared.** `iexpIsMove` is
`vtype`'s `MoveType` flag and nothing else. A type acquires it from `@move`, a
finalizer, a move-typed field or element, and — for references —
`refAdoptInfections`: **a reference is a move type when its permission lacks
`MayAlias` or its region is itself a move type.** That single sentence is why
`+rc x` moves while `+rc-mut x` copies, on the same region.

**The count counts holders.** From the ownership work:

- `+rc[2]` creates the object *and* the first reference. Born at 1.
- `imm a = +rc[2]` adds no holder — the temporary hands over the reference it
  was born with. Still 1.
- `imm b = a` adds one — `a` keeps its reference, `b` gets another. Now 2.

`flowHandleMoveOrCopy` is the whole decision:

```c
if (iexpIsMove(*nodep))          flowHandleMove(*nodep);      // deactivate source
else if (flowIsLvalRead(*nodep)) flowInjectAliasNode(nodep);  // +1
```

`flowIsLvalRead` asks "does this expression still hold its value after it is
read?" — true for a name use, deref, index and field access; false for a
temporary. Counting a temporary would add a holder that never existed, and the
allocation would never reach zero.

It is called from exactly six places — `varDclFlow` (the initializer),
`assignSingleFlow` (the rval), `fnCallFlow` (per argument), `allocateFlow` (the
allocated value), `typeLitFlow` (per field) and `arrayLitFlow` (per element, in
the list form only). The array **fill** form does its own arithmetic instead,
because one value goes to n holders.

**Decrements are never alias nodes.** They come from generation: walking a
`dealias` list at scope exit, and `genlStore` releasing an lval's previous value
unless `FlagFirstAssign` says there was none.

## 5. What it injects

Flow is not a read-only analysis. Four mutations, all of which generation
depends on:

| Injection | Where | Generation uses it for |
| --- | --- | --- |
| `BlockRetTag` | `blockFlow`, for any block not already ending in one | a loop block, **and** a regular block ending in an expression, both get theirs here — it is where the dealias list hangs |
| `AliasTag` | `flowInjectAliasAmt` | `genlRcCounter(val, aliasamt)` |
| `dealias` lists | `flowScopeDealias`, onto every `BreakRetNode` | `genlDealiasNodes` replays them |
| `FlagFirstAssign` | `assignlvalrtype` | `genlStore` skips releasing a previous value that never existed |

**An alias node is built only for a counted reference.** `flowInjectAliasAmt`
returns early unless the type is `RefTag` in region `rc`. A `+so` reference and
a `uni`-permissioned `+rc` reference are both move types and take the move path
instead. Two arms of generation's `AliasTag` case — the `so` arm and the tuple
`counts` arm — are therefore unreachable as the code stands.

**Scope dealiasing.** `flowScopeDealias` walks the variable stack downward from
the top to a start position, so release order is the reverse of declaration
order. Per variable: an `so` or `rc` reference is added to the list, unless it
was moved out; anything else asks `itypeGetDropFnDcl` and, if there is one,
builds a call to the drop fn on a `&uni` borrow.

**Where a jump's start position comes from.** `BlockNode.flowmark` is the flow
stack position `blockFlow` recorded on entering that block. A `break` or
`continue` passes its *target* block's mark, through `blockJumpMark`, so it
releases from where it stands down to the block it names rather than its own
scope alone; `return` passes 0, the whole function. The mark is transient — set
by `blockFlow` and valid only while flow is inside that block — and
`blockJumpMark` falls back to the current position for a jump whose target
failed to resolve.

The `doalias` return is a real optimization: returning a named owning variable
cancels both the `+1` alias and the `-1` dealias rather than emitting both. It
answers **only for a lone returned name**, because it gates `flowLoadValue` over
the whole return expression: a tuple's elements are exempted from release one by
one, but each still needs the move and initialization check that walk makes.

> `flow.h` and `flow.c` disagree about what `flowScopeDealias` returns. The code
> matches `flow.c`.

## 6. What it decides, and what it does not

| Analysis | In flow? | Enforced | Not enforced |
| --- | --- | --- | --- |
| **Move / ownership** | yes | `ErrorMove` on use of a moved-out or uninitialized variable; move out of a global refused | field granularity — moving `p.x` deactivates all of `p`; conditional moves; loop-carried moves |
| **Escape / lifetime** | representation in type check, enforcement here | storing a borrow into a longer-lived lval; returning a borrow of a local | a borrow laundered through a variable; anything across a function boundary — there is no lifetime annotation syntax; freezing a borrow's source |
| **De-aliasing / drops** | flow decides, generation executes | scope-exit release of `so`/`rc` refs and drop-fn structs, from a jump down to the block it names | arrays of owning references; a variable moved out on only one path — see Hazards |
| **Permission** | `MayWrite` only | `ErrorNoMut` on assignment and swap | `MayRead` is never consulted as an access check anywhere; `MayAliasWrite`, `RaceSafe`, `IsLockless` are populated and read nowhere |
| **Initialization** | yes | `ErrorMove` "has not been initialized" | "initialized on one branch" reads as initialized everywhere; the unused-variable warning in `flow.h`'s header does not exist |
| **Array fill rules** | yes | `ErrorBadFill` for a repeated move value; `ErrorFillCount` for a non-constant count | — |

Everything else about permissions is type check's: `permMatches` in
`borrowTypeCheck`, and variance in the reference matchers.

### Diagnostics

| Code | Site | Condition |
| --- | --- | --- |
| `ErrorInvType` | `flowHandleMove` | move out of a global variable |
| `ErrorInvType` | `assignlvalrtype` | lval outlives the borrowed reference stored into it |
| `ErrorNoMut` | `assignlvalrtype`, `swapFlow` | no write permission |
| `ErrorMove` | `nameuseFlow` | uninitialized, or moved out |
| `ErrorBadFill` | `arrayLitFlow` | a fill may not repeat a move value |
| `ErrorFillCount` | `arrayLitFlow` | fill count not constant, or too large |
| `ErrorEscape` | `returnFlowEscape` | returned borrow outlives the local it points at |

`ErrorBadFill` and `ErrorFillCount` are deliberately distinct: the first is a
language rule, the second an implementation limit that should disappear when a
fill lowers to a loop.

## 7. Contract

**Before flow runs:** name resolution succeeded program-wide; this function's
signature and body type checked cleanly; every expression has a resolved
`vtype`; lowering is complete; `RefNode.scope` and `VarDclNode.scope` are
populated; regular blocks already end in a jump but loop blocks do not; globals,
parameters and fields already carry `VarInitialized`.

**After flow, for a function that ran it:** every block ends in a node carrying
a `dealias` list; every recognized counted acquisition has an `AliasNode`; every
first-assignment target carries `FlagFirstAssign`.

**What generation relies on.** `genlBlock`, `genlBreak` and `genlReturn` call
`genlDealiasNodes` and do no analysis of their own. If flow did not run, the
lists are NULL, `genlDealiasNodes` returns immediately, and **nothing is ever
released** — there is no fallback. Likewise, without `FlagFirstAssign` every
first assignment to an uninitialized owning variable decrements garbage.

Without `FlagFirstAssign`, `genlStore` decrements a count that was never
initialized — for an `rc`-region lval, which is the only kind it releases there
at all.

**One layout invariant generation depends on and flow does not state.**
`genlRcCounter` finds the count by bitcasting the reference to `usize*` and
GEPing `-1`. That is correct only because `rc` has exactly one `usize` field and
the built-in permissions are zero-sized. See [Generation](generation.md),
"The allocation header".

## 8. Hazards

- **A moved-out variable is skipped at scope exit on every path**, because
  `VarMoved` is a whole-function summary. That leaks rather than double-frees,
  which is the deliberate choice; the in-code comment says so.
- **`flowIsLvalRead` is not `iexpIsLval`.** They disagree on recursion into
  `objfn` and on string literals. Do not substitute one for the other.
- **`fnCallFlow` does not flow `objfn`**, so a call through an uninitialized
  function-reference variable is not reported.
- **`flowScopeDealias` reads `vtype` raw**, without `itypeGetTypeDcl`, while
  `flowInjectAliasAmt` resolves it. A variable declared through a typedef alias
  to an owning reference is likely missed.
- **`flowLoadValue`'s `default:` arm reports `ErrorUnreachable` and stops.** An
  unhandled tag therefore fails the compile rather than passing through it —
  passing through would mean no move check, no alias injection and no
  initialization check for that value. Whether any tag reaches it is
  unestablished.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the only entry point; the per-function error-delta gate |
| `ir/flow.c` | `flowLoadValue` | the walk's spine — tag dispatch for a value being read |
| | `flowHandleMoveOrCopy` | move vs. alias, for a value going to a new holder |
| | `flowHandleMove` | deactivate the source; refuse a move out of a global |
| | `flowIsLvalRead` | the temporary-vs-lvalue test that makes counting correct |
| | `flowInjectAliasAmt` | wrap a counted reference in an `AliasNode` |
| | `flowScopePush`, `flowScopePop`, `flowAddVar` | the variable stack |
| | `flowScopeDealias` | build a scope's release list; skip moved vars; cancel for a returned name |
| `ir/exp/block.c` | `blockFlow` | scope push/pop, `blockret` injection, dealias capture |
| `ir/exp/if.c` | `ifFlow` | both arms against one shared state |
| `ir/exp/assign.c` | `assignlvalrtype` | `MayWrite`, `VarInitialized`/`VarMoved`, `FlagFirstAssign`, borrow lifetime |
| `ir/exp/nameuse.c` | `nameuseFlow` | the only place the two flags are *diagnosed* on; both `ErrorMove` messages |
| `ir/exp/borrow.c` | `borrowFlow` | **empty** |
| `ir/stmt/return.c` | `returnFlowEscape` | `ErrorEscape` for a returned borrow of a local |
| `ir/exp/arraylit.c` | `arrayLitFlow` | fill-form rules and the n / n-1 alias amount |
| `ir/types/reference.c` | `refAdoptInfections` | where a reference type acquires `MoveType` |
| `ir/types/region.c` | `isRegion`, `regionAllocTypeCheck` | region identity; `_alloc`/`init` validation |
| `genllvm/genlalloc.c` | `genlRcCounter`, `genlDealiasNodes` | what consumes everything flow injected |

Test sources that pin behavior precisely: `test/cases/move/move-flow-*.cone`,
`test/cases/region/region-flow*.cone`, `test/cases/ref/ref-flow.cone`,
`test/cases/ref/ref-flow-return.cone`, `test/cases/core/core-flow-gate.cone`.

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What the three reference axes mean, and what each permits | [References and Regions](../northstar/references-and-regions.md) |
| Which safety properties actually hold today | [Safety](../northstar/safety.md) |
| When a function is type checked at all | [Type Check Phase](type-check.md) |
| What a borrow's type records, and where | [Type Check Reasoning](type-check-reasoning.md), "Borrows: where type check stops" |
| How the allocation header is laid out | [Generation](generation.md), "The allocation header" |
