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
| `src/c-compiler/ir/stmt/aliasdcl.c` | The alias: a binding that stands for another declaration under a spelling of its own, with a visibility of its own. Made for the members a `use` clause folds in, and for a `typedef`; `aliasDclResolve` follows a chain to the declaration, `aliasDclCheckCycle` refuses one that comes back to itself, and `aliasDclThroughAccess` builds the member access a global's folded name is reached by. |
| `src/c-compiler/ir/stmt/fold.c` | The parts of a `use` clause every fold site shares: the source declaration behind a type expression, the star clause's items, `but`. It also owns the module's two folds of its own: `foldGlobalExpand`, which is the whole of a global's, and `foldEnumUseExpand`, which is the whole of a module's `use` of an enum and of the `EnumUseNode` that holds one. |
| `src/c-compiler/ir/dclinfo.c` | The declaration facts a symbol is derived from: sets them where a declaration joins its namespace (`dclInfoJoin`), walks to the enclosing module, prints them in the IR dump. |
| `src/c-compiler/ir/inode.c` | Dispatches the name-resolution pass by IR node tag. Start here when a new node kind must participate in name resolution. |
| `src/c-compiler/ir/clone.c` | Rebinds generic/macro parameters during cloning and repairs resolved declaration references in cloned `NameUse` nodes. |

### Parsing and module namespaces

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/parser/parseexpr.c` | Parses a name as one identifier, and everything after a period as a member access — a path and a member of a value are the same production here. |
| `src/c-compiler/parser/parsemod.c` | Parses module-level declarations, the `mod` declaration and its `extends`, `include`, and `import` with its `use` clause or `.*`; drops an identical repeat of an import and refuses a differing one; answers an import's name against the registry its parent is before the filesystem, loads/reuses modules by canonical path, draws the module tree, names each module, and establishes module hooks. |
| `src/c-compiler/parser/parsetype.c` | Parses struct/trait/enum members and inserts fields, methods and an enum's variants into the type namespace; parses a module's `use` of an enum (`parseUseEnum`), and the fold clause a field, a global and an import carry (`parseFoldClause`). |
| `src/c-compiler/ir/stmt/program.c` | Owns the program's module list and the file registry, and runs name resolution in three walks: what every module's `extends` names, then every module's folds, then every module's body. |
| `src/c-compiler/ir/stmt/module.c` | Owns module namespaces, inserts global declarations with duplicate checks, switches active module hooks, resolves and checks what a module's `extends` names (`modExtendsResolve`, `modExtendsCheckCycle`), puts a module's folded names in place dependency-first (`modFoldNames`) — what it extends, its imports', its globals' and its `use` statements' — and walks module declarations. |
| `src/c-compiler/ir/stmt/import.c` | Binds an imported module's name as an alias carrying the import's visibility, folds what the import's clause admits of the source module's public *namespace* into the importer, one alias per name under its local spelling, and says whether two imports of one module are the same (`importSame`). A module's `extends` is folded here too, as an import marked `isextends`. |
| `src/c-compiler/shared/fileio.c` | Locates a source file by the designated-file convention, and gives a path its one canonical spelling so that the file registry keys it once. |

### Name uses, lexical scopes, and declarations

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/ir/exp/nameuse.c` | Represents name and member uses, resolves a bare name through the hooks, and answers what a use is — type, value, macro — from the declaration it is bound to (`nameUseGroup`, `nameUseNames`). Binding a bare field name is here; **lowering it to `self.field` is `nameUseTypeCheck`'s**, because building that call node needs a type to check it against. Walking a path through module and type namespaces is `fncall.c`'s, with the collapse. |
| `src/c-compiler/ir/exp/cast.c` | Binds a pattern's bare root name against the matched value's enum before its lexical meaning (`castPatternBind`), and reports it when neither answers; `castPatternMark` is the parser's half. |
| `src/c-compiler/ir/exp/block.c` | Pushes lexical scope hooks, binds labeled lifetimes, resolves statements in declaration order, and restores outer bindings on block exit. |
| `src/c-compiler/ir/stmt/fndcl.c` | Establishes function generic-parameter and value-parameter bindings while resolving signatures and bodies. |
| `src/c-compiler/ir/stmt/vardcl.c` | Resolves an initializer before binding its local variable, enforces same-scope uniqueness, and permits nested shadowing through scope hooks. |
| `src/c-compiler/ir/stmt/fielddcl.c` | Resolves field permission, type, and default-value names; namespace insertion is handled by the enclosing type. |
| `src/c-compiler/ir/stmt/const.c` | Resolves constant types and values; module insertion is handled by `module.c`. |

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

Unqualified lookup selects the nearest active binding. Function parameters may therefore shadow names from the containing type or module, and a local in an inner block may shadow a parameter or outer local. A second declaration of the spelling within the same lexical scope is an error. A hidden member of a *type* is still reachable, through `self` or through the type's own name; a hidden member of the enclosing *module* is reachable through the module's own name, which its `mod` declaration gives it. A path begins with the name of the namespace it walks, so naming the module is what makes the hidden name reachable, and a file that declares no module has no spelling for one.

### Types

Named types expose a member namespace. The documented model includes fields, methods, static functions, and potentially nested types. One operator reaches all of them: `p.x` is a member of a value, `Point.make` a member of the type, and what the name before the period binds to is what tells them apart.

Current compiler behavior:

- Structs and traits have one namespace containing fields, methods, static functions, macros, inherited members, folded members (a copy of a folded field, an alias for a folded method), everything an `extends` base has, and `Self`.
- A field, static function or macro cannot collide with another member name. A macro declared in a type is a macro method when its first parameter is `self`, by the same rule as a function; it joins no overload set, and it is not inherited from a trait.
- Methods and static functions each declare a namespace-unique concrete name. A declaration may additionally name an overload set with `fn concrete overload shared(...)`. The concrete name binds directly to its `FnDclNode`; the overload name binds to a separate `FnOverloadDclNode` holding every candidate declared for it, including a set that currently has only one candidate. Two declarations claiming the same concrete name are a duplicate-name error, and an overload name already bound to anything other than an overload node is a collision error.
- Every executable implementation remains a separate `FnDclNode`. The overload node is only a namespace binding, so lookup, call lowering, trait reconciliation, vtables, and code generation always record the selected concrete node.
- A method cannot share a spelling with a field.
- Struct/trait generic parameters form an enclosing lexical context while the type is resolved.
- **An enum's variants are names of the enum's namespace**, beside its fields and methods and sharing their one collision domain, so `Event.KeyEvent` is the spelling and two enums may each declare a `Quit`. A variant is bare in a module only where the module folds it in with `use Event;` — the declaring module included, which gets no exception — and bare anywhere inside the enum's braces. See "Folding an enum's variants into a module" below.
- **Anything written inside an enum's braces sees every name the enum declares bare** — its variants, fields, methods and statics — and a variant's body is written there too: a method of `Colors.Red` writes `Green[]`, not `Colors.Green[]`. The enum's own methods have it because the enum's namespace is hooked while they are resolved. A variant is a module node resolved on its own, so `structNameRes` hooks its enum's namespace in a scope beneath the variant's own ([struct](../nodes/struct.md), step 2): a name the variant declares or inherits wins a clash with one of the enum's, and the enum's names in turn are nearer than the module's. Outside the braces nothing changes: a variant is qualified, folded with `use`, or bare in a pattern on a value of the enum.
  - **An extension's copy of a base's variant means the base's siblings.** The copy is cloned already resolved, so a bare `Green` in its method is what it bound inside the base's braces — `Colors.Green`, as the qualified spelling there is — and that is the only meaning its signature, written against the base, admits. A variant the extension declares is inside the extension's braces and sees the extension's names, the copies included.
  - **A method a value of the enum answers comes to the variant as its own clone**, spliced in and hooked in the variant's scope, so it is not hooked from the enum's (`structHookEnclosingEnum`). ⚠ **A generic enum's method is therefore not bare in its variants' bodies**: the clones join only when an instance is type checked, like a generic trait's defaults, and the enum's own method is not one the variant's `self` can call, so a bare call to it stays `ErrorUnkName` and is written `self.name()`. Its fields are bare, since a bare field is read through `self` by name, and so are its variants and statics.
  - **A generic enum's variant has its own copy of the enum's parameters hooked**, spelled as the enum's, so it names a sibling as the enum's own methods do, `Full[T][v]`, whether that sibling is declared before it or after: an instance's variants are all instantiated before any is type checked ([generic](../nodes/generic.md), "A tagged trait fans out").
- A variant is a name of the enum and never a member of the enum's values: `e.KeyEvent` on a value is `ErrorNoMbr`.
- An enum that extends another holds its own copy of each of its base's variants, bound in its namespace while it is resolved, beside the variants it adds. So `RichColors.Red` names the extension's copy, and `use RichColors;` folds all of its variants, the copies included; `use Colors;` beside it collides on every name the two share.
- `Some`, `None`, `Ok` and `Error` are bare in every program because core writes `pub use Option;` and `pub use Result;`, and every module's automatic import of core is a wildcard, which carries a public fold on.
### Generics and Macros

The declared name of every generic or macro is an ordinary NameDef in its containing namespace, which may be a module/package or a type. It participates in the same cross-category uniqueness rules as every other name there.

Each generic or macro also owns a nested namespace hierarchy analogous to a function's. Its parameters and generic/macro variables are NameDefs in the declaration's private parameter scope, and its body contributes the usual nested lexical scopes. These names are visible only where permitted within that generic or macro declaration. Macro hygiene may impose additional boundaries on names introduced during expansion, but does not create a separate name domain for the macro declaration itself.

### Modules

Every program or library has a main module namespace. A module contains global variables and constants, functions, types, macros, and named modules. All immediate names must be unique, subject to the overload exception described below.

Current compiler behavior:

- The main source and every imported source are represented by `ModuleNode`.
- A parsed function always adds its concrete `FnDclNode` to the module's owned nodes and binds its unique name. When it declares an overload name, the module finds or creates that name's `FnOverloadDclNode`, appends the concrete node, and adds a newly created overload node to the module's owned nodes as well, so it is printed and can be folded in by a wildcard import.
- `include` parses another file directly into the current module, so included declarations share the same namespace and collision domain.
- A module spans the files of a folder, and is named for the folder. Its **designated file** — the file named for the folder, `geometry/geometry.cone` — declares it with `mod geometry;` written as that file's first statement, and a name written there is checked against the folder's rather than replacing it. The folder's name is the module's identity: what an importer binds it under, what a path through it is written with, and what its symbols are spelled after. A file that is not its folder's designated file is a module of one file, named after the file, and its declaration still renames it — which is transitional.
- **A module's own name is an entry in its own namespace**, so a module-level name that a local or a type member hides is reached as `modname.x`. That is the only way past a nearer binding into a module, because a path begins with the name of the namespace it walks.
- `import` answers the name it is given **in two places, the registry first**. A bare identifier is looked up in the importing module's *parent's* namespace — the registry a module is for its children — and a module found there is bound with nothing located, read or registered: that is how a **sister** is reached. Otherwise the name is a path, and the module is loaded or reused, keyed on the file's **canonical** path, so a file is read once and belongs to one module whatever that module turns out to be called and however the path to it was spelled. An import naming a file of the importing module's own folder, or its own submodule, is `ErrorModFile`; one whose path reaches any other module inside a tree, and a bare name that is the importing module's own parent, are `ErrorModReach`.
- **What an import binds is an `AliasDclNode` with a visibility of its own**: the module's name here, and every name its `use` clause admits. `pub` before the statement re-exports all of them at once; `pub use` re-exports the folds alone.
- A module's **global may carry a `use` clause**, folding members of its type in as names of the module: `config Config use *`. See "Folding through a global" below. No other variable may — a local, a parameter and a type's static are `ErrorBadFold` where the clause is written, because none of them is part of a namespace for a name to fold into.
- **A module may extend another**: `mod solids extends shapes;` in its designated file makes every declaration of `shapes`, and every name a `use` clause of `shapes` folded in, a name of `solids` — not the names `shapes`'s imports bind to their modules, which are `shapes`'s dependencies [Jon 23 Sep] — as an alias whose declaration, symbol and state stay `shapes`'s and whose visibility is the one it has in `shapes` — public where `shapes` shows it, private where `shapes` keeps it — and `solids` adds its own declarations beside them. See "A module extending a module" below.
- A nested `mod name { ... }` block is admitted by the grammar and unbuilt: it needs a namespace of its own, a hook pushed and popped around its parse, and paths reaching through it. So is `mod trait`, a module's abstraction.
- A folder creates a namespace exactly when it holds its designated file, and that holds at every level: a subfolder that holds one draws a **submodule**, with a namespace of its own; a subfolder that holds none groups a module's files without making a namespace of them, so its declarations are the enclosing module's and collide with them.
- **A submodule is a name of its parent's namespace**, bound at load under the folder's name, so it is reached as `sub.name` by the ordinary path rule and nothing about that path knows a folder put the name there. It is **private to its parent unless its own declaration writes `pub`**, which is what a path from anywhere else is checked against. Its declarations are spelled after its parent's name, and a submodule of a submodule after both.
- **A module reaches SIDEWAYS by importing a sister's name.** A module is the registry for its children: they are public to each other and to it, invisible outside it unless it publishes them. Nothing arrives unasked — a module's namespace holds what it declares, what a fold brought in, its own children and what its imports bound, so a sister nobody imported is `ErrorUnkName`.
- ⚠ **The registry is SCOPED and not accumulating, and that reading is provisional.** It is the *immediate* parent's namespace and no ancestor's, so descending a level drops the level above out of reach: a module two deep does not see its parent's sisters, and a parent re-exports what its children need. That keeps a module liftable, because its dependencies are stated at its own `mod`. It is decision 10 of the module design brief, adopted to be revisited.

Documented intent allows libraries packaged for import, and named modules nested within a source *file*. Package-level namespace rules and the nested `mod` block remain to be built; the module tree a folder tree carries is built.
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
- There is no root anchor and no parent access. **The enclosing module is named by its own name**, which its folder or its `mod` declaration gives it and which is an entry in its own namespace, so `mymod.x` reaches a module-level `x` that a local or a type member hides. A module that reaches an *upper* module does so by importing and naming it.
- **A parent reaches a submodule by path, `sub.x`**, and needs nothing new to do it: `sub` is a name of the parent's own namespace, which its subfolder put there, and what follows is the same namespace hop any path takes. The hop's privacy check already asks whether the qualifying module is the asking one, so a submodule's private name is `ErrorNotPublic` from its parent, and a submodule that is not `pub` is `ErrorNotPublic` from anywhere but its parent.
- A path may have any number of hops, each of which must resolve to a module or a struct-like type. **An alias resolves to whatever is at the end of its chain**, so a `typedef` of a struct is a hop like the struct itself. A hop through anything else — a number type, a generic instance, a generic parameter — is `ErrorUnkName` at type check.
- A resolved `NameUseNode` points directly to a heterogeneous declaration node. It keeps its one tag; whether it is a type, a value, a macro or a generic parameter is asked of that node (`nameUseGroup`, `nameUseNames`), never stamped on the use. Two things are stamped on it: `FlagQualified`, which says the name was reached through a namespace rather than written bare, and `FlagPattern`, which marks a pattern's bare root until type check binds it against the matched value ("A variant bare in a pattern", below).

A path is parsed as a chain of member accesses and collapsed during name resolution, hop by hop, by `fnCallNameResPath` — [fncall](../nodes/fncall.md), "The path collapse", has the mechanism and the reason it cannot wait for type check.

The NameDef design instead makes lookup return a stable NameDef. A resolved reference remains one kind of `NameUse` node pointing to that definition — as it already does — but the definition or its IR value explicitly indicates whether it is usable as a type, runtime value, callable, macro, namespace, generic, or other semantic kind, and the surrounding use validates that role, where today the use classifies the declaration by its tag.

### Bare names inside a type

While resolving a type body, the compiler places the type's members in the lookup context outside the method's parameter and block scopes. Normal nearest-scope lookup applies:

- A parameter or local with the same spelling shadows the type member.
- If no nearer binding shadows an instance field, its bare name is lowered to `self.field`.
- If no nearer binding shadows an instance method, calling its bare name is lowered to `self.method(...)`. This applies to an overloaded name too: the bare name resolves to the type's `FnOverloadDclNode`, and the lowered member call selects the concrete candidate.
- `self.field` or `self.method(...)` explicitly selects the member when a lexical name shadows it.

Implicit `self` is therefore lowering performed after ordinary name resolution has selected an unqualified type member; it does not take precedence over lexical bindings.

An inherited default method, and an enum's field spliced into a variant, are type members for this purpose. Each joins the type's dictionary while the type is name resolved, before any of its method bodies is, so its bare name is selected and lowered exactly as a member declared in the type is. The one exception is a default cloned from an instance of a generic trait, which joins only when the instance is type checked and so must be reached as `self.name`. A generic enum's fields are spliced into its variants at that same point, but a variant's body is inside the enum's braces and so has the enum's own names beneath its own: a bare common field binds to the enum's field, and is lowered to `self.name` like any other. A field a trait requires is not an inherited member at all: the type declared it, so it is the type's own.

### A variant bare in a pattern

**A pattern knows the enum of the value it is matched against, so it names that enum's variants bare with no `use`.** Every pattern on a value is covered, since `match` desugars to the others: `case is Red`, `case imm c Circle`, a bound `if imm c &Circle = s` and its `elif`, and the expression `s is &Circle`. The name looked up is the pattern's root — the referent's under a reference, the callee's where type arguments are written.

- **The matched value's enum is asked first**, through a reference, and the name's lexical meaning is used only when that enum has no variant of the name. So with `use Colors;` in force, `case is Red` on a `Lamp` means `Lamp`'s `Red`. A value that is not of an enum — a struct, a number, a variant already narrowed — has no variants to offer, and the lexical meaning is the only one.
- **A qualified pattern is taken as written.** `case is Colors.Red` is a path, collapsed by name resolution like any other.
- **The matched value supplies a generic variant's type arguments.** On an `Option[i32]`, `case imm s Some` means `Some[i32]`, because an instance's variant list holds its own instantiated variants. Written arguments work where the name is in scope, as `Some[i32]` is through core. **A name written with arguments is taken lexically**, since the matched enum's list holds instances and the arguments would be applied to one; one that is in scope only as a variant of the matched enum is `ErrorPatArgs`, whose message says to drop the arguments or qualify.
- **A name that is neither is `ErrorUnkName`**, reported once, at type check. So is one that names a value, `ErrorNotType`.

The matched value's type exists only at type check, so the lookup is split. The parser marks the root name (`FlagPattern`, `castPatternMark`); name resolution binds it lexically where it can and, where it cannot, leaves it unbound without reporting it; `castPatternBind` binds it against the matched value's enum before the pattern's type is checked, or reports it. ▸ **One consequence:** an unknown pattern name in a generic function's body is reported per instance, like any type error there, and not at all if the function is never instantiated. [cast](../nodes/cast.md) has the mechanism.

## Visibility

A declaration is private to the namespace that owns it unless it is written `pub`:

- A name declared in a module is private to that module unless declared `pub`.
- A type member is private to its type unless declared `pub`. A variant declared inside an enum is as visible as the enum, and may be declared `pub` itself.
- `pub` has one meaning wherever it appears: this entry is visible from outside the namespace that owns it. A local declaration has no outside to be visible from, so `pub` on one is `ErrorBadPub`; so is `pub` before `include`, which binds no name of its own. `pub` before `import` is re-export: every binding the import makes is a public name here. And `pub` always comes first, before the keyword of whatever it makes public — Jon, 23 Sep 2026: *"Let's make it consistent just for people to know what to expect."* So a fold says it as every declaration does, `pub use`: a module's `use` of an enum, `pub use Colors;`; an import's clause, `import m pub use a, b;`; a global's clause, `… pub use *;`. The fold's older spelling, `use pub`, is `ErrorBadPub` at each of the three, naming the one to write.
- A name's spelling says nothing about its visibility. A leading underscore is a character like any other.

The compiler enforces this on the routes that can reach a private name: `fnCallNameResPath` reports `ErrorNotPublic` for a private declaration reached by a path from outside its module, `importNameRes` skips private nodes when folding, `fnCallLowerMethod` refuses a private member on a receiver that is not `self`, and `typeLitStructReorder` refuses a value for a private field outside the type's methods. The parser sets `FlagPub` on whatever declaration the keyword precedes; where a declaration joins its namespace (`dclInfoJoin`) the flag is read once into its `DclPrivate` bit, and every check after that asks `inodeIsPrivate`, which answers from the bit for a declaration that carries `DclInfo` and from the flag for a node that carries none — a field, a const, a macro, a typedef, an overload name. Generation reads the same bit — see "Symbols".

An overload name's visibility is its candidates': the first candidate declares it, and every later candidate must agree (`ErrorPrivOverload`, either way round). A compiler-defined intrinsic candidate counts as `pub` for the name and is exempt from agreeing, which is how the core types keep a private `_neg` behind a pub `-`. One consequence is deliberate and worth knowing: **visibility is checked on the binding the caller's name reaches**, not on the candidate overload selection then picks, which is why a pub overload name may not hold a private concrete candidate — through the pub name the private one would be reachable.

Visibility should belong to the original definition or declaration, while access is evaluated from the use site. A folded or renamed NameDef must not make a private definition public merely by changing its local spelling, and it does not: the alias an enrichment makes carries its target's visibility, which is what lets a private member of the base come across and stay out of the enrichment's clients' reach. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it.

**The other half of that is the binding having a visibility of its own to widen with.** A fold into a *module* is private to the module that made it unless it says `pub` — `pub use` on a global's clause, on a `use` of an enum and on an import's clause; or `pub import`, which reaches the module's own binding and every name the import folds alike — whatever the name's visibility where it was declared. The two spellings on an import do not overlap, so they cannot disagree: `import m pub use *` publishes the folds and leaves `m` private, `pub import m use *` publishes both, and `pub import m pub use *` says what `pub import` says alone. One keyword, one meaning, on a binding as on a declaration. It never widens past the origin: only a public name folds at all. `fnCallNameResPath` is the route that reads it, since `inodeIsPrivate` answers from the alias's own `FlagPub`; a `pub` fold is reachable as `mod.name` and a plain one is not.

## Include, import, and name folding

`include` contributes declarations to the current module. It does not introduce a namespace.

Plain `import math` binds the imported module under the name it declares for itself; public members are intended to be accessed as `math.name`.

**Where an `import` may be written is decided and not enforced.** An `import` follows a `mod`, and only directly after one, wherever a `mod` appears — so a file that declares no module has nowhere to spell an import and cannot create a dependency, and a module's whole inbound dependency list sits at its own declaration. It is a grammar in which the illegal thing cannot be written, not a rule with a diagnostic behind it. Today's grammar allows an `import` as any global statement, and a file that declares no module imports freely. What the folder sweep settles is where the rule would bite: a file the folder swept in has no module to declare and so nowhere to stand an import. What holds enforcement back is that every source in this repository, in the test suite and in the samples imports with no `mod` at all, so the grammar would refuse all of them for nothing. **There is no placement rule for `use`**: as a clause it is on a declaration, so it lives wherever that declaration lives, and each site already says where that is; as a statement it is in a type body or at module scope, where a module's folds do not depend on the order they were written in.

**An import folds with a `use` clause**, the one a global carries, with the imported module as the source:

- `import math3d use Point3 as Point, Vector3;` — selected names, each under its local spelling.
- `import math3d use * but Matrix;` — every public name but some.
- `import math3d use { Point3 as Point, Vector3 };` — a block, for a long list.
- `import math3d pub use *;` — the folds re-exported: public names of this module, carried on by a wildcard import of it, while `math3d` itself stays private here.
- `import math3d.*;` — `use *` spelled the older way. Writing `.*` beside a clause is `ErrorBadFold`.

Any category of name folds, subject to the importing namespace's single collision domain. What the clause may name and admit:

- **Only a public binding of the source folds**, whether the source declared it or folded it in with `pub`. A private one named in a list is `ErrorNotPublic`; a star clause passes it over. (A module's `extends` is the one fold that takes private names too: "A module extending a module", below.)
- A name the source has not got, listed or after `but`, is `ErrorNoMbr`. The source's own name is the one the import binds already, so a list naming it is `ErrorBadFold` and a star clause passes it over.
- A folded name must be unique in the module, whatever brought the other one: a declaration, another import's fold, a global's, an enum's `use`. `ErrorDupName`, reported at the listed name, or at the `use` of a star clause; `as` or `but` settles it. **Unique means one declaration, not one route** — see "One declaration by two routes" below: the same declaration arriving again under the same name is the binding the name has already.
- **Every name folds as an `AliasDclNode` in the receiving module's namespace**, whose target is the source's own binding and whose `FlagPub` is the import's.
- **It reads the source module's *namespace*, not its declarations**, so a name the source itself folded in and re-exported travels on.
- Imported modules are loaded once and reused, and a sister is not loaded at all.

**A module imports another once.** An identical repeat — the same clause, its names in any order, with the same `pub` on each binding — says nothing new and is ignored. One that differs is `ErrorDupImport`, reported at the second and naming where the first is, since the module's name and its folds would otherwise mean two things. The identical repeat is the rule below met at parse, where it has to be decided because an import binds the module's own name before any fold runs.

**One declaration by two routes** [Jon 23 Sep]. Two bindings of the **same declaration** under the same name in one namespace are not a collision: the second is the binding the name has already. It holds for every fold that binds into a module's namespace — an import's clause, star or listed, a module's `extends`, a global's clause, an enum's `use` — all of which bind through `modFoldBind`:

- **The same** is the declaration at the end of each binding's chain of fold aliases, or the binding itself where it is a declaration, and the same global where either is reached through one. A `typedef` is a declaration of its own and ends the chain, since its target is resolved only after every fold. One member folded through two different globals is two things, and collides.
- **Where one route is public and the other private, the binding is public**, so a re-export is not lost to whichever route happened to fold first. A declaration of the module keeps its own visibility: nothing folds a private name of it back as a public one.
- **Different declarations under one name collide exactly as before** — `ErrorDupName` at the fold, or `ErrorExtendsOverride` where a module's `extends` meets its own declaration.
- **Neither the outcome nor the visibility depends on fold order.** Which binding is kept depends on which route came first; what the name means, and whether it is public, does not.

That is what lets a diamond compile — `d` doing `import a use *; import b use *;` where `a` and `b` each re-export `c`'s `x` — and what lets a module wildcard-import a module that extends it, which hands the module's own declarations back under their own names. `module-fold-diamond` runs the diamond and the public route winning in both orders, `module-extends-back` the second case in both fold orders, and `module-fold-diamond-nameres` pins what still collides. Two consequences, measured: a listed clause naming one name twice, `use one, one`, and two `use Colors;` statements are each one binding now rather than `ErrorDupName`.

**A type's namespace does not follow this rule**, and nothing yet asks it to. `structFoldItem` binds a field's fold with its own `namespaceAdd`, and a folded *field* is a copy carrying a hop, so two routes to one field are two nodes and cannot be compared by identity. Two routes to one method through two fields are two receivers, a genuine collision. The only same-route repeat a type can write is a listed clause naming one member twice, `b Box use area, area`, which is still `ErrorDupName`.

**Transit is not a rule of its own.** A fold is private to the module that made it unless the import says `pub` or `pub use`, which is the visibility rule; so what a third module sees through this one is what this one re-exported, and it is the same answer whatever order the files were loaded in. **Every module's folds run before any module's body is name resolved**, dependency-first, which is what removed the load order from the question — see [module](../nodes/module.md), "Name resolution".

The intended NameDef behavior is:

1. Folding or renaming creates a new NameDef owned by the receiving namespace.
2. The new NameDef may have a different local spelling.
3. It points to the same underlying IR value as the original definition.
4. It retains a link to the original definition or origin for identity, visibility, diagnostics, and generated naming.
5. Collision checks use the receiving namespace's complete name domain, regardless of the imported declaration's category.

Thus import aliasing duplicates a binding, not the underlying type, function, module, or other IR value.

### Folding into a type

A struct's field may carry a `use` clause folding members of the field's type in as names of the struct — delegated inheritance, [refinherit](../../conesite/public/coneref/refinherit.html). It is built, and it is the same operation as the module fold above at the namespace: one collision domain, an error at the fold for a name already taken, renaming with `as`, exclusion with `but`, visibility transitive so only what the field's type shows through a `pub` field folds. The clause is expanded into the struct's dictionary while the struct is name resolved, before any of its method bodies, so a folded name is usable bare inside the type ([struct](../nodes/struct.md), "Name folding").

**Where the two folds differ, measured by building this one: in what the binding holds.** Both make an alias; a type fold's alias is reached through a value, so a use of the name must be lowered to an access path or to a call whose receiver is shifted to the field, and a folded *field* is a copy carrying a hop rather than an alias at all. A module fold reaches through nothing — a module has one instance — so its alias never calls the receiver rewrite. The alias node is the binding record this note asks for: a local spelling, a visibility bit of its own, a target.

### Folding a whole type in: `extends`, and a sibling of it: `use`

A concrete type may be named as another's base, with `extends`, and everything it has becomes a name of the enriching type, which adds methods and no fields ([refinherit](../../conesite/public/coneref/refinherit.html); [struct](../nodes/struct.md), "Enrichment"). And a `use` standing as a statement in a type's body folds in a **sibling** — another type that declared this type's base — selectively, with `as` and `but` ([struct](../nodes/struct.md), "Sibling folding"). A module names the module it extends the same way. So one operation attaches at seven sites:

| Where the clause sits | What the receiver is | What the binding holds |
| --- | --- | --- |
| a **module** body — `import mod use *` | nothing; a module has one instance | an alias targeting the source's own binding, under the clause's spelling, with the import's visibility |
| a **module** declaration — `mod solids extends shapes` | nothing; a module has one instance | an alias targeting the base's own binding, for every declaration and fold of the base (not the names its imports bind), as visible here as there |
| a **module** body — `use Colors` | nothing; a variant is a type, reached through no value | an alias targeting the variant, with the statement's visibility |
| a **type** body — `extends Meter` | the **whole**, which is already the right type | the base's declaration, under an alias; the base's fields as copies of its own |
| a **type** body — `use Trig` | a **sibling of the whole**, which this type's values substitute for | the sibling's declaration, under an alias; nothing else |
| a **field** — `engine Engine use *` | the **part**, reached through the field | a copy carrying a hop, or an alias whose call shifts its receiver |
| a **module's global** — `config Config use *` | the **one instance**, at a fixed address | an alias carrying the global, field and method alike |

**The two type-body rows are the finding.** The two hard things a type adds to the module case are dispatch and per-instance state, and neither of them has either to solve. An enrichment may not change the fields, so its values and its base's have one representation and a base method already takes exactly the right receiver. A sibling declared that same base, so its method's receiver is a type this type's values substitute for, and the recast at the call is the whole adjustment — no copy, no hop, no thunk. So the machinery recurs where it is easy and the hard part stays in the field row, where the receiver has to be found and shifted.

**Which site a clause sits at decides what the collision domain is for, too.** A sibling fold is the one place where a collision is the *point*: two libraries that each enriched one base may both declare `span`, neither is wrong, and the type folding both settles it with `as` or `but` in one declaration that touches neither source. Everywhere else a collision is a mistake.

**A type-body `use` is never a consumer-side import.** Inside a type the word is always producer-side composition — this type is built out of that one — and ordinary lookup inside a type's bodies comes from the enclosing module, which is what `import` fed. The same word one scope out means the consumer-side thing — a module's `use` of an enum, which asks for the enum's variants as the module's own names — and the two do not meet: a type body has no imports of its own.

**Visibility is the target's, and this is where an alias narrowing it would matter.** An enrichment is inside its base's encapsulation boundary, so a private member of the base comes across — under an alias that is private here too, so the enrichment's own methods reach it and the enrichment's clients do not. That answers "may a fold make a private definition public" with no: the alias carries the target's visibility rather than its own opinion. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it yet.

**A sibling is inside the base's boundary too, and that is not the same as being inside the sibling's.** Both declared the same base, so both read its private members; what the sibling declares privately for itself is its own, and only what it shows folds. The asymmetry is the part-versus-whole cut again: towards the base each is the whole, and towards each other each is a stranger.

**Static folding aliases and keeps the original owner; dynamic folding replicates into each instance.** One-instance members — a static variable, a static function, a macro without `self` — are a new name for one thing, so two types folding one static share one storage location and one symbol. A field, and a method with a receiver, land in each instance, and nothing reaches back to whoever declared it. Which is why an `extends` base's fields are *absorbed* as the enriching type's own while a sibling's statics stay their owner's. **No type reads another type's fields, ever**: a method with a receiver reads the fields of the type it was written against, inside the one instance being operated on.

**A static therefore folds on a TYPE and never through a FIELD.** There is no such thing as a static field: a static is a one-instance thing and a field is the per-instance route, so a static has no business arriving through a value. A type's statics are had by writing a type-level clause — `extends`, or a sibling `use` — and `structFoldItem` reports `ErrorBadFold` for one named in a field's clause. **A global is the value route at module scale, so the same refusal applies to it**, with the same reason and the same code (`foldGlobalItem`): the way to have a type's static is to name it through the type.

### Folding through a global

A module's global may carry the same `use` clause a field does, and it folds members of the global's type in as names of the module: `config Config use *` makes `Config`'s members names here, reached through `config`. The clause is the field's exactly — `*`, a list, `as`, `but`, and a block form for a long list — with one addition a field's and a sibling's have no answer for, `pub use`, which an import's clause shares.

**It is the one-instance analogue of a field, and that is what makes it the cheap one.** A field's fold has to find the receiver at the call and shift it (`structFoldReceiver`), copy each folded field so the access path can be rebuilt, and emit a thunk where a folded method fills a vtable slot. A global has exactly one instance at an address known at compile time, so there is nothing to find: every entry is an **alias** carrying the global it is reached through, field and method alike, and a use of the name is **lowered to `global.name`** — `nameUseTypeCheck` for a member read, `fnCallTypeCheck` for a call, before it reads the callee. From that point the node is the path the author could have written by hand, so overload selection, the macro-method probe, borrowing, the receiver adjustments and generation see nothing new and learn nothing about folding. A chain works for the same reason: where the global's own type folded a field in, the member access resolves through that type's aliases and its receiver shift runs from the global.

**What it buys that folding a module cannot is composing a module from a STRUCT** — a module presenting a singleton's interface as its own names, which no module-to-module fold expresses.

**Expanded before the module's other nodes are resolved** (`modNameRes`), because a module's names do not depend on the order they were written in: a function declared above the global still names what the global folded. A folded name is entered in the module's namespace and hooked, so the collision domain is the module's one domain and `ErrorDupName` is reported at the fold — unless the name is bound already to the same member through the same global, which is one binding ("One declaration by two routes"); through another global it is another thing, and collides.

What a clause may fold from and what it may admit:

- The source is a **struct**. A global whose type is anything else, and one whose type is a trait or an enum, is `ErrorUseGlobal` at the clause — an abstraction has no members of its own for a value to reach.
- **Visibility is transitive**: only what the global's type shows folds, so a private member named in a list is `ErrorNotPublic` and a wildcard passes it over.
- A **static** does not fold, for the reason above; nor do `final` and `clone`, which belong to the type's own values' lifecycle.
- A name the type has not got, listed or named after `but`, is `ErrorNoMbr`.

**`pub use` is the binding's own visibility, and this is the site that asks for it.** A fold is private to the module that made it unless the clause says `pub`; the member is public in its type either way, and what `pub` decides is whether the *module* shows the name it gave it. A `pub` fold is reached from outside through the global, so the global must be public too — `ErrorNotPublic` otherwise, naming the global. An import's clause has the same answer, since a module's fold is a module's binding like this one. At a type's sites `pub` on a clause, before its `use` or after it, is `ErrorBadPub`: a folded member of a field's type is as visible as the field it is reached through, and a sibling fold declares no name of its own.

**A `pub` fold is reachable as `mod.name`, and a wildcard import carries it on**, because `importNameRes` reads the source module's namespace, which is where a fold's bindings live. **Which module may see a fold does not depend on load order**: every module's folds run before any module's body is resolved. `module-nameres-fold-visibility` pins the first; `module-transit` against `module-nameres-transit` pins the second, from three positions in the load order. **It travels round a cycle of imports too** [Jon 23 Sep]: where two modules import each other, one reads the other before the other's folds are in place, so the folds run pass after pass until one binds nothing new, and the re-export arrives ("Round a cycle of imports", below; `module-import-cycle`).

#### Round a cycle of imports

The folds are dependency-first, so a module's folds are complete before anything folds from it — except round a cycle of imports, which is legal. There one module reads another mid-fold, and a name the other re-exports is not there yet. So `modFoldAll` runs the fold pass over every module again, until a pass binds nothing new: a fixpoint, the way Rust resolves glob imports. It is cheap because two bindings of the same declaration under one name are one binding ("One declaration by two routes"): a pass re-binding what an earlier pass bound changes nothing, and "nothing new" — no name bound, no binding made public or made a fold — is the whole test. Only a pass that met a module mid-fold, or left a fold waiting, and made progress is followed by another; a program with no cycle takes one.

**It gives what a single pass gives an acyclic program.** Until the passes settle nothing that might arrive is reported: a listed name the source has not got, or has only privately, and a global's fold or an enum's `use` whose type or enum is named through a binding not there yet, wait. The pass after the last reports each fold still waiting, with the message it always had, so a `but`, an `as` and a listed name are judged against the source once it is complete, and a private name stays private unless a public route to the same declaration arrives, when the binding is public, whichever route came first. **A collision is reported once**, when it is met — two different declarations under one name is final — and at the fold a single pass would have met it at: the one that comes second in the order a module's folds run (`extends`, the imports, the globals, the enum `use`s, each as written), not the one that happened to arrive second. `module-import-cycle` runs each case a single pass lost; `module-import-cycle-nameres` pins what is still reported, and the collision in both clause orders.

### Folding an enum's variants into a module

A variant is a name of its enum, so `Colors.Red` is its spelling everywhere, the module that declares the enum included, except in a pattern on a value of the enum, which names it bare ("A variant bare in a pattern", above). **`use Colors;` at module scope is how a module asks for the variants bare**: each variant it admits becomes a name of the module. The clause after the enum is a type body's sibling `use` exactly — every variant by default, a list with `as` (`use Colors Red, Green as Verde;`), a block (`use Colors { Red, Green };`), or every variant `but` some — and `*` is `ErrorBadFold`, since naming the enum already asked for them all.

**It is the module's third fold, and the cheapest.** A variant is a type, reached through no value, so the binding is the one an import's fold makes: an `AliasDclNode` whose target is the variant, with no receiver and a visibility of its own. `foldEnumUseExpand` runs in `modFoldNames` after the imports and the globals' clauses, so the enum may be named through anything they folded in, and before any body resolves, so the name is in place wherever it is used. The statement is held on an `EnumUseNode` on the module's `enumuses` list — not in its node walk, because it is neither a declaration nor a field ([module](../nodes/module.md)).

What it may fold from and what it admits:

- The source is an **enum declaration**, written as its name or a path through modules to it (`use shapes.Colors;`); a name an import folded in is followed to the declaration. Anything else — a struct, a trait, a function, a variant, a module — is `ErrorUseEnum`, and so is an instance of a generic enum, `use Option[i32];`: a generic enum's variants are the enum's, whatever it is instantiated with, so the statement names the enum alone. **A `typedef` of an enum is refused too**, `ErrorUseEnum`, because a typedef's target is resolved with the module's other nodes, after every fold, so what it names is not known when the fold runs.
- An enum that extends another has its copies of the base's variants only once it is resolved, so the fold **demands it resolved first** (`structEnumDemandSet`), in its own module's scope, and then folds every variant it holds, copies and added alike. A path `RichColors.Red` demands it the same way (`fnCallNameResPath`), for a name its namespace does not have yet.
- It admits **variants and nothing else** of the enum: not its fields or methods, which are reached through a value of it, not its statics, not `Self`, not its equality operators. A listed name the enum has but that is not a variant is `ErrorBadFold`; one it has not got, listed or after `but`, is `ErrorNoMbr`.
- A folded name must be unique in the module, whatever brought the other one: a declaration, an import's fold, another enum's `use`. `ErrorDupName`, reported at the fold. Two enums may each have a `Quit`; a module folding both settles it with `as` or `but`. The same variant arriving again — a second `use Colors;`, or an import that re-exports `Colors`'s variants — is the binding the name has already ("One declaration by two routes").
- **`pub use` is the binding's own visibility**, as on a global's clause: the fold is private to the module that made it unless the statement says `pub`, and a `pub` fold is reachable as `mod.Red` and carried on by a wildcard import. It never widens past the origin, so `pub use` of a private enum is `ErrorNotPublic`, reported at the `use`. `use pub Colors;` is `ErrorBadPub`, naming the spelling.

**Core folds its own two enums with `pub use`**, and that is the whole reason `Some`, `None`, `Ok` and `Error` are bare in every program: the automatic import of core is a wildcard, and a wildcard carries a public fold on. No prelude rule stands behind them, and `?T` still lowers to `Option[T]` by the enum's own name.

`enum-nameres-use` and `enum-parse-use` pin the refusals; `module-use-enum` runs a public fold reached by a path and through a wildcard import, and `module-use-enum-nameres` pins what a private one keeps out of reach.

### A module extending a module

`mod solids extends shapes;` — the module's declaration, as its designated file's first statement — makes `solids` a module that **reuses** `shapes`: every declaration of `shapes`, and every name `shapes`'s `use` clauses folded in, becomes a name of `solids`, and `solids` adds its own declarations beside them. What `shapes`'s imports bind to their modules does not come across [Jon 23 Sep].

**For a module, extending and inheriting are one thing**, by the static/dynamic rule above. Static folding aliases and keeps the original owner; a module's state is all static — one instance, at a fixed address — so there is no per-instance copy for inheritance to make, and what `extends` makes is an `AliasDclNode` per name. The declaration, its symbol and its state stay `shapes`'s: a global changed through `solids.count` is the one `shapes.count` reads, and nothing is spelled after `solids` but what `solids` declares.

**It is an import's star fold with four differences**, and it is built as one — an `ImportNode` marked `isextends`, held on the module's `extends` slot rather than its `imports`, expanded by `importNameRes` in `modFoldNames`, first of the module's folds and dependency-first:

- **Every declaration and fold comes across, private ones included, and each alias is as visible in `solids` as it is in `shapes`** [Jon 23 Sep]. `solids` is inside `shapes`'s boundary, as an enriching type is inside its base's, so `solids`'s code reads `shapes`'s private names, and one global reached through either name is one global. What a module extends is part of its own surface, so a public name of `shapes` is public in `solids`, and an importer of `solids` reaches it through it — by path, `solids.area`, and through a wildcard import — exactly as it reaches what `solids` declares. A private name of `shapes` stays private in `solids`, so an importer sees `shapes`'s public surface and nothing more: named by path or in a listed clause it is `ErrorNotPublic`, as any private name of `solids` is, and a wildcard passes it over. A chain transits: what `shapes` took from a module it extends is a name of `shapes`, with the visibility it had there, so it reaches `solids` too.
- **It binds no name of its own.** `shapes` is not a name of `solids` unless an import binds it.
- **A name the base has may not be redeclared**, public or private. A declaration of `solids` spelled like a name of `shapes` — a submodule's name included — is `ErrorExtendsOverride`, reported at the declaration: the type rule, since one name would otherwise mean two things depending on which module reached it, and like the type rule it refuses a candidate joining the base's overload name too. Another fold of `solids`'s colliding with a different declaration under one of the base's names is `ErrorDupName`, reported at that fold; the same declaration reached again is one binding ("One declaration by two routes", above).

- **The names `shapes`'s imports bind to their modules do not come across** [Jon 23 Sep]. A module's imports are its dependencies, not its contents, and scoped imports exist so that each module states its own. `shapes`'s `import c` binds `c` for `shapes` alone: `c.x()` in `solids` is `ErrorUnkName` until `solids` writes `import c` itself, and `shapes`'s `pub import c` does not put `c` in `solids`'s surface either. A declaration of `solids` named `c` is no collision.

**What `shapes`'s `use` clauses fold in is a part of `shapes`, and does come across**, each fold as visible as it is there: a fold on an import's clause, a global's or an enum's. `shapes`'s `import c use x` makes `x` a name of `solids` (a declaration of `solids` named `x` is `ErrorExtendsOverride`) while `c` stays `shapes`'s. Its private folds — the automatic wildcard import of core among them, which is a fold alone and binds no module name — arrive beside `solids`'s own, the same declarations and so one binding each. The binding an import makes of its module's name carries `FlagImportName`, and `importStarAdmits` leaves it out of an `extends`; where a fold of `shapes` brought the same module in under that name as well (`import c` beside `import relay use c`, `relay` re-exporting `c`), `modFoldBind` clears the flag, because the name is then a fold of `shapes` too, and it comes across. `importStarAdmits` admits the private names to an `extends`, and `importFoldItem` takes each alias's `FlagPub` from the base's binding rather than from a clause.

**What `extends` may name** is resolved in a pass of its own, `modExtendsResolve`, run for every module before any fold (`pgmNameRes`), so every edge is known before the first fold follows one. It names a module the module can **already reach** — its own namespace, where an import bound the module's name, and then the registry its parent is, which holds its sisters — looked up, never loaded, and by one name. Refused:

- a name that reaches nothing, a cousin two levels over among them — `ErrorUnkName`;
- a name that is not a module — `ErrorModExtends`; a **trait** is refused with its own message, since `mod arena extends Region` is how a module conforming to a module trait is spelled, which is a different reading and is not built;
- the module itself, and a module it contains — `ErrorModExtends`; its parent — `ErrorModReach`, as for an import;
- a chain of `extends` that comes back to where it started — `ErrorModExtends`, once, at the first module on the cycle the program holds, and the chain is cut there (`modExtendsCheckCycle`);
- as it is parsed, more than one module (`ErrorExtends`), a path (`ErrorModExtends`) and no name (`ErrorNoName`).

`module-extends` runs a chain of two and both importers, both bases' private names and the base's private fold reached from inside, and the extending module importing for itself a module the base imports; `module-extends-back` runs a module importing, `use *`, the module that extends it, in both fold orders; `module-extends-import` extends a module an import reached by its file; `module-extends-nameres` and `module-extends-parse` pin the refusals.

## Aliases

`AliasDclNode` (`ir/stmt/aliasdcl.c`) is the general binding: a local spelling and a target, with the `FlagPub` bit as its own visibility and everything else the target's. Chains resolve through `aliasDclResolve`; a use bound to one answers as its target (`nameUseGroup`), and every site that reads a namespace binding resolves it first. A chain of aliases is ordinary: a type that enriches one which folded a sibling in binds an alias to that alias.

It is made for:

- a folded method, overload set or macro method of a field's type; every member but the fields of an `extends` base; and every member a sibling `use` admits — a static among them, which is the one case where an alias stands for something reached through the type rather than through a value, and where the `FlagMethFld` bit is therefore left off. The target is a member name use bound to the declaration.
- **every name a global's `use` clause folds in**, field and method alike, with `through` naming the global. `FlagPub` is the clause's own, from `pub use`, rather than the target's.
- **every variant a module's `use` of an enum folds in**, the target a name use bound to the variant. Neither flag the member aliases carry: `FlagPub` is the statement's own, from `pub use`, and there is no receiver.
- **every binding an `import` makes** — the imported module's own name, and each name its `use` clause admits, under the clause's spelling. The target is the source module's own binding rather than the declaration at the end of the chain, so the origin is kept; `through` is copied from the source's binding where that one is reached through a global, so a re-exported global fold lowers the same way from any module. `FlagPub` is the import's: on the module's own binding from `pub import`, on a fold from `pub import` or `pub use`.
- **a `typedef`**, whose target is a type expression rather than a member name — the one alias with something of its own to name resolve and type check, which `FlagTypeAlias` says. A typedef therefore has no node kind of its own: it is the binding record, proved to generalise by carrying the construct that motivated the word "alias" in the first place. Its `pub` is the bit on the binding, as `pub` on any declaration is.

**What making `typedef` an alias changed, visibly: an alias may now qualify what it names.** `Sample.make` walks `Reading`'s namespace where `typedef Sample Reading`, because a path's base is asked of the declaration at the end of the chain and an alias answers for its target. It used to be refused, since the collapse found a node that was neither a module nor a type and gave up.

**A chain that comes back to itself names no type**, and every reader walks a chain to its end, so `aliasDclCheckCycle` reports `ErrorCircular` and cuts it before anything follows one: two pointers at two speeds meet only inside a cycle, and the target then becomes `unknown` so the name still answers as a type. One diagnostic per ring, at whichever name the walk reached first.

The aspirational model generalizes aliases: a new NameDef may denote anything nameable. Alias chains should preserve each local binding for diagnostics and visibility while semantic operations can reach the final IR value. A type-valued alias remains structural; creating a distinct nominal type should use a separate construct.

Import folding is the alias above, and renaming on import is that alias with the local spelling an `as` in the import's `use` clause writes. Other aliases may bind expressions or declarations directly. The exact syntax and compile-time restrictions for general aliases remain open.

`typedef` is the alias, so "current `typedef` creates a module-scoped structural alias" is now literally what the IR holds rather than a description of a separate node. What remains of the aspiration is the range of things a target may be, not the record.

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
(owned by the current module), and an enum's variant, which is on the module's
node list but bound in the enum's namespace and owned by the enum, so its
symbols are spelled after it.

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
  a module never sits inside a type. **A submodule is an enclosing module like
  any other**, so a declaration of `geometry/scaling/` is spelled
  `geometry.scaling.twice` and the folder tree it came from is nowhere in it:
  what a symbol carries is modules, and an organisational folder is not one. **There is no package name.** The compiler knows only module names declared in source; the version
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
- **A struct a global's fold needs is resolved once, even round a cycle.** The fold passes retry a fold whose own source has not arrived, but a global's fold resolves its struct on demand, in the struct's module, and where that module is mid-fold a name the struct's declaration reaches through a late re-export (a field's type) is reported unknown, and not retried ([module](../nodes/module.md), Hazards).
- Nested named modules are documented and unbuilt. The declaration syntax is settled — a `mod name { ... }` block where the singular `mod name;` header stands — and the block is refused where it is written; what it needs is a namespace of its own, hook push and pop around its parse, and qualified paths through it.
- General aliases beyond `typedef` and the folded-member alias are not implemented: nothing yet names an expression or a declaration directly, and there is no spelling for one outside a fold clause and `typedef`.
- Generic, macro and metaprogram namespace behavior is partly implemented, incomplete, or aspirational. Delegated inheritance and concrete enrichment are both built; see "Folding into a type" above.
- Packages organize importable libraries but are not yet defined as a distinct namespace layer.
- A path may only pass through a module or a struct-like type, or an alias of one. One whose base is a number type, a generic instance or a generic parameter is refused at type check, because none of those names a namespace at the point the collapse runs. Finishing those at type check, where they do, is the natural other half of the collapse and is not built.
- A module that is one file is still named after that file, and a `mod` declaration in it still renames it. A folder is what replaces filename naming, and a module that has no folder of its own has nothing else to be named after.
- **A module reaches a sister, and an external module by a path; a PACKAGE name resolves to nothing.** The registry a module is for its children is built, so `import` has two halves and only one of them waits: turning an external package name into an interface to read is the package work's, and until then `import` writes a path for anything outside the tree.
- ⚠ **A nested module cannot name a public declaration of its parent.** The registry a child reads is the parent's namespace, and what an `import` may take from it is a module; a type or a function the parent declared `pub` is not reachable by any spelling, since a child may not import its parent and a bare name does not climb. A parent sharing a public type with its own submodules is part of the settled model and is not built.
