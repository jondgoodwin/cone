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
   source touched while a borrow of it held in a local variable — bare, or
   inside a struct, an `Option`, an array or a list the local holds — is still
   to be used: changed, moved, ended, borrowed in conflict, or, under a
   mutable borrow, read. That is for a source reached
   as `uni`, a local's own storage above all; Cone's `mut` is shared, so a
   source reached through a `&mut` or `&ro` reference or a `Rc[mut, T]` owner is
   only kept alive, and reading or writing it through another path is no
   conflict. A borrow a call returns counts as a borrow of every argument —
   `list[0usize]` keeps `list` loaned read-only while it is used — except
   from a container declaring a no-loan kind (`a.alloc(v)` keeps only the
   arena's life); a call's arguments conflict with each other where they
   touch one place, a method receiver's mutable borrow being two-phase. A
   value holding a borrow keeps its source frozen until the value's last use,
   or until it dies where its finalizer may read the borrow; and a value
   returned, or stored where it may outlive the function, may hold no loan of
   the function's own storage, whatever its type. An element borrow through a
   shared path is not refused, and two copies of one `&mut` may reach one
   place two ways. `borrowFlow`, in the
   main walk, still asks only that what is borrowed was not moved out, as a read
   does; it deactivates nothing and records nothing about the borrow.
2. **A lifetime is a `uint16_t` block-nesting depth on the borrow expression's
   type node.** Not a constraint variable, not region inference. 0 is global, 1
   is the caller band (what a borrowed parameter points at), 2+ is a block of
   the function — 2 its top block, where a by-value parameter's own storage
   lives too, so a borrow of one never leaves the function. A borrow through a
   borrowed reference takes that reference's lifetime, not its holder's block
   ([references](../nodes/references.md)). The rule is a numeric comparison at three
   sites — a return, a store into a global or through a reference, a call that
   may store through a `&mut` argument — and covers a bare borrowed reference
   only; what a value holding one may outlive, and a variable's, the loan walk
   decides (item 3). An `if`, a `match` or a block used as a value carries the narrowest
   scope among the values it can give. A call's result carries the narrowest scope among its borrowed
   arguments whose parameter's lifetime it may hold, on a reference node `fnCallFinalizeArgs` builds for that call — or,
   where the call returns several values, on the borrowed elements of a tuple
   type it builds for it: every borrowed reference in a signature written
   without a lifetime shares one, every one written with a name shares it with
   the others of that name and with those its `where` clause orders shorter
   (`lifeCarry`, `ir/types/lifetime.h`), and the shortest is the only lifetime
   those arguments have in common. An argument whose own borrow's lifetime the
   result may not hold, but what it points at may (a struct's ''a', held by a
   method's receiver), gives the scope of the borrow it points at, where that
   has one, and else none: what a borrowed struct holds has no scope here, and
   is the loan walk's. The
   scope stays the band: two names in the caller band are told apart by the
   loan walk (item 3), never by a number. A borrow coerced to another reference type — widened to a base
   trait's reference, or turned into a virtual reference — keeps its scope on a
   copy of the target type that `iexpCoerce` gives the cast, the declared type
   being shared and unable to carry one; an owner lent as a borrowed reference
   takes there the lifetime of a borrow through it.
3. **A variable's lifetime is what it holds now, not its type's scope.** A
   local's type takes its initializer's scope, and an assignment does not
   change it; what the variable may hold at each point, on each path, is the
   loan walk's ("The loan walk", below). So a variable may hold borrows of
   different lifetimes over its life: `r = &inner` into an outer `r` is legal,
   and refused only where `r` is used after `inner` ends, or returned, or stored
   where it may outlive the function. A store into a variable's own storage is
   therefore not compared by scope number (`assignIsLocalPlace`).
4. **The main walk joins an `if`'s arms but does not iterate.** No CFG, no
   fixed point. `ifFlow` walks each arm from the state its conditions leave and
   joins them after: a variable moved on any arm that did not return counts as
   moved from there on, so a move in both arms compiles and a use after a move
   in one is refused. A loop body is walked once. The path walk is the other
   kind: it keeps its state per path, joins the arms of an `if` and the paths
   into a loop's head and out of a block, and walks a loop body again until its
   head stops growing — but only for a function one of its gates marked, and
   over the same block-structured IR. It is what decides a function's drops
   where a variable's state may differ by path ("Drop flags", below). **Not
   building a CFG is a deliberate design choice**, not a simplification to be
   outgrown — the block-structured IR is held to be easy enough to follow
   directly, joins and all.
5. **Ownership is not one model, and flow reads which one from the region
   ref.** One with `aliasRef` (`Rc`) is counted; one declaring `Move` (`So`) has a
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
| `dropgate` | `fnDclTypeCheck`, which runs the path walk's drop-flag client on a function it marks ("Drop flags", below) |
| `jumped` | `ifFlow`, from `blockFlow`: every path through the arm just walked returned, so it takes no part in the join |
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
| `FlowGateResult` | `blockFlow` | a `return`, `break` or block end hands out a value carrying a borrow, a bare borrowed reference too: its scope number does not follow a borrow through a variable, a value holding it, or a call's by-value argument |
| `FlowGateStore` | `fnCallFlow` | a call with a `&mut X` argument, `X` carrying a borrow, or a struct argument holding such a writable borrow (`itypeWritableBorrowDepth`), beside another argument carrying one |
| `FlowGateInCall` | `nameuseFlow`, `nameuseFlowBorrowed` | a variable named while a borrow of it made by an earlier operand of the same call, struct or array literal or value tuple is still waiting for it (`v.add(v.len())`) |

"Carries a borrow" is `itypeCarriesBorrow`: the type is a borrowed reference, or
an owning reference, pointer, array, tuple or struct (an enum's variants
included) reaching one. A borrowed reference to a function is not one: a
function is never a local, so its borrow is global and loans nothing. A struct's answer is remembered on it
(`StructNode.carriesborrow`), because asking afresh at every variable cost flow
10–20% ([Performance](../compiler/performance.md), "Measuring it"). A parameter
is not a trigger: what a caller lent is frozen by the caller, and a store into
one, or through one, is a place assigned.

For `FlowGateInCall`, each operand that is a borrow (`BorrowTag` or
`ArrayBorrowTag`, through casts), or an owner lent by a recast to a borrowed
reference of its kind (`flowGateIsOwnedLent`, the recast `pwOwnedLent` reads:
`both(a, a)` for `a` a `So[R]` or a `So[App]`, and a lent receiver), pushes
the variable at the root of its place onto `inflight` once it is walked, and
the call or literal pops back to where it started. A lend the compiler makes
must trigger it exactly as a written borrow does: a function holding no other
borrow is otherwise never walked, and its conflicts pass unseen. A name use checks the list only when it is not empty, so a function
with nothing waiting pays one test per name use. More than `FlowInflightMax`
waiting at once gates the function.

Each trigger costs O(1) per node, and must cost almost nothing where it does
not fire, since it is asked of every function. So each is an inline test in
`ir/flowgate.h` (included at the end of `ir.h`, after the node headers it reads)
that looks through one name use to the declaration and dismisses a number, void,
a struct already known to carry nothing, a call whose arguments are neither
references nor structs that may carry one, an operand that is not a borrow;
only then is the question asked out
of line in `flow.c`. Asked through calls that resolved each type first, the same
triggers cost flow 25–30% on code holding no borrow; inline, about 4%. An
ordinary compile stops asking once any bit is set; `-V 2` asks every trigger to
the end, so that it can count each, and prints
`Flow gate: G of N functions (holder …, result …, store …, in-call …)`
(`flowGatePrint`). The loan walk holds loans in every local whose type carries
a borrow, which the holder trigger is for; puts what a call may store through
a `&mut X` argument, or a struct holding one, into the local it reaches, the
store trigger's; checks
a call's arguments against each other, the in-call trigger's; and checks what
a function returns, the result trigger's.

**Flow computes no lifetimes of its own.** `VarDclNode.scope` is set during name
resolution; `RefNode.scope` during type check by `borrowTypeCheck`, by
`borrowMutRef` and `borrowAuto` for a borrow the compiler injects, each from
`iexpGetLvalInfo`, and by `fnCallArrIndex`, `fnCallFinalizeArgs`, `iexpCoerce`,
`ifTypeCheck`, `blockTypeCheck` and `varDclTypeCheck` (a parameter's caller
band, a local's initializer's) for a type derived from one. Flow only compares
them.

The live state is on the declarations, in `VarDclNode.flowtempflags`:

| Flag | Set by | Cleared by |
| --- | --- | --- |
| `VarInitialized` | `varDclFlow`, `assignlvalrtype`; pre-set at parse for globals, fields and parameters | only round a module's `init`: `modInitFlowBegin` clears it on each global of the module without a value, and `modInitFlowEnd` puts every such global's flags back |
| `VarMoved` | `flowHandleMove` | `assignlvalrtype` on reassignment |
| `VarHollow`, with the moves that did it in `VarDclNode.hollowed` (and in `hollowall`, every one in the function) | `flowHandleMove`, for a move out through a local sole owner | `assignlvalrtype` on reassignment |
| `VarDropFlag` | the drop-flag client (`dropWalkEnd`), for a variable whose state differs by path at a release | never: generation reads it |

**They are the state along the path being walked, in source order.** Every
change goes through `flowVarSetFlags`, which logs the old flags while an `if` is
being walked, for a variable declared outside its innermost conditional part
(`VarDclNode.flowdepth`, below `flowDepth`); `ifFlow` walks each arm from the
state its conditions leave, takes what each arm that did not return changed
(`flowVarPathTake`), rolls it back, and joins them (`flowVarJoin`): each flag
any of them set is set, an arm that did not change a variable contributing what
it had at the fork, and a missing `else` is an arm. A flag set means "on some
path": a variable moved on one arm may not be used after the `if`, and one
given a value on one arm is taken as given one, for its use. A loop body is
walked once, so what a later pass sees is not here: the path walk's. And whether
a variable holds its value at each of its releases is not decided here where
that may differ by path ("Drop flags").

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
declaring `is Move`, as `So` does. That sentence is why an `Rc[T]` moves while
an `Rc[mut, T]` copies, on the same region, and why a reference into a region ref
declaring neither `Move` nor `aliasRef` copies freely under any aliasable
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
`Rc[mut, T]`, `Rc[imm, T]`, `Rc[ro, T]`, `Rc[mut1, T]`, single or virtual — is one of possibly
many holders of the value, and the others still point at it after the move, so
it is refused the same way (`ErrorMoveOut`, `flowIsSharedOwner`). A **sole**
owning reference — any `So`, and `Rc[uni, T]` (what `Rc` means) — is not
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
`flowHandleMove` it deactivates every variable any one of them moves out of,
and marks each name use a value leaves by (`FlagMoveOut`): `imm y = {a;}`,
`if c {a;} else {a;}` and a loop whose only exits all `break a` leave their
source moved on every path, exactly as `imm y = a` does, so it is finalized
once, by the new holder. A variable moved out of by only some of them is a
conditional move: it may not be used again, and whether its scope's end
releases it is its drop flag's to say, cleared where the value left — the
function's drops go to the path walk (`dropgate`). A function handing back a
block or an `if` — by `return`, or as its body's value — moves what each of its
values hands back the same way (`blockResultMove`). An expression statement
never reaches `flowHandleMove`, so a block whose value is thrown away moves
nothing.

**A move out through a sole owner.** When the inward walk reaches a local
variable holding an owning reference through that reference — `*b`, `**b`
(through an owning reference that is `b`'s value), an element `s[0]` of the
array it points at —
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

The owner's name use in the move is marked (`FlagHollowOut`), so that a
variable hollowed on only some paths — in a branch, or by only some of the
values a block or an `if` hands back — has a drop flag saying which: released
hollow where it was hollowed, whole where it was not. A variable moved whole
by the same move as well is left to that move (`MoveParts.wholes`).

**A recast is its operand.** Type check hands a value between an enrichment and
its base, in either direction, wrapped in a `CastTag` with no `FlagConvert`: the
two share one representation, so nothing is converted. Every walk here that
looks for the variable behind a value looks through that node to its `exp` —
`flowHandleMove` (the source to deactivate), `flowIsLvalRead` (a recast of an
lvalue still has a holder behind it) and `flowIsScopeResult` (a local returned
as its enrichment or base is still the scope's result). Missing any one of them
leaves one value under two names: finalized or freed twice, or counted once for
two holders. A converting cast (`FlagConvert`) makes a new value and is not
looked through, but for one: **a conversion into an owning virtual reference
carries its operand's owner** (`So[App]` from a `So[Spinner]`), adding a vtable
to the one owner, so these walks, and `blockResultMove` and the path walk's
`pwValue`, look through it as through a recast (`flowCastCarries`).

**A match's binding is the matched value.** `case imm c Circle` desugars to a
variable initialized with the matched value converted to its variant (a
`CastTag` carrying `FlagMatchBind`), and the match holds the matched value in
a variable of its own. The binding is that value under the variant's name, not
a second value: `flowMatchBound` answers the matched value's name for it, and
the walks treat the two as one. `flowScopeDealias` never releases the binding,
since the matched value's variable releases it, as the enum, whichever arm ran;
`flowHandleMove` on the binding deactivates the matched value too, and the
binding's name use is what is marked, the flag being the matched value's
variable's (`flowDropOwner`), so a binding moved on one arm leaves the matched
value to be finalized when another arm runs; and `flowIsScopeResult` exempts
the matched value when the binding is handed back. A binding by value (a
variant struct of a matched enum, `flowMatchInPlace`) is the matched value's
own storage: generation gives it the matched value's variable's slot
(`genlLocalVar`), read through the variant's layout, so a swap with it or a
store over it, or over a part of it, changes the value the matched value's
variable releases, and the store releases what it replaces as any store does
(`assignlvalrtype`). A binding by reference holds a copy of the matched
reference, and a store over it releases nothing. A guard's binding
(`case imm c Circle if g`) is a second binding of the same value, and owns
nothing either. The match's own variable
(`_`) is named by one name use that every pattern and binding shares, so no
move of it is marked: a binding's initializer that moves it (a `&uni`
narrowed) deactivates it as before, and no drop flag follows it.

**The count counts holders.** From the ownership work:

- `new Rc[mut, Pt](2)` creates the object *and* the first reference. Born at 1.
- `imm a = new Rc[mut, Pt](2)` adds no holder — the temporary hands over the reference it
  was born with. Still 1.
- `imm b = a` adds one — `a` keeps its reference, `b` gets another. Now 2.
- A tuple is one holder of each counted reference it carries. `imm t = pair()`
  adds nothing (a temporary again); `a, b = t` and `imm u = t` add one per Rc
  element, and `t` releases each at scope exit.
- A struct, an enum, a tuple or an array is one holder of each counted
  reference its death releases (`flowHeldCounted`), however deep: a struct's
  drop releases what its fields own, an enum's what its variant's fields own,
  and a tuple or an array dies element by element. So `imm s2 = s` over a
  struct with a `Rc[mut, T]` field adds one, `imm o2 = o` over an
  `Option[Rc[mut, T]]` adds one through the variant the tag picks, and a copy of
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
the list form). Every holder is counted one at a time: an array's contents
repeating a value copy the expression into each element, so each copy is one
more holder, and the fill form holds only a constant. Past 16 copies the second
stands for every later element and is generated in a loop, so the holder
injected around it is added once per pass.

**Decrements are never reference-count nodes.** They come from generation: walking a
`dealias` list at scope exit, and `genlStore` releasing an lval's previous value
unless `FlagFirstAssign` (or `FlagPartNoPrior`) says there was none, and a value's
death releasing the owners it holds. Each goes through `genlReleaseOwning`: one owner goes away,
through the region's `dealiasRef` where it has one, as the value's death where the
region is `Move`, and as nothing otherwise; a tuple's owning elements one by
one. A hollowed variable's owner goes away the same way, through a
`HollowNode` instead, and its death is hollow.

## 5. What it injects

Flow is not a read-only analysis. These mutations, all of which generation
depends on:

| Injection | Where | Generation uses it for |
| --- | --- | --- |
| `BlockRetTag` | `blockFlow`, for any block not already ending in one | a loop block, **and** a regular block ending in an expression, both get theirs here — it is where the dealias list hangs |
| `RefCountTag` | `flowInjectRefCountAmt` | `genlRegionAlias(val, amt)`: the region's `aliasRef`, once per owner added |
| `dealias` lists | `flowScopeDealias`, onto every `BreakRetNode`; rebuilt by the drop-flag client (`dropApplyExit`) for each exit of a function it walked | `genlDealiasNodes` replays them |
| `FlagFirstAssign` | `assignlvalrtype`, when the variable is uninitialized, moved out or hollowed, or is a match's binding by reference; set or cleared by the drop-flag client from each path's state | `genlStore` skips releasing a previous value the variable does not hold whole |
| `FlagPartNoPrior` | `assignlvalrtype`, on a field or element of a local's own value when the local holds nothing (or is a match's binding by reference); the drop-flag client likewise | `genlStore` skips releasing the part's previous value |
| `FlagDropTest` | the drop-flag client, on a store's target where whether its variable holds its value differs by path | `genlStore` releases the previous value only when the variable's drop flag says it is there |
| `FlagMoveOut`, `FlagHollowOut` | `flowMoveSource`, on the name use a value moves out of, or out through | the path walk follows the move along each path; `genlDropFlagUse` clears, or sets hollow, the variable's drop flag there |
| `HollowTag` | `flowScopeDealias`, in a `dealias` list, for a hollowed variable or one whose part the scope hands back; `assignSingleFlow`, wrapped round the value stored into a hollowed variable (and the drop-flag client, where a later pass of a loop reaches the store hollow) | `genlHollowRelease`: the owner goes, and a death frees without what moved — after the new value is evaluated, for the wrapper, as `genlStore` orders a whole release; with `test` set, only when the drop flag says hollow |
| `DropFlagTag` | the drop-flag client, in a `dealias` list, round the release of a variable whose state differs by path there | the release runs only when the variable's drop flag holds the state it names |
| `VarDropFlag` | the drop-flag client, on a variable given a flag | `genlDropFlagBegin` makes the flag where the variable begins; each store over it, and each marked move, updates it |
| `TempTag` | `flowTempRead`, round a temporary where it is read or thrown away; `moved` by `flowTempHollow`, `kept` by `flowTempEscape` (a raw pointer into it going out) or a move out of it by value ("Temporaries", below); `walkvar` by the loan walk (§6) | `genlTempKeep` keeps its value in a slot; the end of its statement, condition or operand finalizes it (`genlTempsEnd`), hollow where `moved` says; a `kept` one is never finalized |

**A reference-count node is built only for a counted reference, or a value
holding one.** `flowInjectRefCountAmt` returns early unless the type is a
`RefTag` or `VirtRefTag` into a region with `aliasRef` (`flowIsRcRef`,
`regionIsCounted` — a virtual reference is counted exactly as a single
reference is), or a struct, enum, tuple or array whose death releases one
(`flowHeldCounted`); for a tuple it fills the node's `counts` array with `amt`
per element that is or holds a counted reference and `0` per other element,
`amt` then holding the element count, and generation's tuple arm adds the
owners to each such element after an `extractvalue`. The struct, enum and array
arm (`genlAliasHeld`) mirrors the death: through each field of a struct, the
variant an enum's tag picks, and each element of an array. A reference into a
`Move` region (`So`) and a `uni`-permissioned reference into one with `aliasRef`
are both move types and take the move path instead, as does a tuple carrying
one. A reference into a region with neither is copied with no node at all.

**Scope dealiasing.** `flowScopeDealias` walks the variable stack downward from
the top to a start position, so release order is the reverse of declaration
order. Per variable: one that was never initialized or was moved out on the
path being walked is skipped, whatever its type, because it owns nothing to
release or finalize — in a function whose drops the path walk decides, its
lists replace these, each variable released as every path to that exit
says ("Drop flags"); so
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
A local it hands back on only some branches was moved there, and marked
(`blockResultMove`), so the path walk releases it by its drop flag on the
others, and does not exempt it. The exemption is the result's own and is not
deactivation, because each `return` builds its own list: `if c {return a;}`
exempts `a` on that way out only, and `a` is still usable and finalized on the
path that goes on. The match is on the
declaration the result's name resolves to, not on the name: a `return` asks over the whole function's
stack, where an inner block's `a` and an outer `a` both sit, and only the one
handed back is exempt. What survives both is released as its type dies: a
struct or an enum has a drop (`itypeGetDropFnDcl`), and the list gets a call
to it on a `&uni` borrow, positioned on the result expression, or on the jump
that ends the scope where there is no result expression — a `continue` hands
back no value; anything else with anything to do as it dies
(`itypeNeedsFinal`) — an owning reference, single (`RefTag`) or virtual
(`VirtRefTag`), into a region whose release does something (`dealiasRef`, or
`Move`: `regionReleaseActs`), or a tuple or an array of values that finalize or
own such a reference —
is added to the list itself, and generation finalizes it in place
(`genlFinalizeAt`), a tuple element by element, an array in element order. A
tuple carrying a `So` reference is a move type, so destructuring or copying it
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

### Temporaries

A temporary is a value an expression makes that nothing takes — not bound,
stored, passed by value, handed back or moved — whose death does something
(`itypeNeedsFinal`), or which a borrow points at, whatever its type; it dies at
the end of the statement that made it, newest first
(`doc/reference/refinitdrop.html`). Flow finds each where it is read or
thrown away and wraps it in a `TempNode` (`flowTempRead`); generation keeps it
in a slot and finalizes it at the end of its part ([Generation](generation.md),
"Temporaries"). A temporary is never a variable here, so it has no flags and
no drop flag; the loan walk gives one a stand-in variable where a borrow of it
is made (§6, "Temporaries"). What Rust's rule extends past its statement — what
a variable's initializer borrows, or the owner it lends — is no temporary by
flow's time: type check made it a hidden local of the block
([VarDcl](../nodes/vardcl.md), "Temporaries an initializer extends").

**Where a value is a temporary.** `flowIsTemp` is the test: an expression that
does not read a place that keeps its value (`flowIsLvalRead`), and is not an
assignment (its value is what its target keeps), a literal, a call that never
returns, or one of flow's own wrappers. `flowTempRead` is called where a value
is consumed without being taken:

| Site | What |
| --- | --- |
| `blockFlow`, an expression statement | a value thrown away: `mk();` |
| `blockFlow`, the final expression of a block that throws its value away (`blockDiscards`: a loop's, or one with no value, as a statement's and a statement `if`'s branches are; a function's own block only when it returns nothing) | the same; and the block hands nothing back, so a local its final expression names is not exempted from the release (`flowresult` is NULL) |
| `flowLoadThroughRef` | the value a dereference, a field access or an index reads: `mk().n`, `*mkso()`, `mkarr()[1]` |
| `flowLoadValue`, `CastTag` and `IsTag` | the operand of an `is`, and of a cast that does not hand its operand on (`flowCastHandsOn`): an owner lent as a borrowed reference (`g(mkso())`, `mkso().get()`), a conversion |
| `borrowFlowPlace` | the root of a borrowed place: `&*mkso()`, as a method borrowing its receiver builds; and, through `flowTempBorrowed`, a value borrowed itself or a part of one, `&mk()`, `&mk().n`, `&(a + 1)`, `mk().get()`, whatever its type: the borrow needs it in a slot, and the loan walk a root that ends with the statement. One whose death does nothing is `kept`, so generation pushes no finalization for it |

A value taken — a variable's initializer, an assignment's value, an argument,
a field of a literal, a returned or handed-back value — reaches
`flowHandleMoveOrCopy` or a block's result instead, and is never wrapped.

**A value moved out of a temporary.** `flowMoveSource` reaching a `TempNode`
through a dereference, or an index through an owning reference, is a move out
through a temporary sole owner: the move is noted in the node's `moved`
(`flowTempHollow`), and the owner is released hollow, as a hollowed variable
is. Reaching one by value marks it `kept`, as a local array an element moved
out of is left unreleased.

**A borrow of a temporary does not keep it.** A borrow of a temporary that
outlives its statement — `imm r = id(mkso())`, `imm r = mkso().me()`, a
`return` or a store of one — is the loan walk's to refuse where it is used
after the temporary dies (§6, "Temporaries"), so the temporary is finalized at
its statement's end all the same. **A temporary a raw pointer outlives is
kept**, since nothing follows a pointer. Once a statement that made a
temporary is walked, `blockTempEscape` walks it again (`flowTempEscape`),
carrying down how the value at each node goes out of the statement — a
variable's initializer, an assignment's value, a returned, broken-out or
handed-back value — through values that can hold a borrow or a raw pointer
(`flowTempOut`: `itypeCarriesBorrow`, or a pointer within a few levels): a
call's arguments when its result can, every argument of a call that could
store one (`flowTempCallStores`: a `&mut` or a pointer argument to something
that can hold one), a literal's or an allocation's elements, a cast's operand,
a field's or an element's base, a borrow's place. Out becomes `TempOutPtr` at
the first value on the way that holds a pointer, and a `TempNode` reached so
is set `kept`: never finalized, which leaks it rather than leave the pointer
dangling. So `imm p = id(mkso()) as *R` keeps the owner; `imm r =
id(mkso())`, `g(mkso())` and `imm n = mkso().n` do not.

## 6. The loan walk

A second walk, the path walk, over a function a gate marked, once `blockFlow`
has found no error in it (`fnDclTypeCheck` calls `flowPathWalk`,
`ir/flowpath.c`). It has two clients, each with its own gate, and a function
either marks is walked once for both. The loan gate's client enforces
**freezing**: a borrow held in a local freezes its source until the borrow's
last use (`ir/flowloan.c`); and **escapes**: what is returned, or stored where
it may outlive the function, holds no loan of the function's own storage.
Walked for drop flags alone, it makes no loans. The
drop gate's is **drop flags** (`ir/flowdrop.c`, "Drop flags", below).

**It is read-only while it walks.** It injects nothing and changes no node, so it may walk a
loop body more than once; that is its difference from the main walk, which
mutates and visits each node once. What the drop-flag client decides it applies
once the walk is done. It walks the tree as `blockFlow` left it,
through the `RefCountTag` and `HollowTag` wrappers, every block ending in a
jump or a `BlockRetTag`, and fails with `errorUnreachable` on a value tag it has
no case for, as `flowLoadValue` does.

**The words.** A *place* is a root and a path (`Place`). The root is a variable
— local, parameter or global — or what a borrowed-reference variable points at:
`*r`, `r.x` through the injected dereference, `s[i]` on a slice. The path is
steps: a field, an element (any index — `a[0]` and `a[1]` overlap), a
dereference of an owning reference. Two places overlap when they share a root
and one path is a prefix of the other, step by step; only two different fields
are disjoint. Nothing is tracked through a raw pointer.

**Cone's `mut` is not Rust's `&mut`.** `uni` is Rust's `&mut` (one active
reference); `mut` is shared mutable within a thread, `ro` a view others may
change (`refperm.html`). So how a place is reached decides what a borrow of it
may freeze. A place is reached **as `uni`** when no reference on the way to it
may alias: a local (its only owner is the stack, whatever its declaration says),
a field of one, or what a `uni` reference points at. It is reached through a
**shared path** (`Place.shared`) when a reference on the way — the borrowed
reference its root is read through, or an owning reference a step dereferences
— has `MayAlias` (`mut`, `ro`, `imm`, `opaq`, `mut1`); `Place.sharedlen` is how
many steps lead to that reference (0 when it is the root's). A dereference cut
off by the step limit is not marked shared, so the place is held to the
stricter rule.

A *loan* is one borrow (`BorrowTag`, `ArrayBorrowTag`) of a place, the same
loan each time a loop walks it again. Its kind comes from the borrow's
permission and the place (`loanKindOf`):

| Kind | Borrow | Of a place reached |
| --- | --- | --- |
| *exclusive* | `&mut`, `&mut1` / `&uni` | as `uni` / either way |
| *shared* | `&ro` (the default `&`), `&imm` / `&imm` | as `uni` / through a shared path |
| *alias* | `&mut`, `&mut1`, `&ro` | through a shared path |
| *pin* | `&opaq` | either way |

A *holder* is a local or a parameter whose type carries a borrow
(`itypeCarriesBorrow`): a borrowed reference, or a struct, enum, `Option`,
tuple, array, list or owner holding one. It may hold loans, and holds them as
a whole variable: a struct holding two borrows keeps both sources frozen for
as long as either field is used -- but for a struct declaring lifetimes,
whose loans are kept by lifetime (below, "Slots"). The variable a `match`
keeps its scrutinee in, and each binding a `case` makes, are holders like any
other. A parameter holder starts out holding its *caller loans*
(`loanCaller`, `pwCallerLoans`): stand-ins for whatever the caller lent
through it, which nothing here conflicts with (the caller froze it) and which
outlive the call; they are linked from no variable, so no access meets them.
There is one per part of what the parameter lends (`LifePart`,
`ir/types/lifetime.h`): what its own borrowed reference points at, near; and
what that holds, far -- or what a parameter passed by value holds, both near
and far, since it stands for every borrow the value holds, however deep --
whole, or, for a struct declaring lifetimes, one per lifetime. Each stands for
its part's lifetimes: a value may carry it out of the function only where the
result's type holds one they flow to, and into what a parameter points at only
where that holds one (below). The own part stands for exactly the place the
reference points at, so a value read through the reference, or a borrow of
something further on through one that may alias, does not carry it
(`pathSetThrough`). A parameter whose
own reference is written `'static` holds a global borrow, and so no caller
loan. An *access* is what an expression does to a place:

| Access | Conflicts with a live loan that is |
| --- | --- |
| a read, a copy out | exclusive |
| a read-only borrow (`&`, `&ro`) | exclusive |
| an `&imm` borrow | exclusive, alias |
| a write — `=`, an operator changing it in place (`+=`, `++`, `<-`), one side of `<=>` — or a `&mut`/`&mut1` borrow | shared or exclusive |
| a `&uni` borrow | any but a pin |
| a move, a replacement of the whole (its old value finalized), the end of its scope | any, a pin too |
| an `&opaq` borrow | none |

An alias loan needs only its source alive, and a borrow of it to promise no
more than the path can: others may read and write it through the shared path
anyway, so freezing it against them would protect nothing. But the part of the
path *before* the shared reference — the field of a local holding a `Rc[mut, T]`
owner — is reached as `uni`, and the loan reads it: an access to a place with
fewer steps than the loan's `sharedlen` meets an alias loan as a shared one, so
`h.p = new Rc[mut, Pt](..)` or `&mut h.p` while `&mut h.p.x` is live is refused,
since either could release what the borrow points into. A source reached
through a borrowed reference is never ended by anything done here — reassigning
`r` leaves `*r` alive — so an alias loan of `*r` conflicts with nothing a
function can do but a `&uni` or `&imm` borrow, which the type check already
refuses through a `mut` or `ro` reference (`ref_typecheck_perm`).

A holder reaching its loan's source through the loan — `*b = 9`, `r.x` — uses
the holder; it does not access the source.

**What holds what.** Each expression walked yields the loans its value may
carry (a `PathSet`): a borrow, its new loan and whatever the holder at the root
of the borrowed place holds (a reborrow `&mut *r`, or `&r`, keeps what `r`
holds; so does the reborrow type check builds for a `&uni` lent where a `&`
or `&mut` is wanted, which is why `g(p); g(p)` is no move); a holder named, or a place read through it, what it holds, if
the value read can hold a borrow; a cast whose type can hold one, what its
operand carries — a recast, a borrow coerced to a virtual reference, and the
conversion a `case` binding makes of the matched value alike; an `if`, a
block, and a tuple, struct or array literal, the union of theirs; a call,
below. An owning reference coerced to a borrowed one (`imm b &Pt = u`,
`u` a `So[Pt]`: a recast from an owning to a borrowed reference; or `imm b
&<App = v`, `v` a `So[App]`, between virtual references) is a borrow of
what it owns, `*u`, so `u` may not be moved while `b` is used
(`pwOwnedLent`). One coerced to a `&uni` or `&mut1` arrives already rewritten
by type check to the borrow `&uni *u`, since a recast to a move type would be a
move of `u`. A holder's declaration, or an assignment to the whole of it,
*replaces* what it holds with what the value carries; moving it away whole
leaves it holding nothing.

**Near and far.** A value's loans are of two kinds, kept apart in its one set
(`LoanFar`, a bit on the loan's id there): a *near* loan is of a place the
value's own borrows point at — what a reference points at, what a struct's
borrow fields do — and a *far* loan is one those places hold in turn, a borrow
or more further on. `mut q = r; mut p = &mut q` for a parameter `r`: `p`'s
loan of `q` is near, and the caller loan `q` holds is far, since `p` points at
`q`, the function's own, and reaches the caller's place only through it. Where
the walk cannot tell which a loan is -- a call's result, what a call may store
-- it is both, an entry of each kind (`pathSetUnsure`). So a loan *only* near
is exactly where the value points, and held by it nowhere further on: each
reference layer's loans are its own. A borrow's new loan is near; a borrow of a
holder's own storage (`&mut q`, `&mut h.f`) makes what the holder holds far,
while a reborrow through it (`&mut *r`, `&mut r.f`) points where it does and
keeps each loan as it was. A value read from a holder's own storage carries
its loans as they are; one read through a reference (`*p`) carries none only
near, which is of the place it is read from, and each far one as both, since
which of them the value's borrows point at is not known (`pathSetThrough`):
`*z` for `z = &mut pp` carries what `pp` holds, not `pp`. A parameter's own
caller loan is near, what its reference points at holds far. A place reached
through a reference read through another (`**pp`, `*r.g`) is `Place.far`: it
stands for anything a borrow or more past where its variable points. A borrow
of one (`&**pp`) carries what `pathSetThrough` gives where the inner reference
may alias, since a copy of it points there as well; where it is `uni`, the
borrow is a reborrow of it, which lasts no longer than the outer borrow (Rust's
reborrow through `&'a mut &'b mut`, `'a`), and keeps every loan, both near and
far.

**Slots.** A struct declaring lifetimes keeps its loans by lifetime, each
lifetime a *slot*: an entry of a loan set may carry a slot's tag (bits beside
`LoanFar`, `flowloan.h`), relative to the struct the holder's own type is, or,
for a borrowed reference, the one it points at. A tag is set only where it is
known exactly: a struct literal's value for a field holding one slot
(`pwValue`), a store into such a field (`pwStoreTagged`), and a parameter's
caller loan for each slot. A read of a field, or a borrow of it or of
something reached through it, carries only the loans of the field's slots and
those its struct's `where` clause lets into them (`lifeSlotsReach`), and those
with no tag, which may be anywhere (`pwSlotStep`, `pwPlaceSlots`); it drops
every tag once the value is no longer of the struct, as does a call's result,
a cast and any value built of others but a struct literal of a struct
declaring lifetimes. So `p.word()` for `word(self &mut) &[]'src u8` on a
`Parser['src, 'ar]` carries what `p` holds in `'src`, not what it holds in
`'ar`. A callee may move a borrow from one slot to another where its signature
names them as one, or orders them beyond the struct's own order
(`lifeSlotsApart`); after such a call what the struct holds is no longer known
by slot, and its holder's tags are dropped (`pwCallMoves`).

**Stores through a reference** (`pwStoreInto`). A value stored into part of a
holder (`h.r = &x`) adds its loans to what the holder holds. Stored through a
borrowed reference (`*r = v`, `r.f = v`), its loans go where the reference
points: into what `r` holds, as far loans, since what is read back through `r`
carries them, and into each holder a near loan of `r` borrows from — every
loan, for a far place — so that after `imm r = &mut o; *r = Some[&R][&x]` a use
of `o` uses `x`'s loan (`pwStoreLands`). A call handed a
writable borrow of a place that can hold a borrow (`l.push(&x)`, `stash(&mut
o, &x)`, `fill(r, v)` for `r &mut Option[&T]`) may store there anything its
other arguments carry, under the same one-lifetime rule as its result below,
which `fnCallFlowStoredBorrow` reads for lifetimes; so that place takes them,
as a store through the reference would (`pwCallStores`, the target found from
the argument by `pwStoreTarget`), each loan near and with no slot's tag, since
the callee may store what it reads through an argument, into any field. And it may store through every writable
borrow it reaches from there (`itypeWritableBorrowDepth`): through the `&mut
&R` that `&mut p` points at (`put(&mut p, &x)` for `x &mut &mut &R` doing
`**x = v`), through a struct's `&mut` field (`st(&mut h, &x)` doing `*h.r =
v`), or through one a struct handed by value holds (`st(w, &x)`, `w` holding a
`&mut &R`). So the store may land anywhere from the target to that many
borrows past it, and each place in that range is checked and takes the loans
as a store through a reference there would: a borrow past the target is where
its near loans point, two or more, anywhere its loans reach. An argument the
walk cannot key (`st(new W(&mut q), &x)`) is gone by through its own loans. One exception keeps two such places
lent to one call apart: a borrow written as an argument is stored only where the
place's type can hold a borrow of what it borrows (`itypeHoldsBorrowOf`), so
`arrive(&mut world, &mut seen, name)` leaves `world`, a `List[Named]`, holding
nothing of `seen`, a `List[&[]u8]`, and each is free while the other is used.

**Calls** (`pwCall`). A borrow a call returns, or a value that may hold one,
carries the loans of every argument: Cone's rule for a signature without
lifetime annotations is that every borrowed reference in it shares one
lifetime (`reflifefn.html`), which `fnCallFinalizeArgs` already applies to the
result's scope. So a borrow a method returns keeps its receiver loaned while
it is used, the Rust way — the loan is the receiver's own: `list[0usize]`'s
`&list`, read-only, lets `list` be read but not pushed onto; a method taking
`self &mut` and returning a borrow freezes its receiver entirely. A receiver
taken by reference is a loan whether the call borrows it (`list.push(x)` is
`(&mut list).push(x)`, the borrow injected) or it is handed a reference (`r.push(x)`
for `r &uni List` reborrows `*r`, with the permission the method declares for
`self`), or is an owner type check lent to a `self &` or `self &mut` method (a
`So[R]`, or a `So[App]` dispatched through: the recast `pwOwnedLent` reads,
a loan of `*a` reached as its owner reaches it, so `a.absorb(a)` moving `a`
conflicts) (`pwReceiver`). A message names the method: "'list' is borrowed (by
'[]' at 5:15)".

**Named lifetimes.** Where the callee's signature names lifetimes
(`FnSigNode.lifenamed`, `pwNamedSig`; a struct's `Self` naming its own counts),
"every argument" narrows by part (`pwArgCarries`, from `lifeCarry`): all of an
argument whose parameter's own lifetime flows to one the result holds, by the
signature's order; only what its reference points at holds, where only that
flows -- its far loans, not one only near, which is exactly where it points
(the borrow it is written as, a parameter's own caller loan, the local a
variable handed as the argument borrows), so each reference layer's lifetime
governs what is read through it -- and of a struct declaring lifetimes only
the slots that flow, and what has no tag; or nothing. `pick(a &i64, b &'b i64) &i64`
keeps `a`'s source frozen while its result is used, and `b`'s free; `c.next()`
for `next(self &mut) Option[&'a R]` in `Cursor['a]` keeps what `c` holds, not
`c`. A store through a writable argument takes of the others what flows to
what it points at (`pwCallStores`), and an argument for a `'static` parameter
may carry no loan but of a global (`pwStaticArgs`). In the function whose
signature names them, a returned value may carry a caller loan only where its
part's lifetimes flow to one the result holds (`loanCallerApart`), and a value
stored where a reference points, or handed to a call that may store it there,
one only where the place the reference's caller loan stands for holds one its
part flows to -- for a field of a struct declaring lifetimes, one of that
field's, and for a part of the same parameter moved within it to a field not
known, every one (`loanStoredApart`, from `pwStoreEscapes` and
`pwCallStores`): `ErrorEscape` or `ErrorCallEscape` (`loanApart`). Both sides
read the parts the same way, so what a callee is held to is exactly what its
callers assume. Two names flow only where they are one, or the signature's
`where` clause orders them (`'a >= 'b`, `'a == 'b`, transitively;
`FnSigNode.lifeorder`), or a struct a parameter or the result uses orders
them in its own, or a type in the signature implies it: a borrow of a value
holding lifetimes cannot outlast them, so `&'a Pair['b]` and `&'a &'b T`
order `'b >= 'a`, and `self &` in a struct declaring `'a` orders `'a` over
the unnamed lifetime, which is a name like any other there (Rust's implied
bounds; `lifeSigCheck`, `lifeImplied`). A lookup in a small order, no solver,
and nothing else inferred. Both sides read one order, so a bound the callee
relies on is one its callers carry loans by.

A container may declare that its element borrows need no loan on it, with a
marker trait [Jon 26 Sep; names provisional] its `StructNode.lends` records:
`NoLoanMut` (any borrow a method returns; the arena, whose allocations are
fresh and never moved) or `NoLoanRead` (a read-only one). The result then
carries only a *pin* of the receiver's place — it may not be moved, replaced
or ended while the result is used — and a second `a.alloc(v)` is no conflict.
A borrow of a `NoLoanMut` container itself is held as one through a shared
path is (`pwLend`): it keeps the container alive and promises no more, so two
`&mut a` may live at once and `a.alloc(new Spawner(2, &mut a))` is one call. The
third marker, `ShapeChanging` (`List`, `String`, `Dict`, `Pool`), is read by
nothing yet: see "What is not held".

**In flight.** A call's arguments are walked in order, each one's loans pushed
*in flight* (`loanFlightPush`) until the call is made; so are a tuple, struct
or array literal's elements until it is built. That value is certainly used,
so an access conflicting with a loan in flight is reported at once
(`loanFlightAccess`), not left pending: `f(&mut v, v.len())` is refused at
`v`. A method receiver's mutable borrow is **two-phase** (Rust's RFC 2025):
*reserved* while the arguments are walked, it accesses its place as a
read-only borrow would and meets their accesses as one, so `v.push(v.len())`
compiles and `v.push(takeLast(&mut v))` does not; then *activated* at the
call, where it is checked against what every other argument carries
(`loanFlightActivate`) and then accessed as what it is, against the loans
still held — so a holder whose last use is an argument (`imm last = v[0];
v.push(*last)`) is dead by then. An explicit `(&mut v).m(…)` is a receiver too,
and two-phase with it, a leniency beyond Rust's.

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
joins its fall-through end with every `break` that left it; a `return` ends
every scope of the function, as a jump ends the blocks it leaves, and then the
path. A path that jumped away contributes nothing where it would have fallen
through.

**Loops.** A loop body is walked from the state at its head; the paths back to
the head — its end, each `continue` — join into it, and if that added anything
the body is walked again from the grown head. Facts only grow, so it settles,
nearly always on the first walk. An error fires on whichever walk first meets
it, once. A loop still growing after four walks has each holder that changed
around it widened to every loan (`pathSetAll`): conservative, and then it
settles. The paths out are its `break`s. `-V 2` prints
`Path walk: F functions (L for loans), L loops, W walked again, C widened`
(`flowPathPrint`).

**Scope ends.** As a block ends, at its end, at a jump leaving it or at a
`return` (`pwScopeEnd`), its variables die, the last declared first, as their
releases run. A holder dying drops what it was pending on — unless its
finalizer may read a borrow it holds (`itypeDropReadsBorrow`: a type with a
`final` holding a borrow outside a raw pointer, or holding such a value), when
its death is a use first, and fires them (`pwHolderDies`); so is the death of
its old value where it is stored over whole. That is Rust's drop check, with
what a raw pointer reaches — a collection's elements — taken as finalized and
never read, as Rust's `#[may_dangle]` collections promise: a `List[&R]` dies
using nothing, an `H { r &R; fn final … }` uses `r`. Each variable then ends,
an access conflicting with every loan of it: a holder still alive holding one
— declared outside the block, or before the variable in it and so finalized
after it — and used afterwards, is refused. That is the borrow kept past its
source, `{ mut a = 1i64; mut b &i64 = &zero; b = &a; r = b; } *r`, or `{ mut a
= Arena.empty(); h = new Holder(a.alloc(v)); } h.r`. A variable's lifetime
follows what it holds now, so the stores themselves are legal, and so is
`r` never used again after `a` ends; this is where a borrow kept past its
source in a variable of the function is refused. A block handing a value on to a holder — its final
expression, or a `break`'s — keeps what that value carries in flight while its
variables end (`pwScopeEndHanding`), so `imm h = { imm x = ..; new H(&x); }`
is refused at `x`.

**Temporaries.** A temporary at the root of a place — read through (`*mkso()`,
`mk().n`), lent as an owner to a call or a method (`id(mkso())`,
`mkso().me()`) — is a root like a variable: `pwTemp` walks its value and gives
the `TempNode` a stand-in variable (`walkvar`, made once, `PathVar.temp`),
which holds what that value carries if its type can, and which is pushed on a
stack of the temporaries the statements being walked made. Where generation
finalizes them, they end, the newest first (`pwTempsEnd`): at each statement's
end (`pwStmts`), with a `break`'s or a used block's value still in flight;
at an `if` condition's end; at the end of an `and`'s or `or`'s right operand.
Ending is the access a variable's scope end is, so a holder still holding a
borrow of one and used later is refused at the temporary — `imm r =
id(mkso(3)); r.n` is Rust's E0716 — and a block value carrying one is refused
at once. A loan of a stand-in is local, so a `return` or a store away of a
borrow of a temporary (`fn f() &R { mkso(); }`) is an escape. A temporary a
variable's initializer extends arrives as a hidden local (`tempLocalName`),
an ordinary variable of its block; the messages name both kinds as
temporaries (`loanTempKind`).

**Escapes.** A loan is *local* when its place is rooted in the function's own
storage — a local, a by-value parameter, what an owner held in one owns —
reached by no dereference of a borrowed reference (`loanIsLocal`). A caller
loan, a loan of a global, and a reborrow through a reference are not: a
reborrow's lifetime is that of the loans the reference held, which come along
with it. So a value's lifetime is the shortest root among its loans, and:
- **a `return`** hands back nothing carrying a local loan, whatever the
  value's type — a struct, an `Option`, an `Rc` owning one, a call result
  carrying a by-value argument's loan, a variable that was given a local's
  borrow (`ErrorEscape`, at the returned expression);
- **a store** into a place that may outlive the function carries no local
  loan (`pwStoreEscapes`, `ErrorEscape`), each side of a swap a store of the
  other's value; nor does what a call may store through a `&mut X` argument,
  or through any writable borrow it reaches from one or from a struct handed
  by value, reaching such a place (`pwCallStores`, `ErrorCallEscape`). A place may
  outlive the function (`pwPlaceOutlives`) when it is rooted at a global, is
  reached through an owner others may own too (`Place.owned`: an `Rc`, whose
  referent anyone holding a copy reaches), or is what a borrowed reference
  points at where that reference may point beyond the function
  (`loanMayPointOut`, asking its near loans, or every loan for a far place):
  at a caller loan, a global, such an owner's referent, or at nothing the walk
  knows of. Where a reference points is not what that place holds: `*p = &s`
  for `p = &mut q`, `q` a local holding the caller's borrow, is a store into
  `q`, the function's own, which then holds `s`'s loan and is held to it as
  any variable is. A store into a place rooted at a global,
  which outlives every caller too, carries no caller loan either (the first
  `loanNotGlobalIn` finds, `ErrorEscape`): no borrow the caller lent, read
  through a parameter (`G = *x` from a `&mut &T`), held by a by-value
  parameter, or carried inside a value (`G = Some[&T][r]`). A `'static`
  parameter holds no caller loan, so its borrow may go there.

Near and far tell where a reference points from what that place holds:
`imm q = &mut lh` holds `lh`'s loan near and every loan `lh` holds far, so
`q.r = &x` through a reference to a local struct holding a parameter's borrow
is a store into a local, a list node of its own type included; a reference
that may point at the caller's struct on one path and a local's on another is
not. Where the walk could not tell near from far (a reference a call returned,
or one read through another), a loan is both, and the type is the
fallback: where the reference is read from a variable itself, `Place.referent`
(and `pwStoreTarget`) give the type it points at, and `loanMayBePointee` sets
aside a loan of a struct of another type, and the caller loan of a parameter
that is no reference, which are held inside the place, not the place. What
remains imprecise is only there: such a reference to a local holding the
caller's borrows of its own type is taken to point at the caller's place.

The scope numbers still decide a bare borrowed reference returned or stored
away, and the walk runs only on a function where they found nothing; a store
into a variable's own storage they no longer compare (`assignIsLocalPlace`),
since a variable's lifetime follows what it holds. A call storing through a
`&mut` argument into a local is still refused at the call when a bare borrow
beside it is shorter (`fnCallFlowStoredBorrow`); a value carrying one, into a
local, the walk follows.

**What is not held.** The temporary an operator changing its operand in place
borrows it through (`x += 1` is `{imm tmp = &mut x; *tmp = *tmp + 1}`, and
`v <- a, b` appends through one; both named `tempName`) is an access, a write,
and no holder: `k += k` compiles. What is borrowed through it still carries
its loan (`pwVarDcl` keeps it on the temporary, `pwLend` reads it), so
`r = (v <- (1, 2))` holds `v`'s loan. A
borrow a
call returns carries its receiver's loan as the receiver was reached: through a
shared path (`l &mut List`, a field of `self`, a `Rc[mut, T]` owner) that loan
only keeps the source alive, so `imm e = l[0usize]; m.push(p); e.x` compiles
when `m` is another reference to the same list, and `e` dangles. Jon's rule
refuses such an element borrow of a container that changes shape (it declares
`ShapeChanging`); the check is not built, because it would refuse ordinary
code — reading a `List[String]` element through a `&List` parameter, a
method reading its own `self` list field — until `uni` reborrowing makes the
alternatives writable (lending a `&uni` as a `&` or `&mut` is built; lending
it to another `&uni` is not). `collection_flow_freeze`'s header and `refborref.html`
pin each shape. Copies of one `&mut` reach one place two ways unchecked, and a
global a callee changes is invisible.

**Its state** is file-static, as the variable stack is, and safe for the same
reason: flow never runs re-entrantly (`flowPathWalk` refuses to). The buffers
come from the compiler's arena, small at first, and are kept from one walk to
the next; a variable's index is `VarDclNode.flowindex` for the length of a walk.

### Drop flags

[Jon 26 Sep: runtime drop flags, and conditional handling as a general flow
capability.] A value dies at the end of its scope, on the path that still holds
it. Whether a variable holds its value at one of its releases — a scope's exit,
or a store over it — may differ by path: moved on one branch, given a value on
one, hollowed on one, moved in an earlier pass of a loop. Such a variable
carries a hidden byte, its **drop flag**, which generation sets where its value
arrives and clears where it leaves, and the release tests it. Where every path
agrees, there is no flag, and the release is what it is, or nothing.

**The gate** (`FlowState.dropgate`) is set by the main walk at O(1) per state
change: a *tracked* variable — a local or a parameter whose value moves or has
anything to do as it dies (`flowDropTracked`; a match's binding stands for the
matched value's variable, `flowDropOwner`) — moved, hollowed or stored over
deeper in the conditional structure than it was declared (`flowDepth`: an
`if`'s arms and its later conditions, the right operand of `and` and `or`, a
loop's body; `flowDropNote`), or moved or hollowed by only some of the values a
block or an `if` hands on. A function the gate does not mark keeps the main
walk's lists, which are right for it: every change to a tracked variable is
on every path through its scope. That is what keeps the cost off a function
with no such variable, and its code byte-identical.

**What it follows.** Per tracked variable, on the current path, a set of what
it may hold (`PathVar.state`, `DropState`): its whole value, a hollowed one,
nothing because never given one, nothing because moved out. Its declaration
makes it uninitialized, and its initializer, its parameter-hood or a store over
it whole makes it whole; a name use the main walk marked (`FlagMoveOut`,
`FlagHollowOut`) makes it moved or hollow where the walk meets it; its scope's
end makes it nothing, so the paths leaving the scope agree. The state is logged
and joined as the loans are — a union — so it costs what the paths changed.

**What it records** (`dropExit`, `dropStore`, `dropPartStore`), at each release
the walk meets, for each variable that has anything to do as it dies: at an
exit — a `break`, `continue`, `return` or block end — what each variable the
exit releases (the declarations from the scope it leaves down) may hold, after
the exit's value is walked; at a store over a whole variable, what it held
before; at a store over a field or an element of a local's own value, what the
local held. Loop walks gather into one record (`DropSite`), the union.

**Uses it refuses.** A move or a read of a tracked variable some path reaching
it moved, hollowed or never gave a value (a borrow: moved or hollowed only) is
`ErrorMove` "may have been moved out" / "may not have been given a value"
(`dropRefuse`), once per site: a loop moving a variable declared outside it,
a read in a pass after one that moved it. The main walk refuses what one walk
in source order sees.

**What it decides** (`dropWalkEnd`, once the walk reported no error). A
variable whose recorded state at any release holds two of *whole*, *hollow*,
*nothing* gets `VarDropFlag`. Then each exit's release list is rebuilt from the
records (`dropApplyExit`), last declared first, through the same per-variable
release the main walk uses (`flowVarRelease`): a variable holding nothing
there has no entry; one certainly whole or hollow its release as before; one
whose state is mixed its whole release, its hollow release or both, each in a
`DropFlagNode` naming the state it runs in. A variable the exit hands back is
exempt as before, unless a path to the exit moved it — a block or an `if`
handing it on, whose move its flag records. Each store is marked
(`dropApplyStore`, `dropApplyPart`): nothing held, `FlagFirstAssign` or
`FlagPartNoPrior`; certainly held, neither; mixed, `FlagDropTest`. A store
reaching a hollowed variable gets a `HollowNode` round its value, which a
mixed state marks `test`; one wrapped where no path is hollow is unwrapped.

**Generation** (`genlDropFlagBegin`, `genlDropFlagSet`, `genlDropFlagIf`,
`genlDropFlagUse` in `genlalloc.c`; [Generation](generation.md), "Drop flags"):
an `i8` slot named `<var>.held`, stored `1` (whole) where the variable begins
with a value — its declaration, a parameter at function or inline-body entry —
and `0` where it begins without one; `0` at each marked move, `2` at each
marked hollowing, `1` after each store over it whole.

### An init's self

An init's `self &new` (`fnDclIsInit`) is a reference to memory that holds no
value until the init writes one, and that the construction running the init
holds once it returns (doc/reference/refinitdrop.html). Flow holds it to that
with one flag and one context:

- **`VarUnfilled`** is set on `self` as the init's flow begins
  (`fnDclTypeCheck`) and cleared by `*self = value` (`flowNewSelfFill`, from
  `assignFlow`, after the value is walked, which marks the deref
  `FlagFirstAssign` so generation finalizes nothing as it is replaced). It is
  joined by union like every flag, so a store on only some paths, or in a loop
  that may not run, leaves it set. Fields are not filled one at a time: flow
  tracks the whole value, as it does a variable's.
- **A use through `self`** -- the deref of `*self` or `self.x`
  (`derefFlow`, `assignFlowLvalReads`), a method's receiver (`fnCallFlow`) --
  sets `flowThroughSelf` around the walk of the name, and `nameuseFlow`
  refuses it while `VarUnfilled` is set; `nameuseFlowBorrowed` refuses a
  borrow of a place through it the same way.
- **Any other use of `self`** -- passed, copied, stored, returned -- reaches
  `nameuseFlow` without the context and is refused filled or not, since the
  reference would outlive the init.
- **Every return** asks whether `self` is filled (`flowNewSelfReturn`, from
  `blockFlow`'s return arm); the body's last return is one
  `fnImplicitReturn` wrote, with no place of its own, so it is reported at
  `self`, once.

All four are `ErrorInitSelf`. What is not checked: a method called on a filled
`self` may do with its own borrowed self what any method may, and a store of a
filled `self`'s fields is an ordinary store.

## 7. What it decides, and what it does not

| Analysis | In flow? | Enforced | Not enforced |
| --- | --- | --- | --- |
| **Move / ownership** | yes | `ErrorMove` on use of a moved-out or uninitialized variable, and on a borrow of a moved-out one; move out of a field, or of a global, or out through a borrowed or a shared owning reference, refused; a use some path reaching it moved, hollowed or never gave a value (a loop's earlier pass included), by the path walk | element granularity — moving `a[0]` deactivates all of `a` |
| **Escape / lifetime** | representation in type check, enforcement here and in the loan walk | storing a bare borrow into a global or through a reference into a longer-lived place, by assignment or by either direction of a swap; any value — bare borrow, struct, `Option`, `Rc` owner, call result, a variable that was given a local's borrow — returned, or stored where it may outlive the function (a global, what a parameter or a copy of one points at, an `Rc`'s referent), or handed to a call that may store it so, while it holds a loan of the function's own storage (the loan walk); returning a borrow of a local, or a local initialized with one, its type declared or not; returning or storing outward a borrow of a by-value parameter, of `self` by value or through an owner passed by value; storing a borrowed parameter's borrow into a global, bare, read through a `&mut &T` parameter, held by a by-value parameter or carried inside a value; a returned `if`, `match` or block, arm by arm, and one used as a value carrying its shortest arm's lifetime; an owner handed back, or stored, as a borrow; a borrow through a borrowed reference held in a local, which has that reference's lifetime; a borrow arriving through a call's result, singly or as one of several values destructured into lvals, each carrying the narrowest argument borrow's scope; a `&mut` or `&uni` argument (a method's receiver included) to a place that can hold a borrow — `&T` itself, a struct with a borrow field, an `Option` or `List` of borrows, a slice of them — where that place would outlive another borrow passed with it; a borrow coerced to another reference type, whether widened to a base trait's reference or made a virtual reference; with lifetimes named on a signature or a struct, a caller's borrow returned, or stored where a parameter points, as a lifetime its part does not flow to by the `where` clause's order, a struct's field by field, and a borrow not global handed to a `'static` parameter | a borrow captured; `+ 'a` bounds and invariant lifetimes; a struct's tags are dropped wherever a value leaves it, and after a call that may move a borrow between its slots, so what it holds is then kept as one; a store through a reference a call returned, or one read through another, to a local struct of a self-similar type (a list node) holding the caller's borrows is refused |
| **Freezing** | the loan walk, on a gated function | a borrow held in a local, bare or inside a struct, enum, `Option`, array or list, and its copies, freeze the source until the last use (a finalizer that may read it, at the holder's death, included), and so do a borrow a call returns, of every argument, and one a call or a store through a reference puts into a local (from a `NoLoanMut` or `NoLoanRead` container, only its life): `ErrorFrozen` at a change, a move, a conflicting borrow, the source's end, and, under a mutable borrow, a read — for a source reached as `uni`; for one reached through a shared path, only its owner's move, replacement or end, a change or mutable borrow of the owner where it is held, and an `&uni` or `&imm` borrow | an element borrow through a shared path (the language's rule refuses it for a `ShapeChanging` container; not built); two copies of one `&mut`; a global a callee changes |
| **De-aliasing / drops** | flow decides, generation executes | scope-exit release of owning refs, of drop-fn structs and enums, and of tuples and arrays holding what finalizes or owns, from a jump down to the block it names; the previous value's release at a store over a variable, a part of one, or a place reached through a reference; each on the paths that hold the value, by a drop flag where they differ; a temporary's at the end of its statement, condition or operand, newest first, hollow where a value moved out through it | an array an element was moved out of leaks the rest; a value stored into a field of a variable holding nothing leaks; a temporary a borrow or a pointer made from it may outlive is kept, and leaks — see Hazards |
| **Permission** | `MayWrite` and `MayRead` | `ErrorNoMut` on assignment and swap; `ErrorNoRead` on a read through a reference — a dereference, an index, or a field of a virtual reference | `MayAliasWrite` and `IsLockless` are populated and read nowhere; `RaceSafe` is read by the thread check, a type check question (`refThreadBinds`) |
| **Initialization** | yes | `ErrorMove` "has not been initialized"; for a variable that moves or has anything to do as it dies, "may not have been given a value" where some path did not (the path walk) | for any other type, "initialized on one branch" reads as initialized everywhere; a variable never initialized may be borrowed, so a method taking it `&mut` can fill it, and nothing then stops a field it left unset being read through the borrow; the unused-variable warning in `flow.h`'s header does not exist |

Everything else about permissions is type check's: `permMatches` in
`borrowTypeCheck`, and variance in the reference matchers.

### Diagnostics

| Code | Site | Condition |
| --- | --- | --- |
| `ErrorInvType` | `flowHandleMove`, `flowResultMove` | move out of a global variable |
| `ErrorMoveOut` | `flowHandleMove`, `flowResultMove` | move out through a borrowed reference, or through a shared (aliasable) owning one |
| `ErrorMoveField` | `flowHandleMove`, `flowResultMove` (`flowRefuseMoveField`) | move out of a field — a struct's or a tuple's — or out of what is reached through one |
| `ErrorInvType` | `assignBorrowLifetimeCheck`, from `assignlvalrtype` and `swapFlow` | lval — a global, or a place reached through a reference — outlives the borrowed reference stored into it, or swapped into it |
| `ErrorNoMut` | `assignlvalrtype`, `swapFlow` | no write permission |
| `ErrorNoRead` | `flowLoadThroughRef` | no read permission on the reference a dereference, an index or a virtual-reference field reads through |
| `ErrorMove` | `nameuseFlow`, `nameuseFlowBorrowed` | read: uninitialized, or moved out; borrowed: moved out only (`borrowFlow` walks the borrowed place to its variable) |
| `ErrorMove` | `dropRefuse`, from the path walk | a tracked variable moved, read or borrowed where some path reaching it moved or hollowed it, or (not for a borrow) never gave it a value |
| `ErrorEscape` | `returnFlowEscape` | returned borrow outlives the local it points at |
| `ErrorEscape` | `loanEscape`, from the loan walk's `return` and `pwStoreEscapes` | a value returned, or stored where it may outlive the function, carries a loan of the function's own storage; or one stored into a global carries a caller loan |
| `ErrorCallEscape` | `fnCallFlowStoredBorrow` | a `&mut` or `&uni` argument, a receiver included, points at a place that can hold a borrow and outlives another borrow passed to the same call |
| `ErrorCallEscape` | `loanEscape`, from `pwCallStores` | another argument carries a loan of the function's own storage, and a `&mut X` argument, or a writable borrow reached from one or held by a struct argument, reaches a place that may outlive the function |
| `ErrorEscape` | `loanApart`, from the loan walk's `return` and `pwStoreEscapes` | a value returned, or stored where a parameter points, carries a caller loan of a lifetime flowing to none the result, or what that parameter points at (the field, for a struct declaring lifetimes), holds (named lifetimes) |
| `ErrorCallEscape` | `loanApart`, from `pwCallStores`; `loanNotGlobal`, from `pwStaticArgs`; `fnCallStaticArgs` | a call may store a caller loan where a parameter points at nothing of a lifetime it flows to; an argument for a `'static` parameter carries, or is, a borrow that is not global |
| `ErrorFrozen` | `loanUse`, for a conflict `loanAccess` recorded; `loanFlightAccess`, `loanFlightActivate` | a source read, changed, moved, borrowed or ended while a borrow of it that forbids that is still to be used; reported at the access, naming the borrow (or the method that returned it) and its next use. Or, at once, an access conflicting with a loan an earlier operand of the same call or literal carries, or a two-phase receiver conflicting at its call with what another argument carries |

A value an array's contents or `n of x` repeat is evaluated once per element,
so the ordinary move rule judges it: the loop `n of x` lowers to is walked as
any loop, and an array's contents copy the expression into each element
([fncall](../nodes/fncall.md), "The list after `<-`"). Past 16 elements they
copy it twice, the second copy filling the rest in a loop, so a variable moved
into the first element is refused once, at the second, and not again for each
later one.

## 8. Contract

**Before flow runs:** name resolution succeeded program-wide; this function's
signature and body type checked cleanly; every expression has a resolved
`vtype`; lowering is complete; `RefNode.scope` and `VarDclNode.scope` are
populated; regular blocks already end in a jump but loop blocks do not; globals,
parameters and fields already carry `VarInitialized`.

**After flow, for a function that ran it:** every block ends in a node carrying
a `dealias` list; every recognized counted acquisition has a `RefCountNode`; every
first-assignment target carries `FlagFirstAssign`; every temporary whose death
does something is a `TempNode` where it is read or thrown away. The loan walk adds nothing to
the tree; the drop-flag client, where it ran, rebuilt the lists of the exits it
met and set the store marks from every path's state.

**What generation relies on.** `genlBlock`, `genlBreak` and `genlReturn` call
`genlDealiasNodes` and do no analysis of their own. If flow did not run, the
lists are NULL, `genlDealiasNodes` returns immediately, and **nothing is ever
released** — there is no fallback. Likewise, without `FlagFirstAssign` every
first assignment to an uninitialized owning variable releases garbage, and a
reassignment after a move releases what the new owner holds: `genlStore`
releases an lval's previous value — an owner, or a value that finalizes, dying
in place — whenever the flag is absent. `assignlvalrtype` sets it when the
variable is not `VarInitialized`, or is `VarMoved` or `VarHollow`, at the
assignment, or the drop-flag client from every path; for a hollowed one, the
`HollowNode` wrapped round the stored value is what releases the old
allocation. A variable with `VarDropFlag` needs each marked move of it
(`FlagMoveOut`, `FlagHollowOut`) generated as a load of its name, where
`genlDropFlagUse` updates the flag.

## 9. Hazards

- **The gate is what keeps the main walk's lists right.** They are built from
  the state along one walk in source order, which is every path's only where
  no tracked variable changes deeper than its declaration and no block or `if`
  hands a value on from some of its paths. A new way for a variable's state to
  change must note it (`flowDropNote`, or set `dropgate`), or its function's
  drops are decided on one path's state.
- **A move the main walk makes must be marked where the value leaves**
  (`FlagMoveOut`, `FlagHollowOut`), or the path walk and the drop flag do not
  see it. A name use shared by several places in the tree cannot be marked:
  the match's own variable is the one known (flow.c, `flowMoveSource`).
- **A value stored into a field of a variable that holds nothing leaks.**
  `take(e); e.inner = new Inner(2);` and `mut e Holder; e.inner = new Inner(2);`
  finalize nothing of the old field (there is none), and the variable is not
  taken to hold a value by having one field given, so the new field is never
  finalized either. Rust refuses both.
- **A match's binding moved out and then assigned over leaks.** The store reads
  the binding's own state, which the move left moved, so it releases nothing,
  and it does not give the matched value's variable its value back, so that
  variable, moved too, never releases what was stored: `case mut a A {
  takeA(a); a = A[new Fin(2)]; }` leaks the new `Fin`. Moved on some paths only, the
  store still releases nothing, and the original leaks on the paths that kept
  it. The binding is untracked by the path walk (`flowMatchBound`), so neither
  the store nor the matched value's drop flag sees the other.
- **A hollowed variable reassigned by a destructuring of one value**
  (`b, x = pair()`) leaks its old allocation: `assignMultRetFlow` has no single
  value to wrap a `HollowNode` round, so it takes the moved variable's path
  (`FlagFirstAssign`, nothing released).
- **A kept temporary leaks.** `flowTempEscape` keeps a temporary that a
  borrow or a raw pointer going out of its statement may point into, and it
  is never finalized: `imm r &R = mkso()` leaks the owner, as every temporary
  did before. The walk is conservative: any value that can hold a borrow or a
  pointer carries the question on, so `imm n = mkso().me().n` keeps it too.
  Extending a temporary's life to its borrow's, or refusing the borrow, is the
  lifetime work's to decide.
- **A new place a value is consumed without being taken must call
  `flowTempRead`**, or a temporary there is never finalized; and a new place a
  value is taken must not, or it is finalized under its new holder.
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
| `ir/stmt/fndcl.c` | `fnDclTypeCheck` | the only entry point; the per-function error-delta gate; `FlowTimer` round `blockFlow` and the path walk under `-V 1`; the path walk on a function either gate marked, `blockFlow` having found no error in it; `flowGateCount` after it |
| `ir/flowpath.c` | `flowPathWalk`, `flowPathPrint` | the path walk (§6): its entry, for loans, drops or both, and its `-V 2` tallies |
| | `pathSetFacts`, `pathSetState`, `pathRollback`, `pathDelta`, `pathJoin` | the state per path: set a fact (loans, or a drop state), recording the old one; undo to a fork; what a path changed; join paths |
| | `pwValue`, `pwPlace`, `pwThrough`, `pwBorrow`, `pwLend`, `pwOwnedLent`, `pwStore`, `pwSwap`, `pwVarDcl` | the walk's dispatch: what each node accesses, which holders it uses, what loans its value carries |
| | `pwStoreInto`, `pwStoreLands`, `pwPlaceLevel`, `pwCallStores`, `pwStoreTarget`, `pwLendSite` | a store into a holder or through a reference, the holders where it lands, and what a call may store through a `&mut X` argument or any writable borrow it reaches |
| | `pathSetLoansAs`, `pathSetUnsure`, `pathSetThrough`, `LoanFar`, `loanOf`, `loanThrough` | near and far loans: a set's loans made all far or all near, or both where it is not known which, or read through a reference; a loan's id from a set's entry; whether a loan borrows through its root |
| | `pathSetRemake`, `pathSetSlots`, `pathSetUntagged`, `pwSlotStep`, `pwPlaceSlots`, `pwStoreTagged`, `pwCallMoves`, `pwUntagHolder`, `loanTag` | slots: a set's entries remade; the loans of some slots; tags dropped; a field of a struct declaring lifetimes as a place's first step; what a read or a borrow of it carries; a store's tag; a call that may move borrows between slots, after which a holder's tags are dropped |
| | `pwPlaceOutlives`, `pwStoreEscapes` | whether a place stored into may outlive the function; the store refused when it may and the value carries a local loan, or when it is rooted at a global and the value carries a caller loan |
| | `pwNamedSig`, `pwArgCarries`, `pwCallerLoans`, `pwStoreApart`, `pwStaticArgs` | named lifetimes: the callee's signature where it names them; what of each argument a result or a store through a writable one may carry; a parameter's caller loans, part by part; a caller loan stored apart from its lifetime; a `'static` parameter's argument |
| | `pwStmts`, `pwBlock`, `pwBlockExits`, `pwLoop`, `pwIf`, `pwJump`, `pwScopeEnd`, `pwScopeEndHanding`, `pwHolderDies`, `pwExit` | forks and joins, loops to a fixed point, jumps, a scope's end as an access (with a block's value in flight), a finalizing holder's death as a use, and an exit's record for the drop-flag client |
| | `pwDropUse` | a use of a place's root variable, checked by the drop-flag client |
| `ir/flowdrop.c` | `dropMove`, `dropUse`, `dropRefuse` | a marked move's new state; a use some path left without its value, refused once |
| | `dropStore`, `dropPartStore`, `dropExit` | what each variable a release releases may hold there, gathered over every walk |
| | `dropWalkEnd`, `dropApplyExit`, `dropApplyStore`, `dropApplyPart`, `dropPrint` | a flag for each variable whose state differs at a release; each exit's list rebuilt, each store marked; the `-V 2` tally |
| `ir/flowloan.c` | `loanMake`, `loanCaller`, `loanHeldBy` | a borrow's loan, a parameter's caller loan, and who may hold it |
| | `loanIsLocal`, `loanLocalIn`, `loanMayPointOut`, `loanMayBePointee`, `loanEscape` | a loan of the function's own storage; whether a reference may point beyond the function; an escape reported (`ErrorEscape`, `ErrorCallEscape`) |
| | `loanCallerApart`, `loanStoredApart`, `loanNotGlobalIn`, `loanApart`, `loanNotGlobal` | named lifetimes: a caller loan whose part flows to no lifetime where it goes, or a loan that is not global, and their reports |
| | `loanWhole` | a loan of the whole of its root, where a slot's tag stays the root's |
| | `loanAccess`, `loanUse` | a conflicting access records a pending conflict on each holder of the loan; a use of the holder fires it (`ErrorFrozen`) |
| `ir/stmt/module.c` | `modInitOf`, `modInitFlowBegin`, `modInitFlowEnd` | round a module's `init` only: its module's globals without a value start the pass uninitialized, as locals, so `init` assigns each once and reads none first; one never assigned is `ErrorGlobalUninit`. [module](../nodes/module.md), "Init and final" |
| `ir/flow.c` | `flowLoadValue` | the walk's spine — tag dispatch for a value being read |
| | `flowLoadThroughRef` | `MayRead` on the reference a value is read through; called from `derefFlow`, `fnCallArrIndexFlow` and `fnCallFldAccessFlow` |
| | `flowNewSelf`, `flowNewSelfThrough`, `flowNewSelfFill`, `flowNewSelfReturn` | an init's `self &new`: the variable, a use through it, the store that fills it, a return before it is filled ("An init's self") |
| | `flowHandleMoveOrCopy` | move vs. alias, for a value going to a new holder; a tuple literal's element by element |
| | `flowHandleMove` | deactivate the source — each move-typed element's, for a tuple literal; for a block or an `if`, what any value it hands back moves out of, marking each move (`FlagMoveOut`, `FlagHollowOut`) and gating the drops where not every value does; hollow a local sole owner moved out through; refuse a move out of a field (`flowRefuseMoveField`) or a global, or out through a borrowed or a shared owning reference |
| | `flowOwningLocal`, `flowNewHollow` | the local owning reference a move reaches through; the `HollowNode` releasing a hollowed variable as it stands |
| | `flowResultMove` | the same refusals for a returned value, deactivating nothing |
| | `flowIsLvalRead` | the temporary-vs-lvalue test that makes counting correct |
| | `flowTempRead`, `flowIsTemp`, `flowCastHandsOn` | wrap a temporary whose death does something in a `TempNode`, where it is read or thrown away ("Temporaries") |
| | `flowTempHollow` | a move out through a temporary sole owner, noted in its `moved` |
| | `flowTempEscape`, `flowTempCarries`, `flowTempCallStores` | keep each temporary a borrow or a pointer going out of its statement may point into |
| | `flowInjectRefCountAmt` | wrap a counted reference, or a struct, enum, tuple or array whose death releases one, in a `RefCountNode` |
| | `flowIsRcRef`, `flowIsOwningType` | is this type counted; is it an owning reference, or a tuple of them, that a store releases |
| | `flowHeldCounted`, `flowVariantHeldCounted` | does a copy of this struct, enum, tuple or array add a holder to a counted reference its death releases |
| | `flowMatchBound`, `flowMatchInPlace` | the matched value a match's binding stands for, or NULL; whether the binding is by value, naming the matched value's own storage |
| | `flowScopePush`, `flowScopePop`, `flowAddVar` | the variable stack |
| | `flowScopeDealias`, `flowVarRelease` | build a scope's release list; skip an uninitialized, moved-out or handed-back variable; release a hollowed one hollow; one variable's release, whole or hollow, in a `DropFlagNode` where asked |
| | `flowVarSetFlags`, `flowVarLogMark`, `flowVarPathTake`, `flowVarRollback`, `flowVarJoin` | the main walk's variable flags, logged so that an `if`'s arms are walked from one state and joined |
| | `flowDropTracked`, `flowDropNote`, `flowDropOwner`, `flowLvalRootVar` | the drop gate: a tracked variable changed deeper than its declaration; the variable owning a binding's value; the local a store's target is part of |
| | `flowStateInit`, `flowGateResultAsk`, `flowGateCallAsk`, `flowGateOperandAsk`, `flowGateIsOwnedLent`, `flowGateUse`, `flowGateCount`, `flowGatePrint` | the gate (§3, "The gate"): the questions its triggers ask out of line, the waiting operands' borrows, written or an owner's implicit lend, the `-V 2` tallies |
| `ir/flowgate.h` | `flowGateHolder`, `flowGateAssigned`, `flowGateResult`, `flowGateCall`, `flowGateOperand` | the gate's triggers as inline tests, dismissing what cannot carry a borrow without a call |
| `ir/itype.c` | `itypeCarriesBorrow` | may a value of this type hold a borrowed reference; a struct's answer remembered in `StructNode.carriesborrow` |
| | `itypeDropReadsBorrow`, `itypeHoldsBorrowOf`, `itypeWritableBorrowDepth` | may a value's death read a borrow it holds (the drop check); can a borrow of a given type be stored in a value of this type; how many writable borrows deep a store into one can reach |
| `ir/exp/block.c` | `blockFlow`, `blockResultMove` | scope push/pop, `blockret` injection, result walk then dealias capture; a `return`'s move source, a returned block's or `if`'s values moved; a loop body one level deeper; whether every path returned (`jumped`) |
| | `blockDiscards`, `blockTempEscape` | a block throwing its final expression's value away; each statement that made a temporary walked by `flowTempEscape` |
| `ir/exp/if.c` | `ifFlow` | each arm from the state its conditions leave, the arms that did not return joined; later conditions and arms one level deeper |
| `ir/exp/assign.c` | `assignlvalrtype`, `assignSingleFlow`, `assignBorrowLifetimeCheck`, `assignIsLocalPlace` | `MayWrite`, `VarInitialized`/`VarMoved`/`VarHollow`, `FlagFirstAssign`, the `HollowNode` round a hollowed variable's new value, borrow lifetime of a store into a global or through a reference (one into a variable's own storage is the loan walk's) |
| `ir/stmt/swap.c` | `swapFlow` | `MayWrite` on both sides; borrow lifetime once in each direction |
| `ir/exp/nameuse.c` | `nameuseFlow`, `nameuseFlowBorrowed` | the only place the flags are *diagnosed* on, a hollowed variable as a moved one; both `ErrorMove` messages, and for a borrowed variable only the moved-out one |
| `ir/exp/borrow.c` | `borrowFlow`, `borrowFlowPlace` | the borrowed place must not be moved out: the variable at its root goes to `nameuseFlowBorrowed`, which refuses it moved out or hollowed but not uninitialized; a reference it is reached through is loaded as a value and not read through, an index is read; a temporary at its root wrapped; no aliasing tracked |
| `ir/stmt/return.c` | `returnFlowEscape` | `ErrorEscape` for a returned borrow of a local |
| `ir/exp/fncall.c` | `fnCallFlowStoredBorrow` | `ErrorCallEscape` for a `&mut` or `&uni` argument to a place that can hold a borrow (`itypeCarriesBorrow`), a receiver included, the callee could store a narrower borrow through |
| `ir/exp/arraylit.c` | `arrayLitFlow` | each element of the list form a holder; the fill form's one constant read |
| `ir/types/reference.c` | `refAdoptInfections` | where a reference type acquires `MoveType` |
| `ir/types/region.c` | `regionIsCounted`, `regionIsOwning`, `regionMethod` | which region methods a region declares, which is what flow asks of it |
| `genllvm/genlalloc.c` | `genlRegionAlias`, `genlReleaseOwning`, `genlHollowRelease`, `genlDealiasNodes`, `genlTempKeep`, `genlTempsEnd`, `genlTempsJump` | what consumes everything flow injected |
| | `genlDropFlagBegin`, `genlDropFlagSet`, `genlDropFlagIf`, `genlDropFlagUse` | a variable's drop flag: made, updated where its value arrives or leaves, tested round a release |

Test sources that pin behavior precisely: `test/cases/move/move-flow-*.cone`,
`test/cases/region/region_flow*.cone`, `test/cases/ref/ref_flow.cone`,
`test/cases/ref/ref_flow_return.cone`, `test/cases/core/core_flow_gate.cone`, and
for the loan walk `test/cases/ref/ref_flow_freeze.cone`,
`test/cases/ref/ref_flow_freeze_loop.cone`, `test/cases/ref/ref_freeze_success.cone`, for an owner's
implicit lends to one call with no written borrow to gate the function
`test/cases/ref/ref_flow_ownerlend_call.cone` and
`test/cases/trait/trait_flow_vref_owner_call.cone`, and for a borrow a call
returns `test/cases/collection/collection_flow_freeze.cone`,
`test/cases/region/region_flow_arena_freeze.cone` and
`test/cases/collection/collection_freeze_success.cone`, and for a borrow held
inside a value `test/cases/region/region_flow_pool_freeze.cone` and the
`lifetime` group's `lifetime_free_*_holder_*` scenarios, and for drop flags
`test/cases/move/move_drop_flags.cone`, `move_drop_flags_agree.cone`,
`move_flow_paths.cone`, `test/cases/region/region_drop_flags.cone` and
`test/cases/struct/struct_final_reassign.cone`, and for temporaries
`test/cases/struct/struct_final_temporary.cone`.

## 11. What lives elsewhere

| Question | Note |
| --- | --- |
| What the three reference axes mean, and what each permits | [References and Regions](../../../../doc/design/references-and-regions.md) |
| Which safety properties actually hold today | [Safety](../../../../doc/design/safety.md) |
| When a function is type checked at all | [Type Check Phase](type-check.md) |
| What a borrow's type records, and where | [Type Check Reasoning](type-check-reasoning.md), "Borrows: where type check stops" |
| How the allocation header is laid out | [Generation](generation.md), "The allocation header" |
