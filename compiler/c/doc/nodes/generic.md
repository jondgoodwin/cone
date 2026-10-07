Generics and macros have **no node of their own**. A generic is an ordinary
`FnDclNode`, `StructNode` or `ModuleNode` carrying a `GenericInfo` side block;
carrying one is the entire definition. Instantiation is `cloneNode`, and cloning
is what stands in for name resolution on an instance. A generic module is
instantiated by the same memo and the same clone, one declaration at a time
([module](module.md), "Generic modules").

**At a glance.** The parser attaches `GenericInfo` after the name. Name
resolution resolves the **template once, in place**, with parameters hooked.
Type check **never checks a template** — only clones. `genericSubstitute` from a
call either memo-hits an existing instance or clones a new one. Flow and
generation see only instances, reachable **only** through `memonodes`. A
generic's **constraints** are evaluated against the type arguments before the
clone ("Constraints").

*Provenance: read from source; the instance symbols were measured from emitted IR.*

## Principles — [derived]

**Genericity is a side block, not a node kind.** A generic is an ordinary
`FnDclNode` or `StructNode` carrying `GenericInfo`; carrying one is the entire
definition. ▸ **Forbids** a parallel generic IR, and **settles** that every
phase's handling of a generic declaration is its handling of the ordinary one.

**Instantiation is cloning, and cloning stands in for name resolution.** ▸
**Forbids** type substitution into a shared template — there is no environment
threaded through later phases, because an instance is an ordinary declaration
with concrete types by the time anything looks at it. **This is what makes
monomorphization cheap for the compiler**, and it is where
[Performance](../../../../doc/design/performance.md)'s "generics are monomorphized" bet is
paid for.

**Instances are type checked, never the template** [differs: the author intends
to check the template as defined; it was deferred as work, not chosen]. A generic
body holding an error valid for no type argument goes undiagnosed until something
instantiates it.

⚠ **This is a current limitation, not a principle, and the rest of this section
is principles.** It was written here as one — *forbidding* C++-concept and
Rust-trait-bound style checking of the template — which is an implementation
state promoted to a rule, the hazard `_index.md` gives as the whole reason
`[derived]` exists. **The author has since said he expects to check templates as
defined and sees advantages in it.** ▸ **For whoever builds it: checking a
template requires knowing what its parameters guarantee, which is what its
constraints say.** Constraints are built, and checked at each instance only;
checking the body against them where it is declared is the part still owed.
**Under an `or`, the plan is Go's rule** [Jon 27 Sep, accepting Penny's
proposal]: a body governed by `T is A or T is B` may rely only on what *every*
alternative supports — what `A` and `B` both give — as Go's union constraints
let a generic body use only the operations every type in the union has. So `or`
does not stand in the way of declaration checking; `not`, which says nothing of
what a type can do, would, and is not built.

**A constraint is checked by evaluating it, once every argument is known**
[Jon 27 Sep]. ▸ **Forbids** solving one, unifying through one, or using one to
infer a type argument the use did not supply: inference runs first, from the
arguments alone, and a clause is only ever asked of the types that came out.

**Instances are reachable only through `memonodes`.** ▸ **Settles** that
deduplication happens in the IR rather than in the linker, which is why
`linkonce_odr` is a cross-package mechanism only: a described build gives it to
every instance, so that a package's and its importers' copies merge
("Generation").

## Shape

**`GenericInfo`** — three fields, hung off a declaration:

| Field | Meaning |
| --- | --- |
| `parms` | the declared type parameters. Every element is a `GenVarDclNode` |
| `memonodes` | the memo table **and the only path to instances** |
| `where` | a generic type's constraints, as conditions (below); NULL for a function, whose are its `FnDcl`'s |

**A `where` list holds conditions, every one of which must hold.** Each element
is one operand of the `and`s at the top of what was written. A condition is a
**clause**, `T is Name` — an `IsTag` `CastNode`, the node `p is Mobile` makes,
whose `exp` is a use of the type parameter it constrains and whose `typ` is a use
of the trait or type it `is` — or an `OrLogicTag` or `AndLogicTag` `LogicNode`
joining two conditions. `where T is A and T is B` is two elements; `where T is A
or T is B` one, an `or`; `T is A + B` and `[T A + B]` are two clauses, and inside
an `or` alternative an `and` of them. A generic type's list is its
`GenericInfo`'s; every function's, generic or a generic type's method, is
`FnDclNode.where`, so a method that is not generic still has one. Being ordinary
nodes over name uses, the conditions are cloned like any other: in an instance's
copy of a method, a clause over the type's parameter comes out naming the
argument, and one over the method's own parameter stays a use of it.

**`memonodes` is a flat list of pairs** — `[call₀, instance₀, call₁, instance₁, …]`.
Every consumer walks it with `for (nodesFor(...)) { ++nodesp; --cnt; ... }`,
where the extra step inside the body is what makes the stride 2. `NULL` means
never instantiated.

**Registration happens before the instance is type checked**, which is what lets
a generic that recurses at the *same* arguments terminate — the inner call
memo-hits the half-built instance.

**A lifetime is never instanced.** A memo hit compares arguments by
`itypeIsSame`, which ignores a reference's lifetime but for whether it is a
key (an invariant one, below), so `Option[&'a R]` and
`Option[&R]` are one instance; and the instance is cloned from its arguments
with every lifetime erased (`lifeErased`, a function type's promises excepted,
being part of that type), so no use's names leak into the instance's own
types. So is every band (`RefNode.scope`): an argument inferred from a borrow
(`viaLocal(&mut d, &r)` making `T` a `&R`) carries the band of that borrow,
and an instance made from it would hold every use to the first one's, while
one written has none. Erased, a `T` is checked in the body by the loan walk,
which holds what a parameter of type `T` lends as the caller's. The use
`genericMemoize` returns keeps the arguments as written, and the lifetimes
the generic's own name was given, for what they name
(`genericInstanceUse`, `NameUseNode.lifeuse`).

**A bounded parameter's argument is renamed, not erased.** A generic
function's type parameter may carry a lifetime bound, `[T + 'a]` or `where
T + 'a`, which the parser puts in its signature's order as the pair `'+T >=
'a` (`'+T`, `lifeBoundName`, a name no source can write). Its argument is made
the instance's with every lifetime it names, and every borrow it holds
unnamed, renamed `'+T` (`genericInstanceArg`, `lifeRenamed`; a struct with no
lifetimes of its own records it as `LifeUse.held`), and its band dropped, as
a written type argument has none, so the instance's body may store or return
a `T` as `'a` and a call carries an argument for `T` wherever `'a` flows
([Flow](../phases/flow.md), "Named lifetimes"). The renaming is the
parameter's, so it multiplies no instance. `lifeBoundsNameRes` refuses a
bound on anything but the function's own type parameter: a generic type's
parameter takes none yet, since its instance's members are made from
arguments erased (`ErrorLifetimeBound`), and the parser refuses one in a
generic type's brackets or `where` clause.

**An invariant lifetime is the exception.** A key stays a key in the
instance: `lifeErased` keeps an invariant name, and `lifeCanonBrands` then
renames every brand the arguments carry by its place, `'=1`, `'=2`, so that
`List[&'=a Node['=a]]` and `List[&'=b Node['=c]]` are one instance,
`List[&'=1 Node['=2]]`, which assumes no two places one brand; the memo
compares those names (`lifeBrandsEqual`) as well as the types. A function's
instance use keeps its arguments as written too, where they carry brands,
for a call to bind the places to ([Type Check
Reasoning](../phases/type-check-reasoning.md), "Invariant lifetimes:
brands").

**`GenVarDclNode`** is `{ IExpNodeHdr; Name *namesym; Nodes *annot; }`. Its
`vtype` is set NULL and never assigned; `gVarDclTypeCheck` is empty. `namesym`
sits at the same offset as `VarDclNode.namesym` and `NameUseNode.namesym`, which
is what makes the casts in the three `*NameRes` functions safe. `annot` is what
was written after the parameter's name, each `+`-joined name, or NULL.

**Why the annotation is one slot, of no fixed meaning.** `[T ___]` is where a
parameter will be annotated with more than a constraint: a type, making a value
parameter, `[N usize]`, and a kind, `[e Expr]`, `[b Block]`, `[M Module]` — Jon's
direction that generic and macro parameters be typed like any parameter, by
*"the kind of a parameter"* [Jon 26 Sep]. So
the parser reads whatever names are there into `annot` without deciding, and
what each **resolves to** decides what it means: a trait is a constraint, which
is built; anything else is a value or kind parameter, refused at name
resolution as not built (`ErrorGenParmConstr`). A macro's parameter and a
generic module's are refused in the parser, as before, since neither kind of
declaration takes a constraint yet.

**`MacroDclNode`** carries `namesym`, `parms`, `body`, and a `memonodes` that is
**dead** — macros are never memoized; every expansion is a fresh clone. Declared
inside a type it is a member of that type, listed in the type's `nodelist` and
bound in its `namespace` beside the methods, and it carries `FlagMethFld` when
its first parameter is `self` — the same rule that makes a `fn` a method.
`cloneMacroDclNode` copies it along with the instance's methods when a generic
type is instantiated, hooking each parameter to its own copy first so that the
body's uses of them are copied as uses rather than substituted, while the
enclosing type's parameters *are* substituted.

**`CloneState`** carries `instnode` (stamped into every cloned node, and what
`errorMsgNode` walks to print the instantiation trace), `selftype`, and `scope`.
**The declaration substitution map is not in it** — that is three file-scope
globals in `clone.c`.

## Parse

`parseGenericParms` is `[ Ident annot? (, Ident annot?)* ]`, where `annot` is a
type (`parseType`) and more joined by `+`, beginning with a name; a `+` with
no type after it is `ErrorNoType` (`parseTypeReq`). **No
defaults.** An empty list is `ErrorNoGenParms`. `annotate` says whether the
declaration takes annotations: a generic function or type does, and the name
after the parameter's goes into `annot` (Shape, above); a parameter ends at its
`,` or the `]`, so `[T A B]` is the unclosed-list `ErrorBadTok` rather than a
second parameter. A lifetime after a `+`, `[T + 'a]` or `[T A + 'a]`, is a
lifetime bound, not an annotation (`parseBoundAdd`): a function's go to the
`bounds` order `parseFn` adds to its signature's, and a generic type's are
refused, `ErrorLifetimeBound`.

**A macro and a generic module take none.** There the comma is required, and a
second name straight after the first is refused with `ErrorGenParmConstr`,
reported at that second name: `[a i32]` would be a typed macro parameter, and
read as two parameters it would become an arity complaint about the macro's
uses. Recovery skips to the next `,` or `]`, stopping at `;`, `{`, `}` or EOF, so
a whole `+`-combined annotation costs one diagnostic and each parameter carrying
one is reported. The parameter survives; whatever followed its name is dropped.
Anything else after a parameter name is still the unclosed-list `ErrorBadTok`.

**`parseWhere`** reads the `where` clause, which stands just before the block:
after a function's signature (and `inline`), and after a type's name, type
parameters and `is`/`extends` clauses. It is an expression over clauses, as an
expression is over values: `or` of `and`s of terms (`parseWhereOr`,
`parseWhereAnd`), so `and` binds tighter, and a term (`parseWhereTerm`) is `(`
a whole condition `)`, `Ident is Name (+ Name)*`, each name read by
`parseTypeName`, a lifetime comparison, or a lifetime bound, `Ident + 'a`, or a
lifetime after a `+` in an `is` clause, `T is Name + 'a` (`T is 'a` is
`ErrorWhereForm`: `T` is no lifetime). A comparison and a bound go to the
declaration's order and stand in the condition as a marker
(`parseLifeClause`), so an `or` over one is found and refused,
`ErrorLifetimeOr`, as is a `not` before a comparison; a `not` before a bound's
subject is the `not` that is refused everywhere. The condition's top-level `and`s are split into the list's
elements (`parseWhereAdd`), appended to the function's `where` or the type's
`GenericInfo.where`. Every other shape is `ErrorWhereForm`, the clause adds
nothing, and the rest of it is skipped to the block: a subject that is not a
name followed by `is` (a relation, `T < Y`; a constraint on a type expression,
`Option[T] is Node`), `not` before a clause or after its `is`, `is` with no name
after it, an `and` or `or` with no clause after it, and a `(` never closed. A type
with no type parameters writing one is `ErrorWhereNoParms` here; a function's is
known only at name resolution.

**A condition may also follow one entry of a type's `is` list**, after `if`:
`struct Fut[T] is Move if T is Move`. `parseIsCondition` reads it with the same
functions, so it takes the same forms and refuses the same ones, and also a
lifetime comparison, which no instance could decide; the refusals name it a
condition on an `is` entry (`parseInIsCond`). It is kept whole, one condition, not
split at its `and`s, since it decides one entry rather than listing requirements.
`parseStruct` holds the entry apart, in `StructNode.condis`, as a placeholder
field whose `vtype` is the trait and whose `value` is the condition; it is never
the base, wherever it is written. A refused condition drops its entry, and
recovery skips to the next entry, the type's `where` or its block. A `where` after
the list is the type's requirement, as it always was.

**The inline form takes no `or`.** `[T A + B]` requires every trait it names, and
`parseGenericParms` refuses `or` after them, `ErrorGenParmOr`, dropping that
parameter's annotation and skipping to its `,` or `]`: a choice is written in a
`where` clause [Jon 27 Sep].

Attached by `parseFn` only in the named branch — so an anonymous function can
never be generic — and by `parseStruct` after the type name. Nothing about the
symbol is decided here: it is spelled at generation, and the type arguments in
its path are what tell one instance from another.

The non-obvious case is a **variant of a generic tagged trait**: it may not
write its own parameters, so the parser synthesizes a parallel list reusing the
trait's `Name`s, and rewrites the variant's `basetrait` into `Trait[P1,P2,…]`.
That is why base trait and every variant each carry their own `GenericInfo` and
their own `memonodes`. A generic enum that extends another gets its copies of the
base's variants the same shape at name resolution — its own parameters and a
`basetrait` of `Enum[P1,P2,…]` — so they instantiate with its own variants
([struct](struct.md), "An enum extending an enum").

## Name resolution

**There is no `genericNameRes`**, because there is no generic node. A generic
function is resolved by `fnDclNameRes`, a generic type by `structNameRes` — the
same functions that handle non-generic ones, with a `genericinfo` prologue. That
prologue pushes the hook table *first* and resolves the parameters inside it,
because resolving one hooks it: done beforehand, the parameter would bind in the
enclosing scope and the matching pop would never remove it.

**The constraints are resolved there too, once the parameters are hooked**, by
`genericConstraintsNameRes`, from `structNameRes` for a generic type's and from
`fnDclNameRes` for a function's. It first folds each parameter's annotation into
the list, as clauses of a fresh use of the parameter and the annotation's name —
refusing, `ErrorGenParmConstr`, a name that resolves to no trait — and then
resolves every clause of every written condition (`genericConditionNameRes`),
keeping a condition whose clauses each have a subject that is a type parameter
(the function's own, or its generic type's) and a name that is a trait that is
not generic, or a type that is neither a generic type nor a trait
(`genericNamedType`): `where T is bool` [Jon 27 Sep]. A subject that is anything
else is `ErrorWhereSubject`, and a name that is neither `ErrorWhereTrait`; a name
that bound nothing was reported as unknown where it was resolved. A condition
with any clause refused is dropped whole, since an `or` missing a side would say
something else. What is left is the list, the annotations' clauses first. Only
the `where` clause may name a type; in the inline slot a type means a value
parameter (Shape, above). A function that is neither generic nor a member of a
generic type (the resolving `typenode`, which for a variant is the variant,
carrying its enum's parameters) has nothing to constrain: `ErrorWhereNoParms`.

**A condition on an `is` entry is vetted where the type's other `is` names are**
(`structCondIsNameRes`, from `structNameRes`, its parameters hooked), and the
entry is kept in `condis` only if every check passes; nothing of it is taken into
the template. Refused at the declaration: an entry of a trait's list or of an
enum's variant, not built (`ErrorUnbuiltIsCond`); a type with no type parameters
(`ErrorIsCondNoParms`); an entry naming `Copy`, which is written on `Move`
instead (`ErrorIsCondCopy`); one naming no trait (`ErrorInvType`) or a closed
type; and a condition whose clauses do not pass `genericConditionNameRes` called
with the type's own parameters (`genericIsConditionNameRes`), which also refuses,
as `ErrorWhereSubject`, a subject that is a type parameter but not one of the
type's own. The trait it names is resolved on demand, as any `is` name is.

Two consequences that define the phase boundary:

- **A template is resolved exactly once, in place, with its parameters hooked.**
- **An instance is never resolved at all.** `genericInstantiate` goes straight
  from `cloneNode` to `inodeTypeCheckAny`. All name binding in an instance is
  done at clone time — **cloning substitutes for name resolution, using a name
  table whose hook stack is long gone.**

That includes the type-or-value votes name resolution takes by asking
`isTypeNode` of an operand. A use of a type parameter is not a type, so in the
template `&T` and `*T` resolve as a borrow and a dereference; `cloneRefNode`
and `cloneStarNode` take the vote again on the substituted operand, as
`cloneTupleNode` and `cloneArrayNode` do for `(T, T)` and `[2; T]`, which the
template holds as a value tuple and an array literal; `[2; T]` with a type
for T is the old spelling of an array type, and with a value the retired fill
literal, which `cloneArrayNode` refuses as name resolution does
(`ErrorArrayTypeOld`, `ErrorFillLiteral`). The array type,
`Array[T, 2]`, takes no vote: name resolution builds it in the template, its
element the parameter's use, and the clone substitutes the element. A vote whose losing side
is an error cannot wait for the clone, so there the operand abstains:
`inodeIsProvisionalType` recognizes a use of a generic parameter, or a form
whose own vote was cast on one, or a managed reference type whose region is a
generic parameter (`R[mut, T]`), and `ttupleNameRes` lets it vote with neither
side (so `(T, i64)` is a type tuple, not a mix) while `allocateQuesNameRes`
leaves `?T` the `Option[T]` type instead of refusing it. A macro's parameter is
the same declaration, so a macro given a value still clones a value: every
re-decision flips only an operand that was not a type and now is.

### How a cloned name gets re-pointed — two independent mechanisms

**Type parameters, through the global name table.** `clonePushState` hooks each
parameter's `Name` directly to the **argument node**. `cloneNode`, meeting a
use bound to a generic parameter itself, then reads `namesym->node` and clones
it. So substitution is by *name*, through a global, at clone time — and the
argument is **deep-copied at every use site**, but for a number type standing in
it as its declaration, not a name — what a type check builds, such as the
`Array[u8, n]` a string literal taken as bytes holds. A number type is one declaration for the whole
program, and `itypeIsSame` compares it by identity, so a copy would be another
type: the copy of `Option[Array[u8, 5]]` naming its own enum would miss the memo
and instantiate it again, without end. A struct declaration standing there
would be copied whole, so the type check of a string literal names `str` through
a name use (`slitTypeCheck`). The use must be bound to the
parameter, not to a type alias of it: `alias Item = T` in a generic module,
used as `Item`, is a use of the alias, whose own copy substitutes `T`, and
the name `Item` is hooked to nothing.

**Local declarations, through `cloneDclMap`** — a LIFO stack of
`{original, clone}` pairs, searched backwards, **falling back to the original
when not found**. That fallback is load-bearing in both directions: it is what
makes a macro body resolve at its *declaration* site, and what makes a generic
function's self-recursive call re-instantiate rather than point at itself.

**A generic type's members are in the map too, and before any body.** A body
may name one of its type's static functions, statics or overload names bare, or
through `Self`, and name resolution bound the use to the template's member —
which is never generated, so a call left pointing at it is a call to nothing.
`cloneStructNode` therefore copies each function and static in two steps: every
copy is made and bound in the instance's namespace first
(`cloneFnDclShell`, `cloneVarDclShell`), `structCloneMapMembers` maps each
template member and overload set to the instance's of the same name, and only
then are signatures and bodies copied (`cloneFnDclFill`, `cloneVarDclFill`) —
so a body naming a member declared after it is re-pointed too. The map is
popped when the struct's clone ends, so one instance's members never answer for
another's. A method named bare needs none of this, since type check rewrites it
to `self.name` and looks it up in the instance, but it is mapped all the same.
A tagged trait's variants are separate clones, made after the base's instance,
so `genericMemoize` clones them under a map from the base template's members to
that instance's: a variant's body naming an enum's static function bare reaches
the instance's copy. A function cloned on its own is never mapped to its copy,
which is what the self-recursion above relies on. A bare name inside an enum
extension's braces naming a **generic base's** member is in no map: the base's
instance is made only when the extension's instance is type checked, after the
clone, so type check re-points that use instead ([nameuse](nameuse.md), step 0b).

**A generic type's own name is in the map too, to the instance being defined.**
Inside a generic type's braces its bare name means the instance: `Box` inside
`struct Box[T]` is `Box[T]`, and inside `enum Mb[T]` — its own methods and each
variant's body — `Mb` is `Mb[T]` and a variant's bare `No` or `Mb.No` is
`Mb.No[T]`. Name resolution bound each such use to the template; the instance
does not exist yet. So `genericMemoize` **reserves** the instance before cloning
it (`genericReserve`: an unfilled `StructNode`, mapped from the template), and
`cloneStructNode` fills the reserved node (`CloneState.structshell`) rather than
allocating its own — every bare use the clone copies is re-pointed at it by
`cloneDclFix`, as the clone reaches it. A tagged trait reserves the base's
instance and every variant's before cloning any, since a body may name a sibling
declared after it. Nothing is instantiated to name the instance, so a bare name
never misses the memo and never expands again. **A use given type arguments is
the exception**: `Box[i32]` inside `Box[T]` names another instance, and `Box[T]`
names this one through the memo, so `cloneFnCallNode` puts the template back as
the head of any call whose arguments are types (`fnCallHasTypeArgs`). A
construction or a variant's literal — `new Box(v)`, `Mb.No[]` — builds this
instance and stays mapped. Only a generic
*type* is reserved: a generic function named bare in its own body is a call whose
type arguments are inferred, as it is anywhere else. A generic *module* maps its
own name to the instance too (`modInstantiate`), without a reservation — the
instance module exists before its declarations are cloned — and
`cloneFnCallNode` puts the generic back the same way, so `stack[T].count` inside
`stack[T]` reaches this instance through the memo and `stack[f64]` another.

Not yet built: a generic enum's variant that an **extension** copies
(`structEnumCopyVariant`). The copy is cloned from the base's variant template
at the extension's name resolution, where the base has no instance to map to, so
its bare `Mb` or `No` still names the base's template, and the extension's
instance refuses it (`ErrorArgCount`, below). By the ruling it should name the
base's instance at the arguments the extension passes, as a non-generic copy's
bare sibling names the base's variant.

## Type check

**Templates return early.** `fnDclTypeCheck` and `structTypeCheck` both begin
`if (genericinfo) return;` — before the signature is checked, before the body,
before flow. `macroTypeCheck` is empty. A template body is written against
uses of generic parameters, which stand for nothing, so every check would be a
false diagnostic. The cost is silent acceptance — see Hazards.

`genericSubstitute`, from `fnCallTypeCheck` **after** the arguments are checked:

1. Bail unless the callee carries `GenericInfo`.
2. If **any** argument is a type node, take the explicit path: `genericMemoize`,
   then check the replacement, return handled. A generic module always takes
   it, since it has no arguments to infer from: what it is given that is not a
   type is `genericMemoize`'s `ErrorNotType`. A miss there asks
   `modInstantiate` for the instance, which clones, registers and checks it.
3. Otherwise **infer**: build a call node of NULL slots, walk the arguments
   against the template's parameter list, and match each parameter's type
   against its argument's type (`genericInferType`), capturing by `Name` the
   argument's type wherever the parameter's names a generic parameter. The match
   descends through a pointer, a reference and an array reference to what each
   points at (`p *T` given a `*Fin` captures `Fin`), accepting both the type
   tags and the template's dereference, borrow and allocate tags (Clone,
   above). A managed
   reference type, `Rc[mut, T]`, is still the call it was written as, and
   matches a reference of either shape, its last argument against what the
   reference points at; a region that is a type parameter, `R[mut, T]`, also
   captures the reference's region. An array
   slice parameter also descends into the fixed-size array, or reference to one,
   that the call converts to a slice. A generic type's instance, `List[T]`,
   matches an argument that is an instance of the same generic, found in the
   generic's memo, type argument by type argument; a variant passed for its
   enum is matched as its enum's instance. A function signature, reached
   through a function reference parameter (`f &fn(a A) R`), matches the
   signature of the function the argument references, each parameter type and
   then the return type, provided both have as many parameters. An argument
   type naming a generic's own type parameter — the signature of `&half`, a
   generic function not instantiated — captures nothing. Region and permission
   otherwise take no part: the instance's own check of the call judges them. Any other
   shape captures nothing. A slot filled twice must agree by `itypeIsSame`. A
   captured struct is held as a use of its name: an argument whose type is the
   struct's declaration itself (a block's or an `if`'s value types so,
   `iexpMultiInfer`) would be cloned into the instance as a second copy of the
   struct, methods and all.
   Any slot still NULL is "could not infer". A generic method named bare inside its type's braces
   is called on an implicit `self` that is not among the arguments, so they are
   matched against the parameters after it; named through its type,
   `Holder.pick(&h, 6)`, the receiver is the first argument. The instance's name
   use keeps the `FlagQualified` of the name it stands for, so the second is
   never rewritten to `self.pick` (`genericKeepQualified`).

**A generic method called on a receiver** — `h.pick(6)`, `h.pick[i32](6)` — is
instantiated by `fnCallLowerMethod` rather than here, since the method is known
only from the receiver's type ([fncall](fncall.md), "Selecting a candidate"):
`genericMethodInstance` infers from the arguments after `self`, or takes the
written type arguments, and memoizes as a call does. A method's instance is
type checked with its owner as the walk's type, whatever type the call is in,
as a method reached by demand is. A generic method copied with its type — into
a generic type's instance, or a trait's default into an implementer — is still
generic (`cloneFnDclShell` gives the copy a `GenericInfo` of its own, sharing
the original's parameters), and `cloneFnDclFill` hooks each of those parameters
to itself while it copies, so the copy's uses of them stay uses rather than
substitutions, as a macro's copy does. So `Box[i64]` has its own generic
`pair[U]` with its own instances, `Box[i64].pair[i64]`. `genericClone` makes
the one copy that is not generic — the instance itself — and clears it there.

Not yet built: a trait's public generic method met by a type that is the trait.
`structCheckTraitReqs` compares the requirement with the type's method, its own
or the copy of a default, by `fnSigVrefEqual`, and `itypeIsSame` finds no two
uses of a type parameter the same — a use of one is not a type, so neither
resolves to a declaration — so every such type is `ErrorInvType`, "none of
what it declares has the signature". Comparing them would mean matching type
parameters by position.

`genericMemoize` validates arity and that every argument is a type, then looks
up: **the memo key is the stored call's argument list, compared pairwise with
`itypeIsSame`, first match wins.** A miss clones. A failed instantiation returns
a `newErrorNode` rather than nothing, so the caller substitutes it and keeps
checking — `fnCallTypeCheck` has the matching `inodeIsError` guard.

**A generic named bare where a type is wanted names no type.** Only an instance
is a type, so `Box` for a `struct Box[T]` — a generic enum or trait likewise, or a
folded name for one — is refused with `ErrorArgCount` by `itypeRefuseBareGeneric`,
which `itypeTypeCheck` asks of every type it checks: a parameter, a local, a
field, a return type, a referent, an alias's target, a cast's target. The use is
then bound to `errorType`, so an alias's uses and a parameter's arguments do not
report it again. `genericMemoize` asks the same of each type argument, so
`id[Box]` is refused once, at the argument, rather than at every use the instance
makes of its parameter. Inside its own braces a generic's bare name arrives here
already re-pointed at the instance being defined (Clone, above), which carries no
`genericinfo` and is not refused; a different generic named bare there still
names its template, and is. A module's or a module trait's name is refused at
the same places, and bound to `errorType` the same way, by `itypeRefuseModule`
with `ErrorNotType`.

A **tagged trait** fans out: the base trait is cloned, then every entry of its
`derived` list, each registering into its own `memonodes`. **The base and every
variant are cloned and registered before any is type checked**, because a
variant's body may name a later sibling at the same arguments, and so may the
enum's own static function: registered only as
each was checked, that sibling was a miss, and a miss on any variant
instantiates the whole enum again, so it expanded until `ErrorInstDepth`. **The
instance's `derived` is filled with every cloned variant before the enum or any
variant is type checked**, so what a variant's body asks of the whole set sees
all of it, whether the variant is reached in turn or first from the enum's own
check: a match there on a value of the enum is exhaustive over every variant
(`ifExhaustCheck`), a bare pattern name finds its sibling (`castPatternBind`),
and a variant holding its own enum by value is refused as one still being laid
out (`itypeVariantPending`). The instance is checked through
`structTypeCheckEnumInstance`, whose layout lays out every variant with it —
the members of all of them are checked only after, once no layout is in flight
([Type Check](../phases/type-check.md), "Layout before members") — and which
leaves the discriminant's width
(`structSetTagWidth`) out of that check; `genericMemoize` settles it once the
instance and its variants are checked — on the first instance of the generic
only, since the discriminant node and the tag values are shared by every
instance, and measuring each would report a declared type's overflow once per
instance.

### Constraints

**Evaluated, never solved** (Principles). A clause naming a type, `T is bool`,
is met by that type alone (`itypeIsSame`). Every other question a clause asks is
`genericTypeIs(type, trait)`, which is what `is` answers of a type:

- **The compiler's grants.** Every type is exactly one of `Move` and `Copy`
  (`itypeIsMove`); `Integer` is `i8` … `i64`, `u8` … `u64`, `isize` and
  `usize` — an `IntNbrTag`, or a `UintNbrTag` that is not `bool`; and `Pointer`
  is every raw pointer type, `*T` whatever `T` and its permission — a `PtrTag`,
  never a reference; and core's `Hash`, a trait with a method, is granted to
  every integer type and `bool` as well (`coreIsHashTrait`; [struct](struct.md),
  "Hash"). `Integer or bool or Pointer` is exactly what an atomic
  operation takes (`intrinsicIsAtomicType`), so core's `Atomic[T]` requires just
  what its `AtomicValue` marker admits. `Sendable` is the thread check's, and is
  asked of the walk (`itypeThreadBound`) before anything else: it is granted to
  every type holding nothing bound to its thread, and a type declaring it is
  taken at its word only where its type arguments are `Sendable` too
  ([References](references.md), `refThreadBinds`). A borrow of the whole
  program crosses where the clause's parameter is also bounded `+ 'static`
  (`genericStaticHow`, `lifeParmStaticBounded`: the walk then runs as
  `StaticVouched`, `itypeThreadBoundHow`), the call being checked to hand it
  only global borrows ([Lifetimes](../phases/flow.md), "Named lifetimes");
  an unbounded parameter refuses every borrow. A lone `T is Sendable` that
  is unmet is `ErrorNotSendable`, not `ErrorWhereUnmet`: its message names what
  binds the argument and where it sits, which part of the borrow rule a borrow
  failed (`genericNotSendableWhy`), and says that a local's `mut` is not
  what is checked. One that was met while a struct the argument reaches was
  still being laid out is noted (`genericSendableNote`) and judged again once
  type check has finished (`genericSendableCheckAll`, from `conec.c`).
- **A declaration**: the type's `is` list names the trait, or names a trait whose
  own list does (`genericDeclares`).
- **Fitting it structurally**, only for a trait that requires something of a
  value — a method or a field. `structMatches` under `Monomorph` is the test, the
  one a trait's other uses are made by; `genericDemandMatch` first analyzes each
  requirement and each candidate the type has for it (`fnCallDemandCandidates`),
  since signatures compare only once checked. Under `Monomorph` a requirement's
  `Self` is the type asked about (`fnSigVrefEqual`'s `selftype`), since the
  instance calls that type's own method: `o &Self` is met by its `o &Self`. The
  match is otherwise exact, and a virtual reference's is exact throughout. A **marker** — a trait requiring
  nothing of a value, the compiler's own built-in traits among them — is never
  fitted: every type would fit it, so fitting it would say nothing.

**A condition is evaluated whole** (`genericConditionValue`), `or` and `and` as
in an expression, the right side asked only where the left does not decide it,
to one of three values: true, false, or unknown — turning on a parameter not
bound here. A lone clause is decided where its subject is one of the parameters
being bound, with that parameter's argument; one over a parameter bound earlier
(its subject substituted by the clone) was decided then, and is unknown here.
Inside an `or` or an `and` a substituted subject is asked as it stands, since
the whole is decided here. `genericUnmetCondition` finds the first condition
that is false; an unknown one is left to where its parameter is bound. So a
generic method's condition joining its type's parameter and its own, `where T is
Integer or U is bool`, is unknown when the type's instance is made — the method
exists there — and decided, as a requirement, at the method's instance.

**On a generic function or type, a constraint is a requirement.**
`genericMemoize`, on a memo miss and before anything is cloned, asks
`genericRequirementsMet`: an unmet condition is `ErrorWhereUnmet` at the use
asking for the instance — the call, the type literal, the type named — naming
the generic, the clause and the argument, or, for a condition joined by `or` or
`and`, the whole condition (`genericConditionCat`) and the arguments it turned on
(`genericBindingsCat`), and an error node stands for the instance,
so nothing inside the generic is checked against arguments it was never meant
for. A variant answers to its enum's clauses. A type position holding the error
node settles to `errorType` (`itypeTypeCheck`), so what uses it is quiet. A failed
instance is never memoized, so each use asking for it is refused where it is.

**On a method of a generic type, a clause over the type's parameters is a
condition for the method existing.** Before an instance is cloned,
`genericAbsentMembers` evaluates each member's clauses at the arguments, and
`cloneStructNode` is handed the members that fail (`CloneState.absent`, taken by
the one struct cloned next, as `structshell` is) and copies the instance without
them: they are not in its namespace or its member list, so they are neither
checked nor generated there, and what the instance fits structurally is what it
has — `Cell[i32]` is an `Adder`, `Cell[bool]` is not, to a constraint and to a
virtual reference alike. Settled before the reservation and the clone, because a
structural clause may analyze the argument's own methods. A call on the instance
that finds nothing asks `genericReportAbsent`, from `fnCallLowerMethod`, which
finds the template through the instance's memo entry and reports
`ErrorWhereAbsent` naming the unmet condition, as a requirement names it. A
generic method's own clauses over its own parameters stay uses in the copy and
are requirements at its instance.

**An `is` entry's condition decides whether the instance has the entry**, by
the same evaluation. `genericAbsentMembers` lists each `condis` entry whose
condition is not true at the arguments (an unknown counts as not true), and
`cloneStructNode` gives the instance a placeholder for each entry not listed: an
`IsMixin` field whose `vtype` is the clone of the entry's, appended to `fields`,
which the instance's type check takes in as it takes an instance of a generic
trait (`structInheritTrait`: default methods copied, requirements checked). A
trait that is a declaration is recorded in the instance's `traits` at once, and
`Move` or `AtomicValue` marks it `MoveType` at once, as name resolution marks a
declared one, so a type or a constraint asking before the instance is laid out
is answered. An entry not met leaves the instance without it, and so, for Move,
copying unless another rule makes it move: the entry can add Move, never remove
it. `genericConditioned` counts a `condis` as reading the arguments' layouts, so
an instance made as a reference's target settles its arguments first. An unmet
requirement whose argument is such an instance says why
(`genericCondIsWhy`): the entry's condition is false there, or, asked for
`Copy`, an `is Move if` condition is true.

**Depth is the only cycle detector.** No mark can catch runaway expansion,
because every expansion is a fresh node — nothing ever returns to the same node.
`genericInstantiateEnter` counts and refuses past `TypeCheckLoopMax` (256) with
`ErrorInstDepth`. Past that it is the C stack that gives out, with no diagnostic.
Once it has refused, it refuses every expansion until the outermost one has
unwound, without reporting again: each level the refusal returns through would
otherwise start its next expansion down to the limit again, and one that
expands twice a level would report the limit an exponential number of times.

**Macros differ from generics in three ways**: arguments are never checked for
being types, never type checked before substitution, and never memoized. That is
what makes `twice[bump()]` call `bump()` twice, and what lets a macro parameter
be used in type position. It is also what lets an argument go unevaluated: core's
`assertDebug[cond]` puts `cond` inside `if isDebugBuild() {...}`, so a release
build never runs it.

**`srcFile()` and `srcLine()` in a macro's body answer where it is used.**
`macroExpand` sets `CloneState.srcsite` to the use before cloning the body, and
`cloneFnCallNode` gives a call to either that place. The place is the outermost
use (`macroSrcSite`): every node cloned from a body is marked as instantiated by
the use it expands (`instnode`), so a macro used in a body climbs that chain
while each link is a macro's use. An argument is cloned with `srcsite` and
`instnode` cleared, so it keeps the place it was written at and the chain stops
at it: a macro passed as an argument answers where it is written. Nothing else
in the body moves: a diagnostic about the body, `ErrorInstDepth`'s included,
still points into it. `intrinsic_srcloc_macro` pins each case.

**A macro method expands the same way, with the receiver as the first
argument.** `x.name(args)` reaches `macroMethodTypeCheck` from `fnCallTypeCheck`,
which for a member access checks the receiver *before* the arguments and asks the
receiver's type what the name binds — a macro's arguments must stay unchecked
until substituted, so the receiver is the only thing checked ahead of the
lookup. The receiver is inserted at the front of the argument list and
`macroExpand` substitutes it for `self` like any other argument: a body naming
`self` twice evaluates the receiver twice. The call is written with parentheses
or as the bare member (`x.name`); `x.name[i]` indexes what the member names.
Three things follow the method it stands in for. A bare `name` or `name(args)`
inside a method of the type is rewritten to `self.name…` from the enclosing
method's parameter 0, and where no method encloses it — a static function of the
type — it is `ErrorUnkName`, as a bare field is. A member access the body wrote
on `self` carries `FlagSelfRecv` on its clone (set by `cloneFnCallNode` while
`CloneState.selfparm` names the `self` parameter), which is what lets
`fnCallLowerMethod` grant the expansion a private member through the macro's
`self` wherever it is expanded; any other receiver in the expansion is judged by
the use site's function, as a receiver written there would be. And the body may not name a
member bare — `nameUseNameRes` refuses it with `ErrorBareMbr` while
`NameResState.macromethod` is set — because the expansion lands in another
function whose `self`, if any, is not this type's. A macro without a `self`
parameter is a static macro of the type: bound in the namespace, expanded by
nothing today, and `x.name` on it is `ErrorNoMbr`, as a static function would
be. A macro declared on a trait is a member of the trait alone: a trait's
expansion into an implementing type or variant (`structInheritTrait`) folds
only `FnDcl` defaults, so an enum's macro is found on the enum's own namespace,
which is what lets a `match` on its receiver see the enum rather than one
variant.

## Flow

**Templates are never flowed** — the only `blockFlow` entry point is the tail of
`fnDclTypeCheck`, unreachable for a template. That is safe because every
function reaching generation is either non-generic or an instance, and an
instance *is* flowed: `genericInstantiate` ends with `inodeTypeCheckAny`, which
for a declaration with `genericinfo == NULL` runs the full check including flow.

A template body could not be flowed even in principle — `itypeIsMove`,
`permGetFlags` and drop selection all read a type declaration a use of a generic
parameter does not have.

## Generation

`genlGlobalSyms` and `genlGlobalImpl` each have two generic branches, both
walking `memonodes` with the pair stride. For a module this object does not
generate — an imported package's, whose include file carries the generic's
full body, since a generic cannot be `extern` — `genlImportedInstances` walks
the same branches and generates only the instances, private generics'
included: **every object that uses an instance defines it**, because the
generic's own package cannot know which instances its importers make. A generic
module's instances are not walked there: each is a module of the program by
generation, flagged to generate, and `genlProgram` passes the generic over.

**Linkage is the C++ template answer, in a described build.** An instance, and
every member of a generic type's instance (`dclIsInstance` — the members carry
no `instnode` of their own, so their owners are asked, up to the module, and an
instance of a generic module answers for every declaration it holds, globals
included), is `linkonce_odr` with
a COMDAT of kind `any` (`GenlShared`): the package's own instances and each
importer's are identical, and the linker keeps one copy. A compile with no
build description is the program's only object, and there an instance is
internal like every other definition. `dclIsExported` never exports one. What a
library compile does export is each private function, global or type a
generic's body names (`DclExpandReached`), since the importer's instance calls
it. `module_build_link` links a package and a program that both instantiate a
generic function and a generic type at `i64`, and the program alone at `f64`.

**The symbol keys off being an instance**, which `nameSymbol` reads off the
node: the function's own `instnode` carries type arguments
(`itypeInstanceTypeArgs`), or its owner is an instance of a generic type. A
concrete function's path component is its identifier alone, and so is a trait
default's cloned into an ordinary implementer — its `instnode` is the
implementing struct, not a call, so it is a copy rather than an instance. An
instance's component is wrapped `I…E` around its **type arguments**, never the
parameter types, so `fn tag[T](a i32)` at `i32` and at `f32` are two symbols;
and a generic *type*'s instance carries its arguments as an owner, which is why
`fn tally(self) i64` does not collide across instances. `nameType` spells
every argument, structural ones included — tuple, array, signature, void, the
three reference kinds, pointer — so no instance spells to nothing. The rules
are [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols".

## Hazards

- **`cloneNode`'s generic-parameter substitution re-enters `cloneNode` on a
  *global*** — the parameter name's hooked node — whose value at type-check time
  need not be what name resolution saw. Its one guard is for the name being
  hooked to a `GenVarDclNode` — a template being copied, a macro's or a generic
  method's — which it copies as a use.
  Otherwise NULL yields NULL silently, and an unhandled tag kills the compile.
  This one path is where an unrelated defect elsewhere becomes a hard abort.
- **A clone must clear the type check marks**, or the instance silently skips
  its own check. Only four clone functions do; every other copies `flags`
  verbatim. A new declaration-bearing node kind inherits the bug.
- **`MacroDclNode.memonodes` is dead.**
- **A generic type's own bare name is told from its instantiation by its
  arguments alone.** While an instance is cloned the template is mapped to the
  instance, and `cloneFnCallNode` undoes that only for a call whose arguments
  `fnCallHasTypeArgs` recognizes as types. A type argument it did not recognize
  would leave `Box[...]` headed by the instance, which is no generic, and read
  as a literal of this instance.
- **`CloneState.structshell` is taken by the next struct cloned**, whichever it
  is. It is set only for a generic type's own clone, whose root is that struct.
  `CloneState.absent` is set and taken the same way.
- **A member absent from an instance can still be named by a member present
  there.** A present method naming an absent one bare is the generic's own
  mistake. A method is rewritten to `self.name` and found missing on the
  instance (`genericReportAbsent`); a static function's name use was bound by
  name resolution to the template's member and, with no copy to map it to, still
  names it, which `nameUseTemplateMember` reports as `ErrorWhereAbsent` when the
  use was copied into an instance of that generic.
- **A `where` list's nodes are expression nodes that are never checked as
  expressions.** Its clauses are the `IsTag` nodes a runtime `p is Mobile`
  makes, and its joins the `LogicNode`s of `and` and `or`; only name resolution
  (`genericConditionNameRes`) and the evaluator read them. A walk that type
  checked or generated one would take a compile-time question for a runtime
  test.
- **Only a struct fits a trait structurally.** A number type meets the markers
  granted to it and no trait with members.
- **`Self` is read as the implementer only where it is written `Self`.** The
  requirement's type is compared as written (`fnSigReqTypeSame`), through a
  reference, pointer or slice, so a requirement naming its trait outright means
  the trait, and an alias of `Self` is not seen through.

## What lives elsewhere

- Instantiation scheduling, depth bounding, and why the marks cannot police it: [Type Check Phase](../phases/type-check.md), "Generics and macros"
- Hooking, and what a pushed table scopes: [Name Resolution](../phases/name-resolution.md), "Hooking"
- Symbol spelling: [Names and Namespaces](../../../../doc/design/names-and-namespaces.md), "Symbols"; its lowering, linkage and COMDATs: [Generation](../phases/generation.md)
- Cloning a struct, and the `Self` rebinding: [struct](struct.md)
- A generic module — what it may hold, how a path through an instance is
  collapsed, and where an instance runs its `init`: [module](module.md),
  "Generic modules"
