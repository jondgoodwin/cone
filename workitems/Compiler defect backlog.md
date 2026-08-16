# Compiler defect backlog

The residue of the test-suite survey: defects with a clear right answer, no
language decision attached, and no shared cause. Each belongs near an existing
work item rather than to this one; this page exists so that none of them is lost
between the survey that found them and the feature work that will touch them.

The three severe groups are elsewhere: [[Ownership memory safety]],
[[Diagnose instead of crash]], [[Unenforced language rules]]. Full evidence for
everything below is under "Found while building the groups" in
[[Add test suite]].

**Most of these are pinned by an `xfail` scenario**, which means the suite fails
the day one is fixed and the fixer is told to remove the mark. They cannot be
fixed quietly and they cannot rot silently. The few that are not pinned are
marked below, and those are the ones worth doing first, because nothing is
watching them.

## Wrong code generated

| Defect | Cause | Pinned by |
| --- | --- | --- |
| `&mut p.x` — a field borrow without parentheses | `parseAmper` (`parseexpr.c:321-326`) re-applies suffixes to the borrow, so it becomes a field access on `&mut p` carrying `FlagBorrow`. `fnCallTypeCheck`'s field branch ignores the flag and types the access as the field's own type, while `genlexpr.c:977-988` honours it and returns the field pointer. LLVM verification fails. The array-index path (`fncall.c:203-208`) has the fixup the field path lacks | `ref-field-borrow` |
| The nullable-pointer optimization | `genlTypeMeta` (`genllvm/genltype.c:174-207`) gives base and variants a bare pointer type while the variant initializer still stores through the variant's struct type. Construction fails `--verify`; matching on one segfaults | `union-nullable-ptr` |
| Writing through `&mut [N; T]` | `iexpGetLvalInfo`'s `ArrIndexTag` case takes the permission from the reference only for `ArrayRefTag` and `PtrTag`; a `RefTag` whose value type is an array falls through to the permission of the variable holding it. `(*v)[0] = x` works because the `DerefTag` case does consult it | `collection-arrayref-write` |

## Declared but unusable

| Defect | Cause | Pinned by |
| --- | --- | --- |
| Slices cannot be compared | `newArrayRefTypeMethods` (`corelib/corenumber.c:274`) declares `==` and `!=` with a signature whose region, permission and element type are all `unknownType`, which no real slice matches. Needs deciding what slice equality *means* — identity or contents — so it is the one entry here with a question attached | `collection-slice-compare` |
| No method on a generic struct can be called | `self` keeps the generic's type after cloning, so the call finds no candidate. Any signature, even one returning a constant | `generic-struct-method` |
| `&Box[T]` — a reference to a generic instantiation — does not parse | Bare `Box[T]` is a fine type; only the `&` form fails. Blocks any generic collection with `&self` methods | `generic-ref-instance` |
| Writing a trait field through `&<mut` is checked against the binding | `imm m &<mut Meter` is rejected while `mut m` is fine, where a plain `imm r &mut Rect` writes through happily | `trait-vref-lval` |
| A struct field of virtual-reference type cannot be assigned | A `&i32` field can | `trait-vref-lval` |
| The whole-value `` `&[]` `` operator method is unreachable | `&[]mut value` is the borrow *operator* and builds a one-element slice of the struct rather than dispatching. The indexed form does dispatch | **not pinned** |
| `as` onto a struct target is unchecked | `castTypeCheck` skips the size check entirely when `totype->tag == StructTag`, so a reinterpretation into a struct of any size is accepted | **not pinned** |
| `Bool[p]` and `usize[p]` fail | `castTypeCheck` permits `PtrTag` to `Bool`, but the type-literal path never reaches the cast path, so the permitted conversion is unreachable through `Type[...]`. `p into *u8` works | **not pinned** |

## Reported in the wrong place

None of these is pinned, because a scenario asserting a wrong position would have
to be rewritten by the fix rather than fail — so each is recorded here instead.
The exception is the last, which *is* asserted as-is on purpose.

- **`each`'s synthesized increment reports at the function's closing brace**,
  because `parseEach` builds those nodes with the lexer's position at the time the
  block finished parsing. `each-typecheck` annotates the real locations and says
  so in comments.
- **A name-fold clash between two wildcard imports names the wrong file** — it
  reports the conflict against the second module while echoing a source line from
  the first.
- **`ErrorFewArgs` is emitted with the message "Too many arguments provided for
  generic function."** Code and message disagree; one of them is wrong.
  `generic-typecheck-infer` asserts it exactly as it stands, so **fixing either
  will fail that scenario** and force the choice to be made deliberately.

## Behavioral, and possibly intended

Recorded because the survey could not tell, and someone who knows the intent can
settle each in a minute.

- **`continue` inside an `each` hangs forever.** The rewrite appends the
  increment to the end of the loop body, so `continue` jumps over it and the loop
  variable never advances. `break` is fine. Unpinnable — the runner would have to
  hang to observe it, which is what R1.3's timeout exists to survive rather than
  to assert.
- **A method cannot be called through a raw pointer.** Field access
  auto-dereferences; method lookup does not retry after `derefInject` on the
  `PtrTag` path as it does on `RefTag`. Currently pinned as a *reject*
  expectation by `safety-typecheck-ptruse`, so if it is a gap rather than a
  decision, that scenario is asserting the wrong thing.
- **Branch inference does not unify two identical closure types**, though
  coercion to a declared type works. Each anonymous function appears to get a
  distinct type node.
- **A parameterless macro name is not expanded** as a function's final statement,
  nor as the left operand of an operator. As a right operand and as an
  initializer it works.
