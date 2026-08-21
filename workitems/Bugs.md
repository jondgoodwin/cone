Defects with a known fix, needing no design decision and no major refactor.

**What belongs here.** All three must hold:

1. **The correct behavior is already clear.** Nobody has to decide what the
   language should do.
2. **The fix is local** — one function, or a few. No new IR node, no phase
   reordering, no subsystem refactor.
3. **It is a defect, not a missing feature.** "Crashes", "gives the wrong
   answer", "is spelled wrong" — not "is not built yet".

**What does not.** Anything whose fix waits on a language decision, anything
needing a refactor to fix properly, and unimplemented features. Where an item is
*part* bug and part open question, only the bug half lives here and the entry
says which work item holds the rest.

**Each entry carries what is needed to act**: how to see it, where it is, what
the fix is, and which test group the scenario lands in. An entry that cannot say
those things is not ready to be here.

Every entry marked **measured** was reproduced against a release `conec` at
`3daeed7`. Entries under "Unconfirmed" were read from source and never run —
confirm before acting.

---

# Crashes and hangs

## The infection loop in `structTypeCheck` never terminates

**Measured.** The compiler hangs — no output, no diagnostic.

```cone
union Holder {
  struct Full { r +so i32 }
  struct Empty { z i32 }
}
```

Exit 124 under a 12-second timeout. Delete the `+so` and it compiles in ~12 ms.
An `extends` struct with a `final` method is the other way in.

`ir/types/struct.c`:

```c
StructNode *trait = (StructNode *)node->basetrait;
while (trait) {
    trait->flags |= infectFlag;
    trait = (StructNode *)node->basetrait;    // re-reads node, never advances
}
```

**Any type with a base trait that acquires `MoveType` or `ThreadBound` hangs.** A
union variant always has a base trait and an owning reference always infects
`MoveType`, so a tagged union owning anything is enough.

**Second bug, same line:** `basetrait` is a `NameUseNode`, not a `StructNode` —
`structGetBaseTrait` exists to unwrap it. The flags are being OR'd into the name
use node's flag word. Fix both: advance the cursor, and unwrap each hop.

Scenarios in `union` and `struct`. Because the failure is a hang rather than a
diagnostic, the runner's timeout is what has to catch a regression.

## A leaked generic parameter aborts the compiler

**Measured.**

```cone
fn one[T](a T) T { a }
fn two[U](b U) U {
  imm z T = b               // T leaked from 'one'
  b
}
fn main() i32 { two[i32](1) }
```

→ `Internal error: cloning is not implemented for a node of tag 57345`, no
source location. 57345 is `GenVarDclTag`.

Cause: `fnDclNameRes`, `structNameRes` and `macroNameRes` all resolve their
parameter list *before* `nametblHookPush()`, and `gVarDclNameRes` hooks
unconditionally — so the parameter binds in the enclosing module's table and is
never popped. The post-push re-hook then saves the already-leaked value as its
"previous", so the matching pop restores the leak instead of removing it.

Two more symptoms of the same cause:

- **A valid program is rejected.** The leak shadows a real module-level type of
  the same name, forward of the generic only.
- **An invalid one is silently accepted.** A template is never type checked, so
  a function naming a leaked parameter compiles clean until something
  instantiates it.

**Fix:** move the pre-push `inodeNameRes` of `parms` inside the push; the
existing post-push `nametblHookNode` loop then becomes redundant and should go.
`parseGenericParms` parses no bounds and no defaults, so nothing a parameter's
resolution needs can come from an earlier one.

Scenarios in `generic`, covering all three symptoms. The abort case cannot be
asserted by a diagnostic expectation — the process dies.

## `structMakeVtable` walks a NULL list for a non-trait

**Measured.** SIGSEGV.

```cone
struct Sq { mut w i32 }
fn takes(s &<Sq) i32 {0}
fn main() i32 {
  mut q = Sq[3]
  imm r = &q
  takes(r)        // SIGSEGV
}
```

`refvirtTypeCheck` calls `structMakeVtable` once it knows the value type is a
`StructTag`, never checking `TraitType`; `structMakeVtable` ends by walking
`derived`, which only a trait allocates. Writing `&<q` directly is caught
cleanly by `refNameRes` — only the coercion path crashes.

**Fix (the bug half):** `structMakeVtable` must not walk a NULL list, and
`refvirtTypeCheck` should require `TraitType` and report if not.

**Design half stays in [[Types. Struct and Union]]:** whether `&<Struct` on a
plain struct should be a diagnostic or should work.

---

# Memory safety

## Returning owning references in a tuple frees them before the return

**Measured.** A use-after-free followed by a double free.

```cone
fn pair() +rc-mut i32, +rc-mut i32 {
  imm a = +rc-mut 1
  imm b = +rc-mut 2
  a, b
}
```

Compiles clean. `--llvmir` shows two `malloc`s, **four `-1` decrements and no
increments**. In `pair`: build the tuple, decrement `b` to zero and free it,
decrement `a` to zero and free it, return two dangling pointers. The caller then
decrements both again.

**Fix (the bug half):** `flowScopeDealias`'s "do not release what is being
returned" test is
`retexp->tag != VarNameUseTag || namesym != avar->node->namesym`, so a
`VTupleTag` return matches nothing and every element is released.
`returnFlowEscape` already walks a returned tuple element by element for the
borrow check; this needs the same.

**Design half stays in [[Copy & Alias vs. Move Semantics]]:** `assignMultRetFlow`
does no move-or-copy accounting either, and cannot simply call
`flowHandleMoveOrCopy` — it iterates return *types*, so there is no per-element
value node to wrap. Where a destructuring's alias adjustment attaches is a
design question.

## `continue` releases no owning reference

**Measured.** `break` emits the decrement-and-free; `continue` emits nothing.

```cone
fn leaky() {
  while true {
    if true {
      imm shared = +rc-mut 3
      continue
    }
  }
}
```

`blockFlow` passes `retexp = NULL` for a `ContinueTag`, and inside
`flowScopeDealias` that same parameter gates the `so`/`rc` arm — so with NULL
every owning reference falls to the `else` and is dropped from the list. The
drop-fn arm is unaffected, so a struct's `drop` still fires.

**Fix:** separate the two jobs `retexp` is doing. NULL should mean "nothing is
being returned, so cancel nothing" — not "add nothing".

Scenario in `region`, asserting the counter adjustment in the generated IR, in
the style of `region-fill-count`.

## `break` and `continue` release only their innermost scope

**Measured alongside the above.** Only `ReturnTag` passes `startpos = 0`;
`BreakTag`, `ContinueTag` and `BlockRetTag` all pass the mark for the block
being left. A `break` targeting an outer loop from inside nested blocks releases
only the innermost one; every scope in between leaks.

**Fix:** use the *target* block's stack mark rather than the current block's.
`BreakRetNode.block` already names the target, so the mark can be recorded when
flow enters a block. Small plumbing, no design decision — what should happen is
not in doubt.

---

# Wrong output

## `genlBlock` emits an invalid phi

**Measured.** Emitted silently; only `--verify` catches it.

```cone
imm v = while { if n > 1 { break } else { break } }
```

→ `%phival = phi %void` with **no incoming entries**, and `--verify` reports
"PHINode should have one entry for each predecessor of its parent basic block!"

The phi arrays are allocated under `blk->vtype->tag != VoidTag` but the phi is
*built* under `!= UnknownTag`. On the `VoidTag` path `phiCnt` is an
uninitialized arena read.

**Fix:** make the two conditions the same one.

## A `ro` field is writable

**Measured.** `struct C { ro x i32 }` then `c.x = 9` compiles clean.

The only reader of `FieldDclNode.perm` is `iexpGetLvalInfo`'s `FldAccessTag`
arm, which downgrades when `flddcl->perm == (INode*)immPerm` — a **pointer
identity** test against the raw singleton. A written permission is a
`NameUseNode` wrapper built by `newPermUseNode`, so it never matches. Only
compiler-synthesized fields can reach that branch.

**Fix:** compare through `permGetFlags` or `permIsSame`, as the adjacent code
already does.

**Design half stays in [[Permissions]]:** what a field's permission should
*mean*.

## `parseFieldDcl` rejects the wrong permission

**Measured.** `struct A { imm x i32 }` is `Error 1013`; `uni` and `opaq` fields
compile clean.

```c
if (permdcl != (INode*)mutPerm && permdcl == (INode*)immPerm)
```

`immPerm` is never `mutPerm`, so this reduces to `== immPerm` — it rejects
exactly `imm` and admits everything else. Almost certainly `!=` was meant in the
second clause. `parseFieldDcl` also takes a `defperm` argument that its body
never reads.

**Fix waits on [[Permissions]]** to say which permissions are legal on a field —
flipping the condition inverts which one is rejected, so the mechanical fix
needs that answer first. Listed here because the *defect* is not in doubt.

## A struct literal's fields are matched exactly, so a variant literal is refused

**Measured.** *(from [[Type Inference and Coercion]])*

```cone
union Shape {
  struct Circle { r i32 }
  struct Rect { w i32 }
}
struct Holder { s Shape }

imm ok Shape = Circle[3]        // accepted
imm h = Holder[Circle[4]]       // Error 1046: Literal value's type does not
                                // match expected field's type
```

The identical coercion is accepted in a variable initializer and refused in a
struct literal's field, so **the compiler's own inconsistency settles what the
right answer is** — nothing about inference has to be decided first.

Cause: `typeLitStructCheck`'s positional pass asserts `iexpSameType(field, argval)`
— exact equality, no coercion — where a variable initializer goes through
`iexpTypeCheckCoerce`.

**Fix:** coerce instead of comparing. Scenario in `union` or `struct`.

*Related but not claimed here:* array literal elements are matched the same way,
with `itypeIsSame` against the first element and no supertype search. Whether
that is the same defect or a deliberate rule has not been established.

## `.len` on a fixed-size array is rejected

**Measured.** *(from [[Constant literals]], where it was the file's one entry
labelled "Bug")*

```cone
imm bigarray = [1,2,3]
imm length = bigarray.len       // Error 1025: Invalid operation on an array
```

The published array documentation says this works, so the correct behavior is
not in doubt.

**Sizing, honestly:** this is a small addition rather than a repair.
`len`/`maxlen` are registered as `CountIntrinsic` only on the array *reference*
type (`corenumber.c`), and generation extracts field 1 of the slice fat pointer.
`ArrayTag` is in the type group with neither `NamedNode` nor `MethodType`, so a
fixed-size array has no namespace to hold a method at all, and
`fnCallTypeCheck`'s `ArrayTag` arm handles only `FlagIndex`. The length is a
compile-time `ULitTag` in `dimens`, so it can fold to a constant.

**Do not work this twice:** [[Collection Types]] carries
"`.len` and `maxlen` for arrref and array" as feature work — the same gap from
the other side.

---

# Unconfirmed

Read from source, never run. Confirm before acting; each names what would settle
it.

## `iexpGetPermFlags` falls through a disabled assert

*(from [[Permissions]], where it was parked because that item wants the
function)*

Its `DerefTag` arm sets the permission for `RefTag` and `PtrTag` only, then ends
in `assert(0 && "Should be ref or ptr")`. The Release build defines `NDEBUG`, so
that assert is nothing and control **falls through into `case ArrIndexTag:`**,
which reinterprets the `StarNode` as a `FnCallNode`. A slice or virtual-reference
deref takes that path.

**Fix:** add the `ArrayRefTag` and `VirtRefTag` arms, and do not trust the assert
to have been guarding anything.

**Unobservable today** — nothing but the function itself calls it, so there is no
scenario to write until [[Permissions]] picks it up. Fix it anyway, before that
happens.

## Two UTF-8 diagnostic defects

*(from [[Lexer and Parser]])*

- A bad token does not correctly print a bad UTF-8 character code.
- The error-message line pointer does not correctly handle multi-byte
  characters — the caret lands in the wrong column.

Both are local to diagnostic printing in `shared/error.c` and the lexer's bad-token
path, and neither needs a language decision. One line each in the source note, so
scope is assumed rather than shown — reproduce before sizing.

## Paren counting looks unbalanced

*(from [[Lexer and Parser]])*

`lexIncrParens` is called from three sites — `parseTerm`'s `(`, `parseArgs`, and
`parseTypeName`'s `[` — and `lexDecrParens` from one, `parseCloseTok`, which also
serves `parseArrayLit`, `parseFnSig` and `parseStruct`'s generic list, none of
which increment. `lexDecrParens` guards underflow, so the visible effect would be
`paranscnt` sitting at zero when it should not, letting `lexIsStmtBreak` fire
inside a multi-line construct.

*Settle it:* parse a `fn` signature and an array literal each broken across
lines.

## `parseAdd` guards `-` against a statement break but not `+`

*(from [[Lexer and Parser]])*

`+` is also a prefix operator — a region-managed reference — so a line beginning
`+rc T` after a complete statement may be absorbed as a binary addition.

*Settle it:* write that source and read the `--ir` dump.

## A stray `}` at global scope drives the block stack negative

*(from [[Lexer and Parser]])*

`parseBlockEnd` at level 0 consumes the `}` and calls `lexBlockEnd`, which
decrements past zero. The immediate path appears not to read `blkStack[-1]`, but
the `include` path — which returns into `parseInclude` and continues in the outer
file with a different `lex` — was not traced.

*Settle it:* compile a source with an unmatched `}` at global scope under a
sanitizer.

## `genlAddr`'s `FnDclTag` case may be dead, and is wrong if it is not

*(from [[LLVM Generation]])*

It calls `genlGloFnName` (idempotent) *and* `genlFn` (not idempotent — it would
emit a second entry block into an already-generated function). Anonymous
functions are lifted to module scope and reached through a `VarNameUseTag`, so no
reach was found.

## `refHash` and `arrayRefHash` hash the wrong field

Both end with `itypeHash(node->vtype)`. On a reference *type* node `vtype` is
permanently `unknownType` — the pointed-at type is in `vtexp`. So the hash
collapses to a function of tag and region alone, and every `&i32`, `&Point` and
`&mut [3; u8]` lands in one bucket. Correctness survives via linear probing;
the type table degenerates toward a linear scan. Should be `node->vtexp`.

*Settle the cost:* a probe counter in `typetblFindSlot`, over the corpus.

## `arrayRefTypeCheck` never calls `refAdoptInfections`

The four call sites are all in `reference.c`. A slice type written in source
acquires neither `MoveType` nor `ThreadBound`, while the identical type built by
`allocateTypeCheck` does — two spellings of one type disagreeing about move
semantics.

*Settle it:* `imm s +so[]i32 = +so[] [3; 1i32]` with and without the annotation,
reading the source twice in each.

## `ThreadBound` infection is unreachable

`refAdoptInfections` tests `refnode->perm == (INode*)mutPerm` by pointer while
the adjacent `MoveType` test goes through `permGetFlags`, which unwraps the
`NameUseNode`. So `MoveType` works and `ThreadBound` silently does not, in
adjacent lines. Invisible today because nothing consumes the flag except further
infection propagation — it becomes real when [[Concurrency Threads]] lands.

## `newVarDclFull` leaves `genname` uninitialized

Every other field is set; `genname` is not, and the arena never zeroes. Latent
only because the mangling and global-emission paths are reached solely from
nodes built by `newVarDclNode`.

## `genlAddr` and `genlExpr` test different tags for a tuple element

`genlAddr` tests `methfld->tag == UintNbrTag`, a *type* tag; `genlExpr` tests
`ULitTag`, an expression tag. They cannot both be right, and `genlAddr`'s looks
wrong — which would send `&tuple.0` into its `assert(0)`.

*Settle it:* compile a source that borrows a tuple element by index.

## `itypeMangle`'s ternary is always true

`vtype->tag == VirtRefTag ? '<' : ArrayRefTag ? '+' : '&'` — `ArrayRefTag` is a
nonzero constant, so a plain `RefTag` mangles as `'+'`, identically to
`ArrayRefTag`, and `'&'` is unreachable. Visible in an emitted symbol:
`@"Meter_reading:+ro Gauge"` for `fn reading(self &)`. One character.

## `structTypeCheck`'s early returns leak state

The base-trait failure and the two mixin failures return without restoring
`pstate->typenode`, and the mixin ones also skip `clonePopState` — leaving a
hook-table level pushed. Only reachable after a diagnostic has fired, but it
corrupts everything checked afterwards.

## `structAddField` drops a duplicate field after indices are assigned

On a duplicate name it reports and returns without adding to `fields`, while the
parser has already assigned `index` over the original numbering — so positional
literals for that type shift by one.

## `genlConvert` can build a virtual reference from an uninitialized vtable

Converting a reference to a virtual reference linearly scans `vtable->impl` for
the matching struct; on a miss there is no assert and no default, so the fat
pointer is built from garbage. The trait/tag branch has the same shape when no
`IsTagField` is found. Misbehaves silently in both build configurations.

## `genlConvert` duplicates its struct conversion

Once in `case StructTag:` using raw `LLVMBuildAlloca` (mid-block) and once in
`default:` using `genlAlloca` (entry block). The mid-block alloca may not be
promoted by mem2reg.

## `cloneConstDclNode` is dead and would not work if reached

`cloneNode` has no `ConstDclTag` arm, so reaching it hits the `default:` that
calls `errorExit`. It is also the one clone function that does not clear
`TypeChecked | TypeChecking`.

## Two dead parameters in the parser

`parsePrefix`'s `noSuffix` and `parseArrayLit`'s `typenode` are passed the same
value at every call site. `noSuffix` looks like a leftover of the
borrow-precedence change `parseAmper` documents.

## `--ir` writes to `init.ast`

Not `<srcname>.ast` — the name comes from the corelib pseudo-source, so every
compile overwrites the same file regardless of what was compiled.

## Dead and write-only generation state

`gen->block` is set to NULL in `genSetup` and never read or written again.
`gen->compileUnit` is write-only. The debug file name is hardcoded to
`"main.cone"` rather than the real source path.

## `flowLoadValue` may be missing on array indexes in assignment

*(from [[Collection Types]], where it read
"assignLvalInfo needs to invoke flowLoadValue on array indexes?")*

Recorded with a question mark by its author, and **the function it names no
longer exists** — there is no `assignLvalInfo` in the tree. The lval-read logic
now lives in `flowIsLvalRead`, and assignment's flow pass in `assignFlow` /
`assignlvalrtype`. Someone has to work out what the entry now refers to before
it can be sized at all. Kept because a missing `flowLoadValue` on an index
expression would be a real hole, and losing the observation is worse than
carrying a stale one.

## `ErrorManyArgs` covers three unrelated conditions

In `genericMemoize` and the two macro type checks it serves wrong arity, a
non-type argument, and "expects arguments to be provided" — against the project
rule that each diagnostic gets its own `ErrorCode`, and the test file has to
disambiguate them by message substring.
