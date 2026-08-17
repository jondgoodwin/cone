
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

**One reference is not built by the borrow path at all**, and had to be taught the
same lifetime: `&mut a[1]` is re-associated to `(&mut a)[1]`, so
`fnCallArrIndex`'s `FlagBorrow` arm builds the element reference. It now copies
the receiver borrow's scope, an element of what a borrow points at living exactly
as long as the borrow. Before that it kept the 0 default and returning
`&mut a[1]` on a local was accepted while `&mut (p.x)` on the same local was
refused. Any further site that builds a `RefTag`/`ArrayRefTag` for a borrow
without going through `borrowTypeCheck` wants the same treatment.

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

## Receiver adjustment: what "`&` handling for method calls?" is about

That question sits in the borrowed-references list above with no evidence
attached. Here is the evidence, measured against the compiler by
[[Compiler defect backlog]].

**A method call resolves only when the way you hold the value already matches
the way the method declared `self`, except that a receiver held through a
reference or a pointer is dereferenced to reach a value receiver.** Field access,
in the same expression position, adjusts freely — `p.x` reaches through a value,
a reference or a pointer alike.

| Held as | `fn m(self)` | `fn m(self &)` | `fn m(self &mut)` |
| --- | --- | --- | --- |
| a value | works | fails | fails |
| `&T` | works | works | n/a |
| `&mut T` | works | works | works |
| `*T` | works | fails | fails |
| *a field, for contrast* | works | works | works |

The four cells that still fail are the ones that would need a reference made out
of a value or a pointer, and each reports `ErrorNoCandidate` "No method declared
by `m` accepts the call's arguments". Raw pointers are not a special case — they
are the row where only the deref half applies.

**Fixing a cell is one of two operations, and they are not comparable in cost:**

- **Deref**, where the holder is a reference or pointer and the method wants a
  value. A load and a copy. No reference is created, so nothing in permissions,
  aliasing or lifetimes is touched. This is what field access already does.
  **Done.** `fnCallLowerMethod` re-selects against the dereferenced receiver when
  no candidate accepted it as it stood; the dead retry that used to sit at the
  `RefTag` call site is gone. The retry runs only on a total no-match, so an
  overload set holding both a value and a reference candidate still gives the
  reference one to a reference receiver, and an ambiguity is still reported as
  one.
- **Borrow**, where the holder is a value or a pointer and the method wants
  `self &` or `self &mut`. This manufactures a reference, and that is the whole
  of the design question. **Deferred here.**

**The deref half briefly reached wider than methods called by name, and a
pointer has since been narrowed back.** An operator is a method under a
backquoted name, and `corelib` declares the number operators with value
receivers, so an operator the holder type does not declare itself fell through
to the value type's as well: `p * 2` on a `*i32` compiled and meant `(*p) * 2`,
where it used to be an error, while the `p + 2` beside it offset the pointer,
because a pointer declares its own `+`. Two lines that look alike doing entirely
different things, and C's compile error becoming silent arithmetic.

**Jon's ruling: operations on a pointer are on the pointer and not the deref;
the deref has to be written.** `p * 2` is `ErrorNoCandidate` again, and
`safety-typecheck-ptrops` asserts it. `p + 2` still offsets, and `sp.sum()` — a
value receiver reached by name through a pointer, which is what the deref half
was for — still works, run by `safety-pointers`.

**Only a pointer narrows.** A reference's comparison operators are its own
identity operators, declared by `corelib` for `RefTag` and selected by
`fnCallLowerPtrMethod` before the value type is ever consulted; its arithmetic
reaching the value's is by design and stays.

The compiler tells the two apart with `FlagOperator`, set by the three operator
constructors in `fncall.c` — the only way an operator application is built — and
tested where `fnCallLowerMethod` would otherwise deref and retry. An operator
application and a member access by name build the same node shape, so the flag
is the only record of which the source actually wrote; a name predicate over the
interned operator names would also have caught a user's `fn `+`(self, …)` called
by name, which is not an operation on the pointer.

**Why the borrow half is a design question and not a fix.** Three reasons, and
the third is the interesting one:

1. It reverses a documented behavior. "Receivers are not auto-borrowed" is
   recorded as an established fact by [[Add test suite]], and
   `struct-methods.cone` deliberately writes `(&p).sum()`. Retiring that is a
   language change, not a repair.
2. Borrowing from a value needs the binding's permission and a lifetime;
   borrowing from a pointer has neither to derive from. That is the same gap
   "Improve coercion auto-borrow: only works on lvals, perm, regions, and more
   types!" names above, arriving from the receiver side.
3. **A pointer is an unsafe borrowed reference**, and treating it as one makes
   the pointer row of that table fall out of the reference row rather than
   needing rules of its own. That reframing is Jon's, and it is the reason this
   note lives here rather than with pointers as a separate subject: the
   "Trust block" and "Restrict pointer deref outside trust blocks" items at the
   top of this page are the same work. `trust` means "drop the checks, I know
   what I am doing" — so its *absence* is not a gate on anything. Today the
   whole language behaves as though every line were inside a trust block, which
   is why `&mut (*p)` compiles clean right now with no keyword in sight.

**And the shape to design toward:** a method bound to its object is a value in
its own right — C#'s delegate, a fully typed pair of a receiver and a function,
so the compiler knows the parameters. Cone has the pieces scattered: function
references, anonymous functions lifted to module scope, and virtual references
carrying a vtable. Receiver adjustment, closure capture in
[[Types. Function and Closure]] and delegates are three views of one question —
how a callable and the thing it is called on travel together — and are worth
designing at once rather than three times.

