
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

## What lifetime enforcement does today

Established by [[Unenforced language rules]], which closed two of the three
lifetime holes it had recorded. Written down here because the rest of that work
is a feature rather than a missing check, and this is where it belongs.

**How a lifetime is represented.** A borrow's lifetime is a `uint16_t scope` on
the `RefNode` that the *borrow expression* produced. `borrowTypeCheck` computes
it from the variable being borrowed from. The numbering is **0 for a global, 1
for a parameter** — `parseFnSig` stamps every parameter with 1 — and **2 or more
for a local**, one per enclosing block. `newRefNode` initializes it to 0; it used
not to, so every reference type node the borrow path did not build carried
whatever the allocator last left there, and both checks below read it.

**What is enforced**, both from the flow pass, both applying to `RefTag` and
`ArrayRefTag` alike:

- `assignlvalrtype` rejects storing a borrow into an lval that outlives it.
- `returnFlow` rejects returning a borrow whose scope is a local's, walking a
  returned value tuple element by element (`ErrorEscape`).

**What is not, and why each needs work above rather than a check:**

- **A borrow laundered through a variable.** `mut r &i32; r = &local; r`
  compiles. Scope lives on the type node the borrow expression produced, and
  assignment does not carry it onto the variable's declared type, so the return
  sees a plain `&i32` of global scope. The assignment itself is legal, because
  `r` and `local` share a scope. `ref-flow-return` establishes this rather than
  claiming it.
  - This is what "Should borrowed ref vtype be cloned? So, scope value is
    distinguishable?" above is asking, and it wants answering first: while a
    `RefNode` can be shared between expressions, a scope recorded on one is
    ambiguous. Then "Add lifetime borrowed ref check on vardcl" becomes
    tractable — the variable's lifetime has to be flow state, since it changes
    per assignment.
  - Its control-flow join rule (`if c { r = &a } else { r = &b }` takes the
    shorter) is the same join machinery the conditional-move work in
    [[Copy & Alias vs. Move Semantics]] needs. Build them together rather than
    twice.
- **Across function boundaries.** The `refswitch` example on `reflifefn.html` --
  a callee returning a borrow derived from one of two reference parameters -- is
  unreachable without annotations, which have no parser syntax at all. This is
  the "Lifetimes (annotations)" block above.
- **Freezing the source of a borrow.** Documented, unimplemented; the source
  variable stays fully usable while a borrow of it is alive. Not a lifetime
  comparison at all — it is a rule about what the borrowed-from variable may do —
  so it does not fall out of any of the above.

