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

`parseGenericParms` is `[ Ident (,? Ident)* ]`. **No bounds, no constraints, no
defaults, no kinds.** An empty list is `ErrorNoGenParms`.

Attached by `parseFn` only in the named branch — so an anonymous function can
never be generic — and by `parseStruct` after the type name. Nothing about the
symbol is decided here: it is spelled at generation, and the type arguments in
its path are what tell one instance from another.

The non-obvious case is a **variant of a generic tagged trait**: it may not
write its own parameters, so the parser synthesizes a parallel list reusing the
trait's `Name`s, and rewrites the variant's `basetrait` into `Trait[P1,P2,…]`.
That is why base trait and every variant each carry their own `GenericInfo` and
their own `memonodes`.

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

A **tagged trait** fans out: the base trait is instantiated, then every entry of
its `derived` list, each registering into its own `memonodes`.

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
be. A macro declared on a trait is a member of the trait alone: `structTypeCheck`
folds only `FnDcl` defaults into implementing types and variants, so a union's
macro is found on the union's own namespace, which is what lets a `match` on its
receiver see the union rather than one variant.

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

## What lives elsewhere

- Instantiation scheduling, depth bounding, and why the marks cannot police it: [Type Check Phase](../phases/type-check.md), "Generics and macros"
- Hooking, and what a pushed table scopes: [Name Resolution](../phases/name-resolution.md), "Hooking"
- Symbol spelling: [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols"; its lowering, linkage and COMDATs: [Generation](../phases/generation.md)
- Cloning a struct, and the `Self` rebinding: [struct](struct.md)
