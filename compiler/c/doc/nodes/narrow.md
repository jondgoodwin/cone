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
variant. Nothing in flow, the loan walk or generation reads the facts.

*Provenance: read from source.*

## What is known, and where it goes

`NarrowEnv` is a snapshot of the live facts (`gFacts`), saved and restored around anything
that forks the path. It is not walked: type check checks conditions and blocks in source
order, and the environment follows.

| Where | What it does |
| --- | --- |
| `ifTypeCheck` | each condition is checked knowing the earlier ones were false; its block knows it was true (`narrowAssume`); the blocks that do not leave (`ifBlockJumps`), and the path that went by every condition when there is no `else`, are joined after the `if` (`narrowJoin`); a block that leaves takes its changes with it unless it continues a loop |
| `logicTypeCheck` | the right operand of a bool's `&&` knows the left was true, of `||` that it was false; afterwards either path may hold (`narrowAssumeLocal`). A left operand that is not a bool is a method call, which narrows nothing |
| `blockTypeCheck` | a loop block is a frame (`narrowLoopEnter`, `narrowLoopExit`) |
| `fnDclTypeCheck` | a function body starts with nothing known and gives back what it was given (`narrowFnBegin`); a closure's `()` is its own function |
| `assignTypeCheck`, `swapTypeCheck` | `narrowAssigned`: the name is changed; a variant stored over a name held by value is a narrowing to it (`state = State.Run(2)`) |
| `borrowMutRef`, `borrowTypeCheck`, `borrowDerefOf` | `narrowBorrowed`: a writable borrow of the whole enum, or of what a reference variable points at, is a change |
| `varDclTypeCheck` | `narrowDeclared`: a local declared with a variant of its enum (`var s: Shape = Shape.Circle(1)`) is narrowed to it |

A set that comes out empty (a test that contradicts what was believed) is a branch that cannot
run, and is taken as no knowledge rather than refused. A test in this pass of a loop makes a
narrowing of this pass (`NarrowFact.depth`).

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
  counts them (`narrowCallMade`: a call that is not an operator on numbers, a store through a
  name, a swap), keeps the count with each environment as the path's own (a branch's calls are
  its own), and records the count at each test (`narrowNoteTest`). A fact made by a test is
  anchored, or used as a transient copy, only if no call has been made since; otherwise the
  narrowing ends, and the use is `ErrorNarrowStale` naming the call or store.

A member the variant has, asked of a name that was narrowed and has been changed, is
`ErrorNarrowStale` (type check), naming the change and where. A loop is walked once, so a
narrowing it inherits from before it is good for its first pass: each such use is noted, each
change made in the loop is noted, and at the loop's end a use a change reaches is
`ErrorNarrowLoop`. Both are reported at the use.

## What it does not do

- **It never proves a call harmless for a name held by value or through `uni`, `imm`, `mut1`.**
  No other name can change the variant there: a local is reached only by itself, and a
  borrow of it that is live when the test reads it freezes it (the loan walk's read of an
  exclusively borrowed place). Only the name's own assignments, writable borrows of the whole
  enum and swaps end the narrowing.
- **It does not retype the name.** Only a member of the variant named on it sees the variant.
  Passing the name where the variant is wanted is still a type error, since the name is the
  original value (4c).
- **It does not narrow through an owner.** An `Rc[imm, Shape]` bound by a pattern is a copy of the
  owner typed as the variant (cast.md); no name of an owner is narrowed.
- **It does not follow a closure that changes a captured name,** or a pointer made of one, past
  the writable borrow that made it (`narrowBorrowed` ends the narrowing at the borrow).

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
