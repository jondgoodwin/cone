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
| `FlagParallel` | the outer block of a `parallel each`; set with `FlagEach` by the parser and **kept** once `parallelEachLower` has built the loop, for the body's checks and for generation, which outlines the loop. With `FlagParIncl` (a range written `<=`) and `FlagParRange` (a number range, not a source) |
| `FlagLoopElse` | the block a loop leaves through when it has run out: the statements of its `else`, ending in the `break` that carries the loop's value (`blockElseFinish`). Name resolution reads it with the loop *around* the loop it stands in as the innermost one (below) |

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

**A header `if`, and the loop's `else`.** A filter, `each p in src if cond {…}`
(`parseEachFilter`, which the header of an `each` entry of `<-` reads with the same call),
becomes `if not cond { continue }` as the first statement of the body after the
pass's variables, built by `parseEachFilterStmt`; for a range the `continue`
carries the step like any other. A trailing `if` on `break`, `continue` or
`return` is the same jump inside an `if` arm (`parseJumpEnd`). The `if` after
a `break` or `return` with no value is the statement's when its condition is
followed by the end of the statement, and an `if` expression's when a block
follows, which `parseIf` decides when `parseJumpValue` has told it to leave the
condition.

An `else` after a loop (`parseLoopElse`) is a block of statements that is
moved into the loop as the arm taken when the loop runs out, and the loop is
then an expression: `blockElseFinish` flags the block `FlagLoopElse` and ends
it with a `break` carrying its last expression (or nil, or nothing when it ends
in a `return`, `break` or `continue`), aimed at the loop it leaves
(`FlagBreakAimed`, which `breakNameRes` keeps). `while cond {…} else {…}` puts it
where `if not cond { break }` goes. An `each` over anything but a range puts it
first among the loop's statements, ahead of the reader's variables so that it
cannot name them, and `eachLower` takes it out to make the exit of the counted
loop or of the cursor's `None` arm. A range has several places it runs out
(the guard, and the steps that stop a counter wrapping or reaching its bound),
and one `else`: they set a hidden flag instead of breaking, and the guard
`if flag or not (c < b) {…else…}` leaves at the top of the next pass.

**`parallel each` is parsed as `each` is**, with `FlagParallel` beside `FlagEach`.
`parallel` is an ordinary name (`parallelName`) that `parseExprBlock` takes for the
word only when `each` follows it directly (`lexNextIsWord`), so it stays usable as
a variable, a function or a field. A number range is not rewritten to a counter, as
`each`'s is: its two bounds are held in two hidden variables (`first'`, `last'`),
the block has three statements, and `FlagParIncl` says `<=`. A count down, a
`by` step and an `else` are refused here (`ErrorParSource`, `ErrorParElse`); a
header `if` is the same `continue` statement after the pass's variable. As an
entry of `<-` it is read by `parseEntry` the same way (`parseParallelEachEntry`),
and the loop ends in a `yield` ("Type check", the parallel builder).

## Name resolution

`blockNameRes`: save `loopblock` and make this block the innermost loop target
if it is one — that is what an unlabelled `break`/`continue` binds to. Push a
scope and a hook table. Hook `lifesym` if it is free; **if it is already bound,
report the duplicate and do not hook**, so an inner `break 'x` silently reaches
the outer block.

**A loop's `else` is read as outside the loop.** An `else` is written after the
loop, so its `break` and `continue` are the enclosing loop's, though it stands
inside the loop it is the exit of. `blockNameRes` keeps `outerloop`, the loop
around the innermost, and walks a `FlagLoopElse` block with it as the innermost
(`loopblock`). Its variables are in the loop's scope depth, which is what the
borrow-lifetime checks count, and it is placed ahead of the pass's variables, so
the pass's variables are not in view.

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
  A source *written* `&mut src` (a borrow expression with the `mut` permission,
  in `each`, `parallel each` and `<- each` alike) never gets this far: the parser
  refuses it (`parseEachMutSource`, `ErrorEachMutSource`) naming `src.mutItems()`.
  A `&mut` variable or parameter that is itself the source is no written borrow
  and is walked as the reference it is, a read.
- **core's `ArrayIter` or `ArrayIndexed`, a value made for the loop** (not a
  place, not behind a reference; `iter()` with one variable, `indexed()` with
  two, `eachIsCore`): the same counted loop, over the slice the cursor holds and
  from the position it holds (`mut i = cursor.pos`), so a cursor made by the loop
  is no cursor in the code. With `indexed()` the first variable is a copy of `i`
  and the second the element's borrow. A cursor kept in a variable, or taken
  whole by one variable of `indexed()`, is walked through its `next`.
- **core's `ArrayMutItems` or `MutItemsIndexed`, a value made for the loop**
  (`mutItems()` with one variable, its `indexed()` with two): the same counted
  loop as `ArrayIter` and `ArrayIndexed`, lending `&mut s[i]` where those lend
  `&s[i]`. What is lent is what the cursor's own `next` lends, and a bare
  `indexed()` lends `&` on any slice, a `&mut` one too: the meaning comes from the
  library's types, the loop only being faster. `chunks(n)` and `mutChunks(n)` give
  `ArrayChunks` and `ArrayMutChunks` (and `ChunksIndexed`, `MutChunksIndexed`
  from their `indexed()`), cursors with a `next`, which `each` walks through it.
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

A loop's `else` (`FlagLoopElse`, first among the loop's statements) is taken
out first and becomes the block the loop leaves through: the `if i >= len`
arm of a counted loop, the `None` arm of a cursor's match (`eachLeave`). A
cursor's arm is one block deeper than where name resolution counted the block's
variables; the difference makes them look shallower, not deeper, to the
borrow-lifetime checks.

A source is checked once, as the initializer of the hidden variable. Checking an
expression a second time is not idempotent for a call, which type check lowers,
so the lowering uses the node the check left: where the source is a place a
second check leaves alone (`eachRecheckable`), the variable is dropped and the
place is used in its stead; otherwise the variable holds it.

**`FlagParallel`: a `parallel each` is built by `parallelEachLower`**
(`ir/exp/pareach.c`), called in `eachLower`'s place from `blockTypeCheck`. A range
checks its two bounds first (`parallelEachBoundsFirst`: the one that is not an
untyped literal, so that the other takes its type, `parallelEachBoundType`). The
source must be a number range of whole numbers no wider than a usize, an array,
a slice, a type that lends an array, or core's `ArrayChunks` / `ArrayMutChunks`
(below); anything else (a cursor, a type with its
own `len` and `split`) is refused with the reason (`ErrorParSource`), as is a
module that does not import `actors`, whose `parallelEach` runs the loop, and a
generator (`ErrorParRuntime`). Written directly in an actor's behaviour (and not in
another parallel each's body: `parInBody`, per function), the loop is followed by
a statement cutting the behaviour there, an `AwaitNode` with `par` set
(`awaitParNew`), and the body may read no borrow held in a variable but `self`, nor
the loop walk an array lying in the frame (`ErrorParFrame`, `parCheckFrame`,
`parMayBeFrame`); the parser notes a method holding a `parallel each` as holding an
`await`, so its actor has a pending table. A synchronous method of an actor runs
the loop where it stands. The control rules (`break`, `return`, `await`,
a `continue` of an outer loop: `ErrorParControl`) are checked on the body as
written, before it is built, by a walk (`parWalk`). The loop built is `each`'s counted loop over an index
range, the statements

```
[source held and lent as a slice, or the range's bounds]
imm lo' = 0;  imm hi' = s.len;  mut k' = lo';
loop { if k' >= hi' {break}; imm x = &s[k'] (or first + T.from(k')); k'++; ...body }
```

whose hidden variables carry names no source can spell, so generation finds the
parts by name wherever `blockHoist` puts a temporary. `k'` and the loop are the
*piece* generation outlines ([Generation](../phases/generation.md), "A parallel
each"); `lo'` and `hi'` before them are the range of passes, a range's count
worked out there. Once the body is checked, `parallelEachCheckBody` refuses a
write to anything declared outside the loop (`ErrorParWrite`): an assignment, a
swap, or a borrow that may write, whose place is rooted in a variable that is not
the loop's own or the body's, through fields, elements and references, but not
through a raw pointer (the loop trusts its writer). The loop is the block's last
statement, so type check has made it the value of a `blockret` (`parLoopOf`
looks through it).

It also refuses a **copy** that writes shared state without atomics
(`ErrorParCopy`, `parCheckCopies`): a place read as a value (not lent, not the
object of a further access) whose type holds a counted owner whose region is not
`ThreadSafe` (`Rc`) or a traced reference (`parHoldsPlainCount`, through fields,
variants, tuples and arrays; an `Arc` and a move owner do not count) and that is
not the pass's own (`parPlaceIsOwn`: a variable declared in the loop, or a part
of one held inline, not reached through a reference or an index). A copy inside
a function the body calls is not seen, which is why the body may not hand it
anything that could be copied:

A body may not **reach** a value that is not safe to share (`ErrorParReach`,
`parCheckReach`, over the loop: its source's item variable, filter, body and
`yield`). The property is the built-in marker `Shareable`, the sibling of
`Sendable` and Rust's `Sync` (`itypeNotShareableWhy`, [references](references.md)):
a type is not Shareable when a field, variant, element, borrow's or
owner's pointee reaches a reference `refCountsPlain` says counts without
atomics (an aliasable owner of a region not declaring `ThreadSafe`, or a traced
region) or `refWritesShared` says writes through a path others share (`&mut`,
`Arc[mut, T]`), by a region's declarations and never a name. Any name use of a
variable declared outside the loop, a global or a parameter of that type is
refused, except the places `parCheckCopies` already refused (no double report)
and the lowering's hidden variables; a variable the loop declares is its own,
except the item, a borrow into the source (`nodesGet(loop->stmts, 1)`), which is
judged without the `&mut` rule (no other pass reaches it). A name that is itself a
borrow (`self`, a `&mut` parameter) is read through as a `&`: what it reaches is
judged, not its own permission, since `parCheckWrites` refuses the body's writes
through it (a virtual reference is not peeled: unmarked, it is the culprit itself,
`+ Shareable` or `+ Sendable` vouches). A raw
pointer of a type that is not generic is trusted and nothing behind it is
followed; an instance of a generic type is not safe when any type argument is not
(`itypeInstanceTypeArgs`), so `List[Rc[...]]`, which keeps its
block behind a pointer, is found by its argument. The message differs for a root
that is a counted or traced owner itself (`parIsCountedItself`: borrow its contents
before the loop), one that holds it (copy what the loop needs into a local), one
that holds a `&mut`, one that is or holds an unmarked virtual reference, and a closure passed in, where it names the variable the
closure borrows or holds (`genericClosureNotShareableCap`) and what to list.

A loop that **writes through its source** also refuses an outside variable whose
type could point at a written item (`ErrorParAlias`, `parCheckOuterAlias`, run where
the `parCheckAlias` of the place the items are lent from is). What is written is read
from the source's cursor types (`parWrittenElems`: the one type argument of core's
`ArrayMutChunks`, `ArrayMutItems`, `MutItemsIndexed`, through `MutChunksIndexed`, the
zips and their `indexed()`, or a `&mut` slice itself), never a method's name. The set
is the element and what it holds inline (`parInlineTypes`). The check is **by the path
read**: `parCheckOuterAlias` takes the outermost place node (field, index, dereference,
borrow of one; `parPlaceChain`) rooted at an outside variable and `parPlaceReaches`
walks it from the root. A reference crossed (an object whose type is a reference, auto
dereferenced or not) makes the rest *behind*, and refuses if what it points at (a struct,
the item) is a type of the set; an index refuses when it is behind and the element's type
is in the set; a raw pointer ends the walk, trusted. What the place finally holds is
judged whole (`parReachesWritten`): a type of the set as the pointee of a reference or an
element or type argument of a collection reached behind one (`elemlike`), searching fields
and variants for more, and not counting a type argument the struct also holds by value
(an `Atomic`'s). A scalar field read (`self.scale`) is neither a container nor an
element, so is free. A bare name, or a variable named inside a call in the chain, is a path
of one, judged whole. A place `parCheckAlias` refused, or any part of one, is skipped (no
double report).

When the variable is a closure passed to a generic that runs the loop (its type, or the
pointee, is a hidden struct `closureOfStruct` knows), the message names the closure's
borrowed capture that reaches a written type (`ClosureCap`, `!state`) and the fix: a value
copies in by the state list (`[scale]`); a captured reference or collection that could be the
buffer written cannot be copied that way, so it is told to be handed as a value made before the
loop. The closure's state entries are values of its own and reach nothing; a hand-written
struct with a value field is the same.

A **traced reference made** in the loop is refused whole (`ErrorGcStopgap`,
`gcRefuseVisit`, over the block with the loop's source, filter and `yield`): an
allocation, a call or a type literal whose type `itypeHoldsTraced` says holds one
(a traced reference, or a tuple, array, struct or enum holding one inline; it
does not look behind a borrow, another owner or a pointer). A copy of a place is
not a new reference, so one from outside is only `ErrorParCopy` and one the pass
made is refused where it was made. A stopgap until the collector is per actor.

A mutable slice as the source (what `mutItems()` gives, or a `&mut` slice) lends
each item mutably, as `eachLower` does, so a pass changes its own item through its
variable, and the items are disjoint. `&mut` is shared in Cone, so nothing else
stops the body reading the same list under its own name while another pass changes
an item: the hidden slice is then named `sm'` (not `s'`), and `parallelEachCheckBody`
refuses a body that names the place the slice was lent from (`parPathOf`: the
variable and the fields named from it, through references and a method's receiver,
and any place that is that place, or inside it, or holds it) as well
(`ErrorParWrite`).

**Cursors in a `parallel each`** (`parCursorOf`). A source of core's cursor types
(`ArrayChunks`, `ArrayMutChunks`, `ArrayMutItems`, and the `ArrayIndexed`,
`ChunksIndexed`, `MutChunksIndexed`, `MutItemsIndexed` their `indexed()` and an
array's give) is not cut into index ranges of elements: the cursor is held in the
hidden variable (named `s'`, or `sm'` for the ones that lend `&mut`, so that the
alias check above applies), the range is `[0, c.len())` and the item `c.at(k')`
(for a chunk a slice of its run that no other pass reaches). The compiler never
recognises a method by name: what is lent, `&` or `&mut`, is what the cursor's own
types say, so a bare `indexed()` lends `&` on any slice and `mutItems().indexed()`
`&mut`. An indexed cursor takes two variables (any other source one,
`ErrorParSource` otherwise) and its `at(k)` gives the position and the item as a
tuple, which the loop unpacks: `imm pair' = c.at(k'); imm i = pair'.0; imm x =
pair'.1; k'++`. The position is the cursor's own count plus `k'`, so it is global
whichever piece runs it. A write through the pass's own run or item is its own;
naming the buffer the cursor comes from is refused as for `mutItems()`. A cursor
kept in a variable is held by copy and walked the same way; its items count from
the first it has yet to give. A header `if` needs nothing of its own: it is
`eachLower`'s `continue` statement after the pass's variable, ahead of which the
loop's step is inserted.

A **zip** of cursors (core's `Zip` and, from its `indexed()`, `ZipIndexed`; with a
third source from `Zip`'s own `zip`, the flat-triple `Zip3` and its `Zip3Indexed`) is
one of these cursors: it takes two variables (three for `ZipIndexed` and `Zip3`, four
for `Zip3Indexed`, the position first), unpacked from the tuple its `at` gives. The
three-way zip is a struct of its own, not a `Zip` of a `Zip`, so that its item is
one flat tuple that three variables unpack; its `len` is the shortest's and its
`split` cuts all three at one place. It has `len` (the shorter's), `at`
and `split` only where both sources do (`where I is RandomAccess[A] and J is
RandomAccess[B]`; the core traits `RandomAccess` and `ParallelIterable`, met by
the methods), so a zip with a source that cannot be cut or walked by position has
no `len` or `at`, and `parallelEachLower` refuses it with that reason
(`ErrorParSource`). The zip lends `&mut` if either source does (`parCursorOf` reads
its field types, never a method's name), and the checks that look at where the items
are lent from look at every source: for a call, the alias check takes the path of
each argument that holds a borrow (`parLentPaths`, `itypeCarriesBorrow`, never a
function's name), and the behaviour's frame check refuses a local in any
(`parMayBeFrame`).

**The parallel builder**, `xs <- parallel each x in src [if c] yield v`, is an
each entry of `<-` (`parseParallelEachEntry` sets the flag `parseEachLoop` reads
for a statement) whose loop ends in the `YieldEntryTag` entry; `contentsEach`
gives that yield the receiver (`-recv`) as for any each entry. `parallelEachLower`
finds it (`parBuilderYield`: a yield with a receiver among the loop's own
statements, so one in a nested loop is that loop's) and, after checking that the
receiver's type declares `pieceBag`, `emptyPiece`, `depositPiece` and `joinPieces`
(a `List` does: anything else is `ErrorParBuilder`, which is also how a dictionary
is refused) and that the function is not an actor's method (`ErrorParRuntime`),
builds the statements around the loop (`parBuilderLower`):

```
mut bag' = (*recv).pieceBag();                       // before k': the caller's
mut k' = lo';                                        // from here, the piece's
mut list' = (*recv).emptyPiece();                    //   the piece's own list, a local
imm piece' = &mut list';                             //   what the yield appends through
loop { if k' >= hi' { (*recv).depositPiece(&mut bag', lo', list'); break }; ... *piece' <- v }
(*recv).joinPieces(&mut bag')                        // after the loop: the caller's
```

The yield's receiver is switched to `piece'` before the loop is checked, so
`yieldEntryLower` appends to the piece's list. The list is a local of the piece, not
a borrow got from a call, because the shape-changing check refuses an append through
a reference it cannot tell from the reference the source is read through (a
parameter's list) while that borrow lives; a local and a borrow of it, as a plain
each entry's receiver is, it can. The piece gives the list up by moving it into
`depositPiece` at the loop's one exit, so the caller's block end, which would
otherwise release a variable that lives in another function, has nothing to
release (flow sees it moved). The bag is a stack pushed with an atomic
compare-and-swap; `joinPieces` sorts it by where each piece began (`lo'`),
moves each list's values onto the end of the receiver and frees the nodes. The
bag, the list and the borrow carry names no source can spell (`bag'`, `list'`,
`piece'`). The walk of the control rules (`parWalk`) reads a yield's value and
key, so a `break`, `return` or `await` in them is refused as in a body.

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
