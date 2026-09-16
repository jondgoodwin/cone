Name resolution binds every name to its declaration and settles which
parser-ambiguous nodes are types and which are values. It is one eager pass over
the whole program, in source order.

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
   the right scope.
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

Push and pop sites: `modNameRes` (via `modHook`, the whole module namespace),
`structNameRes` (generic parms, then the whole type namespace), `fnDclNameRes`
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

**Qualified lookup bypasses the hook table entirely.** `a::b::name` walks
`qualNames` from `basemod` with `namespaceFind`, so it is not shadowed by
locals. Each intermediate must be a module or a struct.

## 3. Three namespace kinds, two mechanisms

| Kind | Owns a `Namespace` hash table? | Populated |
| --- | --- | --- |
| Module | yes, `ModuleNode.namespace` | at **parse** time by `modAddNamedNode`; extended by `importNameRes` folding |
| Namespaced type | yes, `INsTypeNode.namespace` | at **parse** time; `structNameRes` adds `Self` |
| Lexical block / parameter list | **no** | not a namespace at all — locals are hooked one at a time |

That a module's and a type's names exist before the pass runs is what lets the
pass be a single source-order walk with no forward-reference machinery: binding
a name needs the declaration to *exist*, not to be analyzed, and the parser
guarantees that. Locals are the exception and are deliberately order-dependent —
`varDclNameRes` resolves the initializer **before** hooking the name, so
`imm x = x` binds the outer `x` or fails.

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

Every one of these hinges on `isTypeNode`. For a name use it asks the
declaration the name was bound to, and an unlowered `FnCallNode` naming a
generic struct counts as a type (`itypeIsGenericType`). Without the latter,
`*Box[i64]` reads as a dereference and `[2; Box[i64]]` as an array literal.

**One site rewrites a parent's pointer**: `allocateQuesNameRes` collapses `&x?`
into the allocation node with `FlagQues` set. Everything else mutates in place.

## 5. Where it stops, and why

| Deferred to type check | Because |
| --- | --- |
| `.field` and `.method` — `fnCallNameRes` never walks the call's member slot, so `inodeNameRes` never meets a member name | selecting a member needs the receiver's type. `fnCallLowerMethod` does the lookup, the visibility check and the overload selection together |
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
  name — the `methfld` of a call — which this pass never visits.
- A `NameUseNode` is still a `NameUseTag`: it is bound, not retagged, and
  answers `isTypeNode`, `isExpNode` and `isMetaNode` for its declaration.
- No `TupleTag`, `StarTag` or `QuesTag` remains.
- Every `BreakTag`/`ContinueTag` has a non-NULL `block`.
- `return`/`break`/`continue` appear only as a block's last statement, modulo
  the `FlagLoopStep` allowance for `each`'s synthesized step.
- Every local `VarDclNode` carries its `scope`.
- Every `StructNode` namespace contains `Self`.
- Wildcard import folding is done, so module namespaces are complete.
- **Nothing is typed.** No `vtype` is established, no mark is set.

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
`ErrorNotPublic`, `ErrorDupName` (duplicate local, duplicate lifetime label,
colliding folded import), `ErrorRetNotLast`, `ErrorNoLoop`, `ErrorBadElems`,
`ErrorBadTerm` and `ErrorInvType`.

**A failed lookup does not un-resolve a successful one.** On the private
qualified-name path the declaration stays attached after the diagnostic: it is
the one the program asked for, and leaving the use unresolved would hand the
next pass a null to trip over.

## 8. Hazards

- **The pass is not idempotent, and cannot be.** `inodeNameRes` has no arm for
  `DerefTag`, `PtrTag`, `BorrowTag`, `AllocateTag`, `ArrayLitTag`, `VTupleTag`
  or `TTupleTag` — **all of which it produces**. A second walk falls into the
  `default:` arm, which reports `ErrorUnreachable` and stops.
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
| `ir/stmt/module.c` | `modNameRes`, `modHook` | imports walked before nodes; module hook push/pop |
| `ir/stmt/import.c` | `importNameRes` | wildcard folding; skips private and unnamed nodes |
| `ir/exp/block.c` | `blockNameRes`, `blockContinueStep` | scope push/pop, lifetime labels, jump placement, the one re-entry |
| `ir/stmt/vardcl.c` | `varDclNameRes` | value before name; duplicate check; local hooking and `scope` stamping |
| `ir/stmt/fndcl.c` | `fnDclNameRes` | generic parms, signature, body with parms hooked at scope 1 |
| `ir/types/struct.c` | `structNameRes` | base trait → `Self` → namespace hooked → fields → methods |
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
