Data flow analysis enforces Cone's ownership rules: what moves, what is
counted, what is released, and where a borrow may not reach. It is the part of
the compiler least like other languages and the least guessable from the source.

Read this before changing anything about ownership, moves, aliasing, drops, or
borrow lifetimes — and read "What a reader from Rust will get wrong" before
assuming it does anything a borrow checker does. Most of it does not; the loan
walk, which freezes a borrow's source, does some of it, for some borrows.

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

**It is not Rust's borrow checker, and reasoning by analogy to Rust will be
wrong.** The loan walk takes Rust's rule for freezing a borrow's source, and
takes it for a borrow held in a local only, by its own mechanism.
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
each function's body — so it is the single entry point to the whole pass. The
loan walk (§6) is a second walk of one function's body, right after `blockFlow`
in the same call, and only of a function the gate marked.

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

1. **The borrow checking there is covers a borrow held in a local, and only
   by freezing its source.** The loan walk ("The loan walk", below) refuses a
   source touched while a borrow of it held in a local variable whose type is
   a borrowed reference is still to be used: changed, moved, ended, borrowed
   in conflict, or, under a mutable borrow, read. Everything else is as it was:
   a borrow a method returns (`list[0usize]`, `a.alloc(v)`), one held inside
   another value, and a method called through a reference freeze nothing, and
   two copies of one `&mut` may reach one place two ways. `borrowFlow`, in the
   main walk, still asks only that what is borrowed was not moved out, as a read
   does; it deactivates nothing and records nothing about the borrow.
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
4. **The main walk is path-insensitive and does not iterate.** No CFG, no
   lattice, no join, no fixed point. `ifFlow` walks both arms against one shared
   mutable state, so a move in the `then` arm marks the source moved for the
   `else` arm and for everything after. A loop body is walked once. The loan
   walk is the other kind: it keeps its state per path, joins the arms of an
   `if` and the paths into a loop's head and out of a block, and walks a loop
   body again until its head stops growing — but only for a function the gate
   marked, and over the same block-structured IR. **Not building a CFG is a
   deliberate design choice**, not a simplification to be outgrown — the
   block-structured IR is held to be easy enough to follow directly, joins and
   all.
5. **Ownership is not one model, and flow reads which one from the region
   ref.** One with `alias` (`rc`) is counted; one declaring `Move` (`so`) has a
   single owner, and a copy of a reference to it is a move; one with neither
   (a collector's shape) shares its references for nothing; `borrowRef` is a
   sentinel node, not a struct, and is a reference's default region. No region
   is known by name ([What a region is](../nodes/module.md)).
6. **Permission belongs to the reference, not the binding.**
   `imm fixed = &mut target` is a writable target through an unrebindable name.
7. **`&uni` is not `&mut` with extra rules.** `uni` lacks `MayAlias`, which is
   exactly what makes a reference carrying it a move type.

## 3. State

`FlowState` is made per function by `flowStateInit`. Its fields:

| Field | Read by |
| --- | --- |
| `fnsig` | `blockFlow`, to `flowAddVar` each parameter on entering the function's main block |
| `scope` | `blockFlow`, only as `if (++fstate->scope == 2)` — the test for "this is the main block" |
| `gate` | `fnDclTypeCheck`, which runs the loan walk on a function it marks; `flowGateCount` tallies it for `-V 2` (below, "The gate") |
| `inflight`, `inflightcnt` | the gate's `flowGateUse`, from `nameuseFlow` and `nameuseFlowBorrowed` |

### The gate

The walk also records whether the function holds a borrow in a way that only a
walk following each path could check. `fnDclTypeCheck` reads it: a function it
marks, and in which `blockFlow` reported no error, is walked again by the loan
walk (below); any other is not, which is what keeps the cost of freezing off
code that holds no borrow. `FlowState.gate` gathers one bit per trigger:

| Bit | Set by | When |
| --- | --- | --- |
| `FlowGateHolder` | `varDclFlow`, `assignFlow`, `swapFlow` | a local declared, or a place assigned or swapped, whose type carries a borrow (a local assigned by name is not asked again: its declaration was). The temporary an operator changing its operand in place borrows it through (`x += 1`, `v <- (a, b)`; named `tempName`) is not asked: it is the operator's, as a method's receiver is, and the loan walk holds nothing in it |
| `FlowGateResult` | `blockFlow` | a `return`, `break` or block end hands out a value carrying a borrow that is not itself a bare borrowed reference (a bare one has its scope number checked already) |
| `FlowGateStore` | `fnCallFlow` | a call with a `&mut X` argument, `X` carrying a borrow, beside another argument carrying one |
| `FlowGateInCall` | `nameuseFlow`, `nameuseFlowBorrowed` | a variable named while a borrow of it made by an earlier operand of the same call, struct or array literal or value tuple is still waiting for it (`v.add(v.len())`) |

"Carries a borrow" is `itypeCarriesBorrow`: the type is a borrowed reference, or
an owning reference, pointer, array, tuple or struct (an enum's variants
included) reaching one. A struct's answer is remembered on it
(`StructNode.carriesborrow`), because asking afresh at every variable cost flow
10–20% ([Performance](../compiler/performance.md), "Measuring it"). A parameter
is not a trigger: what a caller lent is frozen by the caller.

For `FlowGateInCall`, each operand that is a borrow (`BorrowTag` or
`ArrayBorrowTag`, through casts) pushes the variable at the root of its place
onto `inflight` once it is walked, and the call or literal pops back to where it
started. A name use checks the list only when it is not empty, so a function
with nothing waiting pays one test per name use. More than `FlowInflightMax`
waiting at once gates the function.

Each trigger costs O(1) per node, and must cost almost nothing where it does
not fire, since it is asked of every function. So each is an inline test in
`ir/flowgate.h` (included at the end of `ir.h`, after the node headers it reads)
that looks through one name use to the declaration and dismisses a number, void,
a struct already known to carry nothing, a call whose arguments are not
references, an operand that is not a borrow; only then is the question asked out
of line in `flow.c`. Asked through calls that resolved each type first, the same
triggers cost flow 25–30% on code holding no borrow; inline, about 4%. An
ordinary compile stops asking once any bit is set; `-V 2` asks every trigger to
the end, so that it can count each, and prints
`Flow gate: G of N functions (holder …, result …, store …, in-call …)`
(`flowGatePrint`). The loan walk only follows borrowed-reference locals so far,
so a function gated for anything else is walked and finds nothing to hold; the
triggers are the ones the walk will need as it grows, and the walk of such a
function costs little.

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
| `VarHollow`, with the moves that did it in `VarDclNode.hollowed` | `flowHandleMove`, for a move out through a local sole owner | `assignlvalrtype` on reassignment |

Because these live on the declaration and are never saved or restored, **they
are a running summary over the whole function, not per-program-point state.**
Every imprecision below follows from that one fact.

A file-static variable stack (`gVarFlowStackp`) records which declarations are
in scope. It is global mutable state, safe only because flow never runs
re-entrantly — it never descends into a callee. `VarFlowInfo.flags` is dead.

## 4. Moves and counting

**Move-ness is a type property: the built-in trait `Move`, which the type
declares or the compiler grants** ([struct](../nodes/struct.md), "Move and
Copy"). Flow never looks at the trait: `iexpIsMove` is `vtype`'s `MoveType`
flag and nothing else — except for a tuple type, which has no declaration to
carry a flag, so `itypeIsMove` asks its elements and answers yes if any of them
does. A type acquires it from `is Move`, a finalizer, a move-typed field or
array element, a move-typed tuple element, and — for references —
`refAdoptInfections`: **a reference is a move type when its permission lacks
`MayAlias` or its region ref is itself a move type**, which a region ref is by
declaring `is Move`, as `so` does. That sentence is why `+rc x` moves while
`+rc-mut x` copies, on the same region, and why a reference into a region ref
declaring neither `Move` nor `alias` copies freely under any aliasable
permission.

A tuple literal has no storage of its own, so `flowHandleMove` on one
deactivates the source of each element that is itself a move value and leaves
a copyable element's alone; it is the `VTupleTag` arm beside the index and
dereference arms that walk inwards to the variable.

**Nothing moves out of a field.** The walk's field arm walks no further: a
move-typed value that is a field — of a struct or a tuple, wherever the struct
is — or that is reached through one (`*b.r`, what an owning field points at;
`row.hs[0]`, an element of an array field) is refused (`ErrorMoveField`,
`flowRefuseMoveField`), at the move, before any question of who owns it. The
struct would be left with a hole in it: usable neither whole nor field by field,
and with no answer to what its death finalizes. The refused move deactivates
nothing. Swap and left-assignment take a value out of a field, and moving the
whole struct takes every field with it. A copy-typed field is never walked, so it
reads out freely.

**Only a sole owner may be moved out of.** The same inward walk refuses three
more sources. A global has no scope in which a deactivated state could be recovered.
A place reached through a **borrowed reference** — a dereference of one, or an
element read straight through one, as a slice's element is — belongs
to whatever was borrowed, which releases or finalizes it at the end of its own
scope, so a move out of it would make a second owner (`ErrorMoveOut`). The
borrow's permission does not matter: a `&uni` is the only path to its value
while it lives, and still does not own it. A place reached the same way through
a **shared owning reference** — one that is not a move type, so may be aliased:
`+rc-mut`, `+rc-imm`, `+rc-ro`, `+rc-mut1`, single or slice — is one of possibly
many holders of the value, and the others still point at it after the move, so
it is refused the same way (`ErrorMoveOut`, `flowIsSharedOwner`). A **sole**
owning reference — any `+so`, and `+rc-uni` (what `+rc` means) — is not
refused, and moving out through one held in a local variable **hollows** the
variable, below. Copy values are never
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
`break a` leave their source moved exactly as `imm y = a` does, so it is
finalized once, by the new holder. A variable moved out of by only some of
them is a conditional move and is not deactivated, because `VarMoved` is per
function: `if c {a;} else {Inner[0];}` bound to a variable still finalizes `a`
twice on the path that moves it (and once, correctly, on the other). An
expression statement never reaches `flowHandleMove`, so a block whose value is
thrown away moves nothing.

**A move out through a sole owner.** When the inward walk reaches a local
variable holding an owning reference through that reference — `*b`, `**b`
(through an owning reference that is `b`'s value), a slice's element `s[0]` —
the value, or an element of it, leaves the allocation, but the variable still
owns the allocation, whose memory must go back (`flowOwningLocal`). Such a move
**hollows** the variable rather than moving it: `VarHollow` is set and the
move's outermost node, the expression naming what moved (`top` in
`flowMoveSource`), is added to `VarDclNode.hollowed`. A hollowed variable is
refused for any further use as a moved one is (`nameuseFlow`), and where it is
released — its scope's exits, or a reassignment — it is released **hollow**
(`HollowNode`): generation walks each recorded move inwards to the variable to
learn what left, and frees the memory without finalizing it
(`genlHollowRelease`; [Generation](generation.md), "The allocation header"). No
field is ever on the chain: `b.inner`, a field through the injected
dereference, is refused as a field.

A hollowing on only some of the values a block or an `if` hands back hollows
the variable anyway, unlike a whole conditional move: the value is then
freed but not finalized on the paths that left it in place, rather than
finalized a second time on the ones that moved it. A variable moved whole by
the same move as well is left to that move (`MoveParts.wholes`).

**A recast is its operand.** Type check hands a value between an enrichment and
its base, in either direction, wrapped in a `CastTag` with no `FlagConvert`: the
two share one representation, so nothing is converted. Every walk here that
looks for the variable behind a value looks through that node to its `exp` —
`flowHandleMove` (the source to deactivate), `flowIsLvalRead` (a recast of an
lvalue still has a holder behind it) and `flowIsScopeResult` (a local returned
as its enrichment or base is still the scope's result). Missing any one of them
leaves one value under two names: finalized or freed twice, or counted once for
two holders. A converting cast (`FlagConvert`) is not looked through.

**A match's binding is the matched value.** `case imm c Circle` desugars to a
variable initialized with the matched value converted to its variant (a
`CastTag` carrying `FlagMatchBind`), and the match holds the matched value in
a variable of its own. The binding is that value under the variant's name, not
a second value: `flowMatchBound` answers the matched value's name for it, and
the walks treat the two as one. `flowScopeDealias` never releases the binding,
since the matched value's variable releases it, as the enum, whichever arm ran;
`flowHandleMove` on the binding deactivates the matched value too; and
`flowIsScopeResult` exempts the matched value when the binding is handed back.
A guard's binding (`case imm c Circle if g`) is a second binding of the same
value, and owns nothing either.

**The count counts holders.** From the ownership work:

- `+rc[2]` creates the object *and* the first reference. Born at 1.
- `imm a = +rc[2]` adds no holder — the temporary hands over the reference it
  was born with. Still 1.
- `imm b = a` adds one — `a` keeps its reference, `b` gets another. Now 2.
- A tuple is one holder of each counted reference it carries. `imm t = pair()`
  adds nothing (a temporary again); `a, b = t` and `imm u = t` add one per rc
  element, and `t` releases each at scope exit.
- A struct, an enum, a tuple or an array is one holder of each counted
  reference its death releases (`flowHeldCounted`), however deep: a struct's
  drop releases what its fields own, an enum's what its variant's fields own,
  and a tuple or an array dies element by element. So `imm s2 = s` over a
  struct with a `+rc-mut` field adds one, `imm o2 = o` over an
  `Option[+rc-mut T]` adds one through the variant the tag picks, and a copy of
  an array of them adds one per element.
- A tuple literal holds its elements' values, each moved or copied into it on
  its own, as a struct literal's fields are: `(r, 5)` over a counted variable
  `r` adds one.

`flowHandleMoveOrCopy` is the whole decision:

```c
if (*nodep is a tuple literal)   each element, as below;
else if (iexpIsMove(*nodep))     flowHandleMove(*nodep);      // deactivate source
else if (flowIsLvalRead(*nodep)) flowInjectRefCount(nodep);   // +1
```

`flowIsLvalRead` asks "does this expression still hold its value after it is
read?" — true for a name use, deref, index and field access, and for a recast
of any of them; false for a temporary. Counting a temporary would add a holder that never existed, and the
allocation would never reach zero.

It is called from exactly seven places — `varDclFlow` (the initializer),
`assignSingleFlow` (the rval), `assignMultRetFlow` (the one rval a
destructuring takes apart), `fnCallFlow` (per argument), `allocateFlow` (the
allocated value), `typeLitFlow` (per field) and `arrayLitFlow` (per element in
the list form, and a fill's tuple literal's elements). The array **fill** form
does its own arithmetic for the value it repeats, because one value goes to n
holders: a value that is or holds a counted reference gains n, or n - 1 for a
temporary.

**Decrements are never reference-count nodes.** They come from generation: walking a
`dealias` list at scope exit, and `genlStore` releasing an lval's previous value
unless `FlagFirstAssign` says there was none, and a value's death releasing the
owners it holds. Each goes through `genlReleaseOwning`: one owner goes away,
through the region's `dealias` where it has one, as the value's death where the
region is `Move`, and as nothing otherwise; a tuple's owning elements one by
one. A hollowed variable's owner goes away the same way, through a
`HollowNode` instead, and its death is hollow.

## 5. What it injects

Flow is not a read-only analysis. Five mutations, all of which generation
depends on:

| Injection | Where | Generation uses it for |
| --- | --- | --- |
| `BlockRetTag` | `blockFlow`, for any block not already ending in one | a loop block, **and** a regular block ending in an expression, both get theirs here — it is where the dealias list hangs |
| `RefCountTag` | `flowInjectRefCountAmt` | `genlRegionAlias(val, amt)`: the region's `alias`, once per owner added |
| `dealias` lists | `flowScopeDealias`, onto every `BreakRetNode` | `genlDealiasNodes` replays them |
| `FlagFirstAssign` | `assignlvalrtype`, when the variable is uninitialized, moved out or hollowed | `genlStore` skips releasing a previous value the variable does not hold whole |
| `HollowTag` | `flowScopeDealias`, in a `dealias` list, for a hollowed variable or one whose part the scope hands back; `assignSingleFlow`, wrapped round the value stored into a hollowed variable | `genlHollowRelease`: the owner goes, and a death frees without what moved — after the new value is evaluated, for the wrapper, as `genlStore` orders a whole release |

**A reference-count node is built only for a counted reference, or a value
holding one.** `flowInjectRefCountAmt` returns early unless the type is a
`RefTag` or `ArrayRefTag` into a region with `alias` (`flowIsRcRef`,
`regionIsCounted` — an owning slice is counted exactly as a single reference
is), or a struct, enum, tuple or array whose death releases one
(`flowHeldCounted`); for a tuple it fills the node's `counts` array with `amt`
per element that is or holds a counted reference and `0` per other element,
`amt` then holding the element count, and generation's tuple arm adds the
owners to each such element after an `extractvalue`. The struct, enum and array
arm (`genlAliasHeld`) mirrors the death: through each field of a struct, the
variant an enum's tag picks, and each element of an array. A reference into a
`Move` region (`so`) and a `uni`-permissioned reference into one with `alias`
are both move types and take the move path instead, as does a tuple carrying
one. A reference into a region with neither is copied with no node at all.

**Scope dealiasing.** `flowScopeDealias` walks the variable stack downward from
the top to a start position, so release order is the reverse of declaration
order. Per variable: one that was never initialized or was moved out is
skipped, whatever its type, because it owns nothing to release or finalize; so
is a match's binding, which owns nothing (`flowMatchBound`); so
is one the scope hands back, which is the caller's to release or finalize, and
`flowIsScopeResult` matches it against the result expression, walking a
`VTupleTag` element by element, a recast to its operand, and a match's binding
to the matched value. A move-typed
array element handed back matches the variable it is taken from
(`flowIsScopeResultOwner`, the walk through elements and owning dereferences
that `flowMoveSource` takes), because moving an element out gives up the whole
variable; releasing it would finalize the element again in the caller, and what
else it held is not released. The whole value, `*b`, or an element taken out
through the variable's own owning reference does not exempt it: the variable
still owns the allocation, and its entry is a `HollowNode` naming what moved,
so the memory goes back without it. A copied part matches nothing, and a field
handed back was refused. A block or an `if` used as a move value matches what
it hands back — its final expression, each `break` that leaves it, each branch.
A local handed back on only some branches is exempt on all of them, and leaks
on the others, as a conditional move does. The exemption is the result's own
and is not deactivation, because each `return` builds its own list and
`VarMoved` is not path-sensitive: `if c {return a;}` exempts `a` on that way
out only, and `a` is still usable and finalized on the path that goes on. The match is on the
declaration the result's name resolves to, not on the name: a `return` asks over the whole function's
stack, where an inner block's `a` and an outer `a` both sit, and only the one
handed back is exempt. What survives both is released as its type dies: a
struct or an enum has a drop (`itypeGetDropFnDcl`), and the list gets a call
to it on a `&uni` borrow, positioned on the result expression, or on the jump
that ends the scope where there is no result expression — a `continue` hands
back no value; anything else with anything to do as it dies
(`itypeNeedsFinal`) — an owning reference into a region, single (`RefTag`) or
slice (`ArrayRefTag`), or a tuple or an array of values that finalize or own —
is added to the list itself, and generation finalizes it in place
(`genlFinalizeAt`), a tuple element by element, an array in element order. A
tuple carrying a `so` reference is a move type, so destructuring or copying it
deactivates the variable and the scope releases nothing of it.

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

## 6. The loan walk

A second walk, over a function the gate marked, once `blockFlow` has found no
error in it (`fnDclTypeCheck` calls `flowPathWalk`, `ir/flowpath.c`). It
enforces **freezing**: a borrow held in a local freezes its source until the
borrow's last use. Borrow freezing is its one client (`ir/flowloan.c`); the path
machinery — state per path, joins, loops walked to a fixed point — is meant to
serve drop flags as well.

**It is read-only.** It injects nothing and changes no node, so it may walk a
loop body more than once; that is its difference from the main walk, which
mutates and visits each node once. It walks the tree as `blockFlow` left it,
through the `RefCountTag` and `HollowTag` wrappers, every block ending in a
jump or a `BlockRetTag`, and fails with `errorUnreachable` on a value tag it has
no case for, as `flowLoadValue` does.

**The words.** A *place* is a root and a path (`Place`). The root is a variable
— local, parameter or global — or what a borrowed-reference variable points at:
`*r`, `r.x` through the injected dereference, `s[i]` on a slice. The path is
steps: a field, an element (any index — `a[0]` and `a[1]` overlap), a
dereference of an owning reference. Two places overlap when they share a root
and one path is a prefix of the other, step by step; only two different fields
are disjoint. Nothing is tracked through a raw pointer. A *loan* is one borrow
(`BorrowTag`, `ArrayBorrowTag`) of a place, the same loan each time a loop
walks it again; it is *exclusive* when its permission has `MayWrite`, *shared*
when it has `MayRead` only, and a *pin* (`opaq`) when it has neither. A *holder*
is a local or a parameter whose type is a borrowed reference: it may hold
loans. An *access* is what an expression does to a place:

| Access | Conflicts with a live loan that is |
| --- | --- |
| a read, a copy out | exclusive |
| a read-only borrow | exclusive |
| a write — `=`, an operator changing it in place (`+=`, `++`, `<-`), one side of `<=>` — or a mutable borrow | shared or exclusive |
| a move, a replacement of the whole (its old value finalized), the end of its scope | any, a pin too |
| an `&opaq` borrow | none |

A holder reaching its loan's source through the loan — `*b = 9`, `r.x` — uses
the holder; it does not access the source.

**What holds what.** Each expression walked yields the loans its value may
carry (a `PathSet`): a borrow, its new loan and whatever the holder at the root
of the borrowed place holds (a reborrow `&mut *r`, or `&r`, keeps what `r`
holds); a holder named, or a place read through it, what it holds; a recast, an
`if`, a block, and a tuple, struct or array literal, the union of theirs; a
call, nothing. A holder's declaration, or an assignment to the whole of it,
*replaces* what it holds with what the value carries.

**How long: the last use, found forward.** At an access that conflicts with a
loan, each holder that may hold the loan on this path gets a *pending conflict*
(`loanAccess`; `Loan.mayhold` lists who to ask). It is not reported yet: the
holder may never be used again. A *use* of a holder — any name use of it but as
the whole target of an assignment or swap — fires the pending conflicts it
carries (`loanUse`): that is the error, `ErrorFrozen`, reported at the access,
naming the borrow and the use. Reassigning the holder whole, or its dying,
drops them, because it was dead at the access. So a pending conflict fires
exactly when a path runs from the access to a use of the holder without
reassigning it, which is what "the holder is live at the access" means. Each
access is reported once, however many loans it conflicts with.

**Paths.** The state — per variable, the loans it may hold and the pending
conflicts a use of it fires (`PathVar`) — is one current state and an undo log
(`pathSetFacts`, `pathRollback`). A fork remembers the log's position; each
arm's end takes what the arm changed (`pathDelta`) and rolls back; the join sets
each fact any arm changed to the union over the arms (`pathJoin`), an arm that
did not change it contributing its value at the fork, so a join costs what the
arms changed rather than the state's size. An `if`'s branches are paths from
the state its conditions leave, a missing `else` a path of its own; an `and`'s
or `or`'s right operand is an arm; a `break` or `continue` ends the blocks it
leaves and deposits its path with the block it names (`pwJump`), and a block
joins its fall-through end with every `break` that left it; a `return` ends the
path, every holder being a local of the function. A path that jumped away
contributes nothing where it would have fallen through.

**Loops.** A loop body is walked from the state at its head; the paths back to
the head — its end, each `continue` — join into it, and if that added anything
the body is walked again from the grown head. Facts only grow, so it settles,
nearly always on the first walk. An error fires on whichever walk first meets
it, once. A loop still growing after four walks has each holder that changed
around it widened to every loan (`pathSetAll`): conservative, and then it
settles. The paths out are its `break`s. `-V 2` prints
`Loan walk: F functions, L loops, W walked again, C widened` (`flowPathPrint`).

**Scope ends.** As a block ends, at its end or at a jump leaving it
(`pwScopeEnd`), its holders die first, dropping what they were pending on; then
each of its variables ends, an access conflicting with every loan of it: a
holder declared outside the block still holding one, and used afterwards, is
refused. That is the laundered borrow kept past its source,
`{ mut a = 1i64; imm b &i64 = &a; r = b; } *r`, which the lifetime check does not
see because assignment does not carry a scope onto a variable.

**What is not held.** The temporary an operator changing its operand in place
borrows it through (`x += 1` is `{imm tmp = &mut x; *tmp = *tmp + 1}`, and
`v <- (a, b)` appends through one; both named `tempName`) is an access, a write,
and holds nothing, as a method's receiver does: `k += k` compiles. A borrow a
call returns — a method's included, `list[0usize]`, `a.alloc(v)` — carries no
loan, a borrow held inside another value has no holder, and a method called
through a reference (`r.bump()` passes `r` itself: a use of it, not a reborrow
of `*r`) accesses nothing. Copies of one `&mut` reach one place two ways
unchecked, and a global a callee changes is invisible.

**Its state** is file-static, as the variable stack is, and safe for the same
reason: flow never runs re-entrantly (`flowPathWalk` refuses to). The buffers
come from the compiler's arena, small at first, and are kept from one walk to
the next; a variable's index is `VarDclNode.flowindex` for the length of a walk.

## 7. What it decides, and what it does not

| Analysis | In flow? | Enforced | Not enforced |
| --- | --- | --- | --- |
| **Move / ownership** | yes | `ErrorMove` on use of a moved-out or uninitialized variable, and on a borrow of a moved-out one; move out of a field, or of a global, or out through a borrowed or a shared owning reference, refused | element granularity — moving `a[0]` deactivates all of `a`; conditional moves; loop-carried moves |
| **Escape / lifetime** | representation in type check, enforcement here | storing a borrow into a longer-lived lval, by assignment or by either direction of a swap; returning a borrow of a local; a borrow arriving through a call's result, singly or as one of several values destructured into lvals, each carrying the narrowest argument borrow's scope; a `&mut &T` argument whose pointee would outlive another borrow passed with it; a borrow coerced to another reference type, whether widened to a base trait's reference or made a virtual reference | a borrow laundered through a variable and returned (kept past its source's scope and used, it is refused by freezing, below); a borrow stored in a field or captured; distinguishing parameter lifetimes — there is no lifetime annotation syntax |
| **Freezing** | the loan walk, on a gated function | a borrow held in a local whose type is a borrowed reference, and its copies, freeze the source until the last use: `ErrorFrozen` at a change, a move, a conflicting borrow, the source's end, and, under a mutable borrow, a read | a borrow a call returns; a borrow held inside another value; a method called through a reference; two copies of one `&mut`; a global a callee changes |
| **De-aliasing / drops** | flow decides, generation executes | scope-exit release of owning refs and slices, of drop-fn structs and enums, and of tuples and arrays holding what finalizes or owns, from a jump down to the block it names | an array an element was moved out of leaks the rest; a variable moved out, or initialized, on only one path — see Hazards |
| **Permission** | `MayWrite` and `MayRead` | `ErrorNoMut` on assignment and swap; `ErrorNoRead` on a read through a reference — a dereference, an index, or a field of a virtual reference | `MayAliasWrite`, `RaceSafe`, `IsLockless` are populated and read nowhere |
| **Initialization** | yes | `ErrorMove` "has not been initialized" | "initialized on one branch" reads as initialized everywhere; a variable never initialized may be borrowed, so a method taking it `&mut` can fill it, and nothing then stops a field it left unset being read through the borrow; the unused-variable warning in `flow.h`'s header does not exist |
| **Array fill rules** | yes | `ErrorBadFill` for a repeated move value; `ErrorFillCount` for a non-constant count | — |

Everything else about permissions is type check's: `permMatches` in
`borrowTypeCheck`, and variance in the reference matchers.

### Diagnostics

| Code | Site | Condition |
| --- | --- | --- |
| `ErrorInvType` | `flowHandleMove`, `flowResultMove` | move out of a global variable |
| `ErrorMoveOut` | `flowHandleMove`, `flowResultMove` | move out through a borrowed reference, or through a shared (aliasable) owning one |
| `ErrorMoveField` | `flowHandleMove`, `flowResultMove` (`flowRefuseMoveField`) | move out of a field — a struct's or a tuple's — or out of what is reached through one |
| `ErrorInvType` | `assignBorrowLifetimeCheck`, from `assignlvalrtype` and `swapFlow` | lval outlives the borrowed reference stored into it, or swapped into it |
| `ErrorNoMut` | `assignlvalrtype`, `swapFlow` | no write permission |
| `ErrorNoRead` | `flowLoadThroughRef` | no read permission on the reference a dereference, an index or a virtual-reference field reads through |
| `ErrorMove` | `nameuseFlow`, `nameuseFlowBorrowed` | read: uninitialized, or moved out; borrowed: moved out only (`borrowFlow` walks the borrowed place to its variable) |
| `ErrorBadFill` | `arrayLitFlow` | a fill may not repeat a move value |
| `ErrorFillCount` | `arrayLitFlow` | fill count not constant, or too large |
| `ErrorEscape` | `returnFlowEscape` | returned borrow outlives the local it points at |
| `ErrorCallEscape` | `fnCallFlowStoredBorrow` | a `&mut &T` argument points at a place that outlives another borrow passed to the same call |
| `ErrorFrozen` | `loanUse`, for a conflict `loanAccess` recorded | a source read, changed, moved, borrowed or ended while a borrow of it that forbids that is still to be used; reported at the access, naming the borrow and its next use |

`ErrorBadFill` and `ErrorFillCount` are deliberately distinct: the first is a
language rule, the second an implementation limit that should disappear when a
fill lowers to a loop.

## 8. Contract

**Before flow runs:** name resolution succeeded program-wide; this function's
signature and body type checked cleanly; every expression has a resolved
`vtype`; lowering is complete; `RefNode.scope` and `VarDclNode.scope` are
populated; regular blocks already end in a jump but loop blocks do not; globals,
parameters and fields already carry `VarInitialized`.

**After flow, for a function that ran it:** every block ends in a node carrying
a `dealias` list; every recognized counted acquisition has a `RefCountNode`; every
first-assignment target carries `FlagFirstAssign`. The loan walk adds nothing to
the tree.

**What generation relies on.** `genlBlock`, `genlBreak` and `genlReturn` call
`genlDealiasNodes` and do no analysis of their own. If flow did not run, the
lists are NULL, `genlDealiasNodes` returns immediately, and **nothing is ever
released** — there is no fallback. Likewise, without `FlagFirstAssign` every
first assignment to an uninitialized owning variable releases garbage, and a
reassignment after a move releases what the new owner holds: `genlStore`
releases one owner of an owning lval's previous value — single, slice, or
tuple element — whenever the flag is absent. `assignlvalrtype` sets it when the
variable is not `VarInitialized`, or is `VarMoved` or `VarHollow`, at the
assignment; for a hollowed one, the `HollowNode` wrapped round the stored value
is what releases the old allocation.

## 9. Hazards

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
  Now that a region's death finalizes, the same holds for a sole owning
  reference moved whole on only some paths (`imm y = if c {b;} else {+so
  Fin[0];}`): it is freed, and finalized, a second time on the path that moved it.
- **A variable hollowed on only one path is released hollow on every path.**
  `if c { imm x = *b; }` frees `b`'s memory on both paths, and on the path that
  did not move leaves what `b` points at unfinalized. That is the deliberate
  side of the choice — a finalizer skipped rather than run twice — and it needs
  the drop flag to be right.
- **A match's binding moved on one arm moves the matched value on every arm.**
  `match e { case imm a A { take(a); } else {...} }` deactivates the matched
  value for the whole function, so when the `else` arm runs, what it holds is
  never finalized. The same drop flag is the answer.
- **A match's binding declared `mut` and assigned over holds its own copy.** The
  conversion copies the matched value into the binding's storage, so a value
  assigned into the binding is in neither the matched value nor anything that
  releases it: `case mut a A { a = A[Fin[2]]; }` finalizes the matched value's
  original as the match ends and leaks `Fin[2]`.
- **A hollowed variable reassigned by a destructuring of one value**
  (`b, x = pair()`) leaks its old allocation: `assignMultRetFlow` has no single
  value to wrap a `HollowNode` round, so it takes the moved variable's path
  (`FlagFirstAssign`, nothing released).
- **A variable initialized on only one path is released on every path**, because
  `VarInitialized` is the same kind of summary: once an assignment anywhere
  before the scope exit has set it, the exit releases the variable whether or
  not that assignment ran. On the path that skipped it, that frees storage that
  never held a reference, and finalizes storage that never held a value — for
  every type with a death: a struct with a `final` or an owning field, an enum,
  a tuple or an array of any of them (`mut t (Fin, i64); if c { t = make(); }`
  runs `Fin`'s `final` over garbage when `c` is false). Only a variable that is never assigned at all is
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

## 10. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the only entry point; the per-function error-delta gate; `FlowTimer` round `blockFlow` and the loan walk under `-V 1`; the loan walk on a gated function `blockFlow` found no error in; `flowGateCount` after it |
| `ir/flowpath.c` | `flowPathWalk`, `flowPathPrint` | the loan walk (§6): its entry, and its `-V 2` tallies |
| | `pathSetFacts`, `pathRollback`, `pathDelta`, `pathJoin` | the state per path: set a fact, recording the old one; undo to a fork; what a path changed; join paths |
| | `pwValue`, `pwPlace`, `pwThrough`, `pwBorrow`, `pwCall`, `pwStore`, `pwSwap`, `pwVarDcl` | the walk's dispatch: what each node accesses, which holders it uses, what loans its value carries |
| | `pwStmts`, `pwBlock`, `pwBlockExits`, `pwLoop`, `pwIf`, `pwJump`, `pwScopeEnd` | forks and joins, loops to a fixed point, jumps, and a scope's end as an access |
| `ir/flowloan.c` | `loanMake`, `loanHeldBy` | a borrow's loan, and who may hold it |
| | `loanAccess`, `loanUse` | a conflicting access records a pending conflict on each holder of the loan; a use of the holder fires it (`ErrorFrozen`) |
| `ir/stmt/module.c` | `modInitOf`, `modInitFlowBegin`, `modInitFlowEnd` | round a module's `init` only: its module's globals without a value start the pass uninitialized, as locals, so `init` assigns each once and reads none first; one never assigned is `ErrorGlobalUninit`. [module](../nodes/module.md), "Init and final" |
| `ir/flow.c` | `flowLoadValue` | the walk's spine — tag dispatch for a value being read |
| | `flowLoadThroughRef` | `MayRead` on the reference a value is read through; called from `derefFlow`, `fnCallArrIndexFlow` and `fnCallFldAccessFlow` |
| | `flowHandleMoveOrCopy` | move vs. alias, for a value going to a new holder; a tuple literal's element by element |
| | `flowHandleMove` | deactivate the source — each move-typed element's, for a tuple literal; for a block or an `if`, what every value it hands back moves out of; hollow a local sole owner moved out through; refuse a move out of a field (`flowRefuseMoveField`) or a global, or out through a borrowed or a shared owning reference |
| | `flowOwningLocal`, `flowNewHollow` | the local owning reference a move reaches through; the `HollowNode` releasing a hollowed variable as it stands |
| | `flowResultMove` | the same refusals for a returned value, deactivating nothing |
| | `flowIsLvalRead` | the temporary-vs-lvalue test that makes counting correct |
| | `flowInjectRefCountAmt` | wrap a counted reference, or a struct, enum, tuple or array whose death releases one, in a `RefCountNode` |
| | `flowIsRcRef`, `flowIsOwningType` | is this type counted; is it an owning reference, or a tuple of them, that a store releases |
| | `flowHeldCounted`, `flowVariantHeldCounted` | does a copy of this struct, enum, tuple or array add a holder to a counted reference its death releases |
| | `flowMatchBound` | the matched value a match's binding stands for, or NULL |
| | `flowScopePush`, `flowScopePop`, `flowAddVar` | the variable stack |
| | `flowScopeDealias` | build a scope's release list; skip an uninitialized, moved-out or handed-back variable; release a hollowed one hollow |
| | `flowStateInit`, `flowGateResultAsk`, `flowGateCallAsk`, `flowGateOperandAsk`, `flowGateUse`, `flowGateCount`, `flowGatePrint` | the gate (§3, "The gate"): the questions its triggers ask out of line, the waiting operands' borrows, the `-V 2` tallies |
| `ir/flowgate.h` | `flowGateHolder`, `flowGateAssigned`, `flowGateResult`, `flowGateCall`, `flowGateOperand` | the gate's triggers as inline tests, dismissing what cannot carry a borrow without a call |
| `ir/itype.c` | `itypeCarriesBorrow` | may a value of this type hold a borrowed reference; a struct's answer remembered in `StructNode.carriesborrow` |
| `ir/exp/block.c` | `blockFlow` | scope push/pop, `blockret` injection, result walk then dealias capture; a `return`'s move source |
| `ir/exp/if.c` | `ifFlow` | both arms against one shared state |
| `ir/exp/assign.c` | `assignlvalrtype`, `assignSingleFlow`, `assignBorrowLifetimeCheck` | `MayWrite`, `VarInitialized`/`VarMoved`/`VarHollow`, `FlagFirstAssign`, the `HollowNode` round a hollowed variable's new value, borrow lifetime |
| `ir/stmt/swap.c` | `swapFlow` | `MayWrite` on both sides; borrow lifetime once in each direction |
| `ir/exp/nameuse.c` | `nameuseFlow`, `nameuseFlowBorrowed` | the only place the flags are *diagnosed* on, a hollowed variable as a moved one; both `ErrorMove` messages, and for a borrowed variable only the moved-out one |
| `ir/exp/borrow.c` | `borrowFlow`, `borrowFlowPlace` | the borrowed place must not be moved out: the variable at its root goes to `nameuseFlowBorrowed`, which refuses it moved out or hollowed but not uninitialized; a reference it is reached through is loaded as a value and not read through, an index is read; no aliasing tracked |
| `ir/stmt/return.c` | `returnFlowEscape` | `ErrorEscape` for a returned borrow of a local |
| `ir/exp/fncall.c` | `fnCallFlowStoredBorrow` | `ErrorCallEscape` for a `&mut &T` argument the callee could store a narrower borrow through |
| `ir/exp/arraylit.c` | `arrayLitFlow` | fill-form rules and the n / n-1 alias amount |
| `ir/types/reference.c` | `refAdoptInfections` | where a reference type acquires `MoveType` |
| `ir/types/region.c` | `regionIsCounted`, `regionIsOwning`, `regionMethod` | which region methods a region declares, which is what flow asks of it |
| `genllvm/genlalloc.c` | `genlRegionAlias`, `genlReleaseOwning`, `genlHollowRelease`, `genlDealiasNodes` | what consumes everything flow injected |

Test sources that pin behavior precisely: `test/cases/move/move-flow-*.cone`,
`test/cases/region/region_flow*.cone`, `test/cases/ref/ref_flow.cone`,
`test/cases/ref/ref_flow_return.cone`, `test/cases/core/core_flow_gate.cone`, and
for the loan walk `test/cases/ref/ref_flow_freeze.cone`,
`test/cases/ref/ref_flow_freeze_loop.cone` and `test/cases/ref/ref_freeze_success.cone`.

## 11. What lives elsewhere

| Question | Note |
| --- | --- |
| What the three reference axes mean, and what each permits | [References and Regions](../../../../doc/design/references-and-regions.md) |
| Which safety properties actually hold today | [Safety](../../../../doc/design/safety.md) |
| When a function is type checked at all | [Type Check Phase](type-check.md) |
| What a borrow's type records, and where | [Type Check Reasoning](type-check-reasoning.md), "Borrows: where type check stops" |
| How the allocation header is laid out | [Generation](generation.md), "The allocation header" |
