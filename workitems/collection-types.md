
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
- .len and maxlen for arrref and array. **Three features, one design**, and the
  manual already says so: `coneref/refarray.html` defines `$-` as sugar for
  `my.len -` and writes its iteration examples in terms of `.len`, and its own
  opening note lists `.len`, multi-element segments, `each` iteration,
  comparison and pattern matching together as what is not implemented.
  Measured today: `.len`, `.maxlen`, `.len()` and `&a.len` on a fixed array all
  give one clean `ErrorNoMeth` "Invalid operation on an array." with a correct
  exit code — no crash, nothing silently wrong — and `.len` on a slice, or on an
  array auto-borrowed to a slice parameter, works. `$-` has no lexer token at
  all; `each` over a collection is rejected at parse, which `test/cases/each`
  records as the whole of its coverage.

  **The fixed-array half is small when you get to it.** `arrayTypeCheck` already
  guarantees a fixed array's single dimension is a `ULitTag`, so `arr.len` folds
  to a compile-time constant in `fnCallTypeCheck`'s `ArrayTag` arm — no
  namespace for `ArrayTag`, no `CountIntrinsic`, no generation. A constant is
  the right lowering for a fixed size, not a stand-in for one.

  [[bugs|Bugs]] used to carry this as a defect on the grounds that "the published
  array documentation says this works". The manual says the opposite in its own
  first note; the body describing `.len` is the aspirational half. It is a
  missing feature, and it is this item's.

## Array literal elements fold, as of today

Not owed any more, recorded so it is not re-derived: an array literal's elements
settle on one type by meeting at a common supertype, the same fold `if` makes
across branches, and are then coerced to it. Before that they had to match the
first element by `itypeIsSame`, which made an array literal the one construct
that refused a variant where its union was wanted.

**What is still owed there belongs to [[type-inference-and-coercion|Type Inference and Coercion]]**, not
here: an array literal is never handed the expected type, so
`imm a [3; i64] = [1, 2, 3]` is refused and a `[4; u8]` literal needs the
suffix on every element.
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

