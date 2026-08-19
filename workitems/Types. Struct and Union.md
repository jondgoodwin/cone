
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
  need for a separate pass. See `design/Analysis.md` rules 4 and 5.

**A union variant may hold its own union by value, and the field is silently
dropped.** Measured while closing the above, and *not* fixed by it -- identical
before and after, so it is a hole of its own rather than a regression.

```cone
union Bad {
  struct Leaf { v i64 }
  struct Wrap { t Bad }    // accepted
}
```

compiles clean and generates `%Bad = type { i8, i64 }` -- tag plus the largest
variant *ignoring* `Wrap`, which has nowhere to put its `t`. That is a
miscompile, not a missing diagnostic. The size check that catches the same shape
in a plain struct does not fire because the union is already marked laid out by
the time its variants' fields are checked: the base trait settles first, then the
variants. Whatever fixes it has to make a variant's field see the union as still
in progress, which is a question about the order inside `structTypeCheck` rather
than about the size rule.

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