
Make union syntax look more like C and multi-level with hidden tag

Struct
- Tuple destructuring: Allow `.1`,  `.*` or `.(x,y,z)`
- Layout attribute: Packed/bitmap/union
- Structs as namespaces to hold other structs/enums, etc.
- Opaque Wasm “anyptr” attribute?

Multi-level struct literals (e.g., globals)

Delegated struct field inheritance:
- Struct-based ‘using’ inheritance
- Capture inherited names and aliases during parse
- During type check, create special multi-part forwarding nodes for each new name and add to dict
- Gen forwarding nodes correctly (vs. a FieldNode or MethodNode)
- Update the Cone reference page header for inheritance as needed

Traits
- Traits may not have private fields/methods
- Associated types

Recursive types

**Mostly closed by [[Analysis re-factor]].** A struct may now hold itself, or
another struct that holds it, through a reference, which is what makes a linked
list or a tree expressible; a cycle with no reference in it is refused, naming
the field and saying to break the cycle with a reference (`ErrorNoSize`).
`struct-recursive` and `struct-typecheck-nosize` cover both sides.

- ~~Forbid recursive type cycles that involve no references~~ -- done, per above.
- **Still open: a cycle with no terminating variant.** A `union` whose every
  variant recurses has no base case, and nothing checks that. Deciding it is a
  termination question about the whole cycle, not a size question about one
  field, so the mechanism above does not reach it.
- ~~How to handle infectious typing decidability for recursive type cycles~~ --
  answered. A type's size is asked where a value of it is held, and a type still
  being laid out answers "no size" rather than being refused. Opacity infection
  is chased where it is reported rather than stored.
- ~~Separate infectious/cycle from rest of type check - how?~~ -- answered by
  *not* separating it. Reading the in-progress mark in place is what removed the
  need for a separate pass. See `design/type-check-phase.md` rules 4 and 5.

- ~~A union variant may hold its own union by value~~ -- found while closing the
  above, and fixed. `union Bad { struct Wrap { t Bad } }` compiled clean and
  generated `%Bad = type { i8, i64 }`: tag plus the largest variant *ignoring*
  `Wrap`, which had nowhere to put its `t`. A miscompile, not a missing
  diagnostic, and the same before and after the re-factor.

  The cause was that a closed trait's laid-out mark says only that its *own*
  fields are settled -- for a union, the tag. Its variants are analyzed
  afterwards, each pulling the trait in as its base and finishing it first, so
  the mark was already set when a variant asked. Asking a union for a size now
  also asks whether every variant is laid out. `union-typecheck-variant` holds
  the case that fails without it, and `union-success` holds the tree that must
  keep working.

  The first attempt analyzed the variants from inside the trait, before the mark.
  That refused the bad case correctly and then crashed on the legal tree: moving
  that step reorders the mixin splice the variants depend on. Asking the question
  where the size is wanted, rather than reordering layout, is what worked.

Improve algorithm for deciding whether types have ‘move’ semantics:
**A variant literal does not coerce to its union in a struct literal's field.**
Measured by [[Compiler defect backlog]]. `Holder[Just[&v]]`, where `Holder`'s
field is declared with the union's type, reports `ErrorBadArray` "Literal value's
type does not match expected field's type" — while the same coercion in a
variable initializer, `mut u Maybe = Just[&v]`, is accepted. Confirmed
identically on a three-variant tagged union, so it is the type-literal field
check rather than anything about the nullable-pointer layout it was found under.
The initializer path proves the coercion exists; the field path does not ask for
it.

- Ttuple: any of its fields are move
- Struct is move if it is marked as move 
- Struct type is move if it implements a finalizer but no clone 

Struct Handling Optimization?  LLVM does not bitcast structs, so the current code stores in an alloca, recasts the pointer and reloads in the new struct.  Perhaps better code is possible if we do what Rust does, treat structs as implicit pointers. To implement this requires:
- Struct typeliterals 627 should alloca, gep and load fields
- Store 592 should load a struct value first, before storing it
- Load 651 should use genAddr instead on a struct value
- Bitcast 442 should simply do a pointer cast on a struct value
- strField 730 should use GEP where we already have the pointer
- Deref 812 should avoid doing the Load
- Still later, we can optimize so that small structs are treated as separate values