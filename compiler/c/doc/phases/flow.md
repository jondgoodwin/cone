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

## Principles — [derived]

⚠ **Read from source; the defects in Hazards were measured, and the claims about
what is *not* enforced are corroborated by scenarios asserting the absence.**
**Unchecked is the claim that these rule rather than describe.**

**Flow is scheduled per function, never globally.** ▸ **Forbids** any rule
needing whole-program reachability — inter-procedural escape, a global alias
graph, a cross-function lifetime. Section 1 below says why: flow needs types, and
types arrive by demand, so there is no moment at which "type checking is done"
for the program.

**It is not a borrow checker, and reasoning by analogy to Rust will be wrong.**
▸ The note carries "What a reader from Rust will get wrong" for exactly this.
**This is the least guessable part of the compiler**, and the principle is that
guessing is not a method here.

**Flow decides; generation replays.** Every release, count adjustment and drop
call is injected here as IR. ▸ **Settles** where an ownership bug lives — a
double release is a flow bug however it surfaces, and
[Generation](generation.md) states the same boundary from its side.

**Where a rule is unenforced, a scenario establishes the opposite.** A violation
that compiles clean is pinned deliberately. ▸ **So a scenario that starts failing
may be one a fix correctly invalidated** — the same convention
[Safety](../../../../doc/design/safety.md) uses, and for the same reason.

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

> `test/cases/core/core_flow_gate.cone` is the scenario that pins this. The
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
   is a parameter, 2+ is a local. The rule is a numeric comparison at three
   sites. A call's result carries the narrowest scope among its borrowed
   arguments, on a reference node `fnCallFinalizeArgs` builds for that call — or,
   where the call returns several values, on the borrowed elements of a tuple
   type it builds for it: without annotation syntax every borrowed reference in a
   signature shares one lifetime, and the shortest is the only one they have in
   common. A borrow coerced to another reference type — widened to a base
   trait's reference, or turned into a virtual reference — keeps its scope on a
   copy of the target type that `iexpCoerce` gives the cast, the declared type
   being shared and unable to carry one.
   `lifeMatches` exists and is called from nowhere.
3. **Lifetime tracking does not survive a variable.**
   `mut r &i32; r = &local; return r` compiles clean: assignment does not carry
   the borrow's scope onto the variable's declared type. `ref_flow_return.cone`
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
resolution; `RefNode.scope` during type check by `borrowTypeCheck`, by
`borrowMutRef` and `borrowAuto` for a borrow the compiler injects, and by
`fnCallArrIndex`, `fnCallFinalizeArgs` and `iexpCoerce` for a type derived from
one. Flow only compares them.

The live state is on the declarations, in `VarDclNode.flowtempflags`:

| Flag | Set by | Cleared by |
| --- | --- | --- |
| `VarInitialized` | `varDclFlow`, `assignlvalrtype`; pre-set at parse for globals, fields and parameters | only round a module's `init`: `modInitFlowBegin` clears it on each global of the module without a value, and `modInitFlowEnd` puts every such global's flags back |
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
`vtype`'s `MoveType` flag and nothing else — except for a tuple type, which has
no declaration to carry a flag, so `itypeIsMove` asks its elements and answers
yes if any of them does. A type acquires it from `@move`, a finalizer, a
move-typed field or array element, a move-typed tuple element, and — for
references — `refAdoptInfections`: **a reference is a move type when its
permission lacks `MayAlias` or its region is itself a move type.** That single
sentence is why `+rc x` moves while `+rc-mut x` copies, on the same region.

A tuple literal has no storage of its own, so `flowHandleMove` on one
deactivates the source of each element that is itself a move value and leaves
a copyable element's alone; it is the `VTupleTag` arm beside the field, index
and dereference arms that walk inwards to the variable.

**Only a sole owner may be moved out of.** The same inward walk refuses three
sources. A global has no scope in which a deactivated state could be recovered.
A place reached through a **borrowed reference** — a dereference of one, or a
field or element read straight through one, as a slice's element is — belongs
to whatever was borrowed, which releases or finalizes it at the end of its own
scope, so a move out of it would make a second owner (`ErrorMoveOut`). The
borrow's permission does not matter: a `&uni` is the only path to its value
while it lives, and still does not own it. A place reached the same way through
a **shared owning reference** — one that is not a move type, so may be aliased:
`+rc-mut`, `+rc-imm`, `+rc-ro`, `+rc-mut1`, single or slice — is one of possibly
many holders of the value, and the others still point at it after the move, so
it is refused the same way (`ErrorMoveOut`, `flowIsSharedOwner`). A **sole**
owning reference — any `+so`, and `+rc-uni` (what `+rc` means) — is not
refused: moving out through one deactivates the reference's variable like any
other source, and the variable then releases nothing. Copy values are never
walked, so they read out through a borrow or a shared owner freely, and swap
and left-assignment take a move value out of either because they leave it
holding one.

The walk has two entries. `flowHandleMove` is the one `flowHandleMoveOrCopy`
takes, for a value going to a new holder, and it deactivates the source.
`flowResultMove` is `blockFlow`'s, for the value a `return` hands the caller:
it refuses the same sources but deactivates nothing, because a local handed
back is exempted from the scope's release by `flowScopeDealias` instead. A
block or an `if` used as a value has no source of its own either, so the walk
goes on into what it hands back — a block's final expression and the value of
each `break` that leaves it (not a loop's final expression, which loops back),
each branch of an `if` — checking every one of those values. Through
`flowHandleMove` it deactivates the variables that *every* one of them moves
out of: `imm y = {a;}`, `if c {a;} else {a;}` and a loop whose only exits all
`break b.inner` leave their source moved exactly as `imm y = a` does, so it is
finalized once, by the new holder. A variable moved out of by only some of
them is a conditional move and is not deactivated, because `VarMoved` is per
function: `if c {a;} else {Inner[0];}` bound to a variable still finalizes `a`
twice on the path that moves it (and once, correctly, on the other). An
expression statement never reaches `flowHandleMove`, so a block whose value is
thrown away moves nothing.

**A recast is its operand.** Type check hands a value between an enrichment and
its base, in either direction, wrapped in a `CastTag` with no `FlagConvert`: the
two share one representation, so nothing is converted. Every walk here that
looks for the variable behind a value looks through that node to its `exp` —
`flowHandleMove` (the source to deactivate), `flowIsLvalRead` (a recast of an
lvalue still has a holder behind it) and `flowIsScopeResult` (a local returned
as its enrichment or base is still the scope's result). Missing any one of them
leaves one value under two names: finalized or freed twice, or counted once for
two holders. A converting cast (`FlagConvert`) is not looked through.

**The count counts holders.** From the ownership work:

- `+rc[2]` creates the object *and* the first reference. Born at 1.
- `imm a = +rc[2]` adds no holder — the temporary hands over the reference it
  was born with. Still 1.
- `imm b = a` adds one — `a` keeps its reference, `b` gets another. Now 2.
- A tuple is one holder of each counted reference it carries. `imm t = pair()`
  adds nothing (a temporary again); `a, b = t` and `imm u = t` add one per rc
  element, and `t` releases each at scope exit.

`flowHandleMoveOrCopy` is the whole decision:

```c
if (iexpIsMove(*nodep))          flowHandleMove(*nodep);      // deactivate source
else if (flowIsLvalRead(*nodep)) flowInjectRefCount(nodep);   // +1
```

`flowIsLvalRead` asks "does this expression still hold its value after it is
read?" — true for a name use, deref, index and field access, and for a recast
of any of them; false for a temporary. Counting a temporary would add a holder that never existed, and the
allocation would never reach zero.

It is called from exactly seven places — `varDclFlow` (the initializer),
`assignSingleFlow` (the rval), `assignMultRetFlow` (the one rval a
destructuring takes apart), `fnCallFlow` (per argument), `allocateFlow` (the
allocated value), `typeLitFlow` (per field) and `arrayLitFlow` (per element, in
the list form only). The array **fill** form does its own arithmetic instead,
because one value goes to n holders.

**Decrements are never reference-count nodes.** They come from generation: walking a
`dealias` list at scope exit, and `genlStore` releasing an lval's previous value
unless `FlagFirstAssign` says there was none. Both go through
`genlReleaseOwning`: an `so` reference is freed, an `rc` one drops a holder, a
tuple's owning elements one by one.

## 5. What it injects

Flow is not a read-only analysis. Four mutations, all of which generation
depends on:

| Injection | Where | Generation uses it for |
| --- | --- | --- |
| `BlockRetTag` | `blockFlow`, for any block not already ending in one | a loop block, **and** a regular block ending in an expression, both get theirs here — it is where the dealias list hangs |
| `RefCountTag` | `flowInjectRefCountAmt` | `genlRcCounter(val, amt)` |
| `dealias` lists | `flowScopeDealias`, onto every `BreakRetNode` | `genlDealiasNodes` replays them |
| `FlagFirstAssign` | `assignlvalrtype`, when the variable is uninitialized or moved out | `genlStore` skips releasing a previous value the variable does not hold |

**A reference-count node is built only for a counted reference, or a tuple
carrying one.** `flowInjectRefCountAmt` returns early unless the type is a
`RefTag` or `ArrayRefTag` in region `rc` (`flowIsRcRef` — an owning slice is
counted exactly as a single reference is), or a `TTupleTag` with at least one
such element; for the tuple it fills the node's `counts` array with `amt` per
rc element and `0` per other element, `amt` then holding the element count,
and generation's tuple arm adjusts each counted element after an
`extractvalue`. A `+so` reference and a `uni`-permissioned `+rc` reference are
both move types and take the move path instead, as does a tuple carrying one,
so the `so` arm of generation's `RefCountTag` case is unreachable as the code
stands.

**Scope dealiasing.** `flowScopeDealias` walks the variable stack downward from
the top to a start position, so release order is the reverse of declaration
order. Per variable: one that was never initialized or was moved out is
skipped, whatever its type, because it owns nothing to release or finalize; so
is one the scope hands back, which is the caller's to release or finalize, and
`flowIsScopeResult` matches it against the result expression, walking a
`VTupleTag` element by element and a recast to its operand. A move-typed field
or element handed back matches the variable it is taken from
(`flowIsScopeResultOwner`, the walk through fields, elements and owning
dereferences that `flowMoveSource` takes), because moving a part out gives up
the whole variable; releasing it would finalize the part again in the caller,
and what else it held is not released. A copied part matches nothing. A block
or an `if` used as a move value matches what it hands back — its final
expression, each `break` that leaves it, each branch — so a field handed back
through one is exempt the same way. A local handed back on only some branches
is exempt on all of them, and leaks on the others, as a conditional move does.
The exemption is the result's own and is not deactivation, because each
`return` builds its own list and `VarMoved` is not path-sensitive:
`if c {return b.inner;}` exempts `b` on that way out only, and `b` is still
usable and finalized on the path that goes on. The match is on the
declaration the result's name resolves to, not on the name: a `return` asks over the whole function's
stack, where an inner block's `a` and an outer `a` both sit, and only the one
handed back is exempt. What survives both is an `so` or `rc`
reference, single (`RefTag`) or slice (`ArrayRefTag`), or a tuple carrying one
(`flowIsOwningType`), added to the list; anything else asks
`itypeGetDropFnDcl` and, if there is one, builds a call to the drop fn on a
`&uni` borrow, positioned on the result expression, or on the jump that ends the
scope where there is no result expression — a `continue` hands back no value.
Generation releases a tuple variable element by element
(`genlReleaseOwning`); a tuple carrying a `so` reference is a move type, so
destructuring or copying it deactivates the variable and the scope releases
nothing of it.

**The result expression is walked before the list is built.** `blockFlow` calls
`flowLoadValue` over a block's result — a `return`'s, a `break`'s or an injected
`blockret`'s — and only then `flowScopeDealias`, because the walk is what marks
a variable moved. A local handed to a call in final position, `hold(a)` as the
last expression, has left the scope by the time the list that would release it
is built. The list is built against the result node as it stood before the
walk, which is the node whose name the exemption above is about.

**Where a jump's start position comes from.** `BlockNode.flowmark` is the flow
stack position `blockFlow` recorded on entering that block. A `break` or
`continue` passes its *target* block's mark, through `blockJumpMark`, so it
releases from where it stands down to the block it names rather than its own
scope alone; `return` passes 0, the whole function. The mark is transient — set
by `blockFlow` and valid only while flow is inside that block — and
`blockJumpMark` falls back to the current position for a jump whose target
failed to resolve.

## 6. What it decides, and what it does not

| Analysis | In flow? | Enforced | Not enforced |
| --- | --- | --- | --- |
| **Move / ownership** | yes | `ErrorMove` on use of a moved-out or uninitialized variable; move out of a global, or out through a borrowed or a shared owning reference, refused | field granularity — moving `p.x` deactivates all of `p`; conditional moves; loop-carried moves |
| **Escape / lifetime** | representation in type check, enforcement here | storing a borrow into a longer-lived lval; returning a borrow of a local; a borrow arriving through a call's result, singly or as one of several values destructured into lvals, each carrying the narrowest argument borrow's scope; a `&mut &T` argument whose pointee would outlive another borrow passed with it; a borrow coerced to another reference type, whether widened to a base trait's reference or made a virtual reference | a borrow laundered through a variable; a borrow stored in a field or captured; distinguishing parameter lifetimes — there is no lifetime annotation syntax; freezing a borrow's source |
| **De-aliasing / drops** | flow decides, generation executes | scope-exit release of `so`/`rc` refs and slices, and of drop-fn structs, from a jump down to the block it names | arrays of owning references; a variable moved out, or initialized, on only one path — see Hazards |
| **Permission** | `MayWrite` and `MayRead` | `ErrorNoMut` on assignment and swap; `ErrorNoRead` on a read through a reference — a dereference, an index, or a field of a virtual reference | `MayAliasWrite`, `RaceSafe`, `IsLockless` are populated and read nowhere |
| **Initialization** | yes | `ErrorMove` "has not been initialized" | "initialized on one branch" reads as initialized everywhere; the unused-variable warning in `flow.h`'s header does not exist |
| **Array fill rules** | yes | `ErrorBadFill` for a repeated move value; `ErrorFillCount` for a non-constant count | — |

Everything else about permissions is type check's: `permMatches` in
`borrowTypeCheck`, and variance in the reference matchers.

### Diagnostics

| Code | Site | Condition |
| --- | --- | --- |
| `ErrorInvType` | `flowHandleMove`, `flowResultMove` | move out of a global variable |
| `ErrorMoveOut` | `flowHandleMove`, `flowResultMove` | move out through a borrowed reference, or through a shared (aliasable) owning one |
| `ErrorInvType` | `assignlvalrtype` | lval outlives the borrowed reference stored into it |
| `ErrorNoMut` | `assignlvalrtype`, `swapFlow` | no write permission |
| `ErrorNoRead` | `flowLoadThroughRef` | no read permission on the reference a dereference, an index or a virtual-reference field reads through |
| `ErrorMove` | `nameuseFlow` | uninitialized, or moved out |
| `ErrorBadFill` | `arrayLitFlow` | a fill may not repeat a move value |
| `ErrorFillCount` | `arrayLitFlow` | fill count not constant, or too large |
| `ErrorEscape` | `returnFlowEscape` | returned borrow outlives the local it points at |
| `ErrorCallEscape` | `fnCallFlowStoredBorrow` | a `&mut &T` argument points at a place that outlives another borrow passed to the same call |

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
a `dealias` list; every recognized counted acquisition has a `RefCountNode`; every
first-assignment target carries `FlagFirstAssign`.

**What generation relies on.** `genlBlock`, `genlBreak` and `genlReturn` call
`genlDealiasNodes` and do no analysis of their own. If flow did not run, the
lists are NULL, `genlDealiasNodes` returns immediately, and **nothing is ever
released** — there is no fallback. Likewise, without `FlagFirstAssign` every
first assignment to an uninitialized owning variable releases garbage, and a
reassignment after a move releases what the new owner holds: `genlStore` frees
an `so` lval's previous value and drops a holder of an `rc` one — single,
slice, or tuple element — whenever the flag is absent. `assignlvalrtype` sets
it when the variable is not `VarInitialized`, or is `VarMoved`, at the
assignment.

**One layout invariant generation depends on and flow does not state.**
`genlRcCounter` finds the count by bitcasting the reference to `usize*` and
GEPing `-1`. That is correct only because `rc` has exactly one `usize` field and
the built-in permissions are zero-sized. See [Generation](generation.md),
"The allocation header".

## 8. Hazards

- **A moved-out variable is skipped at scope exit on every path**, because
  `VarMoved` is a whole-function summary. That leaks rather than double-frees,
  which is the deliberate choice; the in-code comment says so. A reassignment
  reads the same summary at its own site: moved before the assignment in
  source order, the variable is released there on no path. Moved only *after*
  it in source order — by an earlier iteration of a loop both sit in — the
  reassignment releases on every iteration, and from the second one frees what
  the move handed over. That is the loop-carried move the table above lists as
  unenforced, in the one place it double-frees rather than leaks; it is the
  same in both regions.
- **A variable moved out by only some of the values a block, an `if` or a loop
  hands back is not deactivated at all**, so where that value goes to a new
  holder the variable is finalized twice on the path that moved it:
  `imm y = if c {a;} else {Inner[0];}` finalizes `a` in `y` and again at `a`'s
  scope exit when `c` is true. Deactivating it would instead leak it on the
  other path. Either way it needs the drop flag a conditional move needs.
- **A variable initialized on only one path is released on every path**, because
  `VarInitialized` is the same kind of summary: once an assignment anywhere
  before the scope exit has set it, the exit releases the variable whether or
  not that assignment ran. On the path that skipped it, that frees storage that
  never held a reference. Only a variable that is never assigned at all is
  skipped. Releasing the right thing on each path needs a runtime drop flag per
  variable — the same mechanism a conditional move needs — and belongs with it.
- **`flowIsLvalRead` is not `iexpIsLval`.** They disagree on recursion into
  `objfn` and on string literals. Do not substitute one for the other.
- **`fnCallFlow` does not flow `objfn`**, so a call through an uninitialized
  function-reference variable is not reported.
- **`flowLoadValue`'s `default:` arm reports `ErrorUnreachable` and stops.** An
  unhandled tag therefore fails the compile rather than passing through it —
  passing through would mean no move check, no alias injection and no
  initialization check for that value. Whether any tag reaches it is
  unestablished.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the only entry point; the per-function error-delta gate |
| `ir/stmt/module.c` | `modInitOf`, `modInitFlowBegin`, `modInitFlowEnd` | round a module's `init` only: its module's globals without a value start the pass uninitialized, as locals, so `init` assigns each once and reads none first; one never assigned is `ErrorGlobalUninit`. [module](../nodes/module.md), "Init and final" |
| `ir/flow.c` | `flowLoadValue` | the walk's spine — tag dispatch for a value being read |
| | `flowLoadThroughRef` | `MayRead` on the reference a value is read through; called from `derefFlow`, `fnCallArrIndexFlow` and `fnCallFldAccessFlow` |
| | `flowHandleMoveOrCopy` | move vs. alias, for a value going to a new holder |
| | `flowHandleMove` | deactivate the source — each move-typed element's, for a tuple literal; for a block or an `if`, what every value it hands back moves out of; refuse a move out of a global, or out through a borrowed or a shared owning reference |
| | `flowResultMove` | the same refusals for a returned value, deactivating nothing |
| | `flowIsLvalRead` | the temporary-vs-lvalue test that makes counting correct |
| | `flowInjectRefCountAmt` | wrap a counted reference, or a tuple carrying one, in a `RefCountNode` |
| | `flowIsRcRef`, `flowIsOwningType` | is this type counted; must a variable of this type be released |
| | `flowScopePush`, `flowScopePop`, `flowAddVar` | the variable stack |
| | `flowScopeDealias` | build a scope's release list; skip an uninitialized, moved-out or handed-back variable |
| `ir/exp/block.c` | `blockFlow` | scope push/pop, `blockret` injection, result walk then dealias capture; a `return`'s move source |
| `ir/exp/if.c` | `ifFlow` | both arms against one shared state |
| `ir/exp/assign.c` | `assignlvalrtype` | `MayWrite`, `VarInitialized`/`VarMoved`, `FlagFirstAssign`, borrow lifetime |
| `ir/exp/nameuse.c` | `nameuseFlow` | the only place the two flags are *diagnosed* on; both `ErrorMove` messages |
| `ir/exp/borrow.c` | `borrowFlow` | **empty** |
| `ir/stmt/return.c` | `returnFlowEscape` | `ErrorEscape` for a returned borrow of a local |
| `ir/exp/fncall.c` | `fnCallFlowStoredBorrow` | `ErrorCallEscape` for a `&mut &T` argument the callee could store a narrower borrow through |
| `ir/exp/arraylit.c` | `arrayLitFlow` | fill-form rules and the n / n-1 alias amount |
| `ir/types/reference.c` | `refAdoptInfections` | where a reference type acquires `MoveType` |
| `ir/types/region.c` | `isRegion`, `regionAllocTypeCheck` | region identity; `alloc`/`init` validation |
| `genllvm/genlalloc.c` | `genlRcCounter`, `genlDealiasNodes` | what consumes everything flow injected |

Test sources that pin behavior precisely: `test/cases/move/move-flow-*.cone`,
`test/cases/region/region_flow*.cone`, `test/cases/ref/ref_flow.cone`,
`test/cases/ref/ref_flow_return.cone`, `test/cases/core/core_flow_gate.cone`.

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What the three reference axes mean, and what each permits | [References and Regions](../../../../doc/design/references-and-regions.md) |
| Which safety properties actually hold today | [Safety](../../../../doc/design/safety.md) |
| When a function is type checked at all | [Type Check Phase](type-check.md) |
| What a borrow's type records, and where | [Type Check Reasoning](type-check-reasoning.md), "Borrows: where type check stops" |
| How the allocation header is laid out | [Generation](generation.md), "The allocation header" |
