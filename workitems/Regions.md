
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
