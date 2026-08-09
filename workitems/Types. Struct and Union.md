
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
- Forbid recursive type cycles that involve no references and those without a terminable abstract type (e.g., sum type)
- How to handle infectious typing decidability for recursive type cycles
- Separate infectious/cycle from rest of type check - how?

Improve algorithm for deciding whether types have ‘move’ semantics:
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