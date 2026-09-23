This note is the **rules**: what a name means, how lookup, visibility,
qualification, imports, aliases and overloading are supposed to behave, and
where the compiler does not yet match. Parts of it describe intended rather than
current behavior, and say so.

[Name Resolution](name-resolution.md) is the **mechanism** — how the walk
implements these rules, what it retags, and where it stops. Change a rule here;
change how it is carried out there.

## Principles

⚠ **This note is the rules, so its principles ARE its subject** — the sections
below are those rules in detail rather than a separate layer above them.
**Stated here so that what they forbid is visible before the detail starts.**

**A namespace has a single uniqueness domain, whatever a name refers to.** A
module cannot hold a type and a function of the same name. ▸ **Forbids**
per-kind namespaces — the C struct-tag arrangement, or a language where a type
and a value may share a spelling. **Overload names are not an exception**: the
overload name is itself one name in the namespace, mapping to the candidates.

**A binding is not the thing it binds, and one value may have several
bindings.** Import folding and aliasing both produce a second binding to one
declaration. ▸ **Forbids** treating a name as a property of a declaration, and
**settles** why visibility is a bit on the *binding* — the binding for `B`
inside A can be private while `B` is a public package in its own right.

**Visibility is checked on the binding the caller's name reaches**, never on
the declaration overload selection then picks. ▸ **Forbids** a private concrete candidate joining a
public overload name (`ErrorPrivOverload`): through the public spelling the
private member would be reachable from outside its owner, and a symbol that is
private yet needed from outside has no sound linkage. An intrinsic candidate is
exempt, since it is never a symbol.

⚠ **Parts of this note describe intended rather than current behaviour and say
so.** The `NameDef` design below is the clearest case — the implementation
predates it.

The concepts should remain distinct:

- A **name** is an interned, case-sensitive spelling.
- A **NameDef** IR node binds a spelling within a namespace or lexical scope to the node it refers to.
- The **IR node value** of the NameDef is what the NameDef refers to. More than one NameDef may refer to the same IR value, as with import folding or aliasing.
- A **namespace** owns its unique NameDefs. It controls lookup, uniqueness, visibility, and qualification.

## Source-code manifest

The current implementation predates the full `NameDef` design described in
this note. It generally maps an interned `Name` directly to a heterogeneous IR
declaration node. Use this manifest to find the current behavior before
changing it.

### Name representation and lookup infrastructure

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/parser/lexer.c` | Interns keywords and identifier spellings through `nametblFind`, so equal spellings share one `Name`. |
| `src/c-compiler/ir/nametbl.c` | Owns the global intern table and the push/hook/pop mechanism used to expose the nearest lexical or namespace binding through `Name.node`. |
| `src/c-compiler/ir/namespace.c` | Implements the hash table owned by each module or namespaced type: initialize, find, set, add, duplicate detection, and growth. |
| `src/c-compiler/ir/name.c` | Defines well-known interned names and spells every declared symbol — `nameSymbol`, `nameVtable`, `nameVtableImpl`, `nameVtableThunk` — from a declaration's owner chain and facts. See "Symbols". |
| `src/c-compiler/ir/stmt/aliasdcl.c` | The alias: a binding that stands for another declaration under a spelling of its own, with a visibility of its own. Made for the members a field's `use` clause folds in; `aliasDclResolve` follows a chain to the declaration. |
| `src/c-compiler/ir/dclinfo.c` | The declaration facts a symbol is derived from: sets them where a declaration joins its namespace (`dclInfoJoin`), walks to the enclosing module, prints them in the IR dump. |
| `src/c-compiler/ir/inode.c` | Dispatches the name-resolution pass by IR node tag. Start here when a new node kind must participate in name resolution. |
| `src/c-compiler/ir/clone.c` | Rebinds generic/macro parameters during cloning and repairs resolved declaration references in cloned `NameUse` nodes. |

### Parsing and module namespaces

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/parser/parseexpr.c` | Parses a name as one identifier, and everything after a period as a member access — a path and a member of a value are the same production here. |
| `src/c-compiler/parser/parsemod.c` | Parses module-level declarations, `include`, `import`, and wildcard folding; loads/reuses modules, names each one, and establishes module hooks. |
| `src/c-compiler/parser/parsetype.c` | Parses struct/trait/enum members and inserts fields and methods into the type namespace. |
| `src/c-compiler/ir/stmt/program.c` | Owns the program's module list, reuses modules by interned name, and initiates name resolution for every module. |
| `src/c-compiler/ir/stmt/module.c` | Owns module namespaces, inserts global declarations with duplicate checks, switches active module hooks, folds imports before resolving other nodes, and walks module declarations. |
| `src/c-compiler/ir/stmt/import.c` | Implements wildcard import folding by adding imported named nodes to the receiving module namespace. |

### Name uses, lexical scopes, and declarations

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/ir/exp/nameuse.c` | Represents name and member uses, resolves a bare name through the hooks, and answers what a use is — type, value, macro — from the declaration it is bound to (`nameUseGroup`, `nameUseNames`). Binding a bare field name is here; **lowering it to `self.field` is `nameUseTypeCheck`'s**, because building that call node needs a type to check it against. Walking a path through module and type namespaces is `fncall.c`'s, with the collapse. |
| `src/c-compiler/ir/exp/block.c` | Pushes lexical scope hooks, binds labeled lifetimes, resolves statements in declaration order, and restores outer bindings on block exit. |
| `src/c-compiler/ir/stmt/fndcl.c` | Establishes function generic-parameter and value-parameter bindings while resolving signatures and bodies. |
| `src/c-compiler/ir/stmt/vardcl.c` | Resolves an initializer before binding its local variable, enforces same-scope uniqueness, and permits nested shadowing through scope hooks. |
| `src/c-compiler/ir/stmt/fielddcl.c` | Resolves field permission, type, and default-value names; namespace insertion is handled by the enclosing type. |
| `src/c-compiler/ir/stmt/const.c` | Resolves constant types and values; module insertion is handled by `module.c`. |
| `src/c-compiler/ir/types/typedef.c` | Resolves a typedef target and hooks the alias name for subsequent lookup. |

### Type members, methods, generics, and macros

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/ir/instype.c` | Provides shared namespaced-type operations, binding of each concrete function/method name and of its separate overload node, field/method lookup, and all-candidate method selection. |
| `src/c-compiler/ir/types/struct.c` | Owns struct/trait member namespaces; inserts fields, `Self`, the default methods of every abstraction the type is-a or mixes in and, for a variant, its enum's fields, the base resolved on demand first; hooks members and generic parameters during resolution; and performs the inherited-member collision checks. |
| `src/c-compiler/ir/exp/fncall.c` | Resolves fields and overloaded methods from type namespaces, lowers member access/calls, inserts implicit `self`, and finds `init` for type calls. **All of this is `fnCallTypeCheck`'s**, not name resolution's — `fnCallNameRes` walks `objfn` and the arguments and deliberately leaves `methfld` alone, since selecting a member needs the receiver's type. |
| `src/c-compiler/ir/meta/macro.c` | Establishes macro parameter scope and resolves names in macro bodies before expansion. |
| `src/c-compiler/ir/meta/genvardcl.c` | Binds generic variables into the active resolution scope. |

## What creates a namespace

Namespaces are organized at various levels of code: 
- Lexical scopes within functional logic
- Declared Types
- Generics and Macros
- Modules
### Lexical scopes

Function parameters, generic parameters, blocks, and nested blocks establish lexical name contexts, effectively comparable to a namespace for that scope. Inner blocks may shadow names from outer contexts; duplicate declarations in the same local scope are errors. Local visibility is order-dependent because a local name becomes available only after its declaration is resolved.

The compiler currently implements lexical lookup by temporarily hooking declarations onto the globally interned `Name` while traversing the scope's IR. It restores the previous binding when leaving the scope. This is a lookup optimization, not a reason for lexical bindings to differ semantically from namespace NameDefs.

Unqualified lookup selects the nearest active binding. Function parameters may therefore shadow names from the containing type or module, and a local in an inner block may shadow a parameter or outer local. A second declaration of the spelling within the same lexical scope is an error. A hidden member of a *type* is still reachable, through `self` or through the type's own name; a hidden member of the enclosing *module* is not reachable at all, because a path begins with the name of the namespace it walks and a module has no name of its own until it carries a `mod` header.

### Types

Named types expose a member namespace. The documented model includes fields, methods, static functions, and potentially nested types. One operator reaches all of them: `p.x` is a member of a value, `Point.make` a member of the type, and what the name before the period binds to is what tells them apart.

Current compiler behavior:

- Structs and traits have one namespace containing fields, methods, static functions, macros, inherited members, folded members (a copy of a folded field, an alias for a folded method), everything an `extends` base has, and `Self`.
- A field, static function or macro cannot collide with another member name. A macro declared in a type is a macro method when its first parameter is `self`, by the same rule as a function; it joins no overload set, and it is not inherited from a trait.
- Methods and static functions each declare a namespace-unique concrete name. A declaration may additionally name an overload set with `fn concrete overload shared(...)`. The concrete name binds directly to its `FnDclNode`; the overload name binds to a separate `FnOverloadDclNode` holding every candidate declared for it, including a set that currently has only one candidate. Two declarations claiming the same concrete name are a duplicate-name error, and an overload name already bound to anything other than an overload node is a collision error.
- Every executable implementation remains a separate `FnDclNode`. The overload node is only a namespace binding, so lookup, call lowering, trait reconciliation, vtables, and code generation always record the selected concrete node.
- A method cannot share a spelling with a field.
- Struct/trait generic parameters form an enclosing lexical context while the type is resolved.
- An enum's variants are bound in the surrounding **module** rather than in the enum's own namespace, which is why two enums each declaring a `Quit` collide. They belong in the enum's namespace, reached as `Event.KeyEvent` and folded into a module deliberately; that waits on a `use` that folds names into a module, which does not exist — the only fold today is `import mod.*`. It is what `Some`, `None`, `Ok` and `Error` being bare names currently rests on, through corelib's automatic wildcard import. `[planned]`
### Generics and Macros

The declared name of every generic or macro is an ordinary NameDef in its containing namespace, which may be a module/package or a type. It participates in the same cross-category uniqueness rules as every other name there.

Each generic or macro also owns a nested namespace hierarchy analogous to a function's. Its parameters and generic/macro variables are NameDefs in the declaration's private parameter scope, and its body contributes the usual nested lexical scopes. These names are visible only where permitted within that generic or macro declaration. Macro hygiene may impose additional boundaries on names introduced during expansion, but does not create a separate name domain for the macro declaration itself.

### Modules

Every program or library has a main module namespace. A module contains global variables and constants, functions, types, macros, and named modules. All immediate names must be unique, subject to the overload exception described below.

Current compiler behavior:

- The main source and every imported source are represented by `ModuleNode`.
- A parsed function always adds its concrete `FnDclNode` to the module's owned nodes and binds its unique name. When it declares an overload name, the module finds or creates that name's `FnOverloadDclNode`, appends the concrete node, and adds a newly created overload node to the module's owned nodes as well, so it is printed and can be folded in by a wildcard import.
- `include` parses another file directly into the current module, so included declarations share the same namespace and collision domain.
- `import` loads or reuses another module and binds that module's filename-derived name in the importing module.
- The parser does not currently provide syntax for declaring arbitrary named nested modules, although the IR and documentation anticipate modules containing modules.
- Source folders affect file lookup; they do not themselves create namespaces.

Documented intent allows named modules nested within modules and libraries packaged for import. The declaration syntax and package-level namespace rules remain underspecified.
## Uniqueness, overloading and `extern`s

The default rule is: **one spelling, one NameDef, at each namespace level**. This applies across declaration categories. A type and a variable, or a macro and a module, may not coexist under the same spelling in one namespace. With the exception of `extern`s and overloading, declaring duplicate names in the same namespace results in a compiler error.

#### `extern` handling of duplicate names

It is common practice for a package's interface to declare the same variables and functions as the package's source defines. The interface specifies them as `extern`s without values or function bodies; the source drops `extern` and supplies the implementation.

Matching `extern` declarations and an implementation resolve to one canonical NameDef. Their declared types must be equivalent or the compiler reports a conflict. The implementation supplies the NameDef's IR value and makes the current compilation unit responsible for emitting the definition; the matching `extern` contributes no second definition. If no implementation is present, the NameDef remains external and code generation emits only the declaration/reference needed by the linker. More than one implementation for the same concrete name is a duplicate-definition error.

A NameDef therefore records code-generation ownership or provenance separately from the namespace that lexically owns the binding. This determines whether the current compilation unit emits a concrete definition, treats the name as externally supplied, keeps a definition local/static, or emits a coalescible generic instantiation that the linker may merge with equivalent instantiations from other compilation units. Today that record is the declaration's `DclInfo` — see "Symbols" — whose `DclExternal` bit is the supply fact.

#### Function and method overloading

Every concrete function or method declares its own namespace-unique name. This is its stable, directly addressable identity and is the basis for its generated linker name. It may additionally declare that it overloads another name:

`fn intersect_bool overload intersect ...`

Here `intersect_bool` is the concrete function's NameDef, while `intersect` is a distinct overload-set NameDef known to the compiler. The overload-set NameDef refers to the accumulated list of concrete function or method NameDefs that declare they overload that name. This preserves the namespace rule: each spelling still maps to exactly one NameDef. In the compiler today this is a concrete `FnDclNode`, whose `overloadsym` records the set it joins, and an `FnOverloadDclNode` bound to the overload name.

During recursive name/semantic resolution of a call, a use of `intersect` first resolves to the overload-set NameDef. The resolver recursively obtains the argument and candidate-signature types, selects the one concrete definition, and points the call at it before resolution of the call node finishes. An overload-set name is valid only as the function or method being called; it is not a first-class function value and its address cannot be taken. The set must contain exactly one candidate whose signature accepts the arguments, including permitted implicit coercions:

- No matching candidate is a compile error.
- More than one matching candidate is an ambiguity compile error, even if one match is exact or would require fewer or "better" coercions.
- Declaration order never selects between candidates.
- The caller resolves ambiguity by using a concrete function name or by explicitly coercing arguments so that exactly one candidate remains applicable.

After selection, the call refers directly to the chosen concrete function or method NameDef. Return types do not participate in overload selection. Methods follow the same model, with the receiver included in signature matching. Operators use this mechanism as well and normally appear in source through their overload name.

Visibility is checked on the name the caller uses, so an overload set and its candidates must agree: a private concrete candidate may not join a public overload name. `fnOverloadDclAdd` reports `ErrorPrivOverload` where the set is built, for a module's functions and a type's methods alike, and leaves the candidate out; the author makes both private or both public. A compiler-defined intrinsic is exempt, because it is never a symbol — which is how the core types hide `_neg` behind `-`. A public name therefore holds only public candidates, and generation has nothing to reach that its own privacy filter would hide.

Extending a type's overload sets from an extension is intended, but its ownership and collision rules are deferred until extensions are designed. Generic candidates and merging matching `extern` declarations with implementations are likewise deferred; a generic declaration may not currently name an overload set at all.

## Lookup and paths

An unqualified name is resolved through nested lexical contexts and then the enclosing namespace. A path walks namespaces with the period, which is also how a member of a value is reached: one operator, and which of the two a period is depends entirely on what the name before it binds to.

Current compiler behavior:

- `name` begins in the active lexical/module context.
- `module.name` begins wherever `module` is in scope, which is the ordinary bare-name rule and nothing else. A local, a parameter or a type member of that spelling therefore hides the module, and there is no way to reach past it.
- There is no root anchor and no way to name the enclosing module: a module has no name of its own until it carries a `mod` header, and a path starts with the name of the namespace it walks.
- A path may have any number of hops, each of which must resolve to a module or a struct-like type. A hop through anything else — an alias, a number type, a generic instance, a generic parameter — is `ErrorUnkName` at type check.
- A resolved `NameUseNode` points directly to a heterogeneous declaration node. It keeps its one tag; whether it is a type, a value, a macro or a generic parameter is asked of that node (`nameUseGroup`, `nameUseNames`), never stamped on the use. The one thing stamped on it is `FlagQualified`, which says the name was reached through a namespace rather than written bare.

A path is parsed as a chain of member accesses and collapsed during name resolution, hop by hop, by `fnCallNameResPath` — [fncall](../nodes/fncall.md), "The path collapse", has the mechanism and the reason it cannot wait for type check.

The NameDef design instead makes lookup return a stable NameDef. A resolved reference remains one kind of `NameUse` node pointing to that definition — as it already does — but the definition or its IR value explicitly indicates whether it is usable as a type, runtime value, callable, macro, namespace, generic, or other semantic kind, and the surrounding use validates that role, where today the use classifies the declaration by its tag.

### Bare names inside a type

While resolving a type body, the compiler places the type's members in the lookup context outside the method's parameter and block scopes. Normal nearest-scope lookup applies:

- A parameter or local with the same spelling shadows the type member.
- If no nearer binding shadows an instance field, its bare name is lowered to `self.field`.
- If no nearer binding shadows an instance method, calling its bare name is lowered to `self.method(...)`. This applies to an overloaded name too: the bare name resolves to the type's `FnOverloadDclNode`, and the lowered member call selects the concrete candidate.
- `self.field` or `self.method(...)` explicitly selects the member when a lexical name shadows it.

Implicit `self` is therefore lowering performed after ordinary name resolution has selected an unqualified type member; it does not take precedence over lexical bindings.

An inherited default method, and an enum's field spliced into a variant, are type members for this purpose. Each joins the type's dictionary while the type is name resolved, before any of its method bodies is, so its bare name is selected and lowered exactly as a member declared in the type is. The one exception is a default cloned from an instance of a generic trait, which joins only when the instance is type checked and so must be reached as `self.name`. A field a trait requires is not an inherited member at all: the type declared it, so it is the type's own.

## Visibility

A declaration is private to the namespace that owns it unless it is written `pub`:

- A name declared in a module is private to that module unless declared `pub`.
- A type member is private to its type unless declared `pub`. A variant declared inside an enum is as visible as the enum, and may be declared `pub` itself.
- `pub` has one meaning wherever it appears: this entry is visible from outside the namespace that owns it. A local declaration has no outside to be visible from, so `pub` on one is `ErrorBadPub`; so is `pub` before `import` or `include`, whose meaning as re-export belongs to the module work.
- A name's spelling says nothing about its visibility. A leading underscore is a character like any other.

The compiler enforces this on the routes that can reach a private name: `fnCallNameResPath` reports `ErrorNotPublic` for a private declaration reached by a path from outside its module, `importNameRes` skips private nodes when folding, `fnCallLowerMethod` refuses a private member on a receiver that is not `self`, and `typeLitStructReorder` refuses a value for a private field outside the type's methods. The parser sets `FlagPub` on whatever declaration the keyword precedes; where a declaration joins its namespace (`dclInfoJoin`) the flag is read once into its `DclPrivate` bit, and every check after that asks `inodeIsPrivate`, which answers from the bit for a declaration that carries `DclInfo` and from the flag for a node that carries none — a field, a const, a macro, a typedef, an overload name. Generation reads the same bit — see "Symbols".

An overload name's visibility is its candidates': the first candidate declares it, and every later candidate must agree (`ErrorPrivOverload`, either way round). A compiler-defined intrinsic candidate counts as `pub` for the name and is exempt from agreeing, which is how the core types keep a private `_neg` behind a pub `-`. One consequence is deliberate and worth knowing: **visibility is checked on the binding the caller's name reaches**, not on the candidate overload selection then picks, which is why a pub overload name may not hold a private concrete candidate — through the pub name the private one would be reachable.

Visibility should belong to the original definition or declaration, while access is evaluated from the use site. A folded or renamed NameDef must not make a private definition public merely by changing its local spelling, and it does not: the alias an enrichment makes carries its target's visibility, which is what lets a private member of the base come across and stay out of the enrichment's clients' reach. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it.

## Include, import, and name folding

`include` contributes declarations to the current module. It does not introduce a namespace.

Plain `import math` binds the imported module as `math`; public members are intended to be accessed as `math.name`.

Documented folding supports:

- Selectively bringing a member into the importing namespace.
- Renaming while folding, such as importing `math3d.Point3` as `Point`.
- Folding all public names with `.*`.
- Folding any category of name, subject to the importing namespace's single collision domain.

Current compiler behavior is narrower:

- Plain module import and wildcard `.*` folding are parsed.
- Selective folding and `as` renaming are not implemented.
- Wildcard folding inserts the imported declaration's existing IR node directly into the receiving module namespace.
- Imported modules are loaded once and reused.

The intended NameDef behavior is:

1. Folding or renaming creates a new NameDef owned by the receiving namespace.
2. The new NameDef may have a different local spelling.
3. It points to the same underlying IR value as the original definition.
4. It retains a link to the original definition or origin for identity, visibility, diagnostics, and generated naming.
5. Collision checks use the receiving namespace's complete name domain, regardless of the imported declaration's category.

Thus import aliasing duplicates a binding, not the underlying type, function, module, or other IR value.

### Folding into a type

A struct's field may carry a `use` clause folding members of the field's type in as names of the struct — delegated inheritance, [refinherit](../../conesite/public/coneref/refinherit.html). It is built, and it is the same operation as the module fold above at the namespace: one collision domain, an error at the fold for a name already taken, renaming with `as`, exclusion with `but`, visibility transitive so only what the field's type shows through a `pub` field folds. The clause is expanded into the struct's dictionary while the struct is name resolved, before any of its method bodies, so a folded name is usable bare inside the type ([struct](../nodes/struct.md), "Name folding").

**Where the two folds differ, measured by building this one: in what the binding holds.** A module fold binds the declaration itself. A type fold binds a *copy* of a folded field, carrying a hop to the field it is reached through, or an *alias* for a folded method — because only a type fold reaches its target through a value, and a use of the name must be lowered to an access path or to a call whose receiver is shifted to the field. The alias node is the binding record this note asks for: a local spelling, a visibility bit of its own, a target. The module work reuses it as it is, with a qualified name as the target, and never calls the receiver rewrite.

### Folding a whole type in: `extends`

A concrete type may be named as another's base, with `extends`, and everything it has becomes a name of the enriching type, which adds methods and no fields ([refinherit](../../conesite/public/coneref/refinherit.html); [struct](../nodes/struct.md), "Enrichment"). It is the **third** attachment site for one operation, and it lands between the other two:

| Where the clause sits | What the receiver is | What the binding holds |
| --- | --- | --- |
| a **module** body — `import mod.*` | nothing; a module has one instance | the declaration itself |
| a **type** body — `extends Meter` | the **whole**, which is already the right type | the base's declaration, under an alias; the base's fields as copies of its own |
| a **field** — `engine Engine use *` | the **part**, reached through the field | a copy carrying a hop, or an alias whose call shifts its receiver |

**That middle row is the finding.** The two hard things a type adds to the module case are dispatch and per-instance state, and an enrichment has neither to solve: it may not change the fields, so its values and its base's have one representation and a base method already takes exactly the right receiver. So the machinery recurs where it is easy and the hard part stays in the field row, where the receiver has to be found and shifted.

**Visibility is the target's, and this is where an alias narrowing it would matter.** An enrichment is inside its base's encapsulation boundary, so a private member of the base comes across — under an alias that is private here too, so the enrichment's own methods reach it and the enrichment's clients do not. That answers "may a fold make a private definition public" with no: the alias carries the target's visibility rather than its own opinion. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it yet.

## Aliases

Current `typedef` creates a module-scoped structural alias for a type. Type resolution follows the alias to its underlying type.

`AliasDclNode` (`ir/stmt/aliasdcl.c`) is the general binding: a local spelling and a target, a name use bound to the declaration it stands for, with the `FlagPub` bit as its own visibility and everything else the target's. Chains resolve through `aliasDclResolve`; a use bound to one answers as its target (`nameUseGroup`), and every site that reads a namespace binding resolves it first. Today it is made for a folded method, overload set or macro method of a field's type, and for every member but the fields of an `extends` base — a static among them, which is the one place an alias stands for something reached through the type rather than through a value, and where the `FlagMethFld` bit is therefore left off.

The aspirational model generalizes aliases: a new NameDef may denote anything nameable. Alias chains should preserve each local binding for diagnostics and visibility while semantic operations can reach the final IR value. A type-valued alias remains structural; creating a distinct nominal type should use a separate construct.

Import folding/renaming is a namespace alias operation with an explicit source definition, and is the alias above with a qualified name use as its target. Other aliases may bind expressions or declarations directly. The exact syntax and compile-time restrictions for general aliases remain open.

## Generics and macros

Generic and macro syntax exists in the current compiler, but the website documentation labels much of this area incomplete or future-facing. Their namespace structure is defined above; specialization, expansion hygiene, and code-generation ownership remain separate implementation concerns.

## Symbols

The linker has one flat namespace and the language has many. This section is
the rule for crossing that boundary: what a declaration records about where it
lives, how its symbol is spelled from that, and what linkage the symbol gets.
Generation only lowers it — `nameSymbol` spells, `genlLinkage` links — and
spells no name of its own; [Generation](generation.md), "Symbols, linkage and
COMDATs", is the lowering.

**In scope:** how a declared name that could reach the object file is recorded
in its IR node; how it maps to the symbol the link editor sees; that symbol's
linkage and visibility. **Out:** whether a symbol is emitted at all — the rule
is that *if* a name is emitted, its spelling and linkage follow from here — and
anonymous `fn` literals and string constants, whose names (`anon`, `string`,
plus LLVM's uniquing suffix) come from no declared name.

*Provenance: the rules are the author's and the scheme's decisions are the
compiler's. The grammar, the worked examples and the as-built table are
measured from emitted LLVM IR, read back by the demangler in `test/run.py` —
whose selftest over the worked examples runs on every suite invocation — and
pinned by `symbols` checks in the `core`, `generic`, `module`, `struct` and
`trait` groups.*

### The declaration facts

Every node that declares a symbol carries a `DclInfo` by value — `FnDclNode`,
`VarDclNode` (a global), `StructNode` and `ModuleNode` — and `inodeGetDclInfo`
is the one switch that knows which kinds those are. It holds the **owner**, a
pointer to the enclosing module or type node — the chain is walked, never
stored as a string — and five bits:

| Bit | Meaning | Written from |
| --- | --- | --- |
| `DclPrivate` | visible only within its owner | the absence of `pub`, once |
| `DclExternal` | externally supplied: this compile emits no definition | `extern` |
| `DclCName` | C-style name: no owner prefix, never mangled | `extern` [differs: the regime is meant to be declared on a module, and is inferred per declaration from `extern` until it is] |
| `DclSystemCC` | system calling convention | `extern system` |
| `DclNamesChain` | module only: contributes its name to the owner chain | set on every loaded module, never on the root |

**Owner is set where a declaration joins a namespace**: `modAddNode` for a
module's declarations and `iNsTypeAddFn` for a type's methods. That placement
is what makes the cloned cases right without a rule of their own — a generic
instance's methods join the instance and are owned by it, and a trait default
inherited by an implementing type joins that type and is owned by it, not by
the trait it was written in. Three declarations join directly rather than
through a namespace: the drop function `structSetDropFn` synthesizes (owned by
its type), an anonymous `fn` literal lifted to module scope by `parseAmper`
(owned by the current module), and a trait's variant struct, which is bound in
the module but owned by the trait, so its symbols are spelled after it.

An imported file module has no owner: it is a top-level module of the program,
not something inside the root. The root has a name — its file's basename, which
is what lets an import cycle find it — and contributes nothing to any chain.

**What the node stores, and what generation derives.** The author writes
visibility; linkage is the compiler's to derive.

| Stored on the node | Derived at generation |
| --- | --- |
| owner chain | linkage: internal or external |
| declared name | mergeable or unique |
| visibility bit | nothing, today: a program compile sets no export-table visibility, and what a package exports is undecided |
| supply: defined in this compile, or externally supplied | |
| naming regime: C-style or Cone-style | |
| calling convention, for C-style names | |

### Spelling

Rules for Cone-consumed names; C FFI names have their own (S5).

- **S1.** A symbol is a sigil, then a path: the owner chain innermost last,
  each component a length-prefixed identifier, and an instance of a generic
  wrapping its type arguments around its own component. Nothing else is
  encoded — no signature, no visibility, no lifetime, no suffix.
- **S2.** The owner chain is the enclosing modules, outermost first, then the
  enclosing types, then — for a function's `static` alone — the function that
  declares it, since that is the one declaration a function owns a symbol for;
  a module never sits inside a type. **There is no package name.** The compiler knows only module names declared in source; the version
  slot is v0's disambiguator, `s<base62>_` before the top module's identifier,
  and nothing fills it.
- **S3.** A source file with no `mod` declaration contributes no module name,
  so its declarations carry no module component. The program's root is
  prefix-less on purpose, which is why `main` needs no special case: a root
  declaration with nothing to encode is spelled bare, and one with something
  to encode — a root type's method, an instance of a root generic — starts its
  chain at the first named owner.
- **S4.** Each owner in the chain is spelled the way its own path would be.
  The only owner carrying more than its identifier is a generic type instance,
  whose component carries its type arguments, so `fn tally(self) i64` is told
  apart across `Holder[i64]` and `Holder[f64]`.
- **S5. C FFI names.** Every module is flagged C-style or Cone-style. In a
  C-style module the owner chain contributes no prefix, no name is mangled and
  nothing carries a suffix, so such a module cannot declare a generic. The flag
  may carry a literal prefix — `SDL_` — prepended to every name, written by the
  author and never derived. The flag affects the symbol only; resolution is
  through the ordinary module, so a caller writes `sdl.Init`. Inbound (a C
  library's symbols) and outbound (a Cone declaration published to C) are one
  mechanism in two directions, and `main` is the existing outbound case.
  [differs: there is no module flag and no literal prefix; the regime is per
  declaration, from `extern`, and an `extern` inside a Cone module is spelled
  bare wherever it is declared]
- **S6. Vtables.** A vtable is the implementing type then the trait's path,
  `Y<type><trait-path>` — this type as that trait — and a trait's vtable list
  is the trait's path alone, `L<trait-path>`. The thunk that fills a slot a
  folded method satisfies is the vtable's spelling followed by the slot's
  identifier, `Y<type><trait-path><ident>` (`nameVtableThunk`): this type as
  that trait, at that slot. The vtable's LLVM *type* is
  named `<Trait>:Vtable` from the trait's declared name (`nameVtable`): an
  LLVM type name, not an object-file symbol, so it is not encoded.
- **S7.** Overloaded functions need no signature encoding: a concrete candidate
  already has a unique declared name in its namespace, and only that name
  reaches the symbol. Only an instance of a generic needs type arguments
  encoded.
- **S8.** An identifier may contain any character, in backticks, and may be
  Unicode. The symbol therefore encodes the name itself, not only its path: a
  name outside `[A-Za-z0-9_]` is punycoded, and an operator method's name is a
  fixed two-letter code. An encoded symbol is `[A-Za-z0-9_]` throughout, so it
  is never quoted in the IR, and a `.` in one can only start LLVM's
  uniquifying suffix.

#### The grammar

The scheme is Rust v0's where the two languages coincide and Cone's own where
they do not, so that what Cone adds later — closures, const generics — has a
Rust-shaped place to go. `nameSymbol`, `namePath`, `nameIdent` and `nameType`
in `ir/name.c` are the productions; the demangler in `test/run.py` reads them
back.

```
symbol   = "_C" [version] path                   a fn, a global, a method
         | "_C" [version] "Y" type path          a vtable: this type as that trait
         | "_C" [version] "Y" type path ident    the thunk filling that vtable's named slot
         | "_C" [version] "L" path               a trait's vtable list
version  = decimal                               absent = 0, and only 0 is spelled

path     = "C" ident                             a top module
         | "N" "v" [path] ident                  a fn or global, nested in its owner
         | "N" "t" [path] ident                  a type, or a module inside a module
         | "I" path {type} "E"                   an instance of a generic, with its type arguments

ident    = decimal ["_"] bytes                   bytes in [A-Za-z0-9_]; "_" when they begin with "_" or a digit
         | "u" decimal "_" bytes                 punycode over the basic set [A-Za-z0-9_], delimiter "_"
         | "o" code                              an operator method

type     = letter                                a built-in number type, Bool, or void
         | path                                  any other named type
         | "T" {type} "E"                        tuple
         | "F" {type} "E" type                   fn signature: parameters, then return type
         | "A" type decimal "_"                  fixed array: element type, then extent
         | "R" ident ident type                  reference: region, permission, target
         | "S" ident ident type                  array reference
         | "V" ident ident path                  virtual reference to a trait
         | "P" type                              raw pointer

decimal  = "0" | [1-9][0-9]*
```

**Identifiers.** The length is decimal and the bytes follow; the `_` is
required exactly when the bytes begin with `_` or a digit, so `_hid` is
`4__hid` and `1st` is `3_1st`, and a reader takes the length, skips one `_`
if present, then the bytes. The empty identifier is `0`. Punycode is RFC 3492
with `[A-Za-z0-9_]` as the basic set rather than all of ASCII and `_` as the
delimiter; a non-basic ASCII code point — backticked punctuation, a space — is
lifted above the Unicode range before encoding, so the RFC's initial `n` of
128 stands and an identifier with only non-ASCII characters encodes
byte-identically to v0. `größe` is `u9_gre_6ka8i`; `` `a b` `` is
`u8_ab_eh24y`. An operator method's name is `o` and a code, Itanium's where
Itanium has the operator and Cone's own where it does not
(`nameOperatorCode`):

| | | | | | |
| --- | --- | --- | --- | --- | --- |
| `pl` `+` | `mi` `-` | `ml` `*` | `dv` `/` | `rm` `%` | `eq` `==` |
| `ne` `!=` | `lt` `<` | `le` `<=` | `gt` `>` | `ge` `>=` | `an` `&` |
| `or` `\|` | `eo` `^` | `ls` `<<` | `rs` `>>` | `ix` `[]` | `cl` `()` |
| `pp` `++` | `mm` `--` | `pL` `+=` | `mI` `-=` | `mL` `*=` | `dV` `/=` |
| `rM` `%=` | `aN` `&=` | `oR` `\|=` | `eO` `^=` | `lS` `<<=` | `rS` `>>=` |
| `la` `<-` | `rx` `&[]` | `pP` postfix `++` | `mM` postfix `--` | | |

The last row is Cone's own. Postfix `++` and `--` are interned as `+++` and
`---`, which is how the demangler prints them. `!` is a logic node and never a
method, and unary `-` is the ordinary name `_neg`, so Itanium's `nt` and `ng`
are read by the demangler and spelled by nothing.

**Paths.** The root module contributes nothing, so a component whose parent is
the root has an empty parent path: `Nv` followed by a digit is a root fn's
identifier, `NvNt2Pt3get` is `Pt.get`. `I` wraps the instance's own component
— `INv4pickxE` — and an instance that is an owner carries its arguments in
place, `NvINt6HolderxE5tally`. A method of a generic type's instance carries
the instantiating node its owner's cloning stamped on it, which is the owner's
and not its own, so the arguments are spelled on the owner once. A variant of
a tagged trait is owned by the trait, `NtNt7Extense8Variant1`.

**The bare rule** is `nameSymbol`'s: a C-style name, or a declaration with an
empty owner chain that is not itself an instance of a generic — every root fn
and global, `main` among them — is its declared name alone. Everything else is
`_C` and its path. A function with no name at all, a lifted `fn` literal,
spells the empty string and is named `anon` at generation.

**Types** are spelled by `nameType`, which draws exactly the distinctions
`itypeIsSame` draws: a reference's region, permission and target but not its
lifetime; an array's extents; a signature's parameter and return types. The
letters are v0's, matched by node identity against the corelib globals, so a
user type named `i64` inside a module is still a path:

| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| `a` i8 | `s` i16 | `l` i32 | `x` i64 | `i` isize | `f` f32 | `b` Bool |
| `h` u8 | `t` u16 | `m` u32 | `y` u64 | `j` usize | `d` f64 | `u` void |

`n` and `o`, v0's i128 and u128, are read by the demangler and declared by
nothing. A region and a permission are each the identifier of the declaration
they name — `2so`, `2rc`, `3mut`, `2ro`, `4opaq` — and a borrowed reference,
which names no region, spells the empty identifier `0`: `R2so3mutl` is
`&so mut i32`, `R04opaqFxEx` is `&opaq fn(i64) i64`. An array of several
dimensions is an array of arrays, one `A` per dimension and the extents
innermost first: `AAx3_2_` is `[2] [3] i64`.

#### How to read a symbol

Read left to right after `_C`. `N` announces a nested component and the next
letter says value or type; then comes the parent, innermost last; a digit
starts a length, and that many bytes of name follow. `I…E` wraps a generic
instance's arguments. `Y` is the vtable of a type for a trait, `L` a trait's
vtable list. Every row below is a spelling `conec` emits, read back by the
demangler in `test/run.py`:

| Cone declaration | Symbol | Read as |
| --- | --- | --- |
| `fn plainPub()` in the root, `fn main`, root `mut pubGlobal` | `@plainPub` `@main` `@pubGlobal` | bare: nothing to encode (D2) |
| `fn subFn()` in file module `sub` | `_CNvC3sub5subFn` | `sub.subFn` |
| `mut subGlobal` in module `sub` | `_CNvC3sub9subGlobal` | `sub.subGlobal` |
| `struct Pt { fn get(self) }` in the root | `_CNvNt2Pt3get` | `Pt.get` — the root contributes nothing |
| `SubPt.get` in module `sub` | `_CNvNtC3sub5SubPt3get` | `sub.SubPt.get` |
| private `SubPt._hid` | `_CNvNtC3sub5SubPt4__hid` | `sub.SubPt._hid` — the separator `_` is required before a name beginning with `_` or a digit, so two underscores; privacy is a fact, not a spelling |
| root generic `fn pick[T](a T, b T)` at `i64` | `_CINv4pickxE` | `pick[i64]` — the type *argument*, once |
| `fn pickSecond[T,U]` at `i64`, `f64` | `_CINv10pickSecondxdE` | `pickSecond[i64,f64]` |
| `Holder[i64].tally` | `_CNvINt6HolderxE5tally` | `Holder[i64].tally` — the instance is the owner |
| `Meter`'s default `reading` inherited by `Gauge` | `_CNvNt5Gauge7reading` | `Gauge.reading` — spelled as an override written there |
| the `drop` the compiler synthesizes for `Bundle` | `_CNvNt6Bundle4drop` | `Bundle.drop` |
| `Vec.-`, `Vec.+=`, `List.&[]` | `_CNvNt3Vecomi`, `_CNvNt3VecopL`, `_CNvNt4Listorx` | `Vec.-`, `Vec.+=`, `List.&[]` |
| `Gauge`'s vtable for trait `Meter` | `_CYNt5GaugeNt5Meter` | `Gauge as Meter (vtable)` |
| the thunk filling `Powered`'s `thrust` slot in `Car`'s vtable, `thrust` being folded from a field | `_CYNt3CarNt7Powered6thrust` | `Car as Powered.thrust (thunk)` — a method in everything but name and namespace |
| `Meter`'s vtable list | `_CLNt5Meter` | `Meter (vtable list)` |
| the vtable of `Variant1`, a variant of tagged trait `Extense` | `_CYNtNt7Extense8Variant1Nt7Extense` | `Extense.Variant1 as Extense (vtable)` — a variant is owned by its trait |
| `passThrough[T]` at `&opaq fn(i64) i64` | `_CINv11passThroughR04opaqFxExE` | `passThrough[&opaq fn(i64) i64]` — borrowed, so the region is `0` |
| `passThrough` at `(i64,i64)`, at `[2] i64`, at `void` | `_CINv11passThroughTxxEE`, `_CINv11passThroughAx2_E`, `_CINv11passThroughuE` | `passThrough[(i64,i64)]`, `passThrough[[2] i64]`, `passThrough[void]` |
| `fn größe(self)` and `` fn `a b`(self) `` on `Umlaut` | `_CNvNt6Umlautu9_gre_6ka8i`, `_CNvNt6Umlautu8_ab_eh24y` | `Umlaut.größe`, ``Umlaut.`a b` `` — punycode, read back in backticks where source needs them |
| `extern fn abs`, `extern system GetTickCount` | `@abs`, `@GetTickCount` | C names, bare (S5) |
| `modulex.y_z` and `modulex_y.z` | `_CNvC7modulex3y_z`, `_CNvC9modulex_y1z` | distinct by construction |
| `modulesub.cone` compiled as root, then imported | `@scaleInt` vs `_CNvC9modulesub8scaleInt` | two spellings for one declaration; a module name declared in source is what would reunite them |

#### Decisions

Each names the choice, then the alternative it rejects and why.

**D1 · Sigil.** `_C`, then an optional decimal scheme version, then the path;
absent version is 0, exactly as v0's `_R[version]`. Not `_R` itself: Rust
tools would parse Cone's extensions and fail confusingly. `_C` sits in the
space C reserves for implementations, as `_Z` and `_R` do, and says whose
symbol it is.

**D2 · Bare root.** A root declaration with nothing to encode stays bare;
everything else is encoded, and the program root contributes no path
component. Not `C<file-basename>` for the root: it would reunite the two
spellings of a file compiled as root and as import, but the root is
prefix-less on purpose (S3) and a module's name belongs in source, not in a
filename. Not `C0`, an empty top-module identifier: it says the root has a
name when the point is that it has none. Not encoding root fns too, for one
code path: it costs the readability of every IR check and of `main`, buys
nothing while the root is internal, and the bare rule is one line.

**D3 · Identifiers.** v0's length-prefix exactly, with two extensions:
punycode over the safe basic set, so a non-ASCII name is byte-identical to v0
and a backticked ASCII name, which v0 cannot spell, encodes instead of failing;
and `o<code>` for operator methods. Not Rust's trait-method names (`add`,
`index`) for operators: a user may declare a method literally named `add`, and
then `Vec.+` and `Vec.add` spell one symbol. Not punycoding operators
through `u`: decodable, but unreadable in an object file, and the object file
is meant to be read.

**D4 · Paths.** v0's `C`, `Nv`, `Nt` and `I…E`, whose arguments are the type
arguments and never the parameter types (S7); an instance as owner carries its
arguments (S4). Not Rust's `M`/`X` impl paths for methods: Cone's methods live
in the type's namespace, so `Nt` then `Nv` says it directly and reads as
`Type.method`; the impl productions encode a Rust concept Cone does not have.

**D5 · Types.** v0's letters where the type coincides, and Cone's own `R`,
`S`, `V` and `P` for what v0 cannot say, region and permission spelled as the
identifiers of the declarations they name. Region and permission are always
spelled and lifetime never, because those are the distinctions `itypeIsSame`
draws — an encoding that omitted the region would spell `&so mut T` and
`&rc mut T` alike, a collision rather than a distinction. Not v0's `R`/`Q`
for `&`/`&mut`: they carry no region, and permission is not two-valued in
Cone. Not v0's `K…` const encoding for array extents: decimal is readable,
and `K` is the Rust-shaped place for const generics when Cone has them.

**D6 · Vtables.** `Y<type><trait-path>` — v0's "this type, that trait", which
is precisely what a vtable is — and `L<trait-path>` for the list, one per
compilation unit with internal linkage (L4), since its contents are the
implementers this compile saw. Not `X<impl-path><type><trait>`: with an empty
root, the impl path and the type are not separable by a parser.

**D7 · No back-references.** Version 0 has none. They only compress, nothing
is built against the scheme, and adding them is what the version digit is for.
Not in version 0: it doubles the encoder and the demangler for no benefit until symbols
get long.

**D8 · Program linkage.** In a program compile every definition is internal
except `main` and a C-style name; a declaration is external, as an LLVM
`declare` can be nothing else; visibility is never set. The soundness argument
is S3's: a program's bare `@log` must never satisfy a package's reference to
libm's `log`. The consequence is that LLVM's optimizer deletes an internal
definition nothing references, which is why symbols are asserted from the
pre-optimization dump.

**D9 · The overload rule.** A private concrete candidate may not join a public
overload name — `ErrorPrivOverload`, raised where the set is built
(`fnOverloadDclAdd`), everywhere and not only across a package boundary; an
intrinsic candidate is exempt because it is never a symbol. Not deferred to a
visibility bit on the binding: the check is one line here, and it removes a
linkage case (L5).

**D10 · The demangler** lives in `test/run.py`, and `symbols` is a check
target: one line per global with its linkage and its demangled name, so the
suite asserts `Holder[i64].tally` rather than bytes. Not a C demangler in
`conec`: a second implementation of the grammar to maintain before the first
has settled, with no second consumer to justify it.

**Open.** Back-references, and with them the first use of the version digit.
The package linkage table under "Linkage" is the rule and not the code, and
L5's generic-helper half and L6 are open there.

### Linkage

- **L1.** A symbol is external if something outside this object may need to
  resolve against it: it is published to or imported from C; or the compile is
  producing an importable package and the declaration is public; or it is
  private but reachable anyway (L5). Otherwise internal.
- **L2.** Among external symbols, *mergeable* means more than one object may
  legitimately define it, because it is produced at a use site rather than by
  its declaring module: an instance of a generic, and a vtable. Everything else
  is *unique*, and a duplicate definition is a link error.
- **L3.** A trait default cloned into an implementing type is not mergeable:
  the clone lands on the implementing type, which one package declares, so that
  package alone emits it.

The cases, in a program compile:

| Declaration | Linkage |
| --- | --- |
| `fn foo`, public or private, in a file with no `mod` | internal — the symbol is bare, so nothing outside could safely resolve it |
| `fn main` | external — the C runtime resolves against it |
| a global variable | internal |
| an instance of a generic, wherever declared | internal — this object is the only one that references it |
| a vtable for a type declared here | internal |
| a trait's vtable list | internal, in every kind of compile (L4) |
| a C-style declaration, inbound or outbound | external, unique |
| an imported module's declaration — a symbol this object does not define | external: an LLVM `declare` can be nothing else |

**As built, the program rule.** `genlLinkage` asks one question: does this
object define the symbol? A definition — a declaration whose module is flagged
`FlagGenMod`, not `extern`, with a body if a function; every vtable and vtable
list — is `internal`, except `main` and a C-style name, which stay external. A
declaration is external. No visibility is ever set: a private name is spelled
and linked exactly as a public one, since privacy is a fact about the
namespace, not the object file. A package compile does not exist yet; its
cases below are the rule, not the code.

In a package compile:

| Declaration | Linkage |
| --- | --- |
| public `fn foo` | external, unique |
| private `fn _x`, reached only from inside the package | internal |
| public global | external, unique |
| private global, unreached from outside | internal |
| method on a public type | external, unique |
| method on a private type | internal |
| an instance of a generic the package itself instantiated | external, mergeable |
| a vtable | external, mergeable |
| a trait default cloned into a type this package declares | external, unique — one package emits it |
| a trait's vtable list | internal (L4) |

The prior art that settled the derivation: C++ `static` is linkage, not access,
and a header-scoped `static` reached from an inline function is an ODR
violation; C++17 `inline` variables are the linkonce case only because no
single `.cpp` owns a header variable. Rust's author writes `pub` or not, and the
compiler derives linkage from what references the item. Cone has one object per
package, so "outside" means outside the object.

- **L4, the vtable list.** Not mergeable, although L2 would make it so: its
  contents are the implementers *this compile* saw, so two objects would
  produce different bodies under one name and the linker would keep whichever
  it met first. One list per compilation unit, internal, in every kind of
  compile.
- **L5, private but reachable through an overload name.** A private concrete
  candidate may not join a public overload name — `ErrorPrivOverload`, raised
  where the set is built (`fnOverloadDclAdd`), for a module's functions and a
  type's methods alike, and not only across a package boundary. Through a
  public name the candidate would be reachable from outside its owner, and a
  symbol that is private yet needed from outside has no sound linkage; the
  author makes both private or both public. A compiler-defined intrinsic is
  exempt, as it is never a symbol, which is how the core types hide `_neg`
  behind `-`. The refused candidate is left out of the set; the set stands.

**Open:**

- **L5, the other half.** A private helper called from a public generic or
  `inline` body, whose instance the importer emits, is not ruled: the importer
  may emit its own internal copy of the helper, but a private *global* reached
  that way has one owner and cannot be duplicated, so it is either forbidden
  from such bodies or accepted as external and hidden.
- **L6.** The compiler must be told whether it is producing an importable
  package; a `mod util` inside a program looks identical to a package's module,
  and `--library` today changes only the relocation mode. Under S3 a program's
  prefix-less symbols are internal, which is what makes prefix-less sound: a
  program's `@log` cannot satisfy a package's reference to libm's `log`.

### As built

One row per kind of symbol, a program compile. Linkage is LLVM's spelling —
absent means `external` — and the COMDAT is the selection kind of the one each
definition leads; a declaration leads none. No symbol carries a visibility.
Measured on the default x64 Windows triple from the pre-optimization dump,
which is where the `symbols` check target reads them.

| Kind | Spelling today | Linkage · COMDAT |
| --- | --- | --- |
| root `fn`, public or private | `define internal i64 @plainPub(i64 %0) comdat {` · `define internal i64 @_plainPriv(i64 %0) comdat {` — bare, nothing to encode | internal · `nodeduplicate` |
| `fn main` | `define i32 @main() comdat {` — the one definition `genlLinkage` leaves external, by its bare name | external · `nodeduplicate` |
| root global: `mut`, `imm`, private | `@pubGlobal = internal global i64 5, comdat` · `@constGlobal = internal constant i64 7, comdat` · `@_privGlobal = internal global i64 6, comdat` | internal · `nodeduplicate` |
| struct method, static fn, private method | `@_CNvNt2Pt3get` · `@_CNvNt2Pt4make` · `define internal i32 @_CNvNt2Pt4__hid(%Pt* %0) comdat {` — `Pt.get`, `Pt.make`, `Pt._hid` | internal · `nodeduplicate` |
| imported module's `fn` | `declare i64 @_CNvC3sub5subFn(i64)` — `sub.subFn`; a private top-level `fn` or global leaves no symbol | external · none |
| imported module's global | `@_CNvC3sub9subGlobal = external global i64`; `imm` is `external constant` | external · none |
| method on a struct in an imported module | `declare i32 @_CNvNtC3sub5SubPt3get(%SubPt*)`; the private method **is** declared, `declare i32 @_CNvNtC3sub5SubPt4__hid(%SubPt*)`, because the privacy filter in `genlProgram` tests only the module's top-level node | external · none |
| the same file as root and as import | `define internal i64 @scaleInt(i64 %0) comdat {` as root; `declare i64 @_CNvC9modulesub8scaleInt(i64)` when imported — one declaration, two symbols, depending on which compilation the module was the root of | |
| instance of a generic `fn` | `define internal i64 @_CINv4pickxE(i64 %0, i64 %1) comdat {` — `pick[i64]` | internal · `nodeduplicate` |
| method of a generic type's instance | `define internal i64 @_CNvINt6HolderxE5tally(%Holder %0) comdat {` — `Holder[i64].tally`; the instance is the owner, so `fn tally(self) i64` is told apart across instances | internal · `nodeduplicate` |
| trait default cloned into an implementer | `define internal i32 @_CNvNt5Gauge7reading(%Gauge* %0) comdat {` — spelled exactly as an override written there, `Gauge.reading`; no arguments, since a copy is not an instance | internal · `nodeduplicate` |
| synthesized drop function | `_CNvNt6Bundle4drop` — `Bundle.drop` | as its type's methods |
| vtable | `@_CYNt5GaugeNt5Meter = internal constant %"Meter:Vtable" { ... }, comdat` — `Gauge as Meter` | internal · `nodeduplicate` |
| vtable list | `@_CLNt5Meter = internal constant [2 x %"Meter:Vtable"*] [...], comdat` — one per trait, so LLVM never uniquifies one | internal · `nodeduplicate` |
| `extern` | `declare i32 @abs(i32)` — bare inside a module too | external · none |
| `extern system` | `declare dllimport x86_stdcallcc i32 @GetTickCount()` | external · none |
| string literal | `@string = internal constant [5 x i8] c"hello", comdat` | internal · `nodeduplicate` |
| anonymous `fn` | `define internal i32 @anon(i32 %0) comdat {` | internal · `nodeduplicate` |
| `inline` fn | no symbol | |
| overload name | no symbol; each candidate is spelled as an ordinary `fn`, and a public name holds only public candidates (L5) | |
| `stdio` | defined in every importer, since `stdio` is a generating module: `@_CNvC5stdio5print = internal global %IOStream zeroinitializer, comdat`, `define internal %void @_CNvNtC5stdio8IOStream9appendInt(...) comdat {` — internal, so two such objects cannot clash | internal · `nodeduplicate` |
| corelib's `extern fn malloc`, `free` from `genlFree`, `llvm.trap`, `llvm.sqrt.*` | `declare i8* @malloc(i64)` and so on — C and LLVM names, minted outside these rules | external · none |
| `a_b.c` and `a.b_c` | `_CNvC3a_b1c` and `_CNvC1a3b_c` — distinct by construction | |
| an import cycle back to the root | the root is found by name, and each root declaration is defined once | |

Read on COFF: `nodeduplicate` is selection 1, `internal` becomes `Static`, and
`external` is `External`. The optimizer deletes an internal definition nothing
references and folds the rest into `main`, which is why the test runner reads
symbols from `.preir`: a symbol assertion against the post-optimization `.ir`
would see little but `main`.

## Known gaps between implementation and intent

- Overloading:
	- Overloading is implemented with `FnDclNode` and `FnOverloadDclNode` rather than with a general `NameDef`, so the concrete/overload split described above exists only for functions and methods.
	- A generic function may not declare an overload name; the parser reports that combination.
	- Extending a type's overload sets from an extension, generic candidates, and merging matching `extern` declarations with implementations remain deferred.
- Compile unit handling of duplicate, consistent type `extern` vs. value-specified names.
- Selective import folding and `as` renaming are documented but unimplemented. The binding node they need exists (`AliasDclNode`, built for the type fold); import does not use it yet.
- Nested named modules are documented but lack clear declaration syntax and parser support.
- General aliases beyond `typedef` and the folded-member alias are not implemented.
- Generic, macro and metaprogram namespace behavior is partly implemented, incomplete, or aspirational. Delegated inheritance and concrete enrichment are both built; see "Folding into a type" above.
- Packages organize importable libraries but are not yet defined as a distinct namespace layer.
- A path may only pass through a module or a struct-like type. One whose base is an alias, a number type, a generic instance or a generic parameter is refused at type check, because none of those names a namespace at the point the collapse runs. Finishing those at type check, where they do, is the natural other half of the collapse and is not built.
- There is no way to name the module a declaration is in, so a module-level name hidden by a local or by a type member cannot be reached. The `mod` header, which would give the module a name, is not built.
