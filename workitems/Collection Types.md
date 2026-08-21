
- Types: Array, Queues/Rings/List, Dictionary, Graph
- Traits: Iterable
- Iterators, slices, parallelism
- Implicitly region-based and region-like

## List
- Add simple generic type bounds
- Based on &so array reference
- Vec literal
- <- append operator
- Interpolated string handling. <- [] handling

## Slice / Array ref

Implement array ref (slices) with using a generic and made-safe intrinsics/
The collection can return a slice of its own type, as appropriate
strz is an immutable only "ptr" type
- &[] for array ref constructor (vs. & on an array) - keep improving on it: &slice?
- .len and maxlen for arrref and array
- ==, != comparison of arrref
- Size of types and values
- My keyword
- Equality of array and arrayderef
- Test it: &xx[] where xx is a struct should be (&xx).’&[]`[]
- &[]

- Array ref vs. de-reffed array ref
- Typecheck: Use flags in fncall for &[] and to ensure ref is produced

- [] slice content get/set, bounds, struct
- &[] slice creation, also with struct
- Slice range operators .. … by
- Slices indices relative to end 
- Auto-coerce allocated ref->borrow using borrow node, needed so flow pass does not think this is an aliasing event for &own/&rc

  
- ‘Each’ iteration over arrref, array, struct
- Comparison of arrays and de-ref-ed array references (including elements with <)
- Bubble sort example with ‘uni’ list
- Creation of slice from ref or from ref+len
- Array references and allocators! Aliasing, allocation, free, etc.
- allocation

- Array ref
- [len] value
- [len] initializer
- [len] closure?

- Resize / Append    
- search ?  & other things D does?

