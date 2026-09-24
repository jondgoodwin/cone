This note is the **rules**: what a name means, how lookup, visibility,
qualification, imports, aliases and overloading are supposed to behave, and
where the compiler does not yet match. Parts of it describe intended rather than
current behavior, and say so.

[Name Resolution](../../compiler/c/doc/phases/name-resolution.md) is the **mechanism** — how the walk
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
| `compiler/c/parser/lexer.c` | Interns keywords and identifier spellings through `nametblFind`, so equal spellings share one `Name`. |
| `compiler/c/ir/nametbl.c` | Owns the global intern table and the push/hook/pop mechanism used to expose the nearest lexical or namespace binding through `Name.node`. |
| `compiler/c/ir/namespace.c` | Implements the hash table owned by each module or namespaced type: initialize, find, set, add, duplicate detection, and growth. |
| `compiler/c/ir/name.c` | Defines well-known interned names and spells every declared symbol — `nameSymbol`, `nameVtable`, `nameVtableImpl`, `nameVtableThunk` — from a declaration's owner chain and facts. See "Symbols". |
| `compiler/c/ir/stmt/aliasdcl.c` | The alias: a binding that stands for another declaration under a spelling of its own, with a visibility of its own. Made for the members a `use` clause folds in, and for a `typedef`; `aliasDclResolve` follows a chain to the declaration, `aliasDclCheckCycle` refuses one that comes back to itself, and `aliasDclThroughAccess` builds the member access a global's folded name is reached by. |
| `compiler/c/ir/stmt/fold.c` | The parts of a `use` clause every fold site shares: the source declaration behind a type expression, the star clause's items, `but`. It also owns the module's two folds of its own: `foldGlobalExpand`, which is the whole of a global's, and `foldModUseExpand`, which is the whole of a module's standalone `use` and of the `ModUseNode` that holds one — an enum's variants folded here, a submodule's names handed to the import fold (`foldModUseModule`). |
| `compiler/c/ir/dclinfo.c` | The declaration facts a symbol is derived from: sets them where a declaration joins its namespace (`dclInfoJoin`), walks to the enclosing module, prints them in the IR dump. |
| `compiler/c/ir/inode.c` | Dispatches the name-resolution pass by IR node tag. Start here when a new node kind must participate in name resolution. |
| `compiler/c/ir/clone.c` | Rebinds generic/macro parameters during cloning and repairs resolved declaration references in cloned `NameUse` nodes. |

### Parsing and module namespaces

| C file | Name/namespace capability |
| --- | --- |
| `compiler/c/parser/parseexpr.c` | Parses a name as one identifier, and everything after a period as a member access — a path and a member of a value are the same production here. |
| `compiler/c/parser/parsemod.c` | Parses module-level declarations, the `mod` declaration with its `extends` and its default fold (`parseModDefaultFold`), `include`, and `import` with its `use` clause, refusing `.*` and `.name` after the module; refuses a second import of one module, identical or differing; answers an import's name against the registry its parent is before the filesystem, and in a submodule holds a bare name neither answers as an import of a name of the parent, bound in the fold passes; loads/reuses modules by canonical path, draws the module tree, names each module, and establishes module hooks. |
| `compiler/c/parser/parsetype.c` | Parses struct/trait/enum members and inserts fields, methods and an enum's variants into the type namespace; parses a module's standalone `use`, of an enum or a submodule (`parseModUse`), and the fold clause a field, a global and an import carry (`parseFoldClause`). |
| `compiler/c/ir/stmt/program.c` | Owns the program's module list and the file registry, and runs name resolution in four walks: what every module's `extends` names, then the module order, a loop refused (`pgmModuleOrder`, which keeps the order for `init`), then every module's folds, then every module's body. |
| `compiler/c/ir/stmt/module.c` | Owns module namespaces, inserts global declarations with duplicate checks, switches active module hooks, resolves and checks what a module's `extends` names (`modExtendsResolve`), puts a module's folded names in place dependency-first (`modFoldNames`) — what it extends, its imports', its globals' and its `use` statements' — checks what its `mod` line's default fold names (`modDefaultFoldCheck`), and walks module declarations. |
| `compiler/c/ir/stmt/import.c` | Binds an imported module's name as an alias carrying the import's visibility, folds what the import's clause admits of the source module's public *namespace* into the importer, one alias per name under its local spelling, and says whether two imports of one module are the same (`importSame`). An import that writes no clause is given its module's default fold here (`importDefaultFold`). A module's `extends` is folded here too, as an import marked `isextends`. |
| `compiler/c/shared/fileio.c` | Locates a source file by the designated-file convention, and gives a path its one canonical spelling so that the file registry keys it once. |

### Name uses, lexical scopes, and declarations

| C file | Name/namespace capability |
| --- | --- |
| `compiler/c/ir/exp/nameuse.c` | Represents name and member uses, resolves a bare name through the hooks, and answers what a use is — type, value, macro — from the declaration it is bound to (`nameUseGroup`, `nameUseNames`). Binding a bare field name is here; **lowering it to `self.field` is `nameUseTypeCheck`'s**, because building that call node needs a type to check it against. Walking a path through module and type namespaces is `fncall.c`'s, with the collapse. |
| `compiler/c/ir/exp/cast.c` | Binds a pattern's bare root name against the matched value's enum before its lexical meaning (`castPatternBind`), and reports it when neither answers; `castPatternMark` is the parser's half. |
| `compiler/c/ir/exp/block.c` | Pushes lexical scope hooks, binds labeled lifetimes, resolves statements in declaration order, and restores outer bindings on block exit. |
| `compiler/c/ir/stmt/fndcl.c` | Establishes function generic-parameter and value-parameter bindings while resolving signatures and bodies. |
| `compiler/c/ir/stmt/vardcl.c` | Resolves an initializer before binding its local variable, enforces same-scope uniqueness, and permits nested shadowing through scope hooks. |
| `compiler/c/ir/stmt/fielddcl.c` | Resolves field permission, type, and default-value names; namespace insertion is handled by the enclosing type. |
| `compiler/c/ir/stmt/const.c` | Resolves constant types and values; module insertion is handled by `module.c`. |

### Type members, methods, generics, and macros

| C file | Name/namespace capability |
| --- | --- |
| `compiler/c/ir/instype.c` | Provides shared namespaced-type operations, binding of each concrete function/method name and of its separate overload node, field/method lookup, and all-candidate method selection. |
| `compiler/c/ir/types/struct.c` | Owns struct/trait member namespaces; inserts fields, `Self`, the default methods of every abstraction the type is-a or mixes in and, for a variant, its enum's fields, the base resolved on demand first; hooks members and generic parameters during resolution; and performs the inherited-member collision checks. |
| `compiler/c/ir/exp/fncall.c` | Resolves fields and overloaded methods from type namespaces, lowers member access/calls, inserts implicit `self`, and finds `init` for type calls. **All of this is `fnCallTypeCheck`'s**, not name resolution's — `fnCallNameRes` walks `objfn` and the arguments and deliberately leaves `methfld` alone, since selecting a member needs the receiver's type. |
| `compiler/c/ir/meta/macro.c` | Establishes macro parameter scope and resolves names in macro bodies before expansion. |
| `compiler/c/ir/meta/genvardcl.c` | Binds generic variables into the active resolution scope. |

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
- **Anything written inside an enum's braces sees every name the enum declares bare** — its variants, fields, methods and statics — and a variant's body is written there too: a method of `Colors.Red` writes `Green[]`, not `Colors.Green[]`. The enum's own methods have it because the enum's namespace is hooked while they are resolved. A variant is a module node resolved on its own, so `structNameRes` hooks its enum's namespace in a scope beneath the variant's own ([struct](../../compiler/c/doc/nodes/struct.md), step 2): a name the variant declares or inherits wins a clash with one of the enum's, and the enum's names in turn are nearer than the module's. Outside the braces nothing changes: a variant is qualified, folded with `use`, or bare in a pattern on a value of the enum.
  - **An extension's copy of a base's variant means the base's siblings.** The copy is cloned already resolved, so a bare `Green` in its method is what it bound inside the base's braces — `Colors.Green`, as the qualified spelling there is — and that is the only meaning its signature, written against the base, admits. A variant the extension declares is inside the extension's braces and sees the extension's names, the copies included.
  - **Inside an extension's braces its bases' names are bare too**, down the whole chain — static functions, statics and methods, generic or not — exactly as its own are, in its own methods and in the variants it adds, as `mod A extends B` gives `A` the names of `B` (`structEnumHookBaseNames`). They are hooked beside the extension's own, each only where the extension holds no name of its own for it, so the extension's name — a copy, a clone, a variant it adds — is the nearer one. A generic base's member exists only per instance, so the use is bound to the template's and type check points it at the instance's ([struct](../../compiler/c/doc/nodes/struct.md), "An enum extending an enum").
  - **A method a value of the enum answers comes to the variant as its own clone**, spliced in and hooked in the variant's scope, so for an enum that is not generic it is not hooked from the enum's (`structHookEnclosingEnum`). **A generic enum's method is bare in its variants' bodies all the same**, though its clones join only when an instance is type checked, like a generic trait's defaults: the enum's method and method overload names are hooked for the variant, the instance's clone re-points the use at the enum instance's, and type check lowers a bare call to `self.name`, found by name in the variant, so it is the variant's own clone that runs. Its fields are bare, since a bare field is read through `self` by name, and so are its variants and statics.
  - **A generic enum's variant has its own copy of the enum's parameters hooked**, spelled as the enum's, so it names a sibling as the enum's own methods do, `Full[T][v]`, whether that sibling is declared before it or after: an instance's variants are all instantiated before any is type checked ([generic](../../compiler/c/doc/nodes/generic.md), "A tagged trait fans out").
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
- `include` is retired, and is `ErrorInclude` at the word wherever the statement is written: the folder is what brings a module's files in, so every file of a module shares its namespace and collision domain by being in its folder. The word stays a keyword, so no program binds it.
- A module spans the files of a folder, and is named for the folder. Its **designated file** — the file named for the folder, `geometry/geometry.cone` — declares it with `mod geometry;` written as that file's first statement, and a name written there is checked against the folder's rather than replacing it. The folder's name is the module's identity: what an importer binds it under, what a path through it is written with, and what its symbols are spelled after. **A file of the folder whose first statement is `mod` is a module of its own, a ONE-FILE MODULE** [Jon 23 Sep]: a submodule named for its file, `mod lexer;` in `lexer.cone`, whose name is checked as a folder's is, and which is in every other respect the submodule `lexer/lexer.cone` would draw — so growing it into that folder changes no name, no path and no symbol. A file the compiler is given that is not its folder's designated file is a lone file, named after the file, and its declaration still renames it — which is transitional.
- **Every file the compiler builds as a module opens with its `mod` line** [Jon 24 Sep] — a lone file, a module's root, a folder module's designated file, a one-file module — naming the file, or the folder; a first file without one is `ErrorNoModDcl`. The other files of a folder module carry none: the folder names the module they join. **A file's imports come right after its `mod` line**, or first in a file with none, ahead of every other declaration; an `import` after one is `ErrorImportLate`. So a file's header is comments, the `mod` line and the imports, which is all Congo reads of it.
- **A module's own name is an entry in its own namespace**, so a module-level name that a local or a type member hides is reached as `modname.x`. That is the only way past a nearer binding into a module, because a path begins with the name of the namespace it walks.
- `import` answers the name it is given **in two places, the registry first**. A bare identifier is looked up in the importing module's *parent's* namespace — the registry a module is for its children — and a module found there is bound with nothing located, read or registered: that is how a **sister** is reached. Otherwise the name is a path, and the module is loaded or reused, keyed on the file's **canonical** path, so a file is read once and belongs to one module whatever that module turns out to be called and however the path to it was spelled. An import naming a file of the importing module's own folder, or its own submodule, is `ErrorModFile`; one whose path reaches any other module inside a tree, and a bare name that is the importing module's own parent, are `ErrorModReach`.
- **The registry holds more than modules** [Jon 23 Sep]: `import` takes any name the parent's namespace holds. Inside a submodule, `import Point;` binds the parent's **public** `Point` — a type, a function, a global, or a name the parent itself bound, such as a module it re-exported — exactly as `import log;` binds a sister: an alias under the import's own visibility, whose target is the parent's own binding. A private name of the parent is `ErrorNotPublic`, so `pub` shares a name downward as well as outward; a name the parent has not got is `ErrorUnkName`. A submodule is parsed before its parent's own files, so only a sister is found at parse: a bare name that neither the registry nor a file answers there is bound in the fold passes (`importBindName`), and a bare name a file answers is refused there if the parent answers it with something else (`ErrorDupName`). The binding is written, so importing the name twice is `ErrorDupImport` and importing a name the submodule declares is `ErrorDupName`; and it is a dependency like a module's binding (`FlagImportName`), which a module extending this one does not take. Where the parent's answer is a module, the import's `use` clause folds from it as from any module; on anything else a clause is `ErrorBadFold`, since a type's members are reached through the type. A sister and a declaration of the parent cannot share a name, so the import is never ambiguous. 🛑 **But every such import is refused, as a loop** (`ErrorImportLoop`) [Jon 23 Sep]: the child depends on the parent, and the parent on each of its children ("Imports form a DAG", below). The binding is still made, so the child's body resolves and the loop is what is reported; what a parent and its children share moves into a sister they all import.
- **Imports form a DAG, at every scale** [Jon 23 Sep]: see "Imports form a DAG" below.
- **What an import binds is an `AliasDclNode` with a visibility of its own**: the module's name here, and every name its `use` clause admits. `pub` before the statement re-exports all of them at once; `pub use` re-exports the folds alone.
- A module's **global may carry a `use` clause**, folding members of its type in as names of the module: `config Config use *`. See "Folding through a global" below. No other variable may — a local, a parameter and a type's static are `ErrorBadFold` where the clause is written, because none of them is part of a namespace for a name to fold into.
- **A module's `mod` line may name its default fold**: `mod bigint use BigInt;` makes a bare `import bigint;` bind `BigInt` as well as `bigint`, so a package holding one thing becomes the thing where it is imported [Jon 23 Sep]. It is not an export list, and an importer's own clause replaces it. See "A module's default fold" below.
- **A module may extend another**: `mod solids extends shapes;` in its designated file makes every declaration of `shapes`, and every name a `use` clause of `shapes` folded in, a name of `solids` — not the names `shapes`'s imports bind to their modules, which are `shapes`'s dependencies [Jon 23 Sep] — as an alias whose declaration, symbol and state stay `shapes`'s and whose visibility is the one it has in `shapes` — public where `shapes` shows it, private where `shapes` keeps it — and `solids` adds its own declarations beside them. See "A module extending a module" below.
- **A module may conform to a module trait**: `mod trait Runner { ... }` declares a module's abstraction, its functions and globals, each a requirement or a default, and `mod plain is Runner;` says `plain` has each of them — declared, or taken as a copy of the default, which is then `plain`'s own declaration. On the `mod` line the order is `mod prog extends base is Runner use Y;` [Jon 23 Sep]. See "A module conforming to a module trait" below.
- **Nesting is by files and folders only.** A module is never declared inside a file: a `mod name { ... }` block is recognised only to be refused, with its body skipped. A module trait is not a module and is declared inside a file, as any declaration is.
- A folder creates a namespace exactly when it holds its designated file, and that holds at every level: a subfolder that holds one draws a **submodule**, with a namespace of its own; a subfolder that holds none groups a module's files without making a namespace of them, so its declarations are the enclosing module's and collide with them. A file creates a namespace exactly when its first statement is `mod`, and only directly in its parent module's folder: in an organisational subfolder it is `ErrorModFolder`, and beside a module folder of its own name `ErrorModFileFolder`.
- **A submodule is a name of its parent's namespace**, bound at load under its folder's or its file's name, so it is reached as `sub.name` by the ordinary path rule and nothing about that path knows a folder or a file put the name there. It is **private to its parent unless its own declaration writes `pub`**, which is what a path from anywhere else is checked against. Its declarations are spelled after its parent's name, and a submodule of a submodule after both.
- **A module reaches SIDEWAYS by importing a sister's name, and never UP**: importing a name of its parent is a loop, refused. A module is the registry for its children: they are public to each other and to it, invisible outside it unless it publishes them. Nothing arrives unasked — a module's namespace holds what it declares, what a fold brought in, its own children and what its imports bound, so a sister nobody imported is `ErrorUnkName`. That holds for the module's folds too, whichever module's fold pass ran them: a sister's `use Dir;` does not find the `Dir` of the sister whose import reached it, nor a child's its parent's (`module_fold_scope_nameres`; [module](../../compiler/c/doc/nodes/module.md), "Name resolution").
- ⚠ **The registry is SCOPED and not accumulating, and that reading is provisional.** It is the *immediate* parent's namespace and no ancestor's, so descending a level drops the level above out of reach: a module two deep does not see its parent's sisters, and a parent re-exporting what its children need is no way round that, since a child importing what its parent binds depends on the parent and is refused. That keeps a module liftable, because its dependencies are stated at its own `mod`. It is decision 10 of the module design brief, adopted to be revisited.

Documented intent allows libraries packaged for import. Package-level namespace rules remain to be built; the module tree a folder tree carries is built.
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
- There is no root anchor and no parent access. **The enclosing module is named by its own name**, which its folder, its one file or its `mod` declaration gives it and which is an entry in its own namespace, so `mymod.x` reaches a module-level `x` that a local or a type member hides. A module that reaches an *upper* module does so by importing and naming it; a submodule does not reach its *parent* at all — naming it is `ErrorModReach`, and importing one of its names (`import Point;`) is a loop through containment, `ErrorImportLoop`.
- **A parent reaches a submodule by path, `sub.x`**, and needs nothing new to do it: `sub` is a name of the parent's own namespace, which its subfolder or its one file put there, and what follows is the same namespace hop any path takes. The hop's privacy check already asks whether the qualifying module is the asking one, so a submodule's private name is `ErrorNotPublic` from its parent, and a submodule that is not `pub` is `ErrorNotPublic` from anywhere but its parent. To have a submodule's names bare, the parent folds them with a standalone `use sub;` ("Folding a submodule's names into a module", below), never with an import.
- A path may have any number of hops, each of which must resolve to a module or a struct-like type. **An alias resolves to whatever is at the end of its chain**, so a `typedef` of a struct is a hop like the struct itself. A hop through anything else — a number type, a generic instance, a generic parameter — is `ErrorUnkName` at type check.
- A resolved `NameUseNode` points directly to a heterogeneous declaration node. It keeps its one tag; whether it is a type, a value, a macro or a generic parameter is asked of that node (`nameUseGroup`, `nameUseNames`), never stamped on the use. Two things are stamped on it: `FlagQualified`, which says the name was reached through a namespace rather than written bare, and `FlagPattern`, which marks a pattern's bare root until type check binds it against the matched value ("A variant bare in a pattern", below).

A path is parsed as a chain of member accesses and collapsed during name resolution, hop by hop, by `fnCallNameResPath` — [fncall](../../compiler/c/doc/nodes/fncall.md), "The path collapse", has the mechanism and the reason it cannot wait for type check.

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

The matched value's type exists only at type check, so the lookup is split. The parser marks the root name (`FlagPattern`, `castPatternMark`); name resolution binds it lexically where it can and, where it cannot, leaves it unbound without reporting it; `castPatternBind` binds it against the matched value's enum before the pattern's type is checked, or reports it. ▸ **One consequence:** an unknown pattern name in a generic function's body is reported per instance, like any type error there, and not at all if the function is never instantiated. [cast](../../compiler/c/doc/nodes/cast.md) has the mechanism.

## Visibility

A declaration is private to the namespace that owns it unless it is written `pub`:

- A name declared in a module is private to that module unless declared `pub`.
- A type member is private to its type unless declared `pub`. A variant declared inside an enum is as visible as the enum, and may be declared `pub` itself.
- **The enum is the privacy boundary for its variants** — Jon, 23 Sep 2026. Code written anywhere inside an enum's braces — the enum's methods (each variant's clone of one included), its static functions, and every variant's own methods — reaches the private members of the enum and of every variant through any value, not only through `self`: it reads and writes their fields, calls their methods, and gives their private fields values in a literal. An enum that extends another is inside the base's boundary: its added variants' code and its copies' cloned methods reach the privates of its own variants, copies included, and of the base's, down a chain. Nothing else widens: the base does not reach what an extension adds, sibling extensions do not reach each other's variants, a struct's private members are still reached only through `self`, and a function the module owns — a free function, an anonymous one written inside the enum, a generic function instantiated from inside it — is outside. It is the privacy half of the names rule (everything the enum declares is bare inside its braces), and keeps `pub` meaning "part of the interface": before it, the only way for an enum's own code to read a variant's field was to make it public to the whole program.
- `pub` has one meaning wherever it appears: this entry is visible from outside the namespace that owns it. A local declaration has no outside to be visible from, so `pub` on one is `ErrorBadPub`. `pub` before `import` is re-export: every binding the import makes is a public name here. And `pub` always comes first, before the keyword of whatever it makes public — Jon, 23 Sep 2026: *"Let's make it consistent just for people to know what to expect."* So a fold says it as every declaration does, `pub use`: a module's standalone `use`, `pub use Colors;` or `pub use scaling;`; an import's clause, `import m pub use a, b;`; a global's clause, `… pub use *;`. The fold's older spelling, `use pub`, is `ErrorBadPub` at each of the three, naming the one to write.
- A name's spelling says nothing about its visibility. A leading underscore is a character like any other.

The compiler enforces this on the routes that can reach a private name: `fnCallNameResPath` reports `ErrorNotPublic` for a private declaration reached by a path from outside its module, `importNameRes` skips private nodes when folding, `fnCallLowerMethod` refuses a private member on a receiver that is not `self`, and `typeLitStructReorder` refuses a value for a private field outside the type's methods; both grant what the enum boundary grants by asking `structEnumSeesPrivate`, which looks for the enum the receiver's type belongs to on the `extends` chain of the enum that owns the function being checked — the owner of `pstate->fn`, not `pstate->typenode`, which a generic function's instance inherits from wherever it was first called. The parser sets `FlagPub` on whatever declaration the keyword precedes; where a declaration joins its namespace (`dclInfoJoin`) the flag is read once into its `DclPrivate` bit, and every check after that asks `inodeIsPrivate`, which answers from the bit for a declaration that carries `DclInfo` and from the flag for a node that carries none — a field, a const, a macro, a typedef, an overload name. Generation reads the same bit — see "Symbols".

An overload name's visibility is its candidates': the first candidate declares it, and every later candidate must agree (`ErrorPrivOverload`, either way round). A compiler-defined intrinsic candidate counts as `pub` for the name and is exempt from agreeing, which is how the core types keep a private `_neg` behind a pub `-`. One consequence is deliberate and worth knowing: **visibility is checked on the binding the caller's name reaches**, not on the candidate overload selection then picks, which is why a pub overload name may not hold a private concrete candidate — through the pub name the private one would be reachable.

Visibility should belong to the original definition or declaration, while access is evaluated from the use site. A folded or renamed NameDef must not make a private definition public merely by changing its local spelling, and it does not: the alias an enrichment makes carries its target's visibility, which is what lets a private member of the base come across and stay out of the enrichment's clients' reach. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it.

**The other half of that is the binding having a visibility of its own to widen with.** A fold into a *module* is private to the module that made it unless it says `pub` — `pub use` on a global's clause, on a standalone `use` and on an import's clause; or `pub import`, which reaches the module's own binding and every name the import folds alike — whatever the name's visibility where it was declared. The two spellings on an import do not overlap, so they cannot disagree: `import m pub use *` publishes the folds and leaves `m` private, `pub import m use *` publishes both, and `pub import m pub use *` says what `pub import` says alone. One keyword, one meaning, on a binding as on a declaration. It never widens past the origin: only a public name folds at all. `fnCallNameResPath` is the route that reads it, since `inodeIsPrivate` answers from the alias's own `FlagPub`; a `pub` fold is reachable as `mod.name` and a plain one is not.

## Import and name folding

Plain `import math` binds the imported module under the name it declares for itself, and public members are reached as `math.name`. Where `math`'s own `mod` line names a default fold, the plain import folds those names too ("A module's default fold", below).

**An `import` is written in its file's header** [Jon 24 Sep]: right after the file's `mod` line, with the file's other imports and ahead of every other declaration, so a module's whole inbound dependency list sits at its own declaration. An `import` after any other declaration — a standalone `use` included — is `ErrorImportLate`, however the import is spelled; it is still made, so nothing else follows from the refusal. A file the folder swept into its module has no `mod` line, and its imports are what it opens with. **There is no placement rule for `use`**: as a clause it is on a declaration, so it lives wherever that declaration lives, and each site already says where that is; as a statement it is in a type body or at module scope, where a module's folds do not depend on the order they were written in.

**An import folds with a `use` clause**, the one a global carries, with the imported module as the source:

- `import math3d use Point3 as Point, Vector3;` — selected names, each under its local spelling.
- `import math3d use * but Matrix;` — every public name but some.
- `import math3d use { Point3 as Point, Vector3 };` — a block, for a long list.
- `import math3d pub use *;` — the folds re-exported: public names of this module, carried on by a wildcard import of it, while `math3d` itself stays private here.
- `import math3d use *;` — every public name.

The clause is the one spelling of a fold [Jon 23 Sep]. `import math3d.*;` and `import math3d.Point3;` are `ErrorBadTerm`, each message naming the clause to write.

Any category of name folds, subject to the importing namespace's single collision domain. What the clause may name and admit:

- **Only a public binding of the source folds**, whether the source declared it or folded it in with `pub`. A private one named in a list is `ErrorNotPublic`; a star clause passes it over, so one named after `but` is `ErrorNotPublic` too — seen from outside a module a private name behaves as a missing one does, and there is nothing to leave out. (A module's `extends` is the one fold that takes private names too: "A module extending a module", below.)
- A name the source has not got, listed or after `but`, is `ErrorNoMbr`. The source's own name is the one the import binds already, so a list naming it is `ErrorBadFold` and a star clause passes it over.
- A folded name must be unique in the module, whatever brought the other one: a declaration, another import's fold, a global's, a standalone `use`. `ErrorDupName`, reported at the listed name, or at the `use` of a star clause; `as` or `but` settles it. **A name the module writes twice is refused even for the same declaration; a name it never wrote is not** — see "One declaration by two routes" below: the same declaration arriving again by a wildcard, an `extends` or the core import is the binding the name has already.
- **Every name folds as an `AliasDclNode` in the receiving module's namespace**, whose target is the source's own binding and whose `FlagPub` is the import's.
- **It reads the source module's *namespace*, not its declarations**, so a name the source itself folded in and re-exported travels on.
- Imported modules are loaded once and reused, and a sister is not loaded at all.

**A module imports another once.** A second import of the same module is `ErrorDupImport`, reported at the second and naming where the first is. One that differs would leave the module's name and its folds meaning two things. An identical repeat — the same clause, its names in any order, with the same `pub` on each binding — was once ignored; it is refused too, because "if you've got two different ways of bringing in the same thing, that should be an error… it's a cleanliness issue" [Jon 23 Sep]. `importSame` now decides only what the message says. This is the written-twice rule below met at parse, where it has to be decided because an import binds the module's own name before any fold runs.

**One declaration by two routes** [Jon 23 Sep]. What decides whether two bindings of one name collide is whether the module's own source **wrote** both. Every fold that binds into a module's namespace — an import's clause, star or listed, a module's `extends`, a global's clause, a standalone `use` — binds through `modFoldBind`:

- **Written twice is an error, even for the same declaration.** A binding is written unless a star clause made it: a declaration, a `typedef`, the name an import binds to its module, a listed item of a clause, and every variant an enum's `use Colors;` binds (it names the enum, so it is written though it lists nothing). So `import lib use one, one`, two `use Colors;` (one `ErrorDupName` per variant), `import meter; import relay use meter;` with `relay` re-exporting `meter`, `import a use x` beside `import c use x`, and a global's `use area, area` are each `ErrorDupName` at the second, saying it is the same thing twice (`modFoldDupReport`). An earlier form of this rule made every repeat of one declaration one binding, these included; Jon revised it, and accepted the split below: "I'm okay with that compromise."
- **A name the module never wrote merges.** What a wildcard `use *`, an `extends` or the implicit core import brought carries `FlagUnlisted` (`importFoldStar`, and `foldStarItems` for a global's `use *`), and the same declaration arriving under a name where either binding carries it is the binding the name has already. **One side unwritten is enough**: a listed name meeting a wildcard's arrival of the same declaration is one binding, in either order, since the name was not written twice. Where the new binding was written, the one kept counts as written from then on, so a third that writes the name again is refused whichever order the three folded in.
- **The same** is the declaration at the end of each binding's chain of fold aliases, or the binding itself where it is a declaration, and the same global where either is reached through one. A `typedef` is a declaration of its own and ends the chain, since its target is resolved only after every fold. One member folded through two different globals is two things, and collides.
- **Where one route is public and the other private, the binding is public**, so a re-export is not lost to whichever route happened to fold first. A declaration of the module keeps its own visibility: nothing folds a private name of it back as a public one.
- **Different declarations under one name collide everywhere** — `ErrorDupName` at the fold, or `ErrorExtendsOverride` where a module's `extends` meets its own declaration.
- **Neither the outcome nor the visibility depends on fold order.** Which binding is kept depends on which route came first; what the name means, whether it is public, and whether it is refused, do not.

That is what lets a diamond compile — `d` doing `import a use *; import b use *;` where `a` and `b` each re-export `c`'s `x` — what lets the core fold an extending module takes from its base meet its own, and what lets `use Option;` stand beside the core import's `Some` and `None`. `module_fold_diamond` runs the diamond, a listed name beside a wildcard in both orders, `use Option;`, and the public route winning in both orders; `module_fold_written_nameres` pins what is written twice, and `module_fold_diamond_nameres` what collides however it arrived.

**A type's namespace does not follow this rule**, and nothing yet asks it to. `structFoldItem` binds a field's fold with its own `namespaceAdd`, and a folded *field* is a copy carrying a hop, so two routes to one field are two nodes and cannot be compared by identity. Two routes to one method through two fields are two receivers, a genuine collision. The only same-route repeat a type can write is a listed clause naming one member twice, `b Box use area, area`, which is still `ErrorDupName`.

**Transit is not a rule of its own.** A fold is private to the module that made it unless the import says `pub` or `pub use`, which is the visibility rule; so what a third module sees through this one is what this one re-exported, and it is the same answer whatever order the files were loaded in. **Every module's folds run before any module's body is name resolved**, dependency-first, which is what removed the load order from the question — see [module](../../compiler/c/doc/nodes/module.md), "Name resolution".

The intended NameDef behavior is:

1. Folding or renaming creates a new NameDef owned by the receiving namespace.
2. The new NameDef may have a different local spelling.
3. It points to the same underlying IR value as the original definition.
4. It retains a link to the original definition or origin for identity, visibility, diagnostics, and generated naming.
5. Collision checks use the receiving namespace's complete name domain, regardless of the imported declaration's category.

Thus import aliasing duplicates a binding, not the underlying type, function, module, or other IR value.

### Folding into a type

A struct's field may carry a `use` clause folding members of the field's type in as names of the struct — delegated inheritance, [refinherit](../reference/refinherit.html). It is built, and it is the same operation as the module fold above at the namespace: one collision domain, an error at the fold for a name already taken, renaming with `as`, exclusion with `but`, visibility transitive so only what the field's type shows through a `pub` field folds. The clause is expanded into the struct's dictionary while the struct is name resolved, before any of its method bodies, so a folded name is usable bare inside the type ([struct](../../compiler/c/doc/nodes/struct.md), "Name folding").

**Where the two folds differ, measured by building this one: in what the binding holds.** Both make an alias; a type fold's alias is reached through a value, so a use of the name must be lowered to an access path or to a call whose receiver is shifted to the field, and a folded *field* is a copy carrying a hop rather than an alias at all. A module fold reaches through nothing — a module has one instance — so its alias never calls the receiver rewrite. The alias node is the binding record this note asks for: a local spelling, a visibility bit of its own, a target.

### Folding a whole type in: `extends`, and a sibling of it: `use`

A concrete type may be named as another's base, with `extends`, and everything it has becomes a name of the enriching type, which adds methods and no fields ([refinherit](../reference/refinherit.html); [struct](../../compiler/c/doc/nodes/struct.md), "Enrichment"). And a `use` standing as a statement in a type's body folds in a **sibling** — another type that declared this type's base — selectively, with `as` and `but` ([struct](../../compiler/c/doc/nodes/struct.md), "Sibling folding"). A module names the module it extends the same way. So one operation attaches at eight sites:

| Where the clause sits | What the receiver is | What the binding holds |
| --- | --- | --- |
| a **module** body — `import mod use *` | nothing; a module has one instance | an alias targeting the source's own binding, under the clause's spelling, with the import's visibility |
| a **module** declaration — `mod solids extends shapes` | nothing; a module has one instance | an alias targeting the base's own binding, for every declaration and fold of the base (not the names its imports bind), as visible here as there |
| a **module** body — `use Colors` | nothing; a variant is a type, reached through no value | an alias targeting the variant, with the statement's visibility |
| a **module** body — `use scaling` | nothing; a module has one instance | an alias targeting the submodule's own binding, with the statement's visibility — an import's fold exactly |
| a **type** body — `extends Meter` | the **whole**, which is already the right type | the base's declaration, under an alias; the base's fields as copies of its own, and its `final` and `clone` as clones of its own with `Self` retyped |
| a **type** body — `use Trig` | a **sibling of the whole**, which this type's values substitute for | the sibling's declaration, under an alias; nothing else |
| a **field** — `engine Engine use *` | the **part**, reached through the field | a copy carrying a hop, or an alias whose call shifts its receiver |
| a **module's global** — `config Config use *` | the **one instance**, at a fixed address | an alias carrying the global, field and method alike |

**The two type-body rows are the finding.** The two hard things a type adds to the module case are dispatch and per-instance state, and neither of them has either to solve. An enrichment may not change the fields, so its values and its base's have one representation and a base method already takes exactly the right receiver. A sibling declared that same base, so its method's receiver is a type this type's values substitute for, and the recast at the call is the whole adjustment — no copy, no hop, no thunk. So the machinery recurs where it is easy and the hard part stays in the field row, where the receiver has to be found and shifted.

**Which site a clause sits at decides what the collision domain is for, too.** A sibling fold is the one place where a collision is the *point*: two libraries that each enriched one base may both declare `span`, neither is wrong, and the type folding both settles it with `as` or `but` in one declaration that touches neither source. Everywhere else a collision is a mistake.

**A type-body `use` is never a consumer-side import.** Inside a type the word is always producer-side composition — this type is built out of that one — and ordinary lookup inside a type's bodies comes from the enclosing module, which is what `import` fed. The same word one scope out means the consumer-side thing — a module's standalone `use`, which asks for an enum's variants or a submodule's names as the module's own — and the two do not meet: a type body has no imports of its own.

**Visibility is the target's, and this is where an alias narrowing it would matter.** An enrichment is inside its base's encapsulation boundary, so a private member of the base comes across — under an alias that is private here too, so the enrichment's own methods reach it and the enrichment's clients do not. That answers "may a fold make a private definition public" with no: the alias carries the target's visibility rather than its own opinion. Whether an alias may deliberately *narrow* visibility is still open, and nothing built asks it yet.

**A sibling is inside the base's boundary too, and that is not the same as being inside the sibling's.** Both declared the same base, so both read its private members; what the sibling declares privately for itself is its own, and only what it shows folds. The asymmetry is the part-versus-whole cut again: towards the base each is the whole, and towards each other each is a stranger.

**Static folding aliases and keeps the original owner; dynamic folding replicates into each instance.** One-instance members — a static variable, a static function, a macro without `self` — are a new name for one thing, so two types folding one static share one storage location and one symbol. A field, and a method with a receiver, land in each instance, and nothing reaches back to whoever declared it. Which is why an `extends` base's fields are *absorbed* as the enriching type's own while a sibling's statics stay their owner's. **No type reads another type's fields, ever**: a method with a receiver reads the fields of the type it was written against, inside the one instance being operated on.

**A static therefore folds on a TYPE and never through a FIELD.** There is no such thing as a static field: a static is a one-instance thing and a field is the per-instance route, so a static has no business arriving through a value. A type's statics are had by writing a type-level clause — `extends`, or a sibling `use` — and `structFoldItem` reports `ErrorBadFold` for one named in a field's clause. **A global is the value route at module scale, so the same refusal applies to it**, with the same reason and the same code (`foldGlobalItem`): the way to have a type's static is to name it through the type.

### Folding through a global

A module's global may carry the same `use` clause a field does, and it folds members of the global's type in as names of the module: `config Config use *` makes `Config`'s members names here, reached through `config`. The clause is the field's exactly — `*`, a list, `as`, `but`, and a block form for a long list — with one addition a field's and a sibling's have no answer for, `pub use`, which an import's clause shares.

**It is the one-instance analogue of a field, and that is what makes it the cheap one.** A field's fold has to find the receiver at the call and shift it (`structFoldReceiver`), copy each folded field so the access path can be rebuilt, and emit a thunk where a folded method fills a vtable slot. A global has exactly one instance at an address known at compile time, so there is nothing to find: every entry is an **alias** carrying the global it is reached through, field and method alike, and a use of the name is **lowered to `global.name`** — `nameUseTypeCheck` for a member read, `fnCallTypeCheck` for a call, before it reads the callee. From that point the node is the path the author could have written by hand, so overload selection, the macro-method probe, borrowing, the receiver adjustments and generation see nothing new and learn nothing about folding. A chain works for the same reason: where the global's own type folded a field in, the member access resolves through that type's aliases and its receiver shift runs from the global.

**What it buys that folding a module cannot is composing a module from a STRUCT** — a module presenting a singleton's interface as its own names, which no module-to-module fold expresses.

**Expanded before the module's other nodes are resolved** (`modNameRes`), because a module's names do not depend on the order they were written in: a function declared above the global still names what the global folded. A folded name is entered in the module's namespace and hooked, so the collision domain is the module's one domain and `ErrorDupName` is reported at the fold — unless the name is bound already to the same member through the same global and one of the two came by a wildcard, which is one binding ("One declaration by two routes"). A clause listing one member twice writes it twice, and is refused; through another global it is another thing, and collides.

What a clause may fold from and what it may admit:

- The source is a **struct**. A global whose type is anything else, and one whose type is a trait or an enum, is `ErrorUseGlobal` at the clause — an abstraction has no members of its own for a value to reach.
- **Visibility is transitive**: only what the global's type shows folds, so a private member named in a list is `ErrorNotPublic` and a wildcard passes it over — which makes one named after `but` `ErrorNotPublic` as well, since there is nothing to leave out.
- A **static** does not fold, for the reason above; nor do `final` and `clone`, which belong to the type's own values' lifecycle.
- A name the type has not got, listed or named after `but`, is `ErrorNoMbr`.

**`pub use` is the binding's own visibility, and this is the site that asks for it.** A fold is private to the module that made it unless the clause says `pub`; the member is public in its type either way, and what `pub` decides is whether the *module* shows the name it gave it. A `pub` fold is reached from outside through the global, so the global must be public too — `ErrorNotPublic` otherwise, naming the global. An import's clause has the same answer, since a module's fold is a module's binding like this one. At a type's sites `pub` on a clause, before its `use` or after it, is `ErrorBadPub`: a folded member of a field's type is as visible as the field it is reached through, and a sibling fold declares no name of its own.

**A `pub` fold is reachable as `mod.name`, and a wildcard import carries it on**, because `importNameRes` reads the source module's namespace, which is where a fold's bindings live. **Which module may see a fold does not depend on load order**: every module's folds run before any module's body is resolved. `module_nameres_fold_visibility` pins the first; `module_transit` against `module_nameres_transit` pins the second, from three positions in the load order.

#### Imports form a DAG

**Modules need an explicit DAG order, at every scale** [Jon 23 Sep: *"Why would we ever want circular imports. Modules need to have an explicit dag order."*]: sister modules inside a package as well as packages. A module depends on each module it imports, on the module holding a name it imports, on the module it extends, and **on each of its own submodules**: a program is a hierarchical decomposition, so a part cannot lean on the whole it is part of, and a child importing a name of its parent is a loop of two. What they share moves into a sister both import. Once what `extends` names is known, one walk puts the modules in dependency order (`pgmModuleOrder`), and each loop is `ErrorImportLoop`, reported at the edge that closes it and naming the modules round it — `Import loop between modules: q -> q.inner -> q ...`, each step said. **The order is the one module `init`s run in**, and finalizers in its reverse: the program's stitched init and final read it (`genlStitch`, [module](../../compiler/c/doc/nodes/module.md), "Init and final"). Congo refuses the same loops first, from its header scan, between packages and between the modules of a package, with a message of the same shape; the compiler's check is the backstop for a direct `conec` run. `module_import_cycle`, `module_import_parent`, `module_extends_back` and `module_cycle` pin the refusal. ⚠ A module's `is` naming a trait its parent declares reaches up without an import, and is counted as no edge: the ruled edges are imports, `extends` and containment.

#### The fold passes

The folds are dependency-first, so a module's folds are complete before anything folds from it. A module's own folds run in a fixed order, though — `extends`, the imports, the globals, the standalone `use`s — so a global whose type a later `use` of its own module brings waits in the first pass, and a sister that wildcard-imported the module takes the global's re-export only in the next (`module_use_submodule`). So `modFoldAll` runs the fold pass over every module again, until a pass binds nothing new: a fixpoint, the way Rust resolves glob imports. It is cheap because what a star clause brings is never written, so its binding of a declaration the name already holds is that binding ("One declaration by two routes"): a pass re-binding what an earlier pass bound changes nothing, and "nothing new" — no name bound, no binding made public or made a fold — is the whole test. Only a pass that left a fold waiting, or met a module mid-fold, and made progress is followed by another; a program whose folds all find what they name takes one. Round a refused loop, where one module reads another mid-fold, the same passes carry each re-export round it, so the loop is what is reported and not the names it cut short.

Until the passes settle nothing that might arrive is reported: a listed name the source has not got, or has only privately, and a global's fold or an enum's `use` whose type or enum is named through a binding not there yet, wait. The pass after the last reports each fold still waiting, with the message it always had, so a `but`, an `as` and a listed name are judged against the source once it is complete, and a private name stays private unless a public route to the same declaration arrives, when the binding is public, whichever route came first. **A collision is reported once**, when it is met — two different declarations under one name is final, and so is the same declaration written twice, since a listed item, a global's clause and an enum's `use` are each made once, whatever pass makes them — and at the fold a single pass would have met it at: the one that comes second in the order a module's folds run (`extends`, the imports, the globals, the standalone `use`s, each as written), not the one that happened to arrive second. `module_import_cycle_nameres` pins what is still reported beside a refused loop, and the collision in both clause orders.

### Folding an enum's variants into a module

A variant is a name of its enum, so `Colors.Red` is its spelling everywhere, the module that declares the enum included, except in a pattern on a value of the enum, which names it bare ("A variant bare in a pattern", above). **`use Colors;` at module scope is how a module asks for the variants bare**: each variant it admits becomes a name of the module. The clause after the enum is a type body's sibling `use` exactly — every variant by default, a list with `as` (`use Colors Red, Green as Verde;`), a block (`use Colors { Red, Green };`), or every variant `but` some — and `*` is `ErrorBadFold`, since naming the enum already asked for them all.

**It is the module's third fold, and the cheapest.** A variant is a type, reached through no value, so the binding is the one an import's fold makes: an `AliasDclNode` whose target is the variant, with no receiver and a visibility of its own. `foldModUseExpand` runs in `modFoldNames` after the imports and the globals' clauses, so the enum may be named through anything they folded in, and before any body resolves, so the name is in place wherever it is used. The statement is held on a `ModUseNode` on the module's `moduses` list — not in its node walk, because it is neither a declaration nor a field ([module](../../compiler/c/doc/nodes/module.md)). Which of the two folds it makes is decided there, once the source resolves: an enum's, here, or a submodule's (below).

What it may fold from and what it admits:

- The source is an **enum declaration**, written as its name or a path through modules to it (`use shapes.Colors;`); a name an import folded in is followed to the declaration. A module is the other fold, below. Anything else — a struct, a trait, a function, a variant — is `ErrorUseEnum`, and so is an instance of a generic enum, `use Option[i32];`: a generic enum's variants are the enum's, whatever it is instantiated with, so the statement names the enum alone. **A `typedef` of an enum is refused too**, `ErrorUseEnum`, because a typedef's target is resolved with the module's other nodes, after every fold, so what it names is not known when the fold runs.
- An enum that extends another has its copies of the base's variants only once it is resolved, so the fold **demands it resolved first** (`structEnumDemandSet`), in its own module's scope, and then folds every variant it holds, copies and added alike. A path `RichColors.Red` demands it the same way (`fnCallNameResPath`), for a name its namespace does not have yet.
- It admits **variants and nothing else** of the enum: not its fields or methods, which are reached through a value of it, not its statics, not `Self`, not its equality operators. A listed name the enum has but that is not a variant is `ErrorBadFold`; one it has not got, listed or after `but`, is `ErrorNoMbr`.
- A folded name must be unique in the module, whatever brought the other one: a declaration, an import's fold, another enum's `use`. `ErrorDupName`, reported at the fold. Two enums may each have a `Quit`; a module folding both settles it with `as` or `but`. The same variant arriving again by a wildcard — an import that re-exports `Colors`'s variants, or the core import's `Some` beside a module's own `use Option;` — is the binding the name has already; written again — a second `use Colors;`, or a listed import of the variant — it is refused, once per variant ("One declaration by two routes") [Jon 23 Sep].
- **`pub use` is the binding's own visibility**, as on a global's clause: the fold is private to the module that made it unless the statement says `pub`, and a `pub` fold is reachable as `mod.Red` and carried on by a wildcard import. It never widens past the origin, so `pub use` of a private enum is `ErrorNotPublic`, reported at the `use`. `use pub Colors;` is `ErrorBadPub`, naming the spelling.

**Core folds its own two enums with `pub use`**, and that is the whole reason `Some`, `None`, `Ok` and `Error` are bare in every program: the automatic import of core is a wildcard, and a wildcard carries a public fold on. No prelude rule stands behind them, and `?T` still lowers to `Option[T]` by the enum's own name.

`enum_nameres_use` and `enum_parse_use` pin the refusals; `module_use_enum` runs a public fold reached by a path and through a wildcard import, and `module_use_enum_nameres` pins what a private one keeps out of reach.

### Folding a submodule's names into a module

**A standalone `use` names any namespace the module reaches without an import** [Jon 23 Sep]: an enum, above, or a **submodule** of the module — by its name, `use scaling;`, or by a path through submodules, `use geometry.scaling;`. A submodule is brought in by its folder and never imported (`ErrorModFile`), so an import's clause cannot fold it, and this is how a parent has its child's names bare. The clause after the module is the enum's — every public name by default, a list with `as`, a block, every name `but` some, `pub` first to re-export — and `*` is `ErrorBadFold` there too, since naming the module already asked for everything.

**It is an import's clause, folded by the import's own code.** `foldModUseModule` makes an `ImportNode` marked `isuse` over the submodule, sharing the statement's `FoldClause`, and holds it on the `ModUseNode` rather than on the module's `imports`: it binds no name of its own, since the submodule is a name of the module already. From there `importNameRes` does what it does for any import, in every fold pass, after `modFoldNames` has run on the submodule, so what the submodule re-exports is there to fold. So every rule of an import's clause holds as written: only a public binding folds, a private one listed or after `but` is `ErrorNotPublic` and a star clause passes it over; a name the submodule has not got, listed or after `but`, is `ErrorNoMbr`; each name is an `AliasDclNode` whose target is the submodule's own binding, private to the module unless the statement says `pub use`; what the star brings is unwritten and merges ("One declaration by two routes"); and a collision is `ErrorDupName`, reported where a single pass would have met it. A `pub` fold of a submodule is the facade: its names become public names of the parent, reached by path and imported by a sister, while the submodule itself may stay private.

What it refuses, once the source resolves to a module:

- **A module the module reaches through an import**, named by the import's binding or by a path through it: `ErrorUseImported`, pointing the writer at `import m use ...`. An imported module's names are folded by its import's own clause, and one situation has one spelling. A step bound to an alias — the name an import binds, or a name a fold brought in — is a step through an import; a submodule is bound in its parent as the module itself.
- **The module itself, a module containing it, or one beside it**: `ErrorModReach`. Only a module inside this one is reached without an import.
- **A second `use` of one submodule**: `ErrorDupImport`, naming the first, as a second import of one module is. A module folds a submodule once, and writes what it wants in one clause.

`module_use_submodule` runs the whole clause, `but`, a block with `as`, a path one level deeper and a `pub use` re-export seen by path and by a sister's import; `module_use_submodule_nameres` pins the refusals and what a private fold keeps out of reach.

### A module extending a module

`mod solids extends shapes;` — the module's declaration, as its designated file's first statement — makes `solids` a module that **reuses** `shapes`: every declaration of `shapes`, and every name `shapes`'s `use` clauses folded in, becomes a name of `solids`, and `solids` adds its own declarations beside them. What `shapes`'s imports bind to their modules does not come across [Jon 23 Sep].

**For a module, extending and inheriting are one thing**, by the static/dynamic rule above. Static folding aliases and keeps the original owner; a module's state is all static — one instance, at a fixed address — so there is no per-instance copy for inheritance to make, and what `extends` makes is an `AliasDclNode` per name. The declaration, its symbol and its state stay `shapes`'s: a global changed through `solids.count` is the one `shapes.count` reads, and nothing is spelled after `solids` but what `solids` declares.

**It is an import's star fold with four differences**, and it is built as one — an `ImportNode` marked `isextends`, held on the module's `extends` slot rather than its `imports`, expanded by `importNameRes` in `modFoldNames`, first of the module's folds and dependency-first:

- **Every declaration and fold comes across, private ones included, and each alias is as visible in `solids` as it is in `shapes`** [Jon 23 Sep]. `solids` is inside `shapes`'s boundary, as an enriching type is inside its base's, so `solids`'s code reads `shapes`'s private names, and one global reached through either name is one global. What a module extends is part of its own surface, so a public name of `shapes` is public in `solids`, and an importer of `solids` reaches it through it — by path, `solids.area`, and through a wildcard import — exactly as it reaches what `solids` declares. A private name of `shapes` stays private in `solids`, so an importer sees `shapes`'s public surface and nothing more: named by path or in a listed clause it is `ErrorNotPublic`, as any private name of `solids` is, and a wildcard passes it over. A chain transits: what `shapes` took from a module it extends is a name of `shapes`, with the visibility it had there, so it reaches `solids` too.
- **It binds no name of its own.** `shapes` is not a name of `solids` unless an import binds it.
- **A name the base has may not be redeclared**, public or private. A declaration of `solids` spelled like a name of `shapes` — a submodule's name included — is `ErrorExtendsOverride`, reported at the declaration: the type rule, since one name would otherwise mean two things depending on which module reached it, and like the type rule it refuses a candidate joining the base's overload name too. Another fold of `solids`'s colliding with a different declaration under one of the base's names is `ErrorDupName`, reported at that fold; the same declaration reached again is one binding, since what `extends` brings the module never wrote ("One declaration by two routes", above).

- **The names `shapes`'s imports bind to their modules do not come across** [Jon 23 Sep]. A module's imports are its dependencies, not its contents, and scoped imports exist so that each module states its own. `shapes`'s `import c` binds `c` for `shapes` alone: `c.x()` in `solids` is `ErrorUnkName` until `solids` writes `import c` itself, and `shapes`'s `pub import c` does not put `c` in `solids`'s surface either. A declaration of `solids` named `c` is no collision.

**What `shapes`'s `use` clauses fold in is a part of `shapes`, and does come across**, each fold as visible as it is there: a fold on an import's clause, a global's or an enum's. `shapes`'s `import c use x` makes `x` a name of `solids` (a declaration of `solids` named `x` is `ErrorExtendsOverride`) while `c` stays `shapes`'s. Its private folds — the automatic wildcard import of core among them, which is a fold alone and binds no module name — arrive beside `solids`'s own, the same declarations, which neither module wrote, and so one binding each. The binding an import makes of its module's name carries `FlagImportName`, and `importStarAdmits` leaves it out of an `extends`; where a wildcard of `shapes` brought the same module in under that name as well (`import c` beside `import relay use *`, `relay` re-exporting `c`), `modFoldBind` clears the flag, because the name is then a fold of `shapes` too, and it comes across. Listing it instead (`import relay use c`) writes `c` twice, and is `ErrorDupName` in `shapes` [Jon 23 Sep]. `importStarAdmits` admits the private names to an `extends`, and `importFoldItem` takes each alias's `FlagPub` from the base's binding rather than from a clause.

**What `extends` may name** is resolved in a pass of its own, `modExtendsResolve`, run for every module before any fold (`pgmNameRes`), so every edge is known before the first fold follows one. It names a module the module can **already reach** — its own namespace, where an import bound the module's name, and then the registry its parent is, which holds its sisters — looked up, never loaded, and by one name. Refused:

- a name that reaches nothing, a cousin two levels over among them — `ErrorUnkName`;
- a name that is not a module — `ErrorModExtends`; a **trait**, of a struct or of a module, is refused with its own message, since conforming to a trait is a different reading from reuse and is spelled `is`: `mod arena is Region`;
- the module itself, and a module it contains — `ErrorModExtends`; its parent — `ErrorModReach`, as for an import;
- a chain of `extends` that comes back to where it started — a loop like any other, `ErrorImportLoop`, once, at the `extends` that closes it, and cut there; `extends` is a dependency, so a module importing the module that extends it is a loop too ("Imports form a DAG");
- as it is parsed, more than one module (`ErrorExtends`), a path (`ErrorModExtends`) and no name (`ErrorNoName`).

`module_extends` runs a chain of two and both importers, both bases' private names and the base's private fold reached from inside, and the extending module importing for itself a module the base imports; `module_extends_back` refuses a module importing the module that extends it, in both orders; `module_extends_import` extends a module an import reached by its file; `module_extends_nameres` and `module_extends_parse` pin the refusals.

### A module's default fold

A package named for the one thing it provides would put that name in every path twice, `bigint.BigInt`. **The module's own `mod` line settles it** [Jon 23 Sep]: `mod bigint use BigInt;` names what a **bare** `import bigint;` folds, so the importer has `BigInt` directly — "just become the thing". With `extends`, the clause comes last: `mod bigint extends base use BigInt;`.

- **The list is the default whatever it holds.** One name needs no special case, and a name added later only adds to what importers see. `*` and `* but` are admitted, as on an import.
- **An importer's own clause replaces the default whole**: `import bigint use *`, `import bigint use Other`, `import bigint use * but BigInt`. The default is what an import that writes no clause gets, and nothing else.
- **The module's name stays bound.** A bare import binds `bigint` as it always did, and the default is folded on top, so `bigint.helper` still reaches what the default leaves out.
- ⚠ **On the `mod` line, `use` names what IMPORTERS fold.** That is the opposite direction from every other `use`, which folds into the namespace it is written in.
- ⚠ **It is not an export list.** `pub` still decides what is reachable ("no header files, no export list", [module](../../compiler/c/doc/nodes/module.md), "Visibility"). The clause chooses among the module's public names, and does not decide which names are public.

What it may name is checked against the module's own namespace in the fold pass that reports, after the module's own folds, so a name the module re-exports counts (`modDefaultFoldCheck`). Every listed name, and every name after `but`, must be a **public** name of the module: one it has not got is `ErrorNoMbr` ("bigint has no name X to fold in"), a private one `ErrorNotPublic` ("X is private to bigint, so it cannot be a default fold"), the module's own name `ErrorBadFold`, and a name listed twice `ErrorDupName`. Each is reported once, at the `mod` line; an import taking the default passes the same names over rather than reporting them again. Two spellings nobody ruled on are refused as the line is parsed and the clause taken without them (`parseModDefaultFold`): `as` is `ErrorBadFold`, since what an importer calls a name is the importer's to say in its own clause, and `pub` — `pub use` or the retired `use pub` — is `ErrorBadPub`, since each import decides how visible its folds are. A clause written before `extends` is `ErrorBadFold`.

**The mechanism is the import's own.** The clause is parsed by `parseFoldClause` and held on the module (`deffold`). An import that wrote no clause, whose module has a default, is given a copy of it when its first fold pass runs (`importDefaultFold`, from `importNameRes`) and marked `isdefault`, and from there `importNameRes` folds it as it folds any clause. It is a copy because a clause's items are the bindings it makes, and each importer makes its own:

- **under the importer's visibility**, so `pub import bigint;` re-exports `BigInt` with `bigint`, as `pub import` makes every binding it makes public, and a wildcard import of that module carries both on;
- **positioned at the import**, so a default-folded name colliding with a name of the importer is the ordinary `ErrorDupName` at the import, whose advice — rename with `as`, leave out with `but` — now means writing a clause of its own;
- **unwritten by the importer** (`FlagUnlisted`), since the imported module wrote the names: the same declaration reaching the importer again by another route merges with it rather than colliding ("One declaration by two routes").

The default is honoured wherever an import names a module and writes no clause: a sister, a module bound through the parent's registry (`importBindName`), a file or a search-path module. It is **not** applied by a module's `extends`, which folds the base's whole namespace, nor by a standalone `use sub;` of a submodule, which folds every public name as it always has. A module with no `use` on its `mod` line is unchanged.

`module_mod_default` runs a single type, a default of two names, `extends` beside it, an importer's clause replacing it and a `pub import` carrying it on to a wildcard importer; `module_mod_default_nameres` pins what a default may not name, a default meeting a declaration, and what a replaced default leaves out of reach; `module_mod_default_parse` the spellings refused on the line.

### A module conforming to a module trait

A program plugs into a framework — a shell, a web server, a UI host — and the framework's side of that is an interface every program plugging in must meet [Jon 23 Sep]. **That interface is a module trait**, the module's abstraction, spelled `mod trait` because `trait` modifies the kind, as `struct trait` is a struct's. It works the way a struct trait does, with the differences a module's nature makes:

- **It declares functions and globals**, a module's members, rather than fields and methods. A function with a body and a global with an initialiser are **defaults**; one without is a **requirement**. Nothing else is a member: a type would make it a trait requiring types, which is not built.
- **It is a declaration of the module whose file writes it**, named, imported and folded like any declaration: a program names a host's trait by importing the host's module and folding the trait in, `import hosts use Runner`.
- **A module conforms by declaring it**, `mod prog is Runner;`, and only so: nothing is inferred from what a module happens to declare. One trait, by one name — a trait further away is imported first, as `extends` names one module in reach.
- **Conformance is checked where it is written.** Each member is met by what the module has under that name — its own declaration, or what its folds brought, `extends` among them, since conformance is a question of names and signatures — or, where it has nothing, by a copy of the member's default. A requirement with nothing to meet it is `ErrorModTraitMissing`, and what the module has without the member's shape — not a function where one is required, another signature, a global of another type or permission — is `ErrorModTraitMismatch`, each reported at the `is`.
- **A default is cloned into the module**, as a struct trait's default is cloned into the struct, and a module may override it by declaring its own. The copy is the module's own declaration: owned by it, spelled after it, generated with it. Its body, naming another member, names the module's — its own declaration, or another copy — which is what lets a default be glue around what the module supplies. A name that is not a member keeps what it meant where the trait was written.
- **A module is one instance, so conformance is static**: no vtable and no dispatch. A default global is storage of each module that takes it.
- **A path through the trait reaches nothing**: `Runner.run` is `ErrorAbstractMeth`, since the trait's members have no code or storage of their own.

**The copies are taken as the module's folds complete**, before anything folding from it reads it, so a module extending a conforming module takes its copies as the base's declarations — one storage, not a second — and an importer's `use *` folds them like any public name.

**Not built** [Jon 23 Sep]: the host traits themselves; the entry glue, a trait's default that the host calls as the program's entry and that runs `init` and `final` round the program's own; traits requiring types. A module whose declarations happen to match a trait without saying so does not conform, and that is not planned. A generic module conforms on its `mod` line like any module, and each instance has its own copies of the defaults, checked against the trait.

`module_trait` runs a trait's defaults taken whole, overridden by a function and by a global, a default global as each module's own storage, a default reaching a private helper of the trait's module, a module extending a conforming module and conforming too, with every `mod` line clause, and a default fold of a taken default; `module_trait_parse`, `module_trait_nameres` and `module_trait_typecheck` pin the refusals.

## Aliases

`AliasDclNode` (`ir/stmt/aliasdcl.c`) is the general binding: a local spelling and a target, with the `FlagPub` bit as its own visibility and everything else the target's. Chains resolve through `aliasDclResolve`; a use bound to one answers as its target (`nameUseGroup`), and every site that reads a namespace binding resolves it first. A chain of aliases is ordinary: a type that enriches one which folded a sibling in binds an alias to that alias.

It is made for:

- a folded method, overload set or macro method of a field's type; every member of an `extends` base but its fields, its `final` and its `clone`; and every member a sibling `use` admits — a static among them, which is the one case where an alias stands for something reached through the type rather than through a value, and where the `FlagMethFld` bit is therefore left off. The target is a member name use bound to the declaration.
- **every name a global's `use` clause folds in**, field and method alike, with `through` naming the global. `FlagPub` is the clause's own, from `pub use`, rather than the target's.
- **every variant a module's `use` of an enum folds in**, the target a name use bound to the variant. Neither flag the member aliases carry: `FlagPub` is the statement's own, from `pub use`, and there is no receiver.
- **every binding an `import` makes** — the imported module's own name, and each name its `use` clause admits, under the clause's spelling; and each name a standalone `use` of a submodule admits, made by the same code. The target is the source module's own binding rather than the declaration at the end of the chain, so the origin is kept; `through` is copied from the source's binding where that one is reached through a global, so a re-exported global fold lowers the same way from any module. `FlagPub` is the import's: on the module's own binding from `pub import`, on a fold from `pub import` or `pub use`.
- **a `typedef`**, whose target is a type expression rather than a member name — the one alias with something of its own to name resolve and type check, which `FlagTypeAlias` says. A typedef therefore has no node kind of its own: it is the binding record, proved to generalise by carrying the construct that motivated the word "alias" in the first place. Its `pub` is the bit on the binding, as `pub` on any declaration is.

**What making `typedef` an alias changed, visibly: an alias may now qualify what it names.** `Sample.make` walks `Reading`'s namespace where `typedef Sample Reading`, because a path's base is asked of the declaration at the end of the chain and an alias answers for its target. It used to be refused, since the collapse found a node that was neither a module nor a type and gave up.

**A chain that comes back to itself names no type**, and every reader walks a chain to its end, so `aliasDclCheckCycle` reports `ErrorCircular` and cuts it before anything follows one: two pointers at two speeds meet only inside a cycle, and the target then becomes `unknown` so the name still answers as a type. One diagnostic per ring, at whichever name the walk reached first.

The aspirational model generalizes aliases: a new NameDef may denote anything nameable. Alias chains should preserve each local binding for diagnostics and visibility while semantic operations can reach the final IR value. A type-valued alias remains structural; creating a distinct nominal type should use a separate construct.

Import folding is the alias above, and renaming on import is that alias with the local spelling an `as` in the import's `use` clause writes. Other aliases may bind expressions or declarations directly. The exact syntax and compile-time restrictions for general aliases remain open.

`typedef` is the alias, so "current `typedef` creates a module-scoped structural alias" is now literally what the IR holds rather than a description of a separate node. What remains of the aspiration is the range of things a target may be, not the record.

## Generics and macros

Generic and macro syntax exists in the current compiler, but the website documentation labels much of this area incomplete or future-facing. Their namespace structure is defined above; specialization, expansion hygiene, and code-generation ownership remain separate implementation concerns.

### A generic module

**A generic module is named as a generic type is** [Jon 23 Sep]: its name is a declaration of its parent's namespace like any module's, and it has no members of its own to reach — each **instance** has them. `mod stack[T];` declares it; `stack[i64]` names the instance at `i64`, made where it is first named and the same instance wherever the same arguments are written, so `stack[i64]` in two modules is one namespace with one set of globals. A member is reached by a path through the instance, `stack[i64].push(3i64)`, bound at type check, where the instance exists; a private member is `ErrorNotPublic` from outside the module, as through any module. An import of a generic module binds its name, and an instance is written through that name.

What names the generic without arguments where a member is wanted is `ErrorGenModBare`: a path, `stack.push`; a standalone `use stack;`; an import's `use` clause; `extends stack`; and a default fold on its own `mod` line — the generic has nothing of its own to reach or fold. **Inside its own body its bare name is the instance being defined**, as `Box` is inside `struct Box[T]`, so `stack.count` there is the instance's own global, and `stack[T]` the same instance.

Its type parameters are names of the generic's own scope, hooked over its declarations while it is resolved, and a type alias of one — `typedef Item T` — names the argument in each instance. [module](../../compiler/c/doc/nodes/module.md), "Generic modules", has the mechanism and what a generic module may not yet hold.

## Symbols

The linker has one flat namespace and the language has many. This section is
the rule for crossing that boundary: what a declaration records about where it
lives, how its symbol is spelled from that, and what linkage the symbol gets.
Generation only lowers it — `nameSymbol` spells, `genlLinkage` links — and
spells no name of its own; [Generation](../../compiler/c/doc/phases/generation.md), "Symbols, linkage and
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
stored as a string — eight bits, and `cname`, the string a `@c("...")` stated:
on a module the prefix its C names carry, on a function its whole symbol.

| Bit | Meaning | Written from |
| --- | --- | --- |
| `DclPrivate` | visible only within its owner | the absence of `pub`, once |
| `DclExternal` | externally supplied: this compile emits no definition. It says nothing about the name | `extern` |
| `DclCName` | C naming: no owner path, never mangled (S5) | on a module, its `mod` line's `@c`; on a function, its own `@c`, or a C-named module that owns it directly (a type's methods and a generic function are not named by the module); on a global, the C-named module that owns it |
| `DclSystemCC` | system calling convention | `@c(system)`, on the function or on its module |
| `DclNamesChain` | module only: contributes its name to the owner chain | set on every loaded module, and on the root only where a build description says `output: library` |
| `DclExpandReached` | a function, global or type named by a body an importer expands — an `inline`, generic or macro body, a trait's default, a generic type's method | name resolution, where the body names it (`nameUseMarkExpandReached`); read only by a library compile, which exports it (L5) |
| `DclInitPure` | function only: `@initpure`, one a module's `init` may call; recorded, not yet checked | the parser, from `fn @initpure`; kept when the declaration joins its owner |
| `DclLifecycle` | function only: a module's `init`, its `final`, or the `drop` it is given — what the program's stitched init and final call | type check (`modLifecycle`); read only by a library compile, which exports it whatever its visibility |

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
is what lets an import loop back to it find it, and be refused naming it — and contributes nothing to any chain,
except a library root that a build description names: a package built on its
own is imported by that name, so its root is spelled as its importers spell it,
`q.addOne` ([module](../../compiler/c/doc/nodes/module.md), "A described build").

**What the node stores, and what generation derives.** The author writes
visibility; linkage is the compiler's to derive.

| Stored on the node | Derived at generation |
| --- | --- |
| owner chain | linkage: internal or external |
| declared name | mergeable or unique |
| visibility bit | whether a library compile exports it (L1); no export-table visibility is set in any compile |
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
  chain at the first named owner. The exception is a library's root that a
  build description names, which is spelled as the top module it is to its
  importers.
- **S4.** Each owner in the chain is spelled the way its own path would be.
  The only owners carrying more than their identifier are instances — of a
  generic type, and of a generic module — whose component carries the type
  arguments, so `fn tally(self) i64` is told apart across `Holder[i64]` and
  `Holder[f64]`, and a generic module's global `count` across `stack[i64]` and
  `stack[f64]`.
- **S5. C FFI names.** Naming belongs to the module, and `extern` has no say in
  it: `extern` means only "defined elsewhere", so an `extern` declaration in a
  Cone-named module is spelled with the module's Cone path like any other —
  which is how an include file reaches a package's symbols [Jon 23 Sep]. A
  module is C-named by `@c` after `mod` (`mod @c("SDL_") sdl;`): the functions
  and globals it owns directly take no owner path, are never mangled and carry
  no suffix, and a string is a literal prefix prepended to each, written by the
  author and never derived. A type's methods and a generic function in a
  C-named module keep Cone names, since a C name has no room for an owner or
  type arguments. One function is C-named by the same marker after `fn`
  (`fn @c("main") start`): bare, it is the declared name; with a string, the
  string is the whole symbol and no prefix is added, which is the per-name
  override in a C-named module. A bare `@c` on a function its module already
  C-names is `ErrorCNameTwice`. `@c(system)` adds the system calling
  convention. The marker affects the symbol only; resolution is through the
  ordinary module, so a caller writes `sdl.Init`. Inbound (a C library's
  symbols) and outbound (a Cone body published to C, `pub fn @c(...)`) are one
  mechanism in two directions.
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

**The bare rule** is `nameSymbol`'s: a C name is what `@c` states — the
function's own string, or the C-named module's prefix and the declared name —
and a declaration with an empty owner chain that is not itself an instance of a
generic — every root fn and global, `main` among them, `extern` or not — is its
declared name alone. Everything else, an `extern` declaration included, is
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
| `push` of generic module `mod stack[T]`, a submodule of the root, at `i64`; its global `count` | `_CNvIC5stackxE4push`, `_CNvIC5stackxE5count` | `stack[i64].push`, `stack[i64].count` — the module instance is the owner, and its members' components are bare |
| a method of `struct Entry` in `stack[i64]`; `tally[T]`'s `see` at a type of module `user` | `_CNvNtIC5stackxE5Entry7doubled`, `_CNvIC5tallyNtC4user3TagE3see` | `stack[i64].Entry.doubled`, `tally[user.Tag].see` |
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
| `extern fn @c labs` in a Cone module, `extern fn @c(system) GetTickCount` | `@labs`, `@GetTickCount` | C names, bare (S5) |
| `extern fn twice` in Cone module `moduleextern`; `extern fn len(self)` in its `Vec2` | `_CNvC12moduleextern5twice`, `_CNvNtC12moduleextern4Vec23len` | `moduleextern.twice`, `moduleextern.Vec2.len` — `extern` does not change the name |
| `fn Str` in `mod @c("print") cio`; `fn @c("labs") abs` there | `@printStr`, `@labs` | the prefix and the name; the fn's string is the whole symbol (S5) |
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
except `main` and a public C-named one; a declaration is external, as an LLVM
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

The cases, in a program compile with no build description, which is the
program's only object:

| Declaration | Linkage |
| --- | --- |
| `fn foo`, public or private, in a file with no `mod` | internal — the symbol is bare, so nothing outside could safely resolve it |
| `fn main` | external — the C runtime resolves against it |
| a global variable | internal |
| an instance of a generic, wherever declared | internal — this object is the only one that references it |
| a vtable for a type declared here | internal |
| a trait's vtable list | internal, in every kind of compile (L4) |
| a C-named declaration, inbound, or outbound and public (`pub fn @c(...)`) | external, unique |
| a private C-named definition | internal: `@c` names it, and `pub` is what exports it to C [Jon 23 Sep] |
| an imported module's declaration — a symbol this object does not define | external: an LLVM `declare` can be nothing else |

**As built, the program rule.** `genlLinkage` asks one question: does this
object define the symbol? A definition — a declaration whose module is flagged
`FlagGenMod`, not `extern`, with a body if a function; every vtable and vtable
list — is `internal`, except `main` and a public C-named definition, which stay
external. A declaration is external. No visibility is ever set: a private name is spelled
and linked exactly as a public one, since privacy is a fact about the
namespace, not the object file.

**A described build is one object of several**, program or library, since a
build description is how a package is compiled on its own. There the mergeable
symbols of L2 are defined by every object that uses them, as `linkonce_odr`
with a COMDAT of kind `any`, so the linker keeps one copy: an instance of a
generic — the package's own, and each importer's from the body its include file
carries — every member of a generic type's instance, every function and global
of a generic module's instance, and a vtable. A program
compiled from a description gives them this linkage too, since the instances
it makes of an imported generic are the same symbols the package and other
importers define.

In a package compile — a build description saying `output: library` — the
rows are what `genlIsExported` and `genlDefinition` do:

| Declaration | Linkage | |
| --- | --- | --- |
| public `fn foo` | external, unique | built |
| private `fn _x`, reached only from inside the package | internal | built |
| a module's `init` or `final`, or the `drop` it is given, public or not: the program's stitched init and final call it | external, unique | built |
| private `fn _x` or global that an `inline`, generic or macro body or a trait default names (L5) | external, unique | built |
| public global | external, unique | built |
| private global, unreached from outside | internal | built |
| method on a public type | external, unique — a public method; a private one is external only when the type holds an expanded body, which reaches it through a receiver name resolution cannot see | built |
| method on a private type | internal — unless an expanded body names the type, which then counts as a public type | built |
| an instance of a generic the package itself instantiated, every method or static of a generic type's instance, and every function and global of a generic module's instance | external, mergeable: `linkonce_odr`, `comdat any` — for a module's instance that is its globals too, so every object's copy is one storage | built |
| a vtable | external, mergeable: `linkonce_odr`, `comdat any` — its address is what pattern matching compares, so one copy must survive | built |
| a trait default cloned into a type this package declares | external, unique — one package emits it | built, as the implementing type's method |
| a trait's vtable list | internal (L4) | built |
| any definition of `core`, or of a package the search path compiled in | internal: this package does not export it | built |

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

- **L5, private but reachable through an expanded body: external.** Neither
  forbidding such a name nor copying it per importer: a private helper,
  global or type named by a body the importer expands —
  `inline`, generic, macro, a trait's default — is exported once by its
  package and linked against; the importer declares it (`genlFnSym`,
  `genlVarSym`). What counts as reached is what name resolution sees the body
  name (`DclExpandReached`), which over-approximates in one way: a *private*
  `inline` body counts too, whether or not anything outside can reach it. No
  visibility is set, so it is external and not hidden.
- **L6, telling the compiler.** A build description's `output: library` tells
  the compiler it is producing an importable package: it names the root and
  sets `opt->library`, which makes the object position-independent and
  switches generation to the package rule above. `--library` on the command
  line sets the same flag, and is what an `output` line overrides. Any build
  description, library or executable, also sets `opt->described`, which makes
  the mergeable symbols of L2 `linkonce_odr`; with no description they are
  internal, as the program is then the only object. Under S3 a
  program's prefix-less symbols are internal, which is what makes prefix-less
  sound: a program's `@log` cannot satisfy a package's reference to libm's
  `log`.

### As built

One row per kind of symbol, a program compile with no build description; in a
described build, the instance rows and the vtable row are `linkonce_odr` ·
`any` instead (`define linkonce_odr i64 @_CINvC1q6largerxE(...) comdat {`).
Linkage is LLVM's spelling —
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
| imported module's `fn` | `declare i64 @_CNvC3sub5subFn(i64)` — `sub.subFn`; a private top-level `fn` or global leaves no symbol unless a public inline body reaches it, and is then declared the same way | external · none |
| imported module's global | `@_CNvC3sub9subGlobal = external global i64`; `imm` is `external constant` | external · none |
| method on a struct in an imported module | `declare i32 @_CNvNtC3sub5SubPt3get(%SubPt*)`; the private method **is** declared, `declare i32 @_CNvNtC3sub5SubPt4__hid(%SubPt*)`, because the privacy filter in `genlProgram` tests only the module's top-level node | external · none |
| the same file as root and as import | `define internal i64 @scaleInt(i64 %0) comdat {` as root; `declare i64 @_CNvC9modulesub8scaleInt(i64)` when imported — one declaration, two symbols, depending on which compilation the module was the root of | |
| instance of a generic `fn` | `define internal i64 @_CINv4pickxE(i64 %0, i64 %1) comdat {` — `pick[i64]` | internal · `nodeduplicate` |
| method of a generic type's instance | `define internal i64 @_CNvINt6HolderxE5tally(%Holder %0) comdat {` — `Holder[i64].tally`; the instance is the owner, so `fn tally(self) i64` is told apart across instances | internal · `nodeduplicate` |
| trait default cloned into an implementer | `define internal i32 @_CNvNt5Gauge7reading(%Gauge* %0) comdat {` — spelled exactly as an override written there, `Gauge.reading`; no arguments, since a copy is not an instance | internal · `nodeduplicate` |
| synthesized drop function | `_CNvNt6Bundle4drop` — `Bundle.drop` | as its type's methods |
| vtable | `@_CYNt5GaugeNt5Meter = internal constant %"Meter:Vtable" { ... }, comdat` — `Gauge as Meter` | internal · `nodeduplicate` |
| vtable list | `@_CLNt5Meter = internal constant [2 x %"Meter:Vtable"*] [...], comdat` — one per trait, so LLVM never uniquifies one | internal · `nodeduplicate` |
| `extern` in a Cone-named module | `declare i64 @_CNvC12moduleextern5twice(i64)` — `moduleextern.twice`, the module's Cone name; in the root, bare, as every root declaration is | external · none |
| `extern` with `@c`, or in a C-named module | `declare i32 @abs(i32)`; `declare %void @printStr({ i8*, i64 })` for `Str` in `mod @c("print") cio` | external · none |
| `extern fn @c(system)` | `declare dllimport x86_stdcallcc i32 @GetTickCount()` — the DLL import only because it is `extern` | external · none |
| `pub fn @c("cone_square")`, a body | `define i64 @cone_square(i64 %0) comdat {` — exported to C; private, it would be `define internal` | external · `nodeduplicate` |
| `pub fn @c("ConeTicks")` in `mod @c(system) win` | `define x86_stdcallcc i32 @ConeTicks() comdat {` — the convention, and no DLL import on a definition | external · `nodeduplicate` |
| string literal | `@string = internal constant [6 x i8] c"hello\00", comdat` | internal · `nodeduplicate` |
| anonymous `fn` | `define internal i32 @anon(i32 %0) comdat {` | internal · `nodeduplicate` |
| `inline` fn | no symbol | |
| overload name | no symbol; each candidate is spelled as an ordinary `fn`, and a public name holds only public candidates (L5) | |
| `stdio` | defined in every importer, since `stdio` is a package the search path finds, and so a generating module: `@_CNvC5stdio5print = internal global %IOStream zeroinitializer, comdat`, `define internal %void @_CNvNtC5stdio8IOStream9appendInt(...) comdat {` — internal, so two such objects cannot clash | internal · `nodeduplicate` |
| core's `extern fn @c malloc`, `free` from `genlFree`, `llvm.trap`, `llvm.sqrt.*` | `declare i8* @malloc(i64)` and so on — C and LLVM names; `malloc`'s from its `@c`, the others minted outside these rules | external · none |
| `a_b.c` and `a.b_c` | `_CNvC3a_b1c` and `_CNvC1a3b_c` — distinct by construction | |

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
- **A struct a global's fold needs is resolved once, even round a refused loop.** The fold passes retry a fold whose own source has not arrived, but a global's fold resolves its struct on demand, in the struct's module, and where that module is mid-fold round a loop a name the struct's declaration reaches through a late re-export (a field's type) is reported unknown beside the loop, and not retried ([module](../../compiler/c/doc/nodes/module.md), Hazards).
- General aliases beyond `typedef` and the folded-member alias are not implemented: nothing yet names an expression or a declaration directly, and there is no spelling for one outside a fold clause and `typedef`.
- Generic, macro and metaprogram namespace behavior is partly implemented, incomplete, or aspirational. Delegated inheritance and concrete enrichment are both built; see "Folding into a type" above.
- Packages organize importable libraries but are not yet defined as a distinct namespace layer.
- A path may only pass through a module or a struct-like type, or an alias of one. One whose base is a number type, a generic instance or a generic parameter is refused at type check, because none of those names a namespace at the point the collapse runs. Finishing those at type check, where they do, is the natural other half of the collapse and is not built.
- A lone file — the file the compiler is given when it is not its folder's designated file, or a module an import reached by its path — is still named after that file, and a `mod` declaration in it still renames it. A folder, or a place as a one-file module in a module's folder, is what replaces filename naming, and a lone file has nothing else to be named after.
- **A module reaches a sister, and an external module by a path; a PACKAGE name resolves to nothing.** The registry a module is for its children is built, so `import` has two halves and only one of them waits: turning an external package name into an interface to read is the package work's, and until then `import` writes a path for anything outside the tree.