
**Pointers:**
- Trust block
- Restrict pointer deref outside trust blocks

Borrowed references
- Fix handling of multi-subtyped function parameters (esp. ambiguous) & &mut &uni
- auto-&mut should be &uni, probably, even if it resolves to &mut
- & handling for method calls?
- Customized `=` operator method (== clone)
- `.=` operator:  auto-&uni. Mat4.=add(mat4)

- Forbid borrowing from an inlined function
- Support borrow of qualified method, except when ambiguous and no expected type
- Improve method bestfit algorithm to be unambiguous in choice
- Improve coercion auto-borrow: only works on lvals, perm, regions, and more types!
- Should borrowed ref vtype be cloned? So, scope value is distinguishable?
- Handle mutability/borrowed ref check on assignmultretflow (singleflow use rvaltype)
- Add lifetime borrowed ref check on vardcl
- Support ‘global. Ensure fn refs are always treated as ‘global
- Handle lifetime checking across function boundaries (implicit annotations)
- Global borrowed references can be literals (or part of a literal)

Lifetimes (annotations)
- Lifetime-constrained type: ‘a Type
- Fold lifetime-constrained type into borrowed references + subtyping
- Lifetime annotations - update the way lifetimes are encoded
- Handle caller/callee when references are returned
- Handle caller/callee for mutable reference parameters

