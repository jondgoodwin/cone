Flow typing is not a node. It is what type check knows, along the path it is checking,
of which variant each local or parameter of a closed enum's type holds, and a few arms
in the nodes that change or test one. `ir/exp/narrow.c` owns the knowledge; this note
says what it is, who asks it, and what it deliberately does not do.

**At a glance.** A name tested for a variant (`if s is Circle`) is that variant where the
test holds. Type check keeps, for each such variable, the set of variants it may hold
(`NarrowFact.possible`, a bit for each of the enum's `derived`). An `is` test narrows it on
the path where it holds, and the opposite test removes a variant on the other (so the last
variant left is the name's by elimination). A test of a trait pattern (`s is Measured`) names
the variants that comply ([cast](cast.md), "Trait patterns"): where it holds the name may be
any of them, and where it fails those are removed, so what is left is narrowed as usual. A change of the name, an assignment of a value not
known to be a variant, a writable borrow of the whole enum, a swap, a store through a
reference, puts every variant back and remembers the change. The one place the knowledge is
read is a member named on the name (`narrowReceiver`, from `fnCallTypeCheckNode`): if the
member is the variant's alone and the name is narrowed to it, the use is typed as the
variant. Nothing in flow, the loan walk or generation reads the facts. The loan walk is told
where a name itself was narrowed, to hold it against a `mut` borrow ("A name narrowed beside a
`mut` borrow").

*Provenance: read from source.*

## What is known, and where it goes

`NarrowEnv` is a snapshot of the live facts (`gFacts`), saved and restored around anything
that forks the path. It is not walked: type check checks conditions and blocks in source
order, and the environment follows.

| Where | What it does |
| --- | --- |
| `ifTypeCheck` | each condition is checked knowing the earlier ones were false; its block knows it was true (`narrowAssume`); the blocks that do not leave (`ifBlockJumps`, or `ifBlockFallsOut` for a block that leaves through an inner `if`, `match` or block), and the path that went by every condition when there is no `else`, are joined after the `if` (`narrowJoin`); a block that leaves takes its changes with it unless it continues a loop |
| `logicTypeCheck` | the right operand of a bool's `&&` knows the left was true, of `||` that it was false; afterwards either path may hold (`narrowAssumeLocal`). A left operand that is not a bool is a method call, which narrows nothing |
| `blockTypeCheck` | a loop block is a frame (`narrowLoopEnter`, `narrowLoopExit`) |
| `fnDclTypeCheck` | a function body starts with nothing known and gives back what it was given (`narrowFnBegin`); a closure's `()` is its own function |
| `assignTypeCheck`, `swapTypeCheck` | `narrowAssigned`: the name is changed; a variant stored over a name held by value is a narrowing to it (`state = State.Run(2)`) |
| `borrowMutRef`, `borrowTypeCheck`, `borrowDerefOf` | `narrowBorrowed`: a writable borrow of the whole enum, of what a reference variable points at, or of a reference variable itself, is a change |
| `varDclTypeCheck` | `narrowDeclared`: a local declared with a variant of its enum (`var s: Shape = Shape.Circle(1)`) is narrowed to it |

A set that comes out empty (a test that contradicts what was believed) is a branch that cannot
run, and is taken as no knowledge rather than refused. A test in this pass of a loop makes a
narrowing of this pass (`NarrowFact.depth`).

**A borrow of a reference variable.** Where the name is a reference (`p: &uni Slot`), a `mut` or
`uni` borrow of `p` itself is a writable route to the whole enum (`**r = v`) and to the name
(`*r = other` points it at another value), so `narrowBorrowed` ends the narrowing there,
whatever the permission of `p`. A borrow of that borrow (`&mut &mut p`) is reached through it,
so ends nothing more. One made before the test and still held there is the loan walk's, below.

**A change in a later operand of a condition.** The tests of a condition are asked again once
an operand is checked, for the one after it (`narrowAssumeLocal`), and once the whole is, for
the branches (`narrowAssume`). A test whose name has changed since it was checked says nothing
then: `narrowNoteTest` notes with each test how many changes had been made (`gChanged`, every
`narrowKill`, whether or not it ended a narrowing), and `narrowAssumeCond` passes over a test
whose name is among those made since. In `if s is Circle && reset(&mut s)` neither the block
nor an operand after `reset` knows `s` from that test. What the environment holds stands: the
change, so a use is `ErrorNarrowStale`, or nothing, where paths met after a store of a known
variant. A test after the change (`reset(&mut s) && s is Circle`) narrows as any other.

**What is narrowed.** A local or a parameter that is named in source (a hidden variable's name
starts with `-`, or is `_`), of a closed enum (`narrowClosedEnum`: a trait with no base that has
a tag or is `SameSize`, at most 64 variants), held by value or through a borrowed reference.
Not a field, an element, a dereference, a global or a static; not a value reached through an
owner (`Rc`, `Uni`), whose referent others reach; not a value already typed as a variant.

## Reading it

`narrowReceiver` is asked once the receiver of a member call or field access is checked. The
member must be one the variant has and the enum does not (`iNsTypeFindFnField` on each): what
the enum has, the enum's use finds, so a method declared on the enum and a field every variant
shares are reached exactly as they were. Then:

- a value held by name, or a reference no other reference can change the variant under (`uni`,
  `imm`, `mut1`, `MayIntRefSum`): the name use is retyped as the variant, a reference to it
  for a reference, and flagged `FlagNarrowed`. A variant is a prefix of its enum's bytes, so a
  field is read with the variant's layout from the variable's own storage; generation loads
  and addresses a flagged use as the variant (`genlTerm`, `genlAddrType`);
- a reference others may share (`ro`, `mut`): the receiver becomes a use of a hidden local
  `-narrow` that holds the narrowed reference, a recast marked `FlagVariantNarrow`
  (`castNarrowRef`), which the loan walk follows as it follows any reference narrowed into a
  variant (flow.md, "A change through another name"). The local is made when a use first needs
  it, and declared where the narrowing was made: at the head of the block the test led to
  (`narrowInsertHeads`), or right after the `if` that left (`narrowInsertAfters`, from
  `blockHoist`'s end), which the statement tracking says is a statement of its block
  (`narrowStmtBegin`). A narrowing made inside one condition, or by an `if` that is not a
  statement of its block (a `match` is wrapped in a block), has no local to hold it: in a
  condition the use is a narrowed copy of the reference (`transient`), and elsewhere the name is
  not narrowed.

  **The loan walk follows the local only from where it is declared.** A call or a store made
  between the test and there, in a later operand of the condition, could change the variant
  unseen, and so could one between the test and the use of a transient copy. So type check
  logs them (`narrowCallMade`: a call that is not an operator on numbers, a store through a
  name, a swap) as a chain of `NarrowCall`s kept with each environment as the path's own (a
  branch's calls are its own; where two paths meet the chain holds both), and notes the chain
  at each test (`narrowNoteTest`). A fact made by a test is anchored, or used as a transient
  copy, if every call or store logged since could not change the variant
  (`narrowReachedSince`); otherwise the narrowing ends, and the use is `ErrorNarrowStale`
  naming the first that could. A call is asked what the loan walk asks of one made while the
  narrowed reference is held (`reshapeCall`, with the enum as the container and every call
  taken to be able to write a global): handed a way to write an enum of the type whose body
  stores over it, a body that cannot be seen, or one that writes a global through which the
  enum is reached. It names no owner of the borrow nor of the receiver, so a receiver is never
  excused as the field of another struct than the borrow's. A store is asked `reshapeStoreHits`: replacing a value that holds the enum
  whole, not writing a field within a variant. A callee whose body is not type checked yet
  (a recursion) is taken to reach it. Nothing is logged before the first test of a function.

A member the variant has, asked of a name that was narrowed and has been changed, is
`ErrorNarrowStale` (type check), naming the change and where. A loop is walked once, so a
narrowing it inherits from before it is good for its first pass: each such use is noted, each
change made in the loop is noted, and at the loop's end a use a change reaches is
`ErrorNarrowLoop`. Both are reported at the use.

## A name narrowed beside a `mut` borrow

A live borrow freezes a narrowed name. For a name held by value or through `uni`, `imm` or
`mut1`, an exclusive borrow live at the test keeps the test from reading it, and a read-only
one changes nothing. A `mut` borrow of a local, or of what a `uni` reference points at, is an
alias loan: it freezes nothing, and may store another variant while the name is read as the one
tested. There the rule is kept by the loan walk, as for a reference narrowed into a variant.

The name is retyped and no node converts anything, so type check marks where a name is
narrowed: the `is` test (`FlagNarrowTest`) and the store of a variant over it
(`FlagNarrowAssigned`). At each, the walk asks whether a `mut` borrow is held of the place,
of a place inside it, or of the reference variable it is reached through (`pwNarrowName`,
`loanMutAliasAt`). Where one is, it makes a marked loan of the place (`loanNarrowName`),
freezing nothing, held by a stand-in variable that bears the name. Every call made and every
store that could change the variant while it is held pends on the stand-in (flow.md, "A change
through another name"), and a use of the stand-in is each member of the variant named on the
name (`FlagNarrowed`, `pwPlace`), where the refusal, `ErrorVariantReshape`, names the `mut`
borrow. What is borrowed through such a member, and a narrowed reference handed on as the
receiver, carries the loan too (`Place.narrow`, `pwLend`). Where none is held, nothing is done.

Where the name is a reference and the `mut` borrow is of the variable itself (`&mut p`), that
borrow can also point the name elsewhere (`*r = other`, or a call handed `r` that does it),
which changes which enum is read. The stand-in then holds a second loan, of the variable,
marked with the reference's type as an owner's is (`reshapeOwnerCont`): a store of such a
reference over it, or a call that may, is refused where the name is next used as the variant.
This loan is the name's alone (`pwNarrowCarried`): what was borrowed through a member before,
or copied from the reference, stays where it pointed.

A test or store that establishes the variant by itself drops what earlier ones held, so a name
tested again after a change is accepted. Where what is known past a test rests on earlier ones
too (`FlagNarrowKeeps`: two tests leaving one of three variants), their loans are kept beside
its own, except in the arm of a test that establishes the variant alone
(`FlagNarrowArmAlone`). A test inside a larger condition whose other path rests on earlier
tests keeps them on both paths, which can refuse a change that came before the test.

## A compound assignment

`x += e`, and each compound form, is one statement that reaches its place before its right
side and writes into it after. Where the type declares the operator it is a call handed
`&mut x`, and the borrow is in flight while `e` is made. Otherwise it is
`{let tmp = &mut x; *tmp = *tmp + e}`. A plain store reaches its place after the value,
which uses whatever the place is reached through (`-narrow`, a narrowed name's stand-in, a
reference bound into the variant) and so meets a change the value made; `tmp` was made
before, and is no holder (flow.md, "What is not held"). So where `tmp` carries a borrow into
a value that changes shape (a collection, what an owner points at, an enum's variant), the
walk holds those in a stand-in for the length of the right side, and the write through `tmp`
is its use (`pwCompoundPlace`). A call or store there that could change which variant the
enum holds is `ErrorVariantReshape`, whether the place was reached through a narrowed
reference, a narrowed name beside a `mut` borrow, a pattern's binding or a reference bound
into the variant; one that could change a collection's shape or replace an owner is
`ErrorShapeReshape` or `ErrorShapeStore`, whether the element was reached by index, a method
returning a reference or a reference bound to it. A move of what the place was borrowed
from, or a store or swap over it by its own name, is `ErrorFrozen` (`l[0] += take(l)`,
`u.x += {u = ...; 5}`): the stand-in's write is the borrow's later use. Nothing else is
asked of it (`loanUseCompound`): `x += x` reads what it writes.

## What it does not do

- **It never proves a call harmless for a name held by value or through `uni`, `imm`, `mut1`,
  in type check.** Only the name's own assignments, writable borrows of the whole enum and
  swaps end the narrowing there; what another name may change is the loan walk's, above.
- **It does not retype the name.** Only a member of the variant named on it sees the variant.
  Passing the name where the variant is wanted is still a type error, since the name is the
  original value (4c).
- **It does not narrow through an owner.** An `Rc[imm, Shape]` bound by a pattern is a copy of the
  owner typed as the variant (cast.md); no name of an owner is narrowed.
- **It does not follow a closure that changes a captured name,** or a pointer made of one, past
  the writable borrow that made it (`narrowBorrowed` ends the narrowing at the borrow).
- **It does not see a shared name (`ro`, `mut`) pointed at another value within a condition,**
  through a `mut` borrow of it made before the test. The hidden local is declared after the
  whole condition, and a transient copy is made at the use, from what the name points at by
  then, while the call log asks only what could store over the enum. In
  `if p is Circle && repoint(r)` the block reads the new value as a `Circle`.

## Hazards

- **The facts are file-static,** like the variable stack, and safe for the same reason: type
  check checks one function body at a time, saving and clearing them at its edges.
- **A snapshot is a copy.** A fact set after a snapshot was taken is not in it; the local a
  shared reference's narrowing holds is made on the live fact and found by later uses on the
  same path, and made again on a path that did not see it.
- **`ifBlockJumps` reads a type checked block.** The environment of a branch is joined only
  after the branch is checked, and a block that failed to check (not a block node) is joined
  as one that did not leave.
- **Cloned code is checked again.** A generic's instance or a macro's expansion is checked
  where it is used, with whatever is known there; the facts name declarations, which a clone
  makes anew, so nothing crosses.
