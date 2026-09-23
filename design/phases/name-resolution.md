Name resolution binds every name to its declaration and settles which
parser-ambiguous nodes are types and which are values. It is one eager pass over
the whole program, in source order, with one departure: a type reached by
another type that is-a or mixes it in is resolved when it is first needed.

This note is the **mechanism**: how the walk works, what it mutates, where it
stops. [Names and Namespaces](../phases/names-and-namespaces.md) is the **rules** — what a
name means, visibility, imports, aliases, overloading. Change a rule there;
change how the walk implements it here.

*Provenance: read from source; the `Name.node` read sites and the parse-time
namespace guarantee were measured. See
[Measuring](../diagnostics/measuring.md).*

## 1. Principles — [derived]

⚠ **Read from source, with the `Name.node` read sites measured.** **Unchecked
with the author is the claim that these rule rather than describe.**

1. **There is no lookup routine.** No scope chain, no search path. Unqualified
   resolution is one pointer read: `name->dclnode = name->namesym->node`. ▸
   **Forbids** any rule that would need to *search* for a binding — which is why
   a name whose namespace depends on a value's type cannot be resolved here at
   all, and is deferred wholesale.
2. **Scoping is hooking.** Entering a scope *plugs* declarations into each name's
   single `node` slot and stacks the previous; leaving restores it. ▸ **Settles**
   that scope entry and exit must be perfectly paired, and **forbids** resolving
   a node out of walk order, since the slot's contents are only correct inside
   the right scope. The one departure is a type declaration reached from
   another type declaration (section 3), where what is plugged in at the jump
   is known and the target's scope is hooked over it.
3. **It binds and retags in place; it does not rewrite.** A name use is bound —
   `dclnode` is set and the node keeps its tag, since what it is can be asked of
   the declaration — and the parser-ambiguous shapes are retagged. Either way the
   phase mutates the node it was handed; swapping one node for another is type
   check's job, with exactly one exception. ▸ **Settles** that a caller's pointer
   stays valid across this phase, which is what lets the walk hand out nodes
   without indirection.
4. **Anything needing a type is deferred wholesale.** The one lookup primitive
   is a read from a global slot. A name whose namespace depends on a value's
   type cannot use it, so member names, overload selection and instantiation all
   wait.

## 2. Hooking

`ir/nametbl.c` carries the argument:

> Name hooking is a performant alternative to tries or search paths. When a
> namespace context comes into being, its names are 'hooked' into the global
> symbol table, replacing the appropriate IR node with the current one for that
> name. Since all names are memoized symbols, there is no search.

| Function | Does |
| --- | --- |
| `nametblHookPush` | push a `HookTable` — a LIFO stack of `{Name*, previous INode*}` |
| `nametblHookNode` | save `name->node`, then overwrite it |
| `nametblHookNamespace` | hook every occupied slot of a `Namespace` |
| `nametblHookPop` | restore every saved pair in reverse, drop the table |

Push and pop sites: `modFoldNames` and `modNameRes` (via `modHook`, the whole module namespace),
`structNameRes` (generic parms, then the whole type namespace, then each
inherited member as it lands), `structNameResDemand` (another module's
namespace, via `modHook`, over whatever is current), `fnDclNameRes`
(generic parms, then value parms), `macroNameRes` (parms), `blockNameRes`
(locals, accumulated one at a time as they are reached), and
`clonePushState`/`clonePopState` — **which run during type check**, for generic
substitution.

**Some bindings are never hooked and never popped.** `stdPermInit` and
`stdNbrInit` assign `namesym->node` directly before any hook table exists, so
`uni`, `mut`, `imm`, `ro`, `i32`, `bool` and the rest are visible for the whole
compile. `keyAdd` does the same for keywords, with a `KeywordTag` sentinel.

**The slot is not this phase's alone.** `lexScanIdent` reads it on *every*
identifier, to classify keywords and permissions and to release a reserved word;
`varDclNameRes` and `blockNameRes` read it for duplicate detection;
`modAddNamedNode` reads it at parse time; and `cloneNode` reads it during **type
check**, for generic substitution. Anything scoping a change to the hook table
has to account for all five, not just the lookup.

**Lookup order falls out of push order**, not from a written rule: innermost
block locals → outer block locals → function value parms → generic parms →
type members including `Self` → module names, own and folded → the permanently
bound corelib names.

**A path starts in the hook table like anything else.** `a.b.name` looks `a` up
as the bare name it is, so a local of that spelling shadows a module of it.
Only the hops after the first use `namespaceFind`, and each of those must land
on a module or a struct. The collapse that walks them is `fnCallNameResPath` —
[fncall](../nodes/fncall.md), "The path collapse".

## 3. Three namespace kinds, two mechanisms

| Kind | Owns a `Namespace` hash table? | Populated |
| --- | --- | --- |
| Module | yes, `ModuleNode.namespace` | at **parse** time by `modAddNamedNode`, which is also where an import binds the module it names; extended by the fold pass, `importNameRes` and `foldGlobalExpand`, ahead of any module's body |
| Namespaced type | yes, `INsTypeNode.namespace` | at **parse** time; `structNameRes` adds `Self`, the default methods of every abstraction the type is-a or mixes in, and, for a variant, its enum's fields |
| Lexical block / parameter list | **no** | not a namespace at all — locals are hooked one at a time |

That a module's and a type's names exist before the pass runs is what lets the
pass be a source-order walk with no forward-reference machinery: binding
a name needs the declaration to *exist*, not to be analyzed, and the parser
guarantees that. Locals are the exception and are deliberately order-dependent —
`varDclNameRes` resolves the initializer **before** hooking the name, so
`imm x = x` binds the outer `x` or fails.

**Inherited and folded members are the other exception, and they are why the
pass has one use of demand.** A type's dictionary is built whole before any of
its method bodies is resolved, so that a body may name an inherited field, a
default method or a member folded in from a field's type bare, as it names the
type's own — and those members are *copies*, or aliases bound to members, of a
trait or a field type whose own members must therefore be complete first. So
`structNameRes` demands the trait, and the type of each field carrying a `use`
clause (`structNameResDemand`): resolves it now if the walk has not reached
it, in its own module's scope when it lives elsewhere.
Two marks on the type make that safe, `NameResolving` and `NameResolved`, and a
trait found still under way is a cycle, `ErrorCircular`. The demand is confined
to a type reached from a type, so what is hooked at the jump is always module
names and the demanding type's generic parameters, and the target's module
namespace is hooked over them — a namespace that already holds everything that
module folded in, since every module's folds run before any module's body. The
demand asks for `modFoldNames` on that module first, which does something only
where the fold pass itself is what reached the type. What a
demanded type copies in arrives bound and is not walked again. The steps are in
[struct](../nodes/struct.md), "Name resolution".

## 4. What it retags

A name use is not among them. `nameUseNameRes` sets `dclnode` and the node stays
a `NameUseTag`; whether it is a type, a value or a macro is asked of the
declaration (`nameUseGroup`) by everything that needs to know, this table's
functions included.

| Before | After | Function |
| --- | --- | --- |
| `TupleTag` | `TTupleTag` / `VTupleTag`; mixed is `ErrorBadElems` and the tag is left alone | `ttupleNameRes` |
| `StarTag` | `PtrTag` / `DerefTag` | `ptrNameRes` |
| `ArrayTag` | `ArrayLitTag` when the first element is not a type | `arrayNameRes` |
| `RefTag` | `BorrowTag` / `AllocateTag`, by region | `refNameRes` |
| `ArrayRefTag` | `ArrayBorrowTag` / `ArrayAllocTag` | `arrayRefNameRes` |
| `QuesTag` | `FnCallTag` for `Option[T]` | `allocateQuesNameRes` |
| `FnCallTag` that is a namespace hop | the bound name use, or a plain call of it | `fnCallNameResPath` |

The last is the path collapse, and it is a *replacement* rather than a retag:
a period whose left side names a module or a type is a path, and the hop is
folded away — [fncall](../nodes/fncall.md), "The path collapse". It is in this
pass and not in type check because the rows above ask `isTypeNode` of their
operands, and `&mut mymod.Gadget` has to be a resolved type name by then.

Every one of these hinges on `isTypeNode`. For a name use it asks the
declaration the name was bound to, and an unlowered `FnCallNode` naming a
generic struct counts as a type (`itypeIsGenericType`). Without the latter,
`*Box[i64]` reads as a dereference and `[2; Box[i64]]` as an array literal.

**Two sites rewrite a parent's pointer**: `allocateQuesNameRes` collapses `&x?`
into the allocation node with `FlagQues` set, and `fnCallNameResPath` replaces
an argument-less namespace hop with the name it bound. Everything else mutates
in place.

## 5. Where it stops, and why

| Deferred to type check | Because |
| --- | --- |
| `.field` and `.method` on a *value* — `fnCallNameRes` never walks the call's member slot, so `inodeNameRes` never meets a member name | selecting a member needs the receiver's type. `fnCallLowerMethod` does the lookup, the visibility check and the overload selection together. A period on a *namespace* is settled here instead: the receiver's type is not what it needs |
| Rewriting a bare field name to `self.field` | that is lowering — it builds a call node and takes its type from what the call resolves to, and there is no type here to work from |
| Overload selection | needs argument types. The name binds to the `FnOverloadDclNode`; `fnCallLowerOverloadFn` picks the candidate |
| Generic instantiation and macro expansion | there is no `NameRes` function in `ir/meta/generic.c` at all |

The last is worth stating positively: **a template is name-resolved once, in
place, and instances are cloned rather than re-resolved.** `cloneNameUseNode`
calls `cloneDclFix` to re-point a cloned use at the correspondingly cloned
declaration, and a use naming a generic parameter is replaced by a clone of
whatever `clonePushState` hooked its name to. A resolved template plus a substitution map
is the contract; there is never a second name resolution pass.

## 6. Contract

**Guaranteed when the pass finishes without errors:**

- Every reachable `NameUseNode` has a non-NULL `dclnode`, **except** a member
  name — the `methfld` of a call — which this pass never visits. A member the
  path collapse bound is no longer in a member slot: it has become the node, or
  the call's `objfn`.
- No `FnCallNode` left in the tree has a member name whose receiver is a module
  or a struct's *name*. Every one of those was a path and is gone.
- A `NameUseNode` is still a `NameUseTag`: it is bound, not retagged, and
  answers `isTypeNode`, `isExpNode` and `isMetaNode` for its declaration.
- No `TupleTag`, `StarTag` or `QuesTag` remains.
- Every `BreakTag`/`ContinueTag` has a non-NULL `block`.
- `return`/`break`/`continue` appear only as a block's last statement, modulo
  the `FlagLoopStep` allowance for `each`'s synthesized step.
- Every local `VarDclNode` carries its `scope`.
- Every `StructNode` namespace contains `Self`, the default methods of every
  abstraction it is-a or mixes in, an enum's fields where this is a variant, and
  the copies and aliases
  every fold clause admits, wherever the trait or the field's type was a
  declaration when the type was resolved — what an instance of a
  generic trait contributes, and the names folded from a field of a generic's
  parameter type, join at type check.
- Every `StructNode` and `ModuleNode` carries `NameResolved`, the phase's own
  mark; `NameResolving` is never left set.
- Wildcard import folding is done, so module namespaces are complete.
- **Nothing is typed.** No `vtype` is established, and no type check mark is
  set.

**The global gate.** `doAnalysis` returns before type check if this pass
reported anything, so type check never meets an unbound name. That is what lets
`nameUseNameRes` simply leave `dclnode` NULL on failure — nothing downstream
ever sees it. The cost is that a file cannot report a name error and an
unrelated type error in one run. Removing it needs a per-node
unresolved/resolving/resolved state, and every site that reads `dclnode`
handling an unbound one — which the gate makes impossible today.

**There is no poison value in this phase.** `errorType` and `newErrorNode`
belong entirely to type check.

## 7. Diagnostics

The phase owns one `ErrorCode` exclusively: `ErrorBareMbr` (1076), raised by
`nameUseNameRes` when a macro method's body names a member of its type bare —
`NameResState.macromethod` is set for the duration of the body, and the name is
known to be a member here, where type check would only see the wrong receiver.
It also raises `ErrorUnkName` (three sites in `nameUseNameRes`),
`ErrorNotPublic` (a private name through a qualifier; a private field or member
in a fold), `ErrorDupName` (duplicate local, duplicate lifetime label,
colliding folded import, a trait's field arriving under a name the type
declares, a folded name already taken), `ErrorCircular` (two types that each
extend or mix in the other, or a type folding from a field of a type not yet
complete — the same code type check gives a declaration defined in terms of
itself), `ErrorNoMbr` (a fold naming a member the field's type lacks),
`ErrorBadFold` (a fold admitting what cannot fold: a static, a macro without
`self`, the value's own `final` or `clone`, or a source that is not a struct),
`ErrorRetNotLast`, `ErrorNoLoop`, `ErrorBadElems`, `ErrorBadTerm` and
`ErrorInvType`.

**A failed lookup does not un-resolve a successful one.** On the private
qualified-name path the declaration stays attached after the diagnostic: it is
the one the program asked for, and leaving the use unresolved would hand the
next pass a null to trip over.

## 8. Hazards

- **The pass is not idempotent, and cannot be.** `inodeNameRes` has no arm for
  `DerefTag`, `PtrTag`, `BorrowTag`, `AllocateTag`, `ArrayLitTag`, `VTupleTag`
  or `TTupleTag` — **all of which it produces**. A second walk falls into the
  `default:` arm, which reports `ErrorUnreachable` and stops. This is why
  `structNameRes` walks only the methods the type declared and never the
  clones a trait's expansion appended, and why those clones are made from a
  trait already resolved.
- **A demanded trait is resolved with the demanding type's generic parameters
  still hooked.** A name the trait fails to declare that spells one of them
  binds to it silently instead of failing. Nothing correct can meet it; a
  program that does is already in error.
- **`blockContinueStep` is the one re-entry.** After the statement loop,
  `blockNameRes` clones an `each` loop's trailing step ahead of a `continue` and
  re-runs `inodeNameRes` on the copy. It works because resolved `NameUseNode`s
  early-out. It must run after the statement loop (so `continue` targets are
  known) and before `nametblHookPop` (so the copy can still see the loop
  variable).
- **`gVarDclNameRes` hooks unconditionally**, so a generic parameter binds
  wherever it is resolved. `fnDclNameRes`, `structNameRes` and `macroNameRes`
  each resolve their parameter list *inside* `nametblHookPush` for exactly that
  reason; resolving one beforehand leaks it into the enclosing scope, where the
  matching pop never reaches it.
- **`pstate->typenode` is written and never read** in this phase.
  `structNameRes` saves and restores it; nothing consults it.
- **`fnSigNameRes` forces `scope = 0`** so that a signature reached as a *type*
  does not leak its parameter names. `fnDclNameRes` hooks the same parameters
  itself, at scope 1, only when there is a body.
- **Resolve before hooking, in both senses.** A variable's value before its
  name; a struct's base trait before any type member.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `conec.c` | `doAnalysis` | initializes `NameResState`, walks, gates on `errors` |
| `ir/ir.h` | (`NameResState`) | `mod`, `typenode`, `loopblock`, `scope`, and why it is separate from `TypeCheckState` |
| `ir/inode.c` | `inodeNameRes` | the dispatch switch — start here to add a node kind |
| `ir/nametbl.c` | `nametblFind`, `nametblHook*` | interning and the hook stack that implements all scoping |
| `ir/namespace.c` | `namespaceFind`, `namespaceSet` | the hash table a module or type owns |
| `ir/exp/nameuse.c` | `nameUseNameRes` | the whole resolution decision: early-out, qualified walk, privacy; it binds `dclnode` and changes nothing else |
| `ir/exp/nameuse.c` | `nameUseGroup` | what a resolved name answers to `isExpNode`, `isTypeNode` and `isMetaNode`, asked of its declaration |
| `ir/stmt/program.c` | `pgmNameRes` | two walks of the module list: every module's folds, then every module's body |
| `ir/stmt/module.c` | `modFoldNames` | a module's folded names put in place dependency-first — its imports, then its globals' `use` clauses — before any module's body resolves, so load order decides nothing |
| `ir/stmt/module.c` | `modNameRes`, `modHook` | type aliases walked before the other nodes; module hook push/pop; the module's `NameResolving`/`NameResolved` marks |
| `ir/stmt/import.c` | `importNameRes`, `importBindModule` | the module's own binding and each folded name, as aliases carrying the import's visibility; the source's *namespace* is what is read, and a private binding of it does not fold |
| `ir/exp/block.c` | `blockNameRes`, `blockContinueStep` | scope push/pop, lifetime labels, jump placement, the one re-entry |
| `ir/stmt/vardcl.c` | `varDclNameRes` | value before name; duplicate check; local hooking and `scope` stamping |
| `ir/stmt/fndcl.c` | `fnDclNameRes` | generic parms, signature, body with parms hooked at scope 1 |
| `ir/types/struct.c` | `structNameRes` | `Self` → base trait → traits and fold sources demanded → namespace hooked → fields, each trait's members spliced in and hooked → fields indexed → each fold clause expanded and hooked → the type's own methods |
| | `structNameResDemand`, `structInheritTrait` | resolve a trait or a fold's source type ahead of the walk, in its own module's scope; copy a trait's members into the type |
| | `structFoldExpand` | expand a field's `use` clause: a copy per folded field, an alias per folded method, entered and hooked — [struct](../nodes/struct.md), "Name folding" |
| `ir/stmt/aliasdcl.c` | `aliasDclResolve` | the declaration at the end of a chain of aliases, which every reader of a namespace binding asks for first |
| `ir/types/fnsig.c` | `fnSigNameRes` | forces scope 0 |
| `ir/itype.c` | `itypeIsGenericType` | makes an unlowered `Box[i64]` count as a type |
| `ir/exp/allocate.c` | `allocateQuesNameRes` | the one parent-pointer rewrite |
| `ir/clone.c` | `cloneNode`, `cloneDclFix`, `clonePushState` | how a resolved template survives instantiation |

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What a name *means* — visibility, imports, aliases, overloading | [Names and Namespaces](../phases/names-and-namespaces.md) |
| What the parser already bound before this pass ran | [Parse](parse.md) section 5 |
| What type check may assume from here | [Type Check Phase](type-check.md) section 1 |
| Member and overload selection | [Type Check Reasoning](type-check-reasoning.md) section 7 |
