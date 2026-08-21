
### Guarantee valid Values before use

Variable initialization and use checks

- Create rval logic
    
- Variable unused flag and warning at block-end
    
- Ensure uninitialized variables don’t de-alias their lval
    

## Does a partial write read what it writes into?

Flow now reads the parts of an assignment's target that are values in their own
right — the index in `a[i] = 5` and the reference in `*p = 5`, which used to be
read on the right of an assignment and not on the left. `assignFlowLvalReads`
is the walk; [[Bugs]] has the measurement, including that `*p = 5` on an
uninitialized reference emitted the store.

**What that walk deliberately does not do is read the base of a partial
write.** `a[i] = 5` does not read `a`, and `s.f = 5` does not read `s`. Whether
it should is a real question and this item is where it belongs, because the
answer interacts with what flow tracks: state lives on the whole variable, not
per element or per field, so "reads `a`" and "writes `a`" are the same
granularity and a read-modify-write cannot be expressed as two facts about
different parts. Today writing one element of an array that holds nothing
raises nothing, and a later read of the array still reports it uninitialized —
which is consistent, but is consistency by omission rather than by decision.

`array-flow-index` asserts the boundary as a non-diagnostic, so whichever way
this is decided, the scenario has to change with it.
