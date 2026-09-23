`StructNode` is one node for **struct, trait and enum**, distinguished by three
flag bits. It is also the compiler's most consequential node: field layout,
method sets, trait inheritance, the closed variant family, vtables and drop
functions all live here.

**The three kinds in one sentence each.** A **struct** is a record of fields. A
**trait** is an open abstraction: its implementers are declared beside it, name it
with `is`, and may live in another module, so nothing can number them. An
**enum** is the closed family: its variants are declared inside it, so the
compiler owns their layout — a discriminant, the enum's own fields spliced into
every variant, and, unless the declaration writes `@unsized`, padding to one size.

**A trait is a modifier on a kind, not a kind of its own.** `struct trait X`
declares the abstraction of a struct, and `trait X` means exactly that — one
node, one flag, and the same declaration either way. The word it modifies is what
says which **family** the abstraction serves, which is what the kind-match rule
needs: a struct composes a struct or a struct-trait, and an actor will compose an
actor or an actor-trait. So no trait declaration carries anything to say which
family it is for, and nothing is inferred from what it declares. `mod trait` and
`actor trait` are admitted by the grammar so that the spelling of those
abstractions is settled; neither kind exists, so both report `ErrorUnbuiltKind`
where they are written. `enum trait` is refused, and that absence is the one below.

**At a glance.** `parseStruct` does a great deal — tag synthesis, mixin
placeholders, variants in both of their spellings, tag numbering, generic
parameter copying. Name resolution builds the dictionary whole: it inserts `Self`,
gives an enum its equality, takes the default methods of every abstraction the
type is-a or mixes in and splices an enum's fields into its variants, and only
then resolves the method bodies, so an inherited member may be named bare. Type
check indexes fields, computes infectious flags, settles the discriminant's width,
verifies that the fields the abstractions require are declared here, sets
`TypeChecked` **before** methods, verifies their method requirements, then
synthesizes a drop function. Generation lowers to a named LLVM struct, or to
padded variants, or to nothing at all.

*Provenance: read from source.*

## Principles — [derived]

**A subtype relationship is asserted, not noticed, and it is asserted with
`is`.** A concrete type says in its own declaration that it complies with one or
more abstractions, and the compiler verifies the compliance there. ▸ **That is the
only way to comply with an abstraction that has nothing in it to notice** — a
marker trait is empty, so no structural match could distinguish a type that
carries it from one that does not, and only a declaration can say. ▸ And it forces
conformance at the moment of declaring rather than later at a use.
**Structural conformance stays**, and the two coexist on purpose: what `is` adds
is the assertion and the check, not the capability.

**A trait contributes no fields.** What a trait's fields state is a *requirement*,
which the declaring type satisfies by declaring them itself — the same names, the
same types, in the trait's order, beginning at position 0. ▸ **That positional
prefix is what makes a plain reference to the trait a view of the type's own
storage, and a by-value coercion to a same-size base a pure recast** — no
conversion, no copy. ▸ **Forbids** an inheritance model where a base's fields may
sit anywhere in the derived layout, and forbids a type relying on a trait to
declare its state for it.

**Only the first abstraction named may require fields**, since only one of them
can hold position zero. ▸ **Settles** that any number of abstractions may be
named: a field two of them wanted is declared once, so no contest between two
traits' field orders can arise. ▸ A trait is held to the field requirement exactly
as a struct is, because a field has no bodiless form — declaring it *is* the
compliance, where a method may be declared without an implementation and so passed
on. So the prefix holds at every hop by induction, and the check compares against
the base's own field list alone.

**The one field splice left is the enum's.** An enum owns its variants' layout, so
its fields — the discriminant among them — are cloned into every variant, which
declares none of them. ▸ **Forbids** giving a variant a base of its own: its
relationship to its enum is membership, not is-a conformance.

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

⚠ **The layout consequence is a hazard, not a free lunch:** an enum splices
fields in *at a position*, so adding one shifts every later field index in every
variant and positional type literals move with it. Adding one to a trait is the
same hazard reached differently: it is a new requirement, so every implementer
must declare it, and inserting it rather than appending moves every field after it.

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
| `dclinfo` | owner and the facts its symbols are spelled from — [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols". The owner is a module, or the enum for a variant declared inside one. Read for one thing besides naming: rejecting a variant declared outside its enum's module, through `dclInfoGetModule` |
| `basetrait` | the **type expression** of the first abstraction an `is` names, or of the enum a variant belongs to — a `NameUseNode`, or an `FnCallNode` for a generic base. **Not a `StructNode*`.** Two helpers unwrap it and they answer different questions: `structBaseTraitDcl` takes **one hop**, to the declaration this type stands on, while `structGetBaseTrait` recurses to the **bottom-most** one. Picking the wrong one is how the infection loop hangs |
| `derived` | for an **enum**, its variants in declaration order. The index is the `tagnbr` only where nothing pinned one, which is what generation asks before using the tag to index the vtable list |
| `traits` | every abstraction whose members were taken — the base, each further name in the `is` list, and each `mixin` — or NULL. Written where the members are taken (`structInheritTrait`) and read by type check's two requirement checks, the only things that still need to know which trait a requirement came from. **Which entry is the base is asked of `basetrait`, not of this list's order**, since the field walk that fills it runs backwards |
| `fields` | all fields in layout order. A declared field may carry a fold clause (`FieldDclNode.fold`); a folded copy is never here |
| `vtable` | NULL until `structMakeVtable` |
| `tagnbr` | discriminant value, assigned at parse: the value the author pinned, or the next in sequence. `TagUnassigned` is the sentinel between reading a variant's name and settling its value, which is why no flag bit records whether one was written — a type has none to spare, and nothing after parse needs to know |
| `llvmtype` | generation memoizes here; non-NULL means "already generated" |

**What tells the three apart:**

| | `TraitType` | `EnumType` | `SameSize` | `HasTagField` |
| --- | --- | --- | --- | --- |
| `struct` | — | — | — | — |
| `trait` | yes | — | — | — |
| `enum` | yes | yes | yes, unless `@unsized` | yes, once it has variants |
| a *variant* | — | — | inherited | inherited |

**A trait carries neither closed flag, and refuses to.** A `struct` written inside
a trait's body is `ErrorOpenTrait`: an open set of implementers cannot be numbered,
so there is no discriminant to give them, and a closed set is what `enum` is.

**`EnumType` is the enum's, never a variant's.** It says what the author wrote, so
a diagnostic can name the construct and a body can be told what it may declare.
The two closed flags travel down to the variants because they are facts about
layout; this one does not, because it is a fact about the declaration.

A variant is a plain struct with a `basetrait`, a `tagnbr`, and no `derived`.
**A `struct X is Trait` is in no `derived` list** — `derived` means "an enum's
variants", never "a trait's implementers".

The infectious flags — `MoveType`, `ThreadBound`, `OpaqueType`, `ZeroSizeType` —
are computed from the fields during type check. `NullablePtr` is set only during
generation.

⚠ **`OpaqueType` means "no value of this may be held", and three unrelated facts
set it**: the type was declared `@opaque`, it is a trait or an `@unsized` enum, or
one of its fields is unsized. **Only the first means there is no layout.** A
trait's own fields are known and indexed, and a struct with an unsized field is
refused by type check (`ErrorNoSize`, `struct-typecheck-nosize`) long before
anything asks for its layout. `DeclaredOpaque` marks the first case at parse, and
it is what generation asks; reading `OpaqueType` there left every trait an opaque
LLVM struct, which is why a reference to a trait could not be lowered.

## Parse

`parseStruct` arrives with much already done:

- `@move`, `@opaque` and `@unsized` attributes consumed into flags. `@unsized`
  *clears* `SameSize`, which the caller set: padding is the default and the
  attribute declines it. On anything but an enum it is `ErrorBadUnsized`, since
  nothing else has variants to pad.
- **The `trait` modifier is read before the attributes**, so it sits against the
  kind keyword it qualifies. `struct trait` sets `TraitType`, which is the flag
  the bare `trait` arm in `parseGlobalStmts` sets already. Written twice it is
  `ErrorDupTrait`, since the one spelling says everything the two would.
- `enum trait` is `ErrorEnumAbstract`, and so is `struct trait` on a **variant**.
  **The absence is deliberate**: an enum's identity is its variant set, so
  anything a caller could hold behind an abstraction of one either is that set,
  and so is the enum, or is open, and so is a trait — and a variant is one
  concrete member of such a set, which has no abstraction for the same reason.
  `enum-parse-decl` pins both, so nobody adds an abstract enum for symmetry with
  the kinds that do take the modifier.
- An enum may name the **integer type its tag values are laid out in**, read with
  `parseTypeName` and attached to the discriminant's own type node. It is the
  type's, not the field's, which is why it is carried on `EnumNode` rather than
  settled here.
- An **unnamed type is still built**, under `anonName`, so the body is still
  parsed rather than dumped onto the module's statement stream.
- Each method joins the type through `iNsTypeAddFn`, which records the type as
  its owner; that is what spells its symbol `Type_meth` at generation.
- **`is` takes a comma-separated list.** The first name becomes `basetrait`;
  each further one becomes the same `FieldDclNode` placeholder a `mixin` does, so
  nothing new carries them. Only the first may require fields, and the rest are
  therefore field-less, which is why a placeholder for one costs no layout.
- `mixin T` becomes a `FieldDclNode` named `_` flagged `IsMixin`, a **placeholder
  standing for a trait, which name resolution removes once it has taken the
  trait's default methods** — or type check does, when the trait is an instance of
  a generic. It is replaced by fields rather than removed only for an enum.
- **`extends` is refused on a struct or a trait** (`ErrorExtends`), naming `is`
  as what to write. The word is held for enriching a concrete type with methods,
  which is not built. An **enum** keeps it, for an enum that adds variants to
  another's — also not built, and parsed here unchanged so that nothing about it
  moves.
- A field's trailing `use` clause (`parseFoldClause`) is stored on the field,
  with an alias per listed name positioned at the item; **nothing enters the
  namespace at parse**, since whether a name is a field or a method is not
  known until the field's type is. `use` on anything that is not a struct's
  field — a variable, a parameter, a static, a mixin, an `is` clause, a trait's
  or an enum's field — is `ErrorBadFold` there, so the diagnostic is the fold's
  own. **`is` names abstractions and folds nothing**: an abstraction has no
  value for a folded name to be reached through, and delegation is what a field's
  own clause is for.
- **A variant has two spellings and they build the same node.** A `struct` written
  inside an enum is one; so is a **bare name**, which declares an empty struct
  variant and is what lets one construct serve a plain set of named symbols as
  well as a set carrying payloads. Both set `HasTagField` on the enum, take the
  enum's generic parameters, get a synthesized `basetrait`, and are registered at
  module scope — bound in the module, but owned by the enum, so their symbols are
  spelled after it.
  - **What tells a bare-name variant from a common field is the token after the
    name**: `,`, `;` or `=` makes it a variant, and anything else is a type, so it
    is a field. The field node is therefore built while the lexer is still on the
    name, before the choice is made, so that a diagnostic about either points at
    the name rather than at what follows it.
  - **A variant restating what the enum decides is `ErrorVariantDcl`** — its own
    `is`, or its own generic parameters. One code for both, because a reader
    would not branch on which and the remedy is the same: delete it.
- **Tag numbering runs across the whole body**, ascending from zero, and a written
  value resets it, so numbering continues from there. Two variants holding one
  value is `ErrorDupTag`, reported on the second. **A variant may not assert
  conformance to an unrelated trait, and the absence is deliberate** — a variant's
  relationship to its enum is membership, the enum owns its layout, and a second
  base would require fields the enum did not put there.
- **A tag field is synthesized at position 0 of an enum** that has variants and did
  not place its own, and every variant inherits it through mixin expansion. A
  trait's implementers are open-ended, so no value could be
  unique and there is nothing to synthesize; `%Box = { i32 }` for a struct whose
  own declaration is one `i32`, whatever it is-a, and naming several abstractions
  costs nothing, because there is no discriminant for a second one to duplicate.
  **The
  discriminant is marked `IsTagField` here** — the synthesized field, or the enum's
  first `tag`-typed field where one is written — because a variant copies the enum's
  fields as soon as it is name resolved, before the enum is type checked; type check
  validates the mark and refuses a second one.
- **`tag` is recognized only where a field's type is written**, so it is not a
  reserved word and `pub tag i32` still declares a field named `tag`. Written on
  anything but an enum it is refused here, which is what keeps the type-check rule
  below to one question: does this enum carry more than one.
- **An enum with no variants is `ErrorNoVariants`.** Its identity is its variant
  set, so an empty one names nothing a value of it could be.

## Name resolution

`structNameRes`, in a strict order that matters. **The dictionary is built
whole before any method body is resolved**, so that a body may name an
inherited member bare, exactly as it names the type's own.

1. Return at once if the type is already resolved or under way — it may have
   been reached by demand before the module's walk got to it — and mark it
   `NameResolving`.
1a. **An enum gets its `==` and `!=`** (`structEnumAddEquality`). Entered in the
   namespace and *not* in `nodelist`, because this is the enum's own comparison and
   not a requirement on its variants: a vtable slot, a conformance requirement and a
   default cloned into every variant are all read off `nodelist`. Added here rather
   than at type check because name resolution finishes for every module before type
   check begins, so nothing can look the name up too early.
2. Push the hook table.
3. Resolve generic parameters **inside** the push — resolving one hooks it, so
   doing it beforehand would bind it in the enclosing scope and the matching pop
   would never remove it.
4. **Resolve `basetrait` now**, before the type's own namespace is hooked — the
   comment says "before any other name in type is hooked", and the reason is
   scoping: once step 7 hooks the members, they shadow module scope, and the
   type's own name is among them. When it names a declaration this type may stand
   on (a trait, and closed only if this type is), **insert a mixin placeholder for
   it at position 0**, exactly as `mixin` does — which is how the base, the rest
   of an `is` list and `mixin` become one mechanism.
5. **Demand each trait a placeholder names, and the type of each field that
   carries a fold clause** (`structNameResDemand`): resolve it now, in its own
   module's scope if it lives elsewhere, so that its own members are complete
   before they are read. Still before this type's names are hooked, so the
   trait's bodies bind in the trait's scope and not in this type's. A trait
   already under way is a cycle — `A is B is A`, or a trait mixing
   itself in — and is `ErrorCircular` where it is named; the compiler used to
   loop here without end. A fold from a type still under way is refused at
   step 9 instead.
6. Insert `Self` into the namespace, aliasing the struct to itself — done
   ahead of step 4, in fact, since a field's type may name it. This is what
   `parseFnSig`'s `Self` inference for a method parameter depends on.
7. Hook the whole namespace.
8. **Walk the fields backwards** — so that splicing does not move a field not
   yet reached — resolving each ordinary field and **expanding each placeholder
   whose base is resolved** (`structInheritTrait`, under a clone state whose
   `Self` is this type). What the base contributes decides what happens to the
   placeholder: **a trait contributes no fields, so it is simply removed**, while
   **an enum's fields replace it** as clones in the enum's order, entered in the
   namespace (a name the variant already declared is `ErrorDupName`, reported on
   the clone, which keeps the enum's position). Either way, a clone of each
   default method whose name the type does not declare. A
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
2. Type check `basetrait`; require an abstraction — `ErrorInvType`, since a
   subtype relationship runs from a concrete type to an abstraction and never
   between two concrete types; require the closed-ness to match —
   which is what refuses a type outside an enum joining its variant set;
   propagate `SameSize`/`HasTagField` down from the bottom-most base, and
   require a closed type's derived types to share its module. A base
   name resolution did not expand — it is in `traits` when it did — is an
   instance of a generic that exists only now, so **insert a mixin placeholder
   for it at index 0** as name resolution would have.
3. Type check every trait in `traits`, so each is laid out before this type is.
4. **Walk fields backwards.** Backwards so that splicing does not invalidate the
   cursor. An ordinary field is type checked. A placeholder still standing —
   the generic case — is expanded exactly as name
   resolution expands one (`structInheritTrait`), except that nothing is
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
   marked at parse is the discriminant, and a second `tag`-typed field is refused.
   **Asked only of the enum**, never of a variant, whose fields are clones of its
   enum's — asking again would report the enum's mistake once more per variant, at
   the same position and in the same words, which no scenario could tell apart.
5a. **Settle the discriminant's width** (`structSetTagWidth`). It follows the
   largest tag **value**, not the variant count, since a pinned value is what lines
   an enum up with another language's constants and `Red = 0xFF0000` needs four
   bytes however few variants there are. An enum that named its integer type has
   the width fixed there instead — that being the point of naming it — so a value
   too large for it is `ErrorTagWidth` rather than a silent widening away from the
   layout the author asked for. The discriminant's type node is **shared, not
   cloned** (`clone.c`), so every variant's copy of the tag field reads the width
   set once here.
5b. **Verify the field requirements** (`structCheckIsaFields`), the layout having
   just settled, which is what an `is` asserts about. The base's own fields must
   be declared here, under the same names and types, in the base's order,
   beginning at position 0 — `ErrorIsaFields`, reported at the field that breaks
   the prefix, or at the type when the requirement runs off the end of its field
   list. And every abstraction past the first must require no fields at all
   (`ErrorIsaMulti`), since only one can hold position zero. An enum base is exempt
   both ways: it splices its fields in, so a variant declares none of them.
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
- **Under `Coercion`, that is the only path.** A by-value coercion works only
  through a same-size base, which is why an `@unsized` enum has no by-value form
  at all: the free recast rests on every variant already being the enum's size.
- **Structurally**, otherwise: every method of the target must have a
  signature-matching counterpart — a folded method counts, through its alias.
  Then `Monomorph` compares fields **by name, order irrelevant**, while
  `Regref` requires a positional, same-named **prefix**; a folded copy meets
  neither, since it is not a field of the type. So width subtyping always;
  depth subtyping only where no conversion is needed.
- **The prefix `Regref` needs is the declaring type's own fields.** A trait
  contributes none, so what makes the comparison succeed is that the type declared
  them — which `is` verified at the declaration, and which a structural
  conformer happens to satisfy. That is the whole difference this comparison sees
  between the two, and it sees none: a nominal declaration is checked earlier, not
  matched differently.

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

Three shapes, the first two chosen in `genlSetupTaggedTrait`:

- **Nullable pointer** — a `SameSize` enum with exactly two variants, one of
  one field and one of two whose second is pointer-like. **No struct is emitted
  at all**; the value *is* the pointer and null is the empty variant.
- **Same size** — every variant re-emitted with `[N x i8]` trailing padding to
  the largest; the enum's body is a copy of the largest variant's fields.
  Measured: `%Circle = { i8, i32, i32, [4 x i8] }` beside
  `%Rect = { i8, i32, i32, i32 }` and `%Shape = { i8, i32, i32, i32 }`.
- **Unpadded** — each variant emitted at its own size, the tag still first.
  Measured, for an `@unsized` enum of an empty variant and one holding three
  `i64`s: `%Ping = { i8, i32 }` beside `%Payload = { i8, i32, i64, i64, i64 }`.

**The discriminant's width is not generation's.** Type check settles it, because it
follows the largest tag value rather than the variant count and generation cannot
see a pinned value in `derived->used`.

**The tag selects a variant's vtable two ways, and which one is a property of the
tag values.** `structMakeVtable` prewires the vtable list in `derived` order, so
where every variant's tag value *is* its position the tag indexes the list
directly, which is one load (`genlTagsIndexVtables`). A pinned value breaks that —
`Red = 0xFF0000` would index four million entries past the end — so the sparse case
compares instead, one `select` per variant with the last as the fall-through
(`genlVtableForTag`). That costs code at the coercion rather than a table
proportional to the largest value, and it needs no basic blocks, so nothing depends
on where the coercion sits.

**Generation consumes without validating**: `FieldDclNode.index` for every GEP
and `extractvalue`, and `vtblidx` for vtable slots.

## Hazards

- **Whether `&<Struct` should work at all**, rather than be refused, is an open
  language question. `refvirtTypeCheck` requires a `TraitType` today, so the
  answer in force is "refused".
- **A method of a type never gets that type's drop calls** — `structSetDropFn`
  runs after the method loop, so `dropfn` is still NULL while method bodies are
  checked and flow-analyzed.
- **Mixing in two enums brings two tag fields**, and no duplicate-name error fires
  because `namespaceAdd` silently ignores `_`, so what reports it is type check's
  one-discriminant rule. Traits carry no tag, so a chain of `is` bases and any number
  of `mixin`s meet nothing here.
- **A default method cloned from an instance of a generic trait, or a member
  folded from a field whose type is a generic's parameter, cannot be named bare.**
  The instance exists only when type check instantiates it, so what it contributes
  joins the type's dictionary after every body has been resolved; such a name is
  reached as `self.name`. A trait or field type that is a declaration when the type
  is resolved has no such limit. **Only cloned defaults are affected** — a trait
  contributes no fields, so a field of a generic trait is a requirement the type
  declared for itself and is an ordinary member of it.
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
- **An enum's `TypeChecked` does not mean it has a size.** Its size is computed at
  generation from `derived`. `itypeVariantPending` exists for exactly this.
- **`structAddField` drops a duplicate-named field from `fields`** while the
  parser has already assigned indices, so positional literals shift.
- **An enum's equality is declared even where it cannot be given.** Where a variant
  carries fields, comparing two values would have to compare those fields, and Cone
  has no structural comparison for a struct of any kind. The method is entered
  anyway, carrying `NoEqIntrinsic`, and `fnCallLowerMethod` refuses the call with
  `ErrorEnumEquality` — so the author is told why instead of reading the absence of
  `==` as an oversight. Nothing generates that intrinsic.

## What lives elsewhere

- Layout, size, and why `TypeChecked` sits where it does: [Type Check Phase](../phases/type-check.md), "Struct and trait"
- Enums at LLVM level, and vtables: [Generation](../phases/generation.md)
- Field declarations, and what their permissions govern: [vardcl](vardcl.md)
- Virtual references and how a vtable is selected: [references](references.md)
- Instantiating a generic struct: [generic](generic.md)
