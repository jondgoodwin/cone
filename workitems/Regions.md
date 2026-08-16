
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
whose assertion is "this takes twenty seconds" asserts nothing. The crash half is
cross-listed to [[Diagnose instead of crash]]; it was found by [[Add test suite]]
and triaged in [[Ownership memory safety]], which carries the rest of that survey.

Alloc and init for array references
- Function-based initialization

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
