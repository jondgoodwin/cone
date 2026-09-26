Name resolution binds every name to its declaration and settles which
parser-ambiguous nodes are types and which are values. It is one eager pass over
the whole program, in source order, with one departure: a type reached by
another type that is-a or mixes it in is resolved when it is first needed.

This note is the **mechanism**: how the walk works, what it mutates, where it
stops. [Names and Namespaces](../../../../doc/design/names-and-namespaces.md) is the **rules** — what a
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
   is known and the target's scope is hooked over it, or in place of it where
   the target is another module's.
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
| `nametblHookHideBelow` | hook every name the tables beneath hooked back to what it had before any of them, once per name |
| `nametblHookPop` | restore every saved pair in the order saved, drop the table |

Push and pop sites: `modFoldNames` and `modNameRes` (via `modHook`, the whole module namespace),
`structNameRes` (generic parms, then the whole type namespace, then each
inherited member as it lands), `structNameResDemand` (another module's
namespace, via `modHook`), `fnDclNameRes`
(generic parms, then value parms), `macroNameRes` (parms), `blockNameRes`
(locals, accumulated one at a time as they are reached), and
`clonePushState`/`clonePopState` — **which run during type check**, for generic
substitution.

**Some bindings are never hooked and never popped.** `stdPermInit` and
`stdNbrInit` assign `namesym->node` directly before any hook table exists, so
`uni`, `mut`, `imm`, `ro`, `i32`, `Bool` and the rest are visible for the whole
compile. `keyAdd` does the same for keywords, with a `KeywordTag` sentinel.
`initAll` and `finalAll` are bound the same way (`newStitchFn`). So a global
name one of them holds is a duplicate at parse (`modAddNamedNode`), reported once
at the declaration or the module's `mod` line and naming what the name already
is, since the built-in has no place in any source to point at, and a keyword's
sentinel no position at all (`module_builtin_name_parse`). A module's name
comes from its folder or file, not a name token, so it can be a keyword or a
permission, which no name token can be; the `mod` line restating that name is
then taken as naming the module (`parseModuleDcl`) rather than reported as
having none, with follow-ons. A reserved word is the exception to the
duplicate: `modAddNamedNode` reports it as `ErrorReserved`, as `lexScanIdent`
does one it reads, and releases it the same way, so the name is bound and
neither the parent's namespace nor the `mod` line reports it again. ⚠ **Whether that
refusal is right for a built-in type's name** (`i8`, `f64`) is open: these
names are looked up after a module's own, which would let a module's name hide
one, and nothing written says a module may not. The numbers work settles it
(Jon, 24 September 2026): the number types move into `core`, and core's name
rule then decides the case, since no module may declare a name core already
defines; until then today's refusal stands. `initAll` and `finalAll` are
settled already: they hold their place as `i64` does, so only a local
declaration hides them and a module-level one is refused (Jon, 24 September
2026; [module](../nodes/module.md), "Init and final").

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

**A module's names replace what is hooked; they do not layer over it.** `modHook`
pushes two tables: one that hides every name the tables beneath it hooked
(`nametblHookHideBelow`), and over it the module's namespace, which is the table
a fold's binding joins (`modFoldBind`). So wherever a module is entered while
something else is hooked — a module's folds run dependency-first from inside
another's (`modFoldNames`), a type resolved by demand from another module
(`structNameResDemand`), an extending enum demanded from a function body
(`structEnumDemandSet`) — only that module's own names and the permanently bound
ones are in reach. Layering would let the outer scope's names answer whatever
the inner module's namespace does not hold: a sister's `use Dir;` would find the
`Dir` of the sister whose import reached it, and a child's its parent's.
`module_fold_scope_nameres` pins each.

**A path starts in the hook table like anything else.** `a.b.name` looks `a` up
as the bare name it is, so a local of that spelling shadows a module of it.
Only the hops after the first use `namespaceFind`, and each of those must land
on a module or a struct. The collapse that walks them is `fnCallNameResPath` —
[fncall](../nodes/fncall.md), "The path collapse".

## 3. Three namespace kinds, two mechanisms

| Kind | Owns a `Namespace` hash table? | Populated |
| --- | --- | --- |
| Module | yes, `ModuleNode.namespace` | at **parse** time by `modAddNamedNode`, which is also where an import binds the module it names; extended by the fold pass — `importNameRes` for what the module extends and what it imports, `foldGlobalExpand` and `foldModUseExpand` — ahead of any module's body |
| Namespaced type | yes, `INsTypeNode.namespace` | at **parse** time, an enum's variants included; `structNameRes` adds `Self`, the default methods of every abstraction the type's `is` list names, for a variant, its enum's fields, and for an enum that extends another, its copies of the base's variants |
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
names and the demanding type's generic parameters. Where the target lives in
another module, that module's namespace is hooked in place of them (`modHook`,
section 2) — a namespace that already holds everything that
module folded in, since every module's folds run before any module's body. The
demand asks for `modFoldNames` on that module first, which does something only
where the fold pass itself is what reached the type — and where that module's
folds are running, round a loop of imports already refused, the type is resolved
against what it holds so far, once ([module](../nodes/module.md), Hazards). What a
demanded type copies in arrives bound and is not walked again. The steps are in
[struct](../nodes/struct.md), "Name resolution".

**One demand reaches past a type.** An enum that extends another copies its base's
variants while it is resolved, cloning each one resolved as a generic template is
cloned, so the copies exist only from then on. A module's `use RichColors;` in the
fold pass and a path `RichColors.Red` in a function body both need them, so both
demand the enum (`structEnumDemandSet`). From a body, that demand clears the body's
block scope and hooks the enum's own module namespace in place of the body's locals, so the
enum resolves as if the walk had reached it. The copies are no module's nodes and
are never walked. A generic base is written with its arguments, and its copies are
its variant templates with the arguments substituted for its parameters, made here
all the same: generic templates of the extension where it is generic, ordinary
variants where it is not. [struct](../nodes/struct.md), "An enum extending an enum".

## 4. What it retags

A name use is not among them. `nameUseNameRes` sets `dclnode` and the node stays
a `NameUseTag`; whether it is a type, a value or a macro is asked of the
declaration (`nameUseGroup`) by everything that needs to know, this table's
functions included.

| Before | After | Function |
| --- | --- | --- |
| `TupleTag` | `TTupleTag` / `VTupleTag`; mixed is `ErrorBadElems` and the tag is left alone; a generic parameter abstains, and all abstaining is `VTupleTag` | `ttupleNameRes` |
| `StarTag` | `PtrTag` / `DerefTag` | `ptrNameRes` |
| `ArrayTag` | `ArrayLitTag` when the first element is not a type | `arrayNameRes` |
| `RefTag` | `BorrowTag` / `AllocateTag`, by region | `refNameRes` |
| `ArrayRefTag` | `ArrayBorrowTag` / `ArrayAllocTag` | `arrayRefNameRes` |
| `QuesTag` | `FnCallTag` for `Option[T]`, including a generic parameter's `?T` | `allocateQuesNameRes` |
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
A use of a generic parameter is not a type either, so in a generic's template
these votes are provisional: the instance's clone takes the tuple, array,
reference and pointer votes again, and the tuple and `?` votes, whose losing
side is an error, let such an operand abstain (`inodeIsProvisionalType`) —
[generic](../nodes/generic.md), "phase boundary".

**A reference in a pattern stays a reference type** whatever its referent's
name means for now: `refNameRes` asks `castPatternPending` too, because a
pattern's bare root is bound at type check and may be unbound, or mean a value
lexically, until then. `case imm c &Circle` is a narrowing, never a borrow.

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
  name — the `methfld` of a call — which this pass never visits, and a
  pattern's bare root (`FlagPattern`) that has no lexical meaning, which may be
  a variant of the matched value's enum and is left for type check to bind. A
  member the path collapse bound is no longer in a member slot: it has become
  the node, or the call's `objfn`.
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
  abstraction its `is` list names, an enum's fields where this is a variant, and
  the copies and aliases
  every fold clause admits, wherever the trait or the field's type was a
  declaration when the type was resolved — what an instance of a
  generic trait contributes, and the names folded from a field of a generic's
  parameter type, join at type check.
- Every `StructNode` and `ModuleNode` carries `NameResolved`, the phase's own
  mark; `NameResolving` is never left set.
- Wildcard import folding is done, so module namespaces are complete.
- **Every function, global and type an expanded body names carries
  `DclExpandReached`.** An expanded body is one an importer generates in its own
  object rather than calling: an `inline` or generic function, a macro, a
  trait's default, and any method of a generic type (`fnDclIsExpanded`).
  `NameResState.expander` is set for the duration of such a body —
  `fnDclNameRes` and `macroNameRes` set it, a function nested in the body
  inherits it, and `structNameRes` clears it for its members — and
  `nameUseNameRes` marks what each bound use names, a pre-bound path included;
  an overload name marks every candidate. It is the one fact this pass writes
  onto a declaration other than the one it is resolving, and only a library
  compile reads it (`dclIsExported`, [Generation](generation.md)). A method
  reached through a receiver is bound at type check and so is never marked;
  the type it belongs to is, where the body names that type.
- **What each expanded body names is recorded against it** (`exportReachAdd`):
  every declaration on the way to what a use means — a typedef it names, then
  the function, global or type — and a macro or a const, which carry no
  `DclInfo` to mark. **A parameter's default value counts as an expanded body of
  its function** (`NameResState.sigfn`, set by `fnDclNameRes` round the
  signature, read by `fnSigNameRes`), since an importer evaluates it where it
  calls. The include-file generator follows the record, so what a body it copies
  names is declared beside it, in whichever of the package's modules it sits
  ([module](../nodes/module.md), "Generating the include file").
- **Nothing is typed.** No `vtype` is established, and no type check mark is
  set.

**The global gate.** `doAnalysis` returns before type check if this pass
reported anything, so type check never meets an unbound name. That is what lets
`nameUseNameRes` simply leave `dclnode` NULL on failure — nothing downstream
ever sees it. The one unbound name that reaches type check is a pattern's root,
which this pass does not report: what it means depends on the matched value's
type. It is reached only through the `is` test and the conversion its pattern
desugars to, and both bind it (`castPatternBind`) before anything reads it —
to a variant, to its lexical meaning, or, reported, to `errorType`. The cost is that a file cannot report a name error and an
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
It also raises `ErrorUnkName` (two sites: a bare name in `nameUseNameRes`, and
a path's member in `fnCallNameResPath`; never for a pattern's bare root, which
type check reports),
`ErrorNotPublic` (a private name through a qualifier; a private field or member
in a fold), `ErrorDupName` (duplicate local, duplicate lifetime label,
colliding folded import, a trait's field arriving under a name the type
declares, a folded name already taken), `ErrorCircular` (two types that each
extend or name the other in an `is`, or a type folding from a field of a type not yet
complete; the same code type check gives a declaration defined in terms of
itself), `ErrorImportLoop` (modules depending on each other round a loop — by an
import of a module or of a name of it, by `extends`, or by containment, a child
depending on its parent — raised by `pgmModuleOrder` before any fold runs,
[module](../nodes/module.md), "The module order"), `ErrorNoMbr` (a fold naming a member the field's type lacks),
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
  trait already resolved. It is also why a bound pattern's conversion
  (`FlagMatchBind`) does not resolve its type: the node is the `is` test's, which
  resolved it, and `&x` with `x` a value has been retagged a borrow by then.
  A path the test collapsed is the one thing it must pick up: the collapse
  replaced the test's slot, not the node, so the conversion takes the hop's
  member (`castNameRes`). **It is also why the fold passes repeat folds and never
  resolution**: a later pass reads namespaces again and retries a listed item,
  but a global's fold and an enum's `use`, which resolve
  nodes, wait until what they name is bound (`modFoldAwaits`, a lookup that
  resolves nothing) and then run once (`FoldClause.expanded`).
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
- **`pstate->typenode` is read once** in this phase: `fnDclIsExpanded` asks
  whether a method's type is a trait or a generic, whose every method an
  importer expands. `structNameRes` saves and restores it.
- **`fnSigNameRes` forces `scope = 0`** so that a signature reached as a *type*
  does not leak its parameter names. `fnDclNameRes` hooks the same parameters
  itself, at scope 1, only when there is a body.
- **Resolve before hooking, in both senses.** A variable's value before its
  name; a struct's base trait before any type member.

## 9. Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `conec.c` | `doAnalysis` | initializes `NameResState`, walks, gates on `errors` |
| `ir/ir.h` | (`NameResState`) | `mod`, `typenode`, `loopblock`, `macromethod`, `expander`, `scope`, and why it is separate from `TypeCheckState` |
| `ir/stmt/fndcl.c` | `fnDclIsExpanded` | whether an importer expands a function's body, which makes it the `expander` for what the body names |
| `ir/inode.c` | `inodeNameRes` | the dispatch switch — start here to add a node kind |
| `ir/nametbl.c` | `nametblFind`, `nametblHook*` | interning and the hook stack that implements all scoping |
| `ir/namespace.c` | `namespaceFind`, `namespaceSet` | the hash table a module or type owns |
| `ir/exp/nameuse.c` | `nameUseNameRes` | the whole resolution decision: early-out, qualified walk, privacy; it binds `dclnode`, and inside an expanded body marks what it names (`nameUseMarkExpandReached`) |
| `ir/exp/nameuse.c` | `nameUseGroup` | what a resolved name answers to `isExpNode`, `isTypeNode` and `isMetaNode`, asked of its declaration |
| `ir/stmt/program.c` | `pgmNameRes` | five steps over the module list: what every module's `extends` names, the module order (`pgmModuleOrder`), every module's folds (`modFoldAll`), the module-trait conformance a module's fold pass could not make, reported (`modTraitConform`), then every module's body |
| `ir/stmt/program.c` | `pgmModuleOrder` | imports form a DAG [Jon 23 Sep]: every module placed after what it depends on — each module it imports, the module holding a name it imports, the module it extends, each of its own submodules — in `pgm->initorder`, the order module `init`s run in (`genlStitch`); each loop `ErrorImportLoop` at the edge that closes it, naming the modules round it. A loop of `extends` alone is cut there; any other is left whole for the fold passes. [module](../nodes/module.md), "The module order" |
| `ir/stmt/modtrait.c` | `modTraitConform`, `modTraitNameRes` | a module's `is`: the module trait it names, resolved in the trait's own module's scope, and each default the module has no name for cloned into it, its body re-pointed at what the module has for each member; attempted at the end of the module's fold pass so what folds from it takes the copies. A requirement met by nothing is `ErrorModTraitMissing`. [module](../nodes/module.md), "Module traits" |
| `ir/stmt/module.c` | `modExtendsResolve` | what a module's `extends` names, looked up in its own namespace and then its parent's, and refused where it is not a concrete module it may reuse |
| `ir/stmt/module.c` | `modFoldAll` | the fold pass over every module, repeated until one binds nothing new where a pass left a fold waiting — a global whose type a later `use` of its own module brings — or met a module mid-fold round a loop already refused; then the pass that reports what could not be folded (`modFoldReporting`) |
| `ir/stmt/module.c` | `modFoldNames` | one module's folds for the current pass, dependency-first — what it extends, its imports, then its globals' `use` clauses, then its standalone `use` statements — before any module's body resolves, so load order decides nothing. A global's fold and a standalone `use` whose source is not bound yet wait (`modFoldAwaits`); a global's fold and an enum's `use` are made once, and a submodule's `use` runs in every pass, as an import's clause does |
| `ir/stmt/module.c` | `modFoldCollisionAt` | where a fold's collision is reported: at the fold a single pass would have met it at, whichever pass met it |
| `ir/stmt/fold.c` | `foldModUseExpand` | a module's standalone `use`: the source it names resolved; an enum's variants each bound as an alias in the module's namespace, private unless `pub use`; a submodule's names folded by an `ImportNode` marked `isuse`, through `importNameRes` (`foldModUseModule`), which refuses a module reached through an import (`ErrorUseImported`) and any module that is not a submodule (`ErrorModReach`) |
| `ir/stmt/module.c` | `modNameRes`, `modHook` | type aliases walked before the other nodes; module hook push/pop; the module's `NameResolving`/`NameResolved` marks. A generic module's type parameters are hooked over its own names for the walk, and what it holds that an instance is not built for is refused (`modGenericCheckBody`, `ErrorGenModBody`). [module](../nodes/module.md), "Generic modules" |
| `ir/stmt/program.c` | `pgmGenericModulesCheck` | where a generic module may not stand: an executable's root (`ErrorGenModRoot`), a parent of submodules (`ErrorGenModBody`), a `mod` line with a default fold (`ErrorGenModBare`) |
| `ir/stmt/import.c` | `importNameRes`, `importBindModule` | the module's own binding and each folded name, as aliases carrying the import's visibility; the source's *namespace* is what is read, and a private binding of it does not fold — except to a module that extends it, which takes every declaration and fold with the base's own visibility, but not the base's own import bindings (`FlagImportName`, left out by `importStarAdmits`). Run in every fold pass: a star clause reads its module afresh (`importFoldStar`, which marks what it makes `FlagUnlisted`), and a listed item not yet made tries again, waiting rather than reporting until the pass that reports |
| `ir/stmt/import.c` | `importBindName` | a submodule's import of a name of its parent [Jon 23 Sep] — a loop through containment, refused by `pgmModuleOrder`, and bound all the same so the loop is what is reported — bound in the fold passes after the parent's folds (`modFoldNames`): an alias to the parent's own binding, written and `FlagImportName`, public where the import is; a name not there or private waits, then is `ErrorUnkName` or `ErrorNotPublic` in the pass that reports. A module answer becomes the import's `module`, so its `use` clause folds as any import's does; a clause on anything else is `ErrorBadFold`. A bare name a file answered at parse is checked in the pass that reports against what the parent binds under it (`ErrorDupName`) |
| `ir/stmt/module.c` | `modFoldBind` | every fold's binding into a module's namespace — an import's, an `extends`, a global's clause, an enum's `use` — where the same declaration by the same route, one of the two brought by a star clause (`FlagUnlisted`), is the binding the name has already, public if either route is, a fold if either route is one (it clears `FlagImportName`); the same declaration written twice, and a different declaration, collide [Jon 23 Sep] |
| `ir/stmt/module.c` | `modFoldDupReport` | the collision `modFoldBind` returns, as `ErrorDupName` at where `modFoldCollisionAt` places it: the same thing written twice, or one name meaning two things |
| `ir/exp/block.c` | `blockNameRes`, `blockContinueStep` | scope push/pop, lifetime labels, jump placement, the one re-entry |
| `ir/stmt/vardcl.c` | `varDclNameRes` | value before name; duplicate check; local hooking and `scope` stamping |
| `ir/stmt/fndcl.c` | `fnDclNameRes` | generic parms, signature, body with parms hooked at scope 1; then, for an `@intrinsic` declaration, `intrinsicDclNameRes` |
| `ir/stmt/intrinsic.c` | `intrinsicDclNameRes` | an `@intrinsic` declaration checked against the registry — in core's own module (`ErrorIntrinsicPlace`), a name it defines (`ErrorIntrinsicName`), its signature (`ErrorIntrinsicSig`), a body only where a fallback is allowed (`ErrorIntrinsicBody`) — then given its `IntrinsicNode`, or kept as an inline function when its fallback body is to be used ([intrinsic](../nodes/intrinsic.md)) |
| `ir/types/struct.c` | `structNameRes` | for a variant, its enum demanded and its namespace hooked beneath the variant's → `Self` → base trait → traits and fold sources demanded → namespace hooked → fields, each trait's members spliced in and hooked → fields indexed → each fold clause expanded and hooked → the type's own methods |
| | `structNameResDemand`, `structInheritTrait` | resolve a trait or a fold's source type ahead of the walk, in its own module's scope; copy a trait's members into the type |
| | `structEnumWrittenBase`, `structEnumSeedVariants`, `structEnumCopyVariant`, `structEnumDemandSet` | check what an enum's `extends` names, a generic base with its arguments; copy the base's resolved variants into the extension, substituting a generic base's parameters; resolve an extension from a fold or a path that needs its copies |
| | `structFoldExpand` | expand a field's `use` clause: a copy per folded field, an alias per folded method, entered and hooked — [struct](../nodes/struct.md), "Name folding" |
| `ir/stmt/aliasdcl.c` | `aliasDclResolve` | the declaration at the end of a chain of aliases, which every reader of a namespace binding asks for first |
| `ir/types/fnsig.c` | `fnSigNameRes` | forces scope 0 |
| `ir/itype.c` | `itypeIsGenericType` | makes an unlowered `Box[i64]` count as a type |
| `ir/exp/allocate.c` | `allocateQuesNameRes` | the one parent-pointer rewrite |
| `ir/clone.c` | `cloneNode`, `cloneDclFix`, `clonePushState` | how a resolved template survives instantiation |

## 10. What lives elsewhere

| Question | Note |
| --- | --- |
| What a name *means* — visibility, imports, aliases, overloading | [Names and Namespaces](../../../../doc/design/names-and-namespaces.md) |
| What the parser already bound before this pass ran | [Parse](parse.md) section 5 |
| What type check may assume from here | [Type Check Phase](type-check.md) section 1 |
| Member and overload selection | [Type Check Reasoning](type-check-reasoning.md) section 7 |
