Generics and macros have **no node of their own**. A generic is an ordinary
`FnDclNode` or `StructNode` carrying a `GenericInfo` side block; carrying one is
the entire definition. Instantiation is `cloneNode`, and cloning is what stands
in for name resolution on an instance.

**At a glance.** The parser attaches `GenericInfo` after the name. Name
resolution resolves the **template once, in place**, with parameters hooked.
Type check **never checks a template** — only clones. `genericSubstitute` from a
call either memo-hits an existing instance or clones a new one. Flow and
generation see only instances, reachable **only** through `memonodes`.

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
[Performance](../topics/performance.md)'s "generics are monomorphized" bet is
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
template requires knowing what its parameters guarantee, so it arrives with
parameter constraints rather than ahead of them.**

**Instances are reachable only through `memonodes`.** ▸ **Settles** that
deduplication happens in the IR rather than in the linker, which is why
`linkonce` is a cross-package mechanism only.

## Shape

**`GenericInfo`** — two fields, hung off a declaration:

| Field | Meaning |
| --- | --- |
| `parms` | the declared type parameters. Every element is a `GenVarDclNode` |
| `memonodes` | the memo table **and the only path to instances** |

**`memonodes` is a flat list of pairs** — `[call₀, instance₀, call₁, instance₁, …]`.
Every consumer walks it with `for (nodesFor(...)) { ++nodesp; --cnt; ... }`,
where the extra step inside the body is what makes the stride 2. `NULL` means
never instantiated.

**Registration happens before the instance is type checked**, which is what lets
a generic that recurses at the *same* arguments terminate — the inner call
memo-hits the half-built instance.

**`GenVarDclNode`** is `{ IExpNodeHdr; Name *namesym; }` and nothing else. Its
`vtype` is set NULL and never assigned; `gVarDclTypeCheck` is empty. `namesym`
sits at the same offset as `VarDclNode.namesym` and `NameUseNode.namesym`, which
is what makes the casts in the three `*NameRes` functions safe.

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

`parseGenericParms` is `[ Ident (, Ident)* ]`. **No bounds, no constraints, no
defaults, no kinds.** An empty list is `ErrorNoGenParms`.

**The comma is required, and a second name straight after the first is refused**
with `ErrorGenParmConstr`, reported at that second name. Two names side by side
is how both a supertype constraint (`[T Comparable]`, which the reference manual
shows) and a typed macro parameter (`[a i32]`) are spelled, so the parser says
the constraint or the type is unimplemented rather than reading the two names as
two parameters — which is what it did, turning a declaration written in the
documented form into an arity or inference complaint about its *calls*. Recovery
skips to the next `,` or `]`, stopping at `;`, `{`, `}` or EOF, so a whole
`+`-combined constraint costs one diagnostic and each parameter carrying one is
reported. The parameter survives; whatever followed its name is dropped.
Anything else after a parameter name is still the unclosed-list `ErrorBadTok`.

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
template holds as a value tuple and an array literal. A vote whose losing side
is an error cannot wait for the clone, so there the operand abstains:
`inodeIsProvisionalType` recognizes a use of a generic parameter, or a form
whose own vote was cast on one, and `ttupleNameRes` lets it vote with neither
side (so `(T, i64)` is a type tuple, not a mix) while `allocateQuesNameRes`
leaves `?T` the `Option[T]` type instead of refusing it. A macro's parameter is
the same declaration, so a macro given a value still clones a value: every
re-decision flips only an operand that was not a type and now is.

### How a cloned name gets re-pointed — two independent mechanisms

**Type parameters, through the global name table.** `clonePushState` hooks each
parameter's `Name` directly to the **argument node**. `cloneNode`, meeting a
use that names a generic parameter (`nameUseNames` with `GenVarDclTag`), then
reads `namesym->node` and clones it. So substitution is
by *name*, through a global, at clone time — and the argument is **deep-copied
at every use site**.

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
which is what the self-recursion above relies on.

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
the head of any call whose arguments are types (`fnCallHasTypeArgs`). A value
list — `Box[v]`, `Mb.No[]` — builds this instance and stays mapped. Only a generic
*type* is reserved: a generic function named bare in its own body is a call whose
type arguments are inferred, as it is anywhere else.

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
   then check the replacement, return handled.
3. Otherwise **infer**: build a call node of NULL slots, walk the arguments
   against the template's parameter list, and where a parameter's type names a
   generic parameter, capture the argument's type by `Name`. A slot filled twice
   must agree by `itypeIsSame`. Any slot still NULL is "could not infer".

`genericMemoize` validates arity and that every argument is a type, then looks
up: **the memo key is the stored call's argument list, compared pairwise with
`itypeIsSame`, first match wins.** A miss clones. A failed instantiation returns
a `newErrorNode` rather than nothing, so the caller substitutes it and keeps
checking — `fnCallTypeCheck` has the matching `inodeIsError` guard.

**A generic named bare where a type is wanted names no type.** Only an instance
is a type, so `Box` for a `struct Box[T]` — a generic enum or trait likewise, or a
folded name for one — is refused with `ErrorArgCount` by `itypeRefuseBareGeneric`,
which `itypeTypeCheck` asks of every type it checks: a parameter, a local, a
field, a return type, a referent, a typedef's target, a cast's target. The use is
then bound to `errorType`, so a typedef's uses and a parameter's arguments do not
report it again. `genericMemoize` asks the same of each type argument, so
`id[Box]` is refused once, at the argument, rather than at every use the instance
makes of its parameter. Inside its own braces a generic's bare name arrives here
already re-pointed at the instance being defined (Clone, above), which carries no
`genericinfo` and is not refused; a different generic named bare there still
names its template, and is.

A **tagged trait** fans out: the base trait is cloned, then every entry of its
`derived` list, each registering into its own `memonodes`. **The base and every
variant are cloned and registered before any is type checked**, because a
variant's body may name a later sibling at the same arguments, and so may the
enum's own static function, which is checked with the enum: registered only as
each was checked, that sibling was a miss, and a miss on any variant
instantiates the whole enum again, so it expanded until `ErrorInstDepth`. **The
instance's `derived` is filled with every cloned variant before the enum or any
variant is type checked**, so what a variant's body asks of the whole set sees
all of it, whether the variant is reached in turn or first from the enum's own
check: a match there on a value of the enum is exhaustive over every variant
(`ifExhaustCheck`), a bare pattern name finds its sibling (`castPatternBind`),
and a variant holding its own enum by value is refused as one still being laid
out (`itypeVariantPending`). The instance is checked through
`structTypeCheckEnumInstance`, which leaves the discriminant's width
(`structSetTagWidth`) out of that check; `genericMemoize` settles it once the
instance and its variants are checked — on the first instance of the generic
only, since the discriminant node and the tag values are shared by every
instance, and measuring each would report a declared type's overflow once per
instance.

**Depth is the only cycle detector.** No mark can catch runaway expansion,
because every expansion is a fresh node — nothing ever returns to the same node.
`genericInstantiateEnter` counts and refuses past `TypeCheckLoopMax` (256) with
`ErrorInstDepth`. Past that it is the C stack that gives out, with no diagnostic.

**Macros differ from generics in three ways**: arguments are never checked for
being types, never type checked before substitution, and never memoized. That is
what makes `twice[bump()]` call `bump()` twice, and what lets a macro parameter
be used in type position.

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
`fnCallLowerMethod` grant the expansion a private member exactly where the method
could reach it: through `self`, and nowhere else. And the body may not name a
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
walking `memonodes` with the pair stride. In a program compile `genlLinkage`
makes an instance internal like every other definition; the C++ template
answer — `linkonce`, so several translation units may emit one and the linker
keeps one — is the package compile's, which does not exist yet.

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
are [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols".

## Hazards

- **`cloneNode`'s generic-parameter substitution re-enters `cloneNode` on a
  *global*** — the parameter name's hooked node — whose value at type-check time
  need not be what name resolution saw. Its one guard is for the name being
  hooked to a `GenVarDclNode`, a template being copied, which it copies as a use.
  Otherwise NULL yields NULL silently, and an unhandled tag kills the compile.
  This one path is where an unrelated defect elsewhere becomes a hard abort.
- **A clone must clear the type check marks**, or the instance silently skips
  its own check. Only four clone functions do; every other copies `flags`
  verbatim. A new declaration-bearing node kind inherits the bug.
- **`--checktree` has the coverage exactly inverted.** It descends into
  templates, which are never type checked, and never into instances, because
  `memonodes` is not in its switch.
- **`MacroDclNode.memonodes` is dead.**
- **A generic type's own bare name is told from its instantiation by its
  arguments alone.** While an instance is cloned the template is mapped to the
  instance, and `cloneFnCallNode` undoes that only for a call whose arguments
  `fnCallHasTypeArgs` recognizes as types. A type argument it did not recognize
  would leave `Box[...]` headed by the instance, which is no generic, and read
  as a literal of this instance.
- **`CloneState.structshell` is taken by the next struct cloned**, whichever it
  is. It is set only for a generic type's own clone, whose root is that struct.

## What lives elsewhere

- Instantiation scheduling, depth bounding, and why the marks cannot police it: [Type Check Phase](../phases/type-check.md), "Generics and macros"
- Hooking, and what a pushed table scopes: [Name Resolution](../phases/name-resolution.md), "Hooking"
- Symbol spelling: [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols"; its lowering, linkage and COMDATs: [Generation](../phases/generation.md)
- Cloning a struct, and the `Self` rebinding: [struct](struct.md)
