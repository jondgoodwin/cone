`StructNode` is one node for **struct, trait and union**, distinguished by two
flag bits. It is also the compiler's most consequential node: field layout,
method sets, trait inheritance, tagged unions, vtables and drop functions all
live here.

**At a glance.** `parseStruct` does a great deal — tag synthesis, mixin
placeholders, nested variants, generic parameter copying. Name resolution
builds the dictionary whole: it inserts `Self`, mixes in the trait the type
extends and any it names with `mixin` — the trait's fields spliced in, its
default methods cloned — and only then resolves the method bodies, so an
inherited member may be named bare. Type check indexes fields, computes
infectious flags, sets `TypeChecked` **before** methods, verifies the traits'
method requirements, then synthesizes a drop function. Generation lowers to a
named LLVM struct, or to padded variants, or to nothing at all.

*Provenance: read from source.*

## Principles — [derived]

**`extends` and `mixin` are one mechanism: a synthetic mixin field at position
0.** A trait's fields become a *prefix* of the implementer's layout, and its
default methods are cloned in. ▸ **That prefix property is what makes a by-value
coercion to a same-size base trait a pure recast** — no conversion, no copy.
**Forbids** an inheritance model where a base's fields may sit anywhere in the
derived layout.

**Composition is compile-time flattening; polymorphism moves out to traits.**
The author's term is **delegated inheritance**: a field's `use` clause folds
members of the field's type in as names of this type, reached through the
field, with no forwarding method generated. It is *the same name-folding* a
module fold does — see [Modularity](../topics/modularity.md), which owns that
symmetry as an aim. ▸ **Forbids** "pure composition plus extra magic", which is
how the note describes conventional inheritance.

**A type's namespace and a module's are the same machinery, and the two folds
differ in exactly one place.** Measured by building the type side: the
namespace insertion, the collision rule and the aliasing are one operation, and
the binding node — the alias — serves both. What differs is what the binding
holds. A module fold binds the declaration itself; a type fold binds a copy of
a field, or an alias whose *use* has a receiver to shift, because only a type
fold reaches its target through a value. ▸ **Settles** that the module work
reuses the alias node unchanged and never calls the receiver rewrite, and
**forbids** each layer inventing its own namespace rules. [Names and
Namespaces](../phases/names-and-namespaces.md) owns the rules themselves.

**A fold grants names, and nothing else.** A folded method counts toward
structural conformance with a trait, because conformance is a question about
names and signatures; a folded field satisfies no field requirement, because it
is a name for storage inside another type's value; and a fold grants no
subtyping to the field's concrete type, since subtyping in Cone is between a
concrete type and an abstraction, never between two concrete types. ▸
**Forbids** the flat constructor the old reference page offered for `use *`,
and any layout rule that would put a folded field at a position.

⚠ **The layout consequence is a hazard, not a free lunch:** a mixin brings
fields in *at a position*, so adding one shifts every later field index and
positional type literals move with it.

## Shape

| Field | Meaning |
| --- | --- |
| `nodelist` | ordered **methods, static functions and macros only** — never fields. Every walk of it that reads a `FnDclNode` — the vtable, the subtype comparison, trait default folding — skips a `MacroDclTag` first, and on a trait skips a **static function** too (see below) |

⚠ **A static function of a trait is the trait's own, and four walks of `nodelist`
have to say so.** `FlagMethFld` is what distinguishes it: no `self`, so there is
no receiver to specialize, nothing to dispatch on, and nothing about the shape of
a value for it to assert. It is **not** folded into implementers
(`structTypeCheck`), **not** a vtable slot (`structMakeVtable`), **not** a
requirement a subtype must satisfy (`structMatches`), and — unlike a trait's
methods, which the implementers own clones of — **it is generated here or
nowhere** (`genlGlobalSyms` and `genlGlobalImpl`). Miss the last and
`Trait.name()` calls a null; miss any of the first three and every implementer
stops conforming the moment a trait declares one. It is reached as
`Trait.name`, and an implementer or variant cannot name it at all:
`trait-nameres-static` pins both spellings, `trait-success` the call.

⚠ **A generic method costs a trait its virtual reference, and `structMakeVtable`
is where that is said.** A vtable slot holds one machine signature and a generic
method has one per instantiation, so no slot can be filled from it. The slot is
counted anyway, which leaves a requirement no type satisfies and every coercion
to `&<Trait` refused; `ErrorGenericVtable` names the method at its declaration in
the trait, once per trait, when the first virtual reference to it asks for a
vtable. A **private** generic method and a generic **static** function are
neither slots nor requirements and cost the trait nothing.
`trait-typecheck-vref` pins all three, and
`conesite/public/coneref/refvirtref.html`, "Type Restrictions", is the rule.
| `namespace` | every named member: fields, methods, macros, overload sets, `Self`, and what a fold admits — a **copy** of a folded field (a `FieldDclNode` with a `hop`) and an **alias** (`AliasDclNode`) for a folded method, overload set or macro method. The copies and aliases live here only; `fields` and `nodelist` never hold one |
| `dropfn` | NULL until the last step of type check |
| `dclinfo` | owner and the facts its symbols are spelled from — [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols". The owner is a module, or the trait for a variant declared inside one. Read for one thing besides naming: rejecting a variant declared outside its closed trait's module, through `dclInfoGetModule` |
| `basetrait` | the `extends` **type expression** — a `NameUseNode`, or an `FnCallNode` for a generic base. **Not a `StructNode*`.** Two helpers unwrap it and they answer different questions: `structBaseTraitDcl` takes **one hop**, to the declaration of the trait this type extends, while `structGetBaseTrait` recurses to the **bottom-most** one. Picking the wrong one is how the infection loop hangs |
| `derived` | for a **closed** trait, its variants in declaration order. The index *is* the `tagnbr` |
| `traits` | every trait whose members were mixed in — the base trait first, then each `mixin` in field order — or NULL. Written where the members are spliced in (`structInheritTrait`) and read once, by type check's requirement check, which is the only thing that still needs to know which trait a member came from |
| `fields` | all fields in layout order. A declared field may carry a fold clause (`FieldDclNode.fold`); a folded copy is never here |
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

⚠ **`OpaqueType` means "no value of this may be held", and three unrelated facts
set it**: the type was declared `@opaque`, it is a trait that is not `@samesize`,
or one of its fields is unsized. **Only the first means there is no layout.** A
trait's own fields are known and indexed, and a struct with an unsized field is
refused by type check (`ErrorNoSize`, `struct-typecheck-nosize`) long before
anything asks for its layout. `DeclaredOpaque` marks the first case at parse, and
it is what generation asks; reading `OpaqueType` there left every trait an opaque
LLVM struct, which is why a reference to a trait could not be lowered.

## Parse

`parseStruct` arrives with much already done:

- `@move` and `opaque` attributes consumed into flags.
- An **unnamed type is still built**, under `anonName`, so the body is still
  parsed rather than dumped onto the module's statement stream.
- Each method joins the type through `iNsTypeAddFn`, which records the type as
  its owner; that is what spells its symbol `Type_meth` at generation.
- `mixin T` becomes a `FieldDclNode` named `_` flagged `IsMixin`, a **placeholder
  that name resolution replaces with the trait's members** — or type check
  does, when the trait is an instance of a generic.
- A field's trailing `use` clause (`parseFoldClause`) is stored on the field,
  with an alias per listed name positioned at the item; **nothing enters the
  namespace at parse**, since whether a name is a field or a method is not
  known until the field's type is. `use` on anything that is not a struct's
  field — a variable, a parameter, a static, a mixin, a trait's or union's
  field — is `ErrorBadFold` there, so the diagnostic is the fold's own.
- A **nested `struct` inside a trait** sets `HasTagField` on the enclosing trait,
  synthesizes the variant's `basetrait`, assigns `tagnbr` from `derived->used`,
  and registers the variant at module scope — bound in the module, but owned
  by the trait, so its symbols are spelled after it. For a generic trait it copies the
  trait's generic parameters into the variant and builds `basetrait` as
  `Trait[P1,P2,…]`.
- **A tag field is synthesized at position 0 only for a closed type** —
  `flags & HasTagField`, a union or a trait whose variants are declared inside
  it — and every variant inherits it through mixin expansion. An open trait's
  variants may be extended by another module, so no value could be unique and
  there is nothing to synthesize; `%Box = { i32, i32 }` for a struct extending a
  trait declaring one `i32`, and composing several open traits costs nothing,
  because there is no discriminant for a second one to duplicate. **The
  discriminant is marked `IsTagField` here** — the synthesized field, or a base
  trait's first enum-typed field where one is written — because a variant copies
  the trait's fields as soon as it is name resolved, before the trait is type
  checked; type check validates the mark and refuses any other enum-typed field.

## Name resolution

`structNameRes`, in a strict order that matters. **The dictionary is built
whole before any method body is resolved**, so that a body may name an
inherited member bare, exactly as it names the type's own.

1. Return at once if the type is already resolved or under way — it may have
   been reached by demand before the module's walk got to it — and mark it
   `NameResolving`.
2. Push the hook table.
3. Resolve generic parameters **inside** the push — resolving one hooks it, so
   doing it beforehand would bind it in the enclosing scope and the matching pop
   would never remove it.
4. **Resolve `basetrait` now**, before the type's own namespace is hooked — the
   comment says "before any other name in type is hooked", and the reason is
   scoping: once step 7 hooks the members, they shadow module scope, and the
   type's own name is among them. When it names a trait declaration this type
   may extend (a trait, and closed only if this type is), **insert a mixin
   placeholder for it at position 0**, exactly as `mixin` does — which is how
   `extends` and `mixin` become one mechanism.
5. **Demand each trait a placeholder names, and the type of each field that
   carries a fold clause** (`structNameResDemand`): resolve it now, in its own
   module's scope if it lives elsewhere, so that its own members are complete
   before they are copied. Still before this type's names are hooked, so the
   trait's bodies bind in the trait's scope and not in this type's. A trait
   already under way is a cycle — `A extends B extends A`, or a trait mixing
   itself in — and is `ErrorCircular` where it is named; the compiler used to
   loop here without end. A fold from a type still under way is refused at
   step 9 instead.
6. Insert `Self` into the namespace, aliasing the struct to itself — done
   ahead of step 4, in fact, since a field's type may name it. This is what
   `parseFnSig`'s `Self` inference for a method parameter depends on.
7. Hook the whole namespace.
8. **Walk the fields backwards** — so that splicing does not move a field not
   yet reached — resolving each ordinary field and **replacing each placeholder
   whose trait is resolved** with the trait's members (`structInheritTrait`,
   under a clone state whose `Self` is this type): clones of its fields in its
   order, entered in the namespace (a name the type already declared is
   `ErrorDupName`, reported on the clone, which keeps the trait's position), and
   a clone of each default method whose name the type does not declare. A
   requirement with no body is inherited as it is, so that the name is in the
   dictionary: a trait passes it on, and a struct is told to implement it by
   type check. The new entries are hooked as they land. A placeholder whose
   trait is an instance of a generic is left for type check, since the instance
   does not exist yet. Then index the fields, so that a copy made next takes
   the index of the field it stands for.
9. **Expand each fold clause, in field order** (`structFoldExpand`, under
   "Name folding" below), after every trait's members are in place, so a folded
   name colliding with an inherited one is reported at the fold. A clause on a
   field whose type is not a declaration yet — an instance of a generic — waits
   for the instance's type check.
10. Resolve the methods declared here — only those; the clones arrived resolved
    in the trait's scope, and this walk cannot be repeated on a node.
11. Pop, and mark `NameResolved`.

**Reached by demand.** `structNameResDemand` is the one place name resolution
leaves walk order, and it is confined to a type declaration reached from
another type declaration, so what is hooked at the jump is known: module names,
and the demanding type's generic parameters. A type in another module resolves
with that module's namespace hooked over the current one and, if that module has
not begun its own resolution — modules resolve in load order, the root first —
with the names its wildcard imports *will* fold hooked too (`importHookFolds`),
without folding them. Folding early would change which names a qualifier can
reach in that module; see [module](module.md).

## Type check

`structTypeCheck` is the longest ordered sequence in the compiler:

1. **A template returns immediately** — only clones are checked.
2. Type check `basetrait`; require a trait; require the closed-ness to match;
   propagate `SameSize`/`HasTagField` down from the bottom-most base, and
   require a closed trait's derived types to share its module. A base trait
   name resolution did not mix in — it is in `traits` when it did — is an
   instance of a generic that exists only now, so **insert a mixin placeholder
   for it at index 0** as name resolution would have.
3. Type check every trait in `traits`, so each is laid out before this type is.
4. **Walk fields backwards.** Backwards so that splicing does not invalidate the
   cursor. An ordinary field is type checked. A placeholder still standing —
   the generic case — is replaced by the trait's members exactly as name
   resolution replaces one (`structInheritTrait`), except that nothing is
   hooked: no body is resolved after this. Such a type's inherited members
   cannot be named bare (see Hazards). Then expand any fold clause name
   resolution left — a field whose type was an instance of a generic — and
   **refresh every folded copy** (`structFoldRefresh`): a copy took its
   origin's type node and index when the fold was expanded, and type check may
   have replaced that node (an instantiation becomes its instance) and
   re-indexed the fields, so each copy takes its origin's type, permission and
   index over again, the origin demanded first.
5. **Walk forwards**: assign `FieldDclNode.index` over the final order, OR the
   field types' infectious flags together, and validate the tag field: the one
   marked at parse is the discriminant, and any other enum-typed field is
   refused.
6. `final` forces `MoveType`; `clone` clears it. Then propagate up the base
   chain, one `structBaseTraitDcl` hop per iteration.
7. **`TypeChecked` is set here, before the methods.** The placement is
   load-bearing, not an optimization: fields are indexed, size is known, and the
   method set is complete, so a method may use its own type by value —
   `fn twin(self) Self`.
8. Type check every method.
9. **Verify the traits' method requirements** (`structCheckTraitReqs`), now
   that every signature has its types: for each method of each trait in
   `traits`, the type's binding for the name must have the one candidate of the
   trait's signature — an inherited default meets that by construction — and a
   requirement with no body, inherited as such, is unmet in a struct; a trait
   may pass it on.
10. **`structSetDropFn`** — validate a `final` method, then, if any field's type
   has a drop function, synthesize a `drop` method, owned by the type so its
   symbol is spelled as any method's — `Bundle.drop`, `_CNvNt6Bundle4drop` —
   calling `final` and then each droppable field. The
   generated body is built pre-lowered and is **never type checked or flow
   analyzed**.

## Name folding

A field's `use` clause (`FoldClause`, on the `FieldDclNode`) admits names of
the field's type as names of this type. The language is in
[refinherit](../../conesite/public/coneref/refinherit.html); this is the
mechanism, in `structFoldExpand` and what reads its results.

**Two kinds of entry, by what a fold has to do with them.** A folded *field*
becomes a **copy** in this type's namespace: a `FieldDclNode` carrying the
field's own type, permission and index within its own type, plus a `hop` to
the field of this type it is reached through. A fold of a fold copies the whole
chain — `A` folding `b` through `a`, where `B` had folded `b` through `c`,
gets a copy of `c` hopping to `a` and a copy of `b` hopping to that — so the
chain always ends at a declared field of this type, and only the outermost copy
is entered under the local name. A folded *method, overload set or macro
method* becomes an **alias** (`AliasDclNode`): a local spelling and a target,
a member name use bound to the declaration. The method itself is untouched.
Jon's rule, and why the two differ: position lives in field nodes and only
there; a method has no position, so what it needs is a receiver, and the
receiver is data, found through the field's clause.

**Expansion**, in field order once every trait's members are in place: the
field must be `pub` (`ErrorNotPublic`), its type a struct that is not a trait
(`ErrorBadFold`) and complete — not this type, not one still being resolved
(`ErrorCircular`). For `use *`, an alias is made for every public member of
the source's namespace, its own copies and aliases included so a fold chains,
less what `but` names (`ErrorNoMbr` for a name the source lacks) and less
`Self`, the statics, macros without `self`, and the source's own `final` and
`clone`. Then each item: the name looked up in the source (`ErrorNoMbr`), must
be public (`ErrorNotPublic`), resolved through the source's own aliases, and
sorted by tag — a field is copied, a member method or macro binds the alias, a
static or anything else is `ErrorBadFold` — and entered in the namespace,
where a name already taken, by a declared member, an inherited one or another
fold, is `ErrorDupName` at the fold. Name resolution hooks each entry as it
lands, so a method body may name it bare.

**Reading an entry.** Every site that reads a namespace binding resolves an
alias to its declaration first, by one function, `aliasDclResolve`:
`iNsTypeCandidates` (so overload selection, trait conformance and vtable
matching), `fnCallLowerMethod`, the macro-method probe in `fnCallTypeCheck`,
`borrowRefIndexDispatches`, the `isTrue` coercion, and `nameUseGetDcl` for a
use bound to an alias. Visibility is *not* resolved: it is the alias's own bit,
public by construction. A copy needs no such step — it carries the tag, type
and permission every reader wants — so the sites that only read type or
permission are right unchanged, and only the sites that build an access or an
offset look at the `hop`.

**Building the access** is `fnCallFieldAccess`: for a copy, an access per hop,
root first, then one for the copy itself — the nesting the hand-written path
produces, so borrowing, permissions and generation see the true target. The
field arm of `fnCallLowerMethod` calls it; a bare `self.x` arrives there through
`nameUseTypeCheck`.

**The receiver of a folded method** is found from the clauses, read in place
(`structFoldReceiver`): the field of this type whose clause admits the name,
then on into that field's type where the name is folded there too, then the
call continues exactly as today from overload selection. A reference receiver
dereferences and reborrows the field it lands on with the reference's own
permission, so `(&mut car).thrust()` reaches `thrust(self &mut)` as
`(&mut car.engine).thrust()` does; a field that is itself a reference is the
receiver as it stands; a value receiver stays a value, which reaches a method
taking `self` by value. The fold adds no borrowing rule the language lacks.

**A generic instance expands its clauses in type check**, since a field of type
`T` has no declaration until the instance exists (`cloneFieldDclNode` clones the
clause unexpanded; a star clause makes its items over). The template's own
bodies were resolved before that, so a folded name of such a field is reached as
`self.name` inside them, as an inherited member of a generic trait is.

**Generation is untouched except the vtable.** A folded field never fills a
slot (`structAddVtableImpl` refuses a copy, as `structMatches` does). A folded
method fills one through a thunk: `structAddVtableImpl` records the field path
beside the method (`VtableImpl.foldpaths`), and `genlVtableThunk` emits a
function of the slot's own type that shifts the receiver one hop per field — an
address for a field held by value, a load for one held through a reference —
and tail-calls the method. Spelled `nameVtableThunk`, after the type, the trait
and the slot; nothing in the language can name it. The direct call has no
thunk: the compiler knows the type and shifts the receiver at compile time.

### Matching

`structMatches` refuses anything but "a trait is a supertype of a struct".

- **Fast path**: if the target is `SameSize`, walk the source's base chain
  looking for it — found means `CastSubtype`, since the supertype's fields are a
  prefix.
- **Under `Coercion`, that is the only path.** A by-value struct coercion works
  only through a same-size base trait.
- **Structurally**, otherwise: every method of the target must have a
  signature-matching counterpart — a folded method counts, through its alias.
  Then `Monomorph` compares fields **by name, order irrelevant**, while
  `Regref` requires a positional, same-named **prefix**; a folded copy meets
  neither, since it is not a field of the type. So width subtyping always;
  depth subtyping only where no conversion is needed.

**`structVirtRefMatches` mutates.** Asking whether a struct conforms to a trait
also *registers* the vtable implementation — which is why narrowing a
`&<Shape` back to a structural conformer works at all. Its failures are silent;
the diagnostics are commented out.

## Flow

Flow does little with a struct as such. The one mechanism that matters:
`flowScopeDealias` asks `itypeGetDropFnDcl` for any non-reference variable
leaving scope that was initialized, not moved out and not the scope's own
result, and appends a synthesized `dropfn(&uni var)` call to the block's dealias
list. A variable that was only declared, or whose value now lives in another
variable, gets no call: the drop fn would run over storage that holds no value
of the type. Neither does one the scope hands back, which the caller receives
and finalizes. **That is the entire mechanism by which struct destruction
happens.**

Per-field release is not flow's — it is `genlDealiasFlds` at generation, walking
`fields` by `index`. It resolves each field's declared type with
`itypeGetTypeDcl` before asking which region owns it: a field's `vtype` is the
name it was written with, so a typedef of an owning reference stands there as a
`NameUseNode` and matches no region read raw.

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
- **Mixing in two closed types brings two tag fields**, and no duplicate-name
  error fires because `namespaceAdd` silently ignores `_`, so what reports it is
  type check's "only once in a base trait". Open traits carry no tag, so a chain
  of `extends` and any number of open `mixin`s meet nothing here.
- **A member inherited from an instance of a generic trait, or folded from a
  field whose type is a generic's parameter, cannot be named bare.** The
  instance exists only when type check instantiates it, so its members join
  the type's dictionary after every body has been resolved; they are reached
  as `self.name`. A trait or field type that is a declaration when the type
  is resolved has no such limit.
- **A folded copy is a snapshot until the folding type is laid out.** It takes
  its origin's type node and index at expansion, in name resolution; if the
  origin's type is an instantiation of a generic, type check replaces that node
  in the origin and the copy is refreshed only when this type's field walk ends
  (`structFoldRefresh`). Nothing reads a copy's type before then except through
  a cycle of references between two folding types, where the refresh demands
  the origin field's own check and reads it, whatever its type's check has got to.
- **The demanding type's generic parameters stay hooked while a trait is
  resolved by demand.** A name the trait fails to declare that happens to spell
  one of them binds to it silently, where it would otherwise be `ErrorUnkName`.
  Only a program already in error can meet it.
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
