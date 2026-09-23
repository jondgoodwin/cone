`StructNode` is one node for **struct, trait and enum**, distinguished by three
flag bits. It is also the compiler's most consequential node: field layout,
method sets, trait inheritance, concrete enrichment, the closed variant family,
vtables and drop functions all live here.

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
abstractions is settled; neither abstraction exists, so both report
`ErrorUnbuiltKind` where they are written — `mod` itself is a built declaration,
which [module](module.md) owns. `enum trait` is refused, and that absence is the
one below.

**At a glance.** `parseStruct` does a great deal — tag synthesis, mixin
placeholders, variants in both of their spellings, tag numbering, generic
parameter copying. Name resolution builds the dictionary whole: it inserts `Self`,
gives an enum its equality, takes the default methods of every abstraction the
type is-a or mixes in, splices an enum's fields into its variants, takes
everything a concrete `extends` base has, and only then resolves the method
bodies, so an inherited member may be named bare. Type
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

**An enrichment adds methods and no fields, and that is what makes its values
and its base's interchangeable.** A type declared with `extends` over a concrete
base takes the base's fields as its own and adds methods of its own; it declares
no field, so the two have one representation and values of either substitute for
the other, in both directions, at no cost beyond a recast. ▸ **The DECLARATION is
the licence and identical representation is only the argument for why the licence
is sound**, so two types that happen to look alike and named no base stay
unrelated, and nothing lifts through a container: `Pair[Gauge]` and `Pair[Meter]`
are two instances of one template, neither of which extends anything. ▸
**Forbids** structural typing between concrete types, and forbids an enrichment
overriding what it took — one value would otherwise mean two things, depending on
which of the two names reached it.

**An enum that extends another holds copies of its variants and substitutes for it
in neither direction.** The extension's set is its own copy of each of the base's
variants — at the same tag values, each a declaration of its own — followed by the
ones it adds. ▸ **Forbids** substitution in either direction: adding variants to a
set makes a *supertype*, the reverse of the direction every subtype relationship in
Cone runs, and Cone does not take that direction. ▸ **Settles** that a match on the
base stays exhaustive over the base's own variants, which is what the whole closed
family rests on: a base-typed value can only ever hold one of them. ▸ Membership is
one enum per variant: `Colors.Red` is a value of `Colors`, and the extension's copy
`RichColors.Red` a value of `RichColors`.

**Each enum lays out its own set, so neither constrains the other.** ▸ **Settles**
that an extension may add a variant larger than anything its base holds, may differ
from its base about `@unsized`, and never changes the base's layout: an
`Option`-shaped base keeps the bare-pointer layout whatever extends it. ▸ **Forbids**
only what the two still share: the discriminant's type node, so an extension may not
widen it, and — narrowings rather than principles — a requirement or a common field
of its own, since the first would need every copy to implement it and the second
would move what the copies' methods read.

**An enrichment is inside its base's encapsulation boundary; its own clients are
not.** It is acting as the base, which is what declaring the base verifies, so it
reads and writes the base's private members and the names it takes keep the
base's visibility. ▸ **Settles** the asymmetry with a field's fold, which admits
only public members through a public field: the folder is a *client* of the part,
and an enrichment is the *whole*. ▸ ⚠ **What it costs is the fragile base
problem in a new place**: the base's author can no longer change the
representation without breaking an enrichment they cannot see. The coupling is
taken knowingly and SemVer is the protection, which holds because two versions of
a package may coexist in one binary; the residue is that renaming or retyping a
private field at the same size is invisible to everyone but an extender, so a
type that may be extended has its representation in its contract. That is a
documentation obligation and it is discharged in
[refinherit](../../conesite/public/coneref/refinherit.html).

**The enum is the privacy boundary for its variants** — Jon, 23 Sep 2026. A
closed enum is one type written in one place, so code anywhere inside its braces
— the enum's methods (and each variant's clone of them), its statics, every
variant's methods — reaches every variant's private members, and the enum's,
through any value; an extension of it is inside the same boundary. ▸ **Settles**
that `pub` on a variant's member means "part of the enum's interface", where
before it was the only way for the enum's own code to read it. It is the privacy
half of the names rule (everything an enum declares is bare inside its braces).
▸ **Forbids** nothing new, and widens nothing else: a struct's private members are
still reached only through `self`, and a function the module owns is outside
every enum, including one written or instantiated inside the braces. The
mechanism is `structEnumSeesPrivate`, asked by `fnCallLowerMethod` and the type
literal's private-field check; the site is the owner of `pstate->fn`, because
`pstate->typenode` is inherited by a generic function's instance from wherever it
was first called.

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
nowhere** (`genlGlobalSyms` and `genlGlobalImpl`, and for an instance of a
generic trait or enum `genlGenericInstanceSyms`). Miss the last and
`Trait.name()` calls a null, or, in an instance, `genlGlobalImpl` generates a
body for a function that was never declared and the compiler crashes; miss any
of the first three and every implementer stops conforming the moment a trait
declares one. It is reached as `Trait.name`, and an implementer or variant
cannot name it at all: `trait-nameres-static` pins both spellings,
`trait-success` the call, and `enum-success` a generic enum's.

The converse holds for a **method**: the implementers' and variants' clones are
the only copies generated, so the trait's or enum's own is reachable by no
path. `Trait.name` or `Enum.name` naming one — called with a receiver, borrowed,
or through an overload name with a method among its candidates — is refused by
the path collapse with `ErrorAbstractMeth` ([fncall](fncall.md), "The path
collapse"); without that the call loads a null. `trait-nameres-method-path`
pins it.

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
| `namespace` | every named member: fields, methods, macros, overload sets, `Self`, **an enum's variants** — each a `StructNode`, bound at parse, and never a member of the enum's values: a lookup through a value passes one over (`fnCallLowerMethod`) — and what a fold admits — a **copy** of a folded field (a `FieldDclNode` with a `hop`) and an **alias** (`AliasDclNode`) for a folded method, overload set or macro method, for every member of an `extends` base but its fields, `final` and `clone` (those two are copied into `nodelist`, as a trait's defaults are), and for every member a sibling `use` admits. The copies and aliases live here only; `fields` and `nodelist` never hold one |
| `dropfn` | NULL until type check settles the layout, and set before the methods are checked |
| `dclinfo` | owner and the facts its symbols are spelled from — [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols". The owner is a module, or the enum for a variant declared inside one — for an extension's copy of a base variant, the extension, so the copy's methods are spelled after it. Read for one thing besides naming: rejecting a variant declared outside its enum's module, through `dclInfoGetModule` |
| `basetrait` | the **type expression** of the first abstraction an `is` names, or of the enum a variant belongs to — a `NameUseNode`, or an `FnCallNode` for a generic base. **Not a `StructNode*`.** Two helpers unwrap it and they answer different questions: `structBaseTraitDcl` takes **one hop**, to the declaration this type stands on, while `structGetBaseTrait` recurses to the **bottom-most** one. Picking the wrong one is how the infection loop hangs |
| `extendsbase` | the **type expression** whatever base an `extends` names, on the same terms: the concrete type this enriches, or, **on an enum, the enum whose variants join this one's set**. **A separate slot from `basetrait` on purpose**: they are different assertions, a type may write both, and every walk that reads `basetrait` is asking about an abstraction — which is also why an enum's base is here and not there, since no substitution runs between the two enums. `structEnumBaseDcl` unwraps this one for an enum. A generic enum is named here with its arguments (`Option[T]`), an `FnCallNode` until type check replaces it with the instance |
| `extendsdcl` | an **enriched** base's declaration, written once its members have been taken and NULL until then — so it says both *which* type this enriches and *that* the enrichment has happened, which is what tells name resolution's expansion from type check's. `structExtendsRoot` walks it to the bottom of the chain, and `structExtendsEquiv` compares two roots: that comparison is the whole substitution rule. **Always NULL for an enum**, deliberately: an enum extension licenses no substitution, so it writes nothing the rule reads |
| `siblings` | a **field-like node per type-body `use`**, or NULL: its `vtype` the type expression of the sibling named, its `fold` what the clause admits. Never in `fields`, because a sibling contributes no representation; the node type is reused for what it already carries through cloning — a type expression and a clause. Read only by `structUseSiblings` |
| `lifecycle` | unlowered copies of this type's `final` and `clone`, set aside as its layout settles and before its methods are type checked (`structKeepLifecycle`), or NULL. Read only by an enrichment taken after that — in type check, where this type or its enrichment is a generic's instance — since by then the methods themselves are lowered (see Hazards) |
| `derived` | for an **enum**, its variants in declaration order — **an extension's begins with its copies of its base's list**, in the base's order, and they are in no module's node list, so this is how the module walk and generation reach them (`structEnumCopyCount` says how many: those whose `instnode` is the extension). A generic instance's list holds the instances of its template's variants, copies included, put there by `genericMemoize`. A variant is in exactly one enum's list. The index is the `tagnbr` only where nothing pinned one, which is what generation asks before using the tag to index the vtable list |
| `traits` | every abstraction whose members were taken — the base, each further name in the `is` list, and each `mixin` — or NULL. Written where the members are taken (`structInheritTrait`) and read by type check's two requirement checks, the only things that still need to know which trait a requirement came from. **Which entry is the base is asked of `basetrait`, not of this list's order**, since the field walk that fills it runs backwards |
| `fields` | all fields in layout order. A declared field may carry a fold clause (`FieldDclNode.fold`); a folded copy is never here |
| `vtable` | NULL until `structMakeVtable` |
| `tagnbr` | discriminant value, assigned at parse: the value the author pinned, or the next in sequence. `TagUnassigned` is the sentinel between reading a variant's name and settling its value, which is why no flag bit records whether one was written — a type has none to spare, and nothing after parse needs to know. **A variant of an extension keeps the sentinel until name resolution**, which is when the base's values are known and its own can continue from them |
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
- **`is` and `extends` are read in a loop, each once, in either order**, because
  they are different assertions: `is` names the abstractions this type complies
  with, `extends` names the one concrete type it enriches. A repeat of either is
  `ErrorExtends`, as is `extends` on a **trait** — an abstraction holds no value,
  so there is nothing of it to enrich. On an **enum** the keyword means the third
  relationship, the enum whose variants join this one's set, and it is read into
  `extendsbase` like any other base. A **variant** writing it is `ErrorVariantDcl`
  beside the `is` it may not write either: its fields are its enum's, so it has no
  representation of its own to stand on.
- **An enum that extends another declares, beside its variants, only what can be
  given to its copies of its base's variants**: a method with a body, a static
  function and a static (`parseIsEnumExtension`). The rest is `ErrorEnumExtends`
  where it is written, each with its reason: a **requirement** (a method with no
  body), because the copies were written against the base and have no body to meet
  it in; a **common field**, because it would move what the copies' methods read;
  a **discriminant** field, or an integer type for its tag, because its layout is
  its base's, shared with the variants it takes from it, so no discriminant is
  synthesized here either; and a **macro** or a **mixin**, which reach no copy
  (`parseEnumExtensionMember`). A body with no variant at all is the same code,
  reported at the declaration's own name — the block has ended by then, so the
  lexer is on whatever follows it.
- A field's trailing `use` clause (`parseFoldClause`) is stored on the field,
  with an alias per listed name positioned at the item; **nothing enters the
  namespace at parse**, since whether a name is a field or a method is not
  known until the field's type is. `use` on anything that is not a struct's
  field, a struct's body or **a module's global** — a local, a parameter, a
  static, a mixin, an `is` clause, a trait's or an enum's field or body — is
  `ErrorBadFold` there, so the diagnostic is the fold's own. **A module's global
  does fold**, because a module is the namespace a name would fold into and a
  global is its one-instance analogue of a field
  ([Names and Namespaces](../phases/names-and-namespaces.md), "Folding through a
  global"); `parseVarDcl` admits the clause only where `ParseMayFold` says so,
  which is the module-level declaration alone. **`is` names abstractions and
  folds nothing**: an abstraction has no value for a folded name to be reached
  through, and delegation is what a field's own clause is for.
- **`pub use` is refused at every type-level site**, and so is the retired
  `use pub`, `ErrorBadPub`: a folded
  member of a field's type is as visible as the field it is reached through, and
  a sibling fold declares no name of its own. Only a module's global has a
  visibility of its own to declare.
- **A long list may be written as a block**, `use { a, b as c }`, at any site. The
  braces hold the list and nothing else, so a block is never a star clause and
  `but` has no place in one.
- **A `use` standing as a statement in a struct's body folds a SIBLING in**
  (`parseUseSibling`): the type it names, then what it admits of it. Held in a
  field-like node on `siblings`, as `mixin` and a further `is` are held in one, so
  that one node type carries a type expression and a fold clause through cloning —
  and off the field list, because a sibling contributes no representation. It
  declares no name of its own, so `pub` before it is `ErrorBadPub`; each folded
  name carries its target's visibility instead. **Naming the sibling is what asks
  for its whole member set**, so a bare clause is a star clause and `*` written
  out is `ErrorBadFold`: one spelling, and `but` still narrows it.
- **A variant has two spellings and they build the same node.** A `struct` written
  inside an enum is one; so is a **bare name**, which declares an empty struct
  variant and is what lets one construct serve a plain set of named symbols as
  well as a set carrying payloads. Both set `HasTagField` on the enum, take the
  enum's generic parameters, get a synthesized `basetrait`, and are put on the
  module's node list, so the module's walks resolve, check and generate them. **A
  variant's NAME is bound in the enum's namespace**, not the module's: it is
  reached as `Colors.Red`, two enums may each have a `Quit`, and it is bare in a
  module only where a module's `use` of the enum folds it in
  ([module](module.md)). It is owned by the enum, so its symbols are spelled after
  it.
  - **A variant shares the enum's one namespace with its fields and methods**, so
    a variant of the same spelling as either is `ErrorDupName`, reported on
    whichever is written second.
  - **What tells a bare-name variant from a common field is the token after the
    name**: `,`, `;` or `=` makes it a variant, and anything else is a type, so it
    is a field. The field node is therefore built while the lexer is still on the
    name, before the choice is made, so that a diagnostic about either points at
    the name rather than at what follows it.
  - **A variant restating what the enum decides is `ErrorVariantDcl`** — its own
    `is`, or its own generic parameters. One code for both, because a reader
    would not branch on which and the remedy is the same: delete it.
- **Tag numbering runs across the whole body**, ascending from zero, and a written
  value resets it, so numbering continues from there — **except in an extension,
  whose numbering waits for name resolution**, since the values its base's variants
  hold are not known until the base is. Two variants holding one
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
  set, so an empty one names nothing a value of it could be. An extension with none
  is `ErrorEnumExtends` instead: it would hold its base's, so what it lacks is not a
  value but a reason to exist.

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
   check begins, so nothing can look the name up too early. **An extension's waits
   for step 4c**, because which comparison it gets depends on whether any variant in
   the whole set — its base's included — carries fields.
2. Push the hook table. **A variant pushes one more first, beneath it, and hooks its
   enum's namespace there** (`structEnclosingEnum`, from the variant's owner;
   `structHookEnclosingEnum`), all of it but — for an enum that is not generic — the
   methods a value answers, which the variant has as its own clones, hooked nearer.
   A generic enum's clones join only once an instance is type checked, so its
   methods and method overload names are hooked from the enum: the use binds to the
   enum's, the instance's clone maps it to the enum instance's (`genericMemoize`),
   and type check lowers a bare method call to `self.name`, found by name in the
   variant, whose own clone it is. The reason for all of it:
   anything written inside an enum's braces sees every name the enum declares bare,
   and a variant's body is written there, but a variant is a module node resolved on
   its own, so the enum's hooking at step 7 never reaches it. Beneath, so every name
   the variant hooks itself — `Self`, its generic parameters, its fields and
   methods, what it inherits — wins a clash with one of the enum's. The enum is
   demanded before it is hooked, because an extension's namespace holds its copies
   only once it is resolved (step 4c). **An extension's bases' names are hooked in
   the same frame** (`structEnumHookBaseNames`, as at step 9a), so a variant the
   extension adds sees them bare too. Both frames are popped at the end. An
   extension's copy of a base's variant never reaches here: it is cloned resolved,
   so its bodies keep what they bound in the base's scope.
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
4a. **Resolve an `extends` base and demand it too**, for the same reason, and
   check here that it may be enriched at all (`structExtendsEligible`). Taking
   its members waits for step 8a, after the field walk, so that whatever is left
   in the field list by then is a field this type declared — which is what
   `extends` forbids.
4c. **An enum's `extends` is taken here, whole** (`structEnumWrittenBase`,
   `structEnumOwnNamesFresh`, `structEnumSeedVariants`): the base is resolved and
   demanded as at 4a, a method or static this enum declares under a name the base
   has, anywhere down its chain, is `ErrorExtendsOverride`, each of its
   variants is demanded and **copied** (`structEnumCopyVariant`) to the front of this
   enum's `derived` list and bound in its namespace, this enum's own are numbered from
   the base's last value, its `==` is made, and the base stands as a **mixin
   placeholder at position 0** so the field walk splices its fields in exactly as a
   variant's enum does — or, for a generic base named with its arguments, leaves it
   standing for type check, as a generic variant's enum is left. Before step 7, so
   the copies are names of this enum when its namespace is hooked. A clause that is
   refused is cleared, so nothing downstream asks about it again.
4b. **Resolve each sibling a body `use` names**, for the same reason again. It is
   demanded at step 5 with everything else, which is what makes its own `extends`
   taken before the base it shares is compared with this type's.
5. **Demand each trait a placeholder names, the type of each field that
   carries a fold clause, and each sibling** (`structNameResDemand`): resolve it now, in its own
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
7. Hook the whole namespace. An enum's holds its variants, so the enum's own
   method bodies name them bare with no `use`; a variant's bodies have the same
   names from the frame step 2 hooked beneath its own.
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
   does not exist yet.
8a. **Take the concrete base's members** (`structEnrichFromBase`, under
   "Enrichment" below): its fields copied in at the front, its `final` and
   `clone` cloned into this type's methods with `Self` retyped, every other member
   of it entered as an alias. Here rather than in the walk, because the walk is what
   removes the placeholders, and before the two steps below, so that the copies
   are indexed with everything else and a fold clause that came across on a copied
   field is expanded against the copy.
8b. **Expand each body `use`, in the order written** (`structUseSiblings`, under
   "Sibling folding" below). After the base, because the shared base is what
   licenses the fold and this type's own members must be in place for a collision
   to be reported at the clause that caused it. Then index the fields, so that a
   copy a fold makes next takes the index of the field it stands for.
9. **Expand each fold clause, in field order** (`structFoldExpand`, under
   "Name folding" below), after every trait's members are in place, so a folded
   name colliding with an inherited one is reported at the fold. A clause on a
   field whose type is not a declaration yet — an instance of a generic — waits
   for the instance's type check.
9a. **An enum that extends another hooks its bases' names** (`structEnumHookBaseNames`),
   down the whole chain, in the frame its own are hooked in, skipping every name it
   already holds and every name a nearer base holds: anything written inside an
   extension's braces sees its bases' names bare, exactly as it sees its own, and
   the nearer name wins. Last, because by now its namespace holds everything it
   will — the copies, the clones of a base's methods the walk spliced in — and a
   frame is unhooked in the order it was hooked, so one name hooked twice in it
   would be restored to the first hook rather than to what was there before. See
   "An enum extending an enum".
10. Resolve the methods declared here — only those; the clones arrived resolved
    in the trait's scope, and this walk cannot be repeated on a node. **An enum
    extending one that is not generic then clones its own bodied methods into its
    copies** (`structEnumCloneOwnMethods`), which arrived resolved at step 4c with
    their enum's members already spliced; see "An enum extending an enum".
11. Pop, and mark `NameResolved`.

**Reached by demand.** `structNameResDemand` is the one place name resolution
leaves walk order, and it is confined to a type declaration reached from
another type declaration, so what is hooked at the jump is known: module names,
and the demanding type's generic parameters. A type in another module resolves
with that module's namespace hooked over the current one, and that namespace
already holds everything the module folded in, because every module's folds run
before any module's body is resolved. What the demand asks for first is
`modFoldNames` on that module, which is a no-op except where the fold pass itself
is what reached the type — a global's fold in one module demanding a struct of
another. See [module](module.md).

## Type check

`structTypeCheck` is the longest ordered sequence in the compiler:

1. **A template returns immediately** — only clones are checked.
1a. **An `extends` base name resolution could not take is taken here** — this
   type is an instance of a generic, or the base is, so one of them was not a
   declaration until now. Eligibility is checked exactly as name resolution checks
   it; the members are taken at step 4a. Nothing is hooked, because no body is
   resolved after this, so such a member is reached as `self.name` inside this
   type's own methods (see Hazards). **An enum this one extends is type checked
   here** and nothing else: name resolution took its variants and members already,
   and what is wanted from it now is its discriminant's width, settled at step 5a on
   the node the two of them share. A generic base was written with its arguments,
   so `extendsbase` is an instantiation, type checked here into the instance it
   names; the placeholder standing for it is expanded at step 4, as a generic
   variant's enum is.
2. Type check `basetrait`; require an abstraction — `ErrorInvType`, since a
   subtype relationship runs from a concrete type to an abstraction and never
   between two concrete types; require the closed-ness to match —
   which is what refuses a type outside an enum joining its variant set;
   propagate `SameSize`/`HasTagField` down from the bottom-most base, and
   require a closed type's derived types to share its module. A base
   name resolution did not expand — it is in `traits` when it did — is an
   instance of a generic that exists only now, so **insert a mixin placeholder
   for it at index 0** as name resolution would have. Where `traits` holds the
   *template* of the generic `basetrait` names, the mixin was done before the copy
   was made (a generic enum's copy of a variant of an enum that is not generic), and
   the entry becomes the instance.
3. Type check every trait in `traits`, so each is laid out before this type is.
4. **Walk fields backwards.** Backwards so that splicing does not invalidate the
   cursor. An ordinary field is type checked. A placeholder still standing —
   the generic case — is expanded exactly as name
   resolution expands one (`structInheritTrait`), except that nothing is
   hooked: no body is resolved after this. Such a type's inherited members
   cannot be named bare (see Hazards).
4a. **Take the concrete base's members** where step 1a found one to take, as name
   resolution takes them at its own step 8a and in the same place in the order.
   **Then any body `use` name resolution could not expand** — this type or the
   sibling was an instance of a generic, so one of them was not a declaration until
   now. Nothing is hooked, so such a member is reached as `self.name` inside this
   type's own methods (see Hazards). Then expand any fold clause name
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
   set once here — **and so does every enum extending this one**, which is why an
   extension may not widen it: the node is the base's, and widening it would relay
   out the base's own values for the sake of a set the base knows nothing about. A
   value that does not fit is `ErrorTagWidth` at the extension, the same question a
   declared integer type asks. **An instance of a generic enum skips this step** —
   `genericMemoize` checks it through `structTypeCheckEnumInstance`, with its
   `derived` already listing every variant — and `genericMemoize` settles the width
   once the instance and its variants are checked: once per generic, since the
   template and every instance share the node and the tag values, and measuring
   each instance would report a declared type's overflow once per instance.
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
   `fn twin(self) Self`. Just before it, a type that may be enriched sets its
   `final` and `clone` aside unlowered in `lifecycle` (`structKeepLifecycle`), for
   an enrichment taken after its methods are checked; so an enrichment reads
   `TypeChecked` on its base to know which to copy.
8. **`structSetDropFn`** — validate a `final` method, then, if any field's type
   has a drop function, synthesize a `drop` method, owned by the type so its
   symbol is spelled as any method's — `Bundle.drop`, `_CNvNt6Bundle4drop` —
   calling `final` and then each droppable field. The
   generated body is built pre-lowered and is **never type checked or flow
   analyzed**. **Before the methods, and load-bearing**: each method's flow pass
   asks `itypeGetDropFnDcl` about its by-value `self` and its locals of this
   type, and a `dropfn` still NULL then finalizes neither. The fields are
   checked by now, so every field's drop function is known. **A trait or an
   enum synthesizes none**: its `dropfn` is its own `final` or NULL. A method
   of a trait is its implementers' — cloned into them, required of them, never
   generated for the trait — so a synthesized `drop` there was one more
   requirement, which no variant or implementer could meet (its own `drop`
   takes its own `self`), and a generic enum's instance cloned it into each
   variant, which the pre-lowered body cannot survive. An enum's common fields
   are spliced into each variant, so the variant's own `drop` finalizes them.
9. Type check every method — those in `nodelist` before step 8, so not the
   generated `drop`.
10. **Verify the traits' method requirements** (`structCheckTraitReqs`), now
   that every signature has its types: for each method of each trait in
   `traits`, the type's binding for the name must have the one candidate of the
   trait's signature — an inherited default meets that by construction — and a
   requirement with no body, inherited as such, is unmet in a struct; a trait
   may pass it on.

## Name folding

A field's `use` clause (`FoldClause`, on the `FieldDclNode`) admits names of
the field's type as names of this type. The language is in
[refinherit](../../conesite/public/coneref/refinherit.html); this is the
mechanism, in `structFoldExpand` and what reads its results.

**What every fold site shares lives in `ir/stmt/fold.c`**, lifted there when a
module's global became the clause's second kind of source: the declaration behind
a type expression (`foldSourceDcl`), a star clause's items and its `but`
(`foldStarItems`, `foldExcluded`), and which members a star admits
(`foldAdmitsOwn`). What stays here is what the *site* decides — the copy with its
hop, the receiver shift, the eligibility rules — because that is where the sites
differ.

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
or an enum (`ErrorBadFold`, the enum tested first because it carries
`TraitType` too) and complete — not this type, not one still being resolved
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

`structMatches` answers two unrelated questions, and the second one is under
"Substitution" below. For subtyping it refuses anything but "a trait is a
supertype of a struct".

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

## Enrichment

`extends` names a concrete base whose members become this type's, and whose
values and this type's substitute for each other freely. The language is in
[refinherit](../../conesite/public/coneref/refinherit.html); this is the
mechanism, in `structEnrichFromBase` and `structExtendsEquiv`.

**It is a name fold, bar the value's lifecycle, and that is the whole finding.**
One representation means a base method already takes exactly the right receiver,
so there is no clone to make, no signature to retype and no receiver to shift.
Beside the field fold above, the two are the same operation reached from opposite
ends: a field's clause reaches the *part*, so it needs a hop and a receiver
rewrite, and an enrichment *is* the whole, so it needs neither. Measured: a base
method is one symbol however many types reach it, where an inherited trait default
is a copy per implementer (`struct-extends`, `a-base-method-is-not-cloned-per-enrichment`).

**Three things are not aliases.** The base's **fields are copied**, because a field
node carries its index and its own check state and each type lays its own out;
the copies are this type's declared fields in every respect, so they satisfy an
`is` field requirement, fill a vtable slot and are constructed positionally
exactly as fields written here would be. And what the base's own **fold clauses**
admitted is left alone: the clause travels with the field it is written on, is
cloned unexpanded (`cloneFieldDclNode`) and is expanded again here against this
type's copy of that field — which is what keeps a folded name's hop pointing at a
field of the type that holds it. So the base's delegated names come across too,
and nothing about them is re-pointed by hand.

And the base's **`final` and `clone` are cloned** (`structEnrichLifecycle`), with
`Self` retyped to this type, exactly as a trait's default is cloned into an
implementer (`structInheritTrait`). They are the value's own lifecycle rather than
something a call reaches: `structSetDropFn` reads `final` off this type's
namespace expecting a method whose receiver is this type, and the drop function it
generates is this type's, called for every value typed as it. An alias would be
read there as a malformed `final`, and leaving them out would leave the base's
finalizer unrun for every value typed as the enrichment. So the copy is this type's
own method, spelled after it (`LoggedFile.final`), and a value is finalized by the
same body under either name; a `clone` written against `Self` hands back the
enrichment. The drop this type generates is built exactly as the base's is: its
copy of `final` first, then each field that finalizes — and those fields are the
base's, since an enrichment adds none. The body is bound in the base's scope, so a
private method of the base called bare from it runs on the enrichment's receiver,
which substitutes. What is copied must be unlowered, and in type check the base's
own methods no longer are, so there the copy is taken from what the base set aside
(`lifecycle`; see Hazards). Measured in `struct-extends-lifecycle`, and across a
module boundary in `struct-extends-import`. Neither folds from a **sibling**: a
type folding one has its own copy from the base they share.

The base's **`@move` and `@opaque` flags are carried** (`MoveType`, `OpaqueType`,
`DeclaredOpaque`, OR'd onto this type as the members are taken). What the base's
fields and `final` make of it is inferred again from the copies at layout, but an
attribute exists only as the flag parse set on the base, and the carry is what
makes an enrichment of an `@move` type move and one of an `@opaque` type refuse a
value. Measured in `move-flow-infection` and `move-success` for `@move`, and
`struct-typecheck-nosize` for `@opaque`.

**Every other member becomes an alias** — methods, overload sets, macro methods
and statics alike, private ones included, under the base's own visibility. A
static keeps its owner and its signature, so the base's factory reached as
`Gauge.make()` still hands back a `Meter`; binding that value to a `Gauge` is how
a value of an enriched type is obtained from a library that never heard of the
enrichment.

**What the enriching type may not do**: declare a field (`ErrorExtendsField`,
which is what keeps the representations identical — a field that owns a resource
included, so what its values finalize is exactly what the base's do), or declare a
name the base already has (`ErrorExtendsOverride`, whether the base's is a field or
a method, and a `final` of its own over the base's among them) — one value would
otherwise mean two things, depending on which name reached it. An enrichment of a
base that declares no `final` may declare one, since that is not an override
(see Hazards for what it costs).
**What may be enriched** is a concrete struct and nothing else: not an
abstraction, which holds no value; not an enum, where adding is adding variants and
only another enum may (see "An enum extending an enum"); not a variant, whose fields
are its enum's; not a number type. A cycle, including
a type naming itself, is `ErrorCircular` at the clause.

### Substitution

`structExtendsEquiv` is the whole rule: two types substitute for each other
exactly where `structExtendsRoot` — the bottom of the `extends` chain — answers
the same declaration for both. So it covers a chain of any depth and two
*siblings* that named one base, and it answers about the declarations rather than
about the shapes. **The declaration is the licence; identical representation is
only the argument for why the licence is sound.** Two look-alikes that named no
base have no licence, and nothing lifts through a container.

Three places read it, and each is a different half of "in both directions at no
cost":

- `structMatches`, ahead of its trait test, answering `CastSubtype` under every
  constraint including `Coercion` — a by-value crossing is a recast, which
  `genlRecast` lowers through a store and a load, the sizes being equal by
  construction.
- `refMatches`, in the **invariant** `&mut` case, which is the one exception the
  language grants to invariance: neither type adds state the other could break, so
  a write of either through a reference to the other stores one representation.
  It answers `CastSubtype` rather than the permission's own verdict, because the
  two are still distinct types and the pointer has to be recast — without it
  generation passes a pointer to one where the signature says the other, which
  LLVM's module verifier rejects and a plain compile does not.
- `structFindSuper` and `structRefFindSuper`, where either name stands for both,
  so an inferred type in common is whichever was seen first.

⚠ **It answers about two distinct declarations only.** One declaration is not
substituting for anything, and every caller has asked that question already;
`structMatches` is reached with equal types from `cast.c`'s narrowing check, and
answering yes there swallowed the diagnostic that says a variant is already as
narrow as it gets.

### Sibling folding

A `use` in a struct's body folds in a **sibling**: another type that declared
this type's base. So one base plus two libraries that each enriched it become one
type, declared once, in the namespace of whoever needs it and without either
library being touched. The language is in
[refinherit](../../conesite/public/coneref/refinherit.html); this is the
mechanism, in `structUseSiblings` and what it calls.

**The shared base is the whole licence, and it is the substitution rule above
doing the work.** A sibling's method takes a receiver of the sibling's type, and
this type's values substitute for that type because both declared one base — so
`structExtendsEquiv` answers at the call and there is nothing else to do. Which
is why the fold makes **only aliases**: no field copied, no receiver shifted, no
signature retyped, no body cloned. `structUseSiblingEligible` is therefore the
only new rule, and it asks one question — does `structExtendsRoot` answer the same
declaration for both — plus what cannot be a sibling at all: a type with no base
of its own to share, an abstraction, an enum, itself, and **the base or anything
along the chain down to it**, which `extends` has reached already so that folding
it would collide on every name rather than add one. All `ErrorUseSibling`.

**Only what the sibling declares itself folds** (`structUseSiblingOwns`). A field
in its namespace is the representation both types take from the base, so it is a
field here already; an alias there is what the sibling took from that same base or
delegated through a field of its own — the first arrives here by the same route,
and the second is reached by naming the type it came from. What is left is the
sibling's own contribution, which is the reason to name it; a listed item that
names anything else is `ErrorBadFold`.

**Otherwise it is the field fold's rules, read off the same `FoldClause`.** A
name the sibling lacks is `ErrorNoMbr`, in the list and after `but` alike; a
private one is `ErrorNotPublic`, because a sibling is inside the *base's*
encapsulation boundary and not inside this type's, so what it declares privately
stays its own; `final` and `clone` are its values' lifecycle and do not fold,
ahead of the visibility check since that holds whether they are public or not; and
a name already taken — by a declared member, an inherited one, the base's, or
another sibling's — is `ErrorDupName` at the clause, **which is the collision this
construct exists to let one declaration settle, with `as` or `but`, without
touching either source.** A static folds and keeps its owner, as it does from a
base: aliasing is what a static fold means everywhere, and `Rich.origin()` still
hands back the base type.

**A sibling still being resolved is `ErrorCircular`** at the clause, reported on
whichever of a mutually folding pair was reached by demand.

**Nothing in generation changes.** There is no thunk and no vtable case of its
own: an alias resolves to the sibling's declaration, and the call is a direct call
to it with the receiver recast, exactly as a call on a value of the sibling's own
type is (`struct-use-sibling`, `a-folded-method-is-not-cloned-per-folding-type`).

## An enum extending an enum

`extends` on an enum names the enum whose variants this one copies into its set. The
language is in [refenum](../../conesite/public/coneref/refenum.html), "Extending an
enum"; this is the mechanism, in `structEnumSeedVariants`, `structEnumCopyVariant`
and what reaches the copies.

**Each base variant is copied the way a generic template is instantiated.** At the
extension's name resolution (step 4c above) each of the base's variants is demanded
resolved and then cloned (`cloneNode`, as `genericInstantiate` clones a resolved
template), with `Self` inside the copy meaning the copy. So every name inside it
stays bound to what it named in the base's scope, and nothing is resolved again in
the wrong module. The copy is then made the extension's:

- its `basetrait` names the extension, which is what its membership, its type check
  and its layout follow;
- the base in its `traits` list becomes the extension, so it answers the
  requirements the extension inherited from the base;
- its `SameSize` flag is the extension's, so it is padded or not as the extension
  says;
- its owner is the extension (`dclInfoJoin`), so its methods' symbols are spelled
  after the extension and never collide with the base's;
- it is bound in the extension's namespace. An added variant of the same name is
  `ErrorDupName`, at the added one.

The copies go first in `derived` and in the base's order, and the extension's own
variants are numbered on from the base's last value, so **every copy has its
original's tag value by construction**. The base stands as a mixin placeholder at
position 0, so the field walk gives the extension its base's discriminant and common
fields.

**A generic enum may stand on either side of the clause.** A generic base is written
with its arguments — `extends Option[i32]`, or `extends Option[T]` from an enum
passing its own parameter on — and what is copied is then the base's variant
*template* (`Some`, a name-resolved `StructNode` with its own `genericinfo`, built
by `parseAddVariant`), cloned with the base's parameters substituted by those
arguments: the substitution `genericInstantiate` performs, through
`clonePushState(parms, args)`. Two passes, because substitution hooks a
parameter's **name** to its argument, and an argument may name one of the
extension's parameters spelled like the base's (`Pending[T] extends Option[T]`,
or crosswise, `Flip[T, E] extends Result[E, T]`). In one pass the `T` inside the
argument would be taken for the base's `T` and substituted again, without end. The
first pass renames the base's parameters to stand-ins (`-extends-T`, which no source
can spell); the second puts the arguments in their place.

- **A generic extension's copy is a generic template of the extension**, made as
  `parseAddVariant` makes a variant of a generic enum: `genericinfo` for the
  extension's parameters, and a `basetrait` written `Pending[T]`. An instance
  `Pending[i32]` instantiates the enum and every variant in its `derived` list,
  copies included, through `genericMemoize`'s tagged path — the path every generic
  enum's variants take. Nothing new instantiates.
- **An extension that is not generic, of an instance** (`IntOrWait extends
  Option[i32]`), gets ordinary variants: the arguments are concrete, so the copy is
  the instantiation, made at name resolution.
- **Either way the base is an instance**, which exists only at type check. So its
  placeholder is left standing at name resolution, as a generic variant's enum is
  (`structNameResTrait` answers NULL for an instantiation), and holds its own clone
  of the written instantiation; type check replaces `extendsbase` with the instance
  and expands the placeholder. The copies inherit the same: each splices the
  extension's fields in at its own type check.
- **A generic extension of an enum that is not generic** (`Labeled[T] extends
  Shape`) copies variants that already hold their enum's fields, spliced at their
  own name resolution, and records the extension's template in `traits` where the
  base was. So an instance of such a copy is not given the fields a second time:
  `structTypeCheck` takes a recorded template of the generic its `basetrait` names
  as the mixin already done, and replaces it with the instance.
- The base's own layout is untouched: its instances' `derived` lists hold only its
  own variants, so `Option[&i32]` is still a bare pointer beside a tagged
  `Pending[&i32]`.

The arguments must be written, as many as the base has parameters, and each a type
or one of the extension's parameters: `extends Option` has nothing to put in
`Option`'s `T`'s place. A count that does not match is `ErrorArgCount`, at the
clause, and an argument that is no type is `ErrorNotType`.

**A copy is no module's node.** Like a generic instance it is reached through what
made it: `structEnumCopyCount` says how many of the extension's `derived` list are
copies — those whose `instnode` is the extension, which is also what answers none
for a generic extension's *instance*, whose copies' instances are reached through
their templates' `memonodes` like every other instance. The module walk type checks
the copies right after the extension
(`structEnumCheckCopies`, from `modTypeCheck`; a generic extension's are templates,
and return at once), and generation reaches them from the extension
(`genlGlobalSyms`, `genlGlobalImpl`, ahead of a generic's early return, since a
generic extension's copies are how their instances are reached). Not from the
extension's own type
check: that is often demanded from inside an added variant's, which checks its enum
first, and a copy's method body that builds that variant by value would then find it
still in flight.

**Anything that needs an extension's variants before its name resolution demands
it** (`structEnumDemandSet`): a module's `use RichColors;` in the fold pass, which
then folds the copies with the added variants, and a path `RichColors.Red` in
`fnCallNameResPath` — asked only for a name the namespace does not have yet, since an
added variant is bound at parse. A path can be resolved in the middle of a function
body, so the demand clears the body's block scope and hooks the extension's module
over it. ⚠ **A base variant's body may not name the extension's copies**: the copies
exist only once the base variants are resolved, so that is a cycle, reported as
`ErrorCircular`.

**The two enums are two types, and nothing substitutes between them.** It has to be
enforced, because the discriminant is one shared node and the common fields are
clones: `structMatches`'s structural test would say yes in *both* directions and a
virtual reference's mapping would find every slot. Two `EnumType` declarations never
match, in `structMatches` ahead of the trait test and in `structVirtRefMatches` ahead
of the mapping. The extension's base therefore lives in `extendsbase` and never in
`basetrait`, and `extendsdcl` stays NULL: those are what the substitution walks read.

**Membership is one enum per variant.** A variant is a value of the enum its
`basetrait` names and of nothing else, and `structMatches` asks exactly that of an
enum, padded or not, before any structural test: `Colors.Red` is not a `RichColors`,
and the copy `RichColors.Red` is not a `Colors`. Two variants of one set infer that
set as their type in common by the ordinary rule, since a copy and an added variant
name the same enum.

▸ **What the refusals buy is the exhaustive match.** A base-typed value can only hold
one of the base's own variants, so a match naming those is exhaustive with no `else`.

**An extension's own methods reach every variant of its set, its copies included.**
A method with a body declared on the extension is a default of the extension, as a
base's is of the base, so each of its variants has its own clone, with `Self` the
variant; the base's variants never see it, and neither does the base. The variants
it adds get it the ordinary way, from their enum's placeholder (`structInheritTrait`).
The copies need it given, and when depends on how the copy was made:

- **Over a base that is not generic**, a copy arrives resolved with its enum's
  members already spliced, and its `traits` names the extension, so nothing would
  splice the extension's in. `structEnumCloneOwnMethods` clones them in at the end
  of the extension's name resolution, once its own methods are resolved, under a
  clone state whose `Self` is the copy. That holds for a generic extension of such
  a base too (`Tagged[T] extends Shape`): its copies are templates, and an instance
  of one clones what it holds, so the method's `T` is substituted by name like the
  rest of the copy.
- **Over a generic base**, a copy is made from the base's variant template, which
  was never spliced, so it is spliced at its own type check from the extension —
  or the extension's instance — that its `basetrait` names, and the extension's
  methods come with the base's, as they do for an added variant. Nothing new.

A name the copy already answers is left to it, as a variant's own method wins over
its enum's default: a base's variant may have declared the name itself, and keeps
its own in every copy of it. Called through a reference to the extension, the
variant's clone runs, selected by the tag, exactly as for a base's method. **That
the clone is in every copy is what keeps the vtable whole**: `structMakeVtable`
gives every public method of the extension a slot and prewires one implementation
per variant, in tag order, and `structAddVtableImpl` adds nothing for a variant that
lacks the method, which would shift every later variant's vtable onto the wrong
tag. A static function and a static stay the extension's own, as a base's do: named
bare inside its braces and qualified from outside, and never cloned.

**Inside an extension's braces its bases' names are bare too**, down the whole
chain — their static functions, statics and methods, generic or not — exactly as
its own are, in its own methods and static functions and in the variants it adds
(`structEnumHookBaseNames`, steps 2 and 9a). That is the rule for everything written
inside an enum's braces, and it is the rule for `mod A extends B`, whose `A` has
`B`'s declarations as its own names; an extension already reads its base's private
names. A name the extension holds itself is the nearer one and wins: a copy of a
base's variant, a clone of a base's method, a variant it adds, and a variant's own
member or local. It cannot redeclare a base's name (`ErrorExtendsOverride`), so
that is all a clash can be. Its copies of the base's variants keep what they bound
in the base's braces, which are the same names. What each kind of name binds to:

- **A base that is not generic** gives what it declares directly: its methods are
  already the extension's, as clones spliced in at step 8, and its statics and
  static functions are bound to the base's own, which have symbols.
- **A generic base** has members only per instance, and the instance the extension
  stands on exists only at type check, so a bare name is bound at name resolution
  to the base template's member. Type check points it at the instance's before
  anything reads it (`nameUseBaseInstanceMember`, `structEnumBaseInstanceMember`):
  from the type whose function is being checked — the extension, its instance, a
  variant it adds or a copy — up the resolved `extendsbase` chain to the instance
  listed in the template's `memonodes`, and the member of that name there. A static or a
  static function is then called as the instance's; a method or a field is lowered
  to `self.name`, by name, as the extension's own are, so `self` is this variant
  and the method its own clone. The same happens to an overload name, in
  `fnCallTypeCheck` before the set is selected from. A **qualified** name is not
  re-pointed: `Crate2.make()` still asks for the template's member and is still
  refused (`nameUseTemplateMember`).

**A name the base has is not declared again** (`structEnumOwnNamesFresh`,
`ErrorExtendsOverride`, at step 4c, before the copies are made): not a method's —
whether redeclared or overloaded — and not a variant's, a static's or a static
function's, nor the `==` every enum is given. The copies answer each such name the
base's way, so a second declaration would give it two meanings in one set. The
check walks the whole chain rather than the base's namespace alone, because a
generic base holds what it inherits from its own generic base only per instance.

**Privacy runs up the chain, not across it.** An extension is inside its base's
privacy boundary (Principles), so `structEnumSeesPrivate` walks from the enum that
owns the code being checked down `structEnumBaseDcl` looking for the enum the
receiver's type belongs to. A copy's clone of a base variant's method is owned by
the copy, whose enum is the extension, so it reaches what the extension's own code
reaches — the base's variants' privates included. So does the extension's own code:
its method's clone in a copy is owned by the copy, and its static function by the
extension. The walk never goes the other way,
so the base does not reach an added variant's privates, and two extensions of one
base do not reach each other's: the sibling rule an enrichment keeps (Name folding)
holds here too. enum-privacy and enum-typecheck-privacy pin both directions.

**What an extension may not do**, all `ErrorEnumExtends` unless named otherwise:
declare a requirement, since a copy has no body to meet it in; declare a common
field, since it would move what the copies' methods read; declare a macro or a
mixin, neither of which reaches a copy; declare a name its base has, down the chain
(`ErrorExtendsOverride`); declare a discriminant or the integer type one is laid out
in; extend anything but an
enum, or itself; name a generic base without its arguments, or with the wrong number
(`ErrorArgCount`); pin a value the set already holds (`ErrorDupTag`) or one too wide
for the shared discriminant (`ErrorTagWidth`); or add a variant named as a copy is
(`ErrorDupName`). ⚠ **For a generic extension a tag too wide goes unreported**, as
it does for every generic enum: an instance is type checked before `genericMemoize`
fills its `derived` list, so `structSetTagWidth` finds no variant to measure, and a
pinned value past the discriminant's width is truncated where it is stored.

**A chain — an extension of an extension — needs no mechanism of its own.** It
works by the same demand: the middle enum makes its copies while it is resolved,
and they are what the outer one copies, so every copy keeps the tag value it was
declared with and the whole chain shares the bottom enum's discriminant. A method
the middle declares comes along the same way: its copies and the variants it adds
have it by the time the outer enum copies them. It is claimed language
(refenum.html, "Extending an extension"), plain and generic, and enum-extends pins
it. The outer enum sees every level's names bare: the chain is walked for them.

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
  at all**; the value *is* the pointer and null is the empty variant. Each enum
  decides it for its own set, so an `Option`-shaped base keeps it whatever extends
  it: the extension's copies are other declarations, and with a third variant the
  extension is tagged.
- **Same size** — every variant re-emitted with `[N x i8]` trailing padding to
  the largest, rounded up to the strictest variant alignment; the enum's body is
  its own fields (tag and common fields) and then bytes to that size, never a
  copy of one variant's layout, whose padding a first-class load or store would
  drop along with any other variant's field that sits in it.
  Measured: `%Circle = { i8, i32, i32, [4 x i8] }` beside
  `%Rect = { i8, i32, i32, i32 }` and `%Shape = { i8, i32, [8 x i8] }`.
- **Unpadded** — each variant emitted at its own size, the tag still first.
  Measured, for an `@unsized` enum of an empty variant and one holding three
  `i64`s: `%Ping = { i8, i32 }` beside `%Payload = { i8, i32, i64, i64, i64 }`.

**The discriminant's width is not generation's.** Type check settles it, because it
follows the largest tag value rather than the variant count and generation cannot
see a pinned value in `derived->used`.

**Every variant is in one enum's list**, an extension's copies being declarations of
their own, so the memoized `llvmtype` slot is set once, by its own enum
(`genlSameSizeTrait`), and each set is padded to its own largest. The copies' named
structs share their originals' spelling, which LLVM numbers apart (`%Circle`,
`%Circle.26`).

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
- **Mixing in two enums brings two tag fields**, and no duplicate-name error fires
  because `namespaceAdd` silently ignores `_`, so what reports it is type check's
  one-discriminant rule. Traits carry no tag, so a chain of `is` bases and any number
  of `mixin`s meet nothing here.
- **A member taken from an `extends` base, or folded from a sibling, cannot be
  named bare where either side is a generic.** Both are taken in type check
  there, after every body has been resolved, so `self.name` is how such a member
  is reached inside the folding type's own methods. Where both are plain
  declarations there is no such limit, and `struct-extends` and
  `struct-use-sibling` name everything bare.
- **A default method cloned from an instance of a generic trait, or a member
  folded from a field whose type is a generic's parameter, cannot be named bare.**
  The instance exists only when type check instantiates it, so what it contributes
  joins the type's dictionary after every body has been resolved; such a name is
  reached as `self.name`. A trait or field type that is a declaration when the type
  is resolved has no such limit. **Only cloned defaults are affected** — a trait
  contributes no fields, so a field of a generic trait is a requirement the type
  declared for itself and is an ordinary member of it.
- **A method that has been type checked is no longer fit to copy.** Type check
  lowers a body in place — a bare method call is given its receiver — so a copy
  of a checked body, checked again as another type's method, is lowered twice and
  refused (`ErrorNoCandidate` on the doubled receiver). Name resolution copies
  before any type check begins, so it never meets this. An enrichment taken in
  type check does, because its base was checked first, which is why a type sets
  its `final` and `clone` aside unlowered in `lifecycle` as its layout settles
  (`structKeepLifecycle`) and an enrichment taken after that copies those
  (`structEnrichLifecycle`). `struct-extends-lifecycle` pins both generic
  shapes with a finalizer that calls a method bare.
- **A finalizer an enrichment adds runs only while the value is typed as the
  enrichment.** Where the base declares no `final`, the enrichment may declare one,
  and it is this type's drop and not the base's. A value that crosses to the base's
  name by value is dropped as the base — so `imm p Plain = PlainFin[1]`, or passing
  a `PlainFin` to a parameter typed `Plain`, runs no finalizer at all. Measured, not
  pinned: nothing refuses it and nothing decides it. A base's own `final` has no
  such gap, since every enrichment carries a copy.
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
