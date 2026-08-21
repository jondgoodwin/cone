
- For allocation, use `new` as an alternative to `+` for region-based references?
- For region reference type notation, what else besides `+`? An aliasable named-generic?
## So and Rc
- [[Managed Reference Metadata Access Prototype]]

### dealias & drop, final, free

- Convert +rc/so free to use drop mechanism
- Cast node logic change for region/perm static displacement
- Region-owned ref free logic
- Dealias
	- Region ref dealias logic w/ free return bool (and strip out old logic)
	- Region alias logic
	- Conditional and loop/break handling for move/dealias

See [[Init and Final]]

Allocation
- Region alloc or init failure can either panic or produce Option value 

### Fallible allocation: `?+rc-mut value`

**Allocation always yields `Option[ref]`.** The primitive can fail, so that is its
honest return type. `?+rc-mut value` is the form that hands the caller that Option
to test. Plain `+rc-mut value` is sugar over it — the equivalent of an
`Option.memallocpanic` — which unwraps and panics when the allocation returned
None.

So the syntax is deliberately the inverse of the semantics: the shorter spelling
is the *more derived* operation. Panicking is overwhelmingly the common case and
should not be the verbose one, while a program willing to handle allocation
failure opts in with `?`.

Code generation already works this way and needs no change. `genlallocref` emits
the null check as a branch to a panic block and calls `genlPanic` there only when
`FlagQues` is absent. `Option[&T]` needs no tag or wrapper, because `genltype.c`
collapses a two-variant union whose other variant holds a single pointer into a
bare pointer — so "a null or a real reference" is the literal representation, and
`genlexpr` specialises both testing it and constructing it.

**What is broken is the lowering, and it is built the wrong way round**: plain
allocation is the primitive and `?` is a modifier bolted onto it, the opposite of
the model above. Today `?+rc-mut v` dies on an access violation and `?+so v`
hangs. Specifically:

- The allocate node's `vtype` stays an un-instantiated `Option[...]` **expression**
  (`FnCallTag`) sitting in a type slot, where the non-`?` form has a real `RefTag`.
- `itypeIsGenericType` — the only thing that can make `isTypeNode()` true for a
  generic instantiation — tests for `GenericNameTag`, a tag **nothing in the
  compiler ever assigns**. A written-out `Option[T]` works only because the type
  parser instantiates it before anything asks.
- Name resolution builds a cycle: the allocate node's `vtype` is the Option call
  whose `args[0]` *is* that allocate node, patched mid-type-check to a `RefNode`
  whose lex parent is the same allocate node. Instantiating a generic across that
  recurses, which is why one form faults and the other hangs.

Substituting `itypeTypeCheck` for `inodeTypeCheckAny` gets the `vtype` to a type
node and still fails, so this is not a repair to the existing lowering. Building it
the way round described above is the fix.

The test suite cannot hold this defect in any form: `?+so` hangs, and a scenario
whose assertion is "this takes twenty seconds" asserts nothing. It was found by
[[Add test suite]] and triaged in [[Ownership memory safety]], which carries the
rest of that survey. It was also listed in [[Diagnose instead of crash]], which
has since been completed and removed it: the crash half is not an error path that
left a NULL behind, so nothing that item built reaches it, and this is the only
place it lives now.

Alloc and init for array references
- Function-based initialization

## An array holding owning references is never released

Measured by [[Compiler defect backlog]], which found it while tracing something
else and routed it here because it is de-aliasing work rather than a defect with
a local fix.

`flowScopeDealias` (`ir/flow.c:230-260`) builds a scope's dealias list from
variables whose **own** type is an owning reference — it tests
`reftype->tag == RefTag`. An array's type is `ArrayTag`, so it falls to the
`else` branch, which asks `itypeGetDropFnDcl`; that returns NULL for anything but
a `StructTag`. So **every element of an array of owning references leaks**,
however the array was built. `region-fill-count` records it in passing, because
counting *n* holders for a fill is what makes the outcome a leak rather than a
use after free.

Why it is not a one-line repair like its siblings: closing it needs flow analysis
to see *through* an aggregate to the owning references inside it, and code
generation to emit a loop releasing each element. Flow analysis tracks whole
values only, never a field or an element — established by [[Add test suite]] —
so this is the first thing that would need element granularity, and a struct
holding an array of owning references wants the same treatment. That is the same
boundary the drop/dealias design has to settle anyway, which is why it belongs
here rather than being patched at the one site that noticed.

**Weak refs**, existence check and de-ref
- Existence check for “weak” pointers? 
- Effect on de-ref of weak refs?
- Move-semantics-based Rc region:  how to handle .clone
- Restrict +so (move regions) permissions to imm/mut?    

## Arenas (and final)
## Pools
[pool](https://llvm.org/pubs/2005-05-21-PLDI-PoolAlloc.pdf)

## Tracing GC


- [Dart and LLVM-safepoint](https://medium.com/dartlang/dart-on-llvm-b82e83f99a70)

## Two dealias holes, moved to [[Bugs]]

`continue` releases no owning reference at all, and `break`/`continue` release
only their innermost scope rather than every scope up to the block they target.
Both measured; both now in [[Bugs]] with their repros.

They are recorded there rather than here because neither needs a decision — but
they are the same shape as the `VarMoved` stopgap above: a release list built
from a running summary rather than from the path actually taken. **A redesign
that makes dealiasing path-aware should close all three at once**, and if that
redesign is imminent it is worth doing instead of the two point fixes.
