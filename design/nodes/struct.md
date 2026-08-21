`StructNode` is one node for **struct, trait and union**, distinguished by two
flag bits. It is also the compiler's most consequential node: field layout,
method sets, trait inheritance, tagged unions, vtables and drop functions all
live here.

**At a glance.** `parseStruct` does a great deal — tag synthesis, mixin
placeholders, nested variants, generic parameter copying. Name resolution
inserts `Self` and hooks the whole namespace. Type check expands mixins, indexes
fields, computes infectious flags, sets `TypeChecked` **before** methods, then
synthesizes a drop function. Generation lowers to a named LLVM struct, or to
padded variants, or to nothing at all.

*Provenance: read from source; the two crashes this note used to list in Hazards
were measured, then fixed.*

## Shape

| Field | Meaning |
| --- | --- |
| `nodelist` | ordered **methods and static functions only** — never fields |
| `namespace` | every named member: fields, methods, overload sets, and `Self` |
| `dropfn` | NULL until the last step of type check |
| `mod` | owning module. Read in one place: rejecting a variant declared outside its closed trait's module |
| `basetrait` | the `extends` **type expression** — a `NameUseNode`, or an `FnCallNode` for a generic base. **Not a `StructNode*`.** Two helpers unwrap it and they answer different questions: `structBaseTraitDcl` takes **one hop**, to the declaration of the trait this type extends, while `structGetBaseTrait` recurses to the **bottom-most** one. Picking the wrong one is what used to hang the infection loop |
| `derived` | for a **closed** trait, its variants in declaration order. The index *is* the `tagnbr` |
| `fields` | all fields in layout order |
| `vtable` | NULL until `structMakeVtable` |
| `tagnbr` | discriminant value, assigned at parse |
| `llvmtype` | generation memoizes here; non-NULL means "already generated" |

**What tells the three apart:**

| | `TraitType` | `SameSize` | `HasTagField` |
| --- | --- | --- | --- |
| `struct` | — | — | — |
| `trait` | yes | — | set once it contains nested variants |
| `union` | yes | yes | yes, once it has variants |
| a *variant* | — | inherited | inherited |

A variant is a plain struct with a `basetrait`, a `tagnbr`, and no `derived`.
**A `struct X extends Trait` is in no `derived` list** — `derived` means "closed
variants", never "implementers".

The infectious flags — `MoveType`, `ThreadBound`, `OpaqueType`, `ZeroSizeType` —
are computed from the fields during type check. `NullablePtr` is set only during
generation.

## Parse

`parseStruct` arrives with much already done:

- `@move` and `opaque` attributes consumed into flags.
- An **unnamed type is still built**, under `anonName`, so the body is still
  parsed rather than dumped onto the module's statement stream.
- `gennamePrefix` extended with the type name, so each method gets `Type_meth`.
- `mixin T` becomes a `FieldDclNode` named `_` flagged `IsMixin`, a **placeholder
  that survives to type check**.
- A **nested `struct` inside a trait** sets `HasTagField` on the enclosing trait,
  synthesizes the variant's `basetrait`, assigns `tagnbr` from `derived->used`,
  and registers the variant at module scope. For a generic trait it copies the
  trait's generic parameters into the variant and builds `basetrait` as
  `Trait[P1,P2,…]`.
- **A tag field is synthesized at position 0** when
  `flags & (TraitType | HasTagField)` — a two-bit mask, so **every trait gets
  one, open or closed**, and every `extends` struct inherits it through mixin
  expansion. That is why `%Box = { i8, i32, i32 }` for a struct extending a
  trait declaring one `i32`.

## Name resolution

`structNameRes`, in a strict order that matters:

1. Push the hook table.
2. Resolve generic parameters **inside** the push — resolving one hooks it, so
   doing it beforehand would bind it in the enclosing scope and the matching pop
   would never remove it.
3. **Resolve `basetrait` now**, before the type's own namespace is hooked — the
   comment says "before any other name in type is hooked", and the reason is
   scoping: once step 5 hooks the members, they shadow module scope, and the
   type's own name is among them.
4. Insert `Self` into the namespace, aliasing the struct to itself. This is what
   `parseFnSig`'s `Self` inference for a method parameter depends on.
5. Hook the whole namespace, then resolve fields and members.

## Type check

`structTypeCheck` is the longest ordered sequence in the compiler:

1. **A template returns immediately** — only clones are checked.
2. Type check `basetrait`; require a trait; require the closed-ness to match;
   propagate `SameSize`/`HasTagField` down from the bottom-most base, and
   require a closed trait's derived types to share its module. Then **insert a
   synthetic mixin field for the base trait at index 0** — which is how
   `extends` and `mixin` become one mechanism.
3. **Walk fields backwards.** Backwards so that splicing does not invalidate the
   cursor. A mixin field is replaced in place by **clones** of the trait's
   fields, and the trait's default methods are folded in — a name already
   present must match the required signature; a trait method with no body that
   nothing implements is an unmet requirement.
4. **Walk forwards**: assign `FieldDclNode.index` over the final order, OR the
   field types' infectious flags together, and identify the tag field.
5. `final` forces `MoveType`; `clone` clears it. Then propagate up the base
   chain, one `structBaseTraitDcl` hop per iteration.
6. **`TypeChecked` is set here, before the methods.** The placement is
   load-bearing, not an optimization: fields are indexed, size is known, and the
   method set is complete, so a method may use its own type by value —
   `fn twin(self) Self`.
7. Type check every method.
8. **`structSetDropFn`** — validate a `final` method, then, if any field's type
   has a drop function, synthesize a `<Name>_drop` calling `final` and then each
   droppable field. The generated body is built pre-lowered and is **never type
   checked or flow analyzed**.

### Matching

`structMatches` refuses anything but "a trait is a supertype of a struct".

- **Fast path**: if the target is `SameSize`, walk the source's base chain
  looking for it — found means `CastSubtype`, since the supertype's fields are a
  prefix.
- **Under `Coercion`, that is the only path.** A by-value struct coercion works
  only through a same-size base trait.
- **Structurally**, otherwise: every method of the target must have a
  signature-matching counterpart. Then `Monomorph` compares fields **by name,
  order irrelevant**, while `Regref` requires a positional, same-named
  **prefix**. So width subtyping always; depth subtyping only where no
  conversion is needed.

**`structVirtRefMatches` mutates.** Asking whether a struct conforms to a trait
also *registers* the vtable implementation — which is why narrowing a
`&<Shape` back to a structural conformer works at all. Its failures are silent;
the diagnostics are commented out.

## Flow

Flow does little with a struct as such. The one mechanism that matters:
`flowScopeDealias` asks `itypeGetDropFnDcl` for any non-reference variable
leaving scope, and appends a synthesized `dropfn(&uni var)` call to the block's
dealias list. **That is the entire mechanism by which struct destruction
happens.**

Per-field release is not flow's — it is `genlDealiasFlds` at generation, walking
`fields` by `index`.

## Generation

Three shapes, chosen in `genlSetupTaggedTrait`:

- **Nullable pointer** — a `SameSize` trait with exactly two variants, one of
  one field and one of two whose second is pointer-like. **No struct is emitted
  at all**; the value *is* the pointer and null is the empty variant.
- **Same size** — every variant re-emitted with `[N x i8]` trailing padding to
  the largest; the base trait's body is a copy of the largest variant's fields.
  Measured: `%Circle = { i8, i32, i32, [4 x i8] }` beside
  `%Rect = { i8, i32, i32, i32 }` and `%Shape = { i8, i32, i32, i32 }`.
- **Tagged** — an ordinary field flagged `IsTagField`, widened to 2/3/4 bytes by
  variant count.

**Generation consumes without validating**: `FieldDclNode.index` for every GEP
and `extractvalue`, `vtblidx` for vtable slots, and `derived` order as tag
order — `genlallocref` hard-codes `derived[1]` as `Option`'s `Some`.

## Hazards

- **Whether `&<Struct` should work at all**, rather than be refused, is an open
  language question. `refvirtTypeCheck` requires a `TraitType` today, so the
  answer in force is "refused".
- **A method of a type never gets that type's drop calls** — `structSetDropFn`
  runs after the method loop, so `dropfn` is still NULL while method bodies are
  checked and flow-analyzed.
- **A generic type's drop functions collide on one symbol**: `<namesym>_drop`
  with no module prefix and no type arguments.
- **Multiple mixins produce multiple tag fields**, and no duplicate-name error
  fires because `namespaceAdd` silently ignores `_`.
- **A trait's `TypeChecked` does not mean it has a size.** A union's size is
  computed at generation from `derived`. `itypeVariantPending` exists for
  exactly this.
- **`structAddField` drops a duplicate-named field from `fields`** while the
  parser has already assigned indices, so positional literals shift.

## What lives elsewhere

- Layout, size, and why `TypeChecked` sits where it does: [Type Check Phase](../phases/type-check.md), "Struct and trait"
- Unions at LLVM level, and vtables: [Generation](../phases/generation.md)
- Field declarations, and what their permissions govern: [vardcl](vardcl.md)
- Virtual references and how a vtable is selected: [references](references.md)
- Instantiating a generic struct: [generic](generic.md)
