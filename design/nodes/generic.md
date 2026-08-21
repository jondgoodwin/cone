Generics and macros have **no node of their own**. A generic is an ordinary
`FnDclNode` or `StructNode` carrying a `GenericInfo` side block; carrying one is
the entire definition. Instantiation is `cloneNode`, and cloning is what stands
in for name resolution on an instance.

**At a glance.** The parser attaches `GenericInfo` after the name. Name
resolution resolves the **template once, in place**, with parameters hooked.
Type check **never checks a template** — only clones. `genericSubstitute` from a
call either memo-hits an existing instance or clones a new one. Flow and
generation see only instances, reachable **only** through `memonodes`.

*Provenance: read from source; the symbol collision was measured. The
parameter-leak escalation this note used to list was measured, then fixed.*

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
**dead** — macros are never memoized; every expansion is a fresh clone.

**`CloneState`** carries `instnode` (stamped into every cloned node, and what
`errorMsgNode` walks to print the instantiation trace), `selftype`, and `scope`.
**The declaration substitution map is not in it** — that is three file-scope
globals in `clone.c`.

## Parse

`parseGenericParms` is `[ Ident (,? Ident)* ]`. **No bounds, no constraints, no
defaults, no kinds.** An empty list is `ErrorNoGenParms`.

Attached by `parseFn` only in the named branch — so an anonymous function can
never be generic — and by `parseStruct` after the type name. `genname` is fixed
to the plain source name here; the type-argument suffix is added at generation.

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
parameter's `Name` directly to the **argument node**. `cloneNode`'s
`GenVarUseTag` arm then reads `namesym->node` and clones it. So substitution is
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
`GenVarUseTag` nodes standing for nothing, so every check would be a false
diagnostic. The cost is silent acceptance — see Hazards.

`genericSubstitute`, from `fnCallTypeCheck` **after** the arguments are checked:

1. Bail unless the callee carries `GenericInfo`.
2. If **any** argument is a type node, take the explicit path: `genericMemoize`,
   then check the replacement, return handled.
3. Otherwise **infer**: build a call node of NULL slots, walk the arguments
   against the template's parameter list, and where a parameter's type is a
   `GenVarUseTag`, capture the argument's type by `Name`. A slot filled twice
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

## Flow

**Templates are never flowed** — the only `blockFlow` entry point is the tail of
`fnDclTypeCheck`, unreachable for a template. That is safe because every
function reaching generation is either non-generic or an instance, and an
instance *is* flowed: `genericInstantiate` ends with `inodeTypeCheckAny`, which
for a declaration with `genericinfo == NULL` runs the full check including flow.

A template body could not be flowed even in principle — `itypeIsMove`,
`permGetFlags` and drop selection all read a type declaration a `GenVarUseTag`
does not have.

## Generation

`genlGlobalSyms` and `genlGlobalImpl` each have two generic branches, both
walking `memonodes` with the pair stride. Instances get
`LLVMLinkOnceAnyLinkage` — the C++ template answer, so several translation units
may emit one and the linker keeps one.

**Mangling keys off `instnode`.** A concrete function needs no suffix.
An instance's name gets `':' + itypeMangle(...)` per **parameter type** — the
return type is not mangled. A generic *type*'s methods recover their arguments
from `self`'s type through `itypeMangleNamed`, which is why
`fn tally(self) i64` does not collide across instances.

`itypeMangle` writes a named type through `itypeMangleNamed`, and the structural
types under their own punctuation: `&`/`+`/`<` for the three reference kinds,
`*` for a pointer, `(a,b)` for a tuple, `[n;elem]` for an array, `f(parms)ret`
for a signature, `v` for void. The last four were added after a generic
instantiated at a tuple, an array, a function reference or void was found to
mangle to **nothing**, so every such instance of one generic shared a name.

## Hazards

- **`cloneNode`'s `GenVarUseTag` arm has no guard.** It re-enters `cloneNode` on
  a *global* whose value at type-check time need not be what name resolution
  saw. NULL yields NULL silently; an unhandled tag kills the compile. This one
  arm is where an unrelated defect elsewhere becomes a hard abort.
- **A clone must clear the type check marks**, or the instance silently skips
  its own check. Only four clone functions do; every other copies `flags`
  verbatim. A new declaration-bearing node kind inherits the bug.
- **Two instances can still collide on one symbol.** Measured: `fn tag[T](a i32) i32`
  instantiated at `i32` and `f32` emits `@"tag:i32"` and `@"tag:i32.1"` —
  because `T` appears in no parameter, both mangle identically and LLVM
  disambiguates *within the module only*. Under `linkonce`, across object files
  the linker keeps one. **The rule: a generic function collides whenever its
  type arguments do not all appear in its parameter types.**
- **`--checktree` has the coverage exactly inverted.** It descends into
  templates, which are never type checked, and never into instances, because
  `memonodes` is not in its switch.
- **`MacroDclNode.memonodes` is dead**, and `GenericNameTag` is assigned by
  nothing.

## What lives elsewhere

- Instantiation scheduling, depth bounding, and why the marks cannot police it: [Type Check Phase](../phases/type-check.md), "Generics and macros"
- Hooking, and what a pushed table scopes: [Name Resolution](../phases/name-resolution.md), "Hooking"
- Symbol naming and `linkonce`: [Generation](../phases/generation.md)
- Cloning a struct, and the `Self` rebinding: [struct](struct.md)
