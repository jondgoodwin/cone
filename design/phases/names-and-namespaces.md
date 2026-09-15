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

**Visibility is checked against the spelling the caller used**, never against
the declaration reached. ▸ **Settles** how a public overload name may
legitimately select a private candidate: the set is public, the member is not,
and calling through the set is the way in.

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
| `src/c-compiler/ir/name.c` | Defines well-known interned names and spells every declared symbol — `nameSymbol`, `nameVtable`, `nameVtableImpl` — from a declaration's owner chain and facts. See "Symbols". |
| `src/c-compiler/ir/dclinfo.c` | The declaration facts a symbol is derived from: sets them where a declaration joins its namespace (`dclInfoJoin`), walks to the enclosing module, prints them in the IR dump. |
| `src/c-compiler/ir/inode.c` | Dispatches the name-resolution pass by IR node tag. Start here when a new node kind must participate in name resolution. |
| `src/c-compiler/ir/clone.c` | Rebinds generic/macro parameters during cloning and repairs resolved declaration references in cloned `NameUse` nodes. |

### Parsing and module namespaces

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/parser/parseexpr.c` | Parses unqualified, relative-qualified, and root-qualified name paths plus dotted member names. |
| `src/c-compiler/parser/parsemod.c` | Parses module-level declarations, `include`, `import`, and wildcard folding; loads/reuses modules, names each one, and establishes module hooks. |
| `src/c-compiler/parser/parsetype.c` | Parses struct/trait/union members and inserts fields and methods into the type namespace. |
| `src/c-compiler/ir/stmt/program.c` | Owns the program's module list, reuses modules by interned name, and initiates name resolution for every module. |
| `src/c-compiler/ir/stmt/module.c` | Owns module namespaces, inserts global declarations with duplicate checks, switches active module hooks, folds imports before resolving other nodes, and walks module declarations. |
| `src/c-compiler/ir/stmt/import.c` | Implements wildcard import folding by adding imported named nodes to the receiving module namespace. |

### Name uses, lexical scopes, and declarations

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/ir/exp/nameuse.c` | Represents name and member uses, stores qualification paths, resolves qualified paths through module/type namespaces, resolves unqualified names through hooks, and retags uses by declaration kind. Binding a bare field name is here; **lowering it to `self.field` is `nameUseTypeCheck`'s**, because building that call node needs a type to check it against. |
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
| `src/c-compiler/ir/types/struct.c` | Owns struct/trait member namespaces, inserts fields and `Self`, hooks members and generic parameters during resolution, and performs inherited member lookup/collision checks. |
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

Unqualified lookup selects the nearest active binding. Function parameters may therefore shadow names from the containing type or module, and a local in an inner block may shadow a parameter or outer local. A second declaration of the spelling within the same lexical scope is an error. Explicit qualification remains available to reach a hidden namespace member where the language provides a qualified form.

### Types

Named types expose a member namespace. The documented model includes fields, methods, static functions, and potentially nested types. Instance members use `.`, while static/type members use `::`.

Current compiler behavior:

- Structs and traits have one namespace containing fields, methods, static functions, inherited members, and `Self`.
- A field or static function cannot collide with another member name.
- Methods and static functions each declare a namespace-unique concrete name. A declaration may additionally name an overload set with `fn concrete overload shared(...)`. The concrete name binds directly to its `FnDclNode`; the overload name binds to a separate `FnOverloadDclNode` holding every candidate declared for it, including a set that currently has only one candidate. Two declarations claiming the same concrete name are a duplicate-name error, and an overload name already bound to anything other than an overload node is a collision error.
- Every executable implementation remains a separate `FnDclNode`. The overload node is only a namespace binding, so lookup, call lowering, trait reconciliation, vtables, and code generation always record the selected concrete node.
- A method cannot share a spelling with a field.
- Struct/trait generic parameters form an enclosing lexical context while the type is resolved.
- Unions reuse struct-like IR flags. Documented nested union variants are intended to be hoisted into the surrounding module rather than placed in a union namespace, but union support is incomplete.
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

Visibility is checked on the name the caller uses. A public overload-set NameDef may expose concrete functions whose unique names are private, because those concrete names are implementation identities and are not looked up by the caller. Code generation must nevertheless make every concrete candidate reachable wherever its public overload set can be called.

Extending a type's overload sets from an extension is intended, but its ownership and collision rules are deferred until extensions are designed. Generic candidates and merging matching `extern` declarations with implementations are likewise deferred; a generic declaration may not currently name an overload set at all.

## Lookup and qualified paths

An unqualified name is resolved through nested lexical contexts and then the enclosing namespace. A qualified path uses `::` to walk namespaces.

Current compiler behavior:

- `name` begins in the active lexical/module context.
- `module::name` begins in the current module.
- `::module::name` begins in the program's root module.
- Qualification supports multiple components.
- Each intermediate component must currently resolve to a module or struct-like type.
- A resolved `NameUseNode` points directly to a heterogeneous declaration node and is retagged as a type, value, macro, or generic use according to that node's tag.

The NameDef design instead makes lookup return a stable NameDef. A resolved reference remains one kind of `NameUse` node pointing to that definition. The definition or its IR value explicitly indicates whether it is usable as a type, runtime value, callable, macro, namespace, generic, or other semantic kind; the surrounding use validates that role. Name resolution does not retag the reference or infer its role from the numeric category of a node tag.

### Bare names inside a type

While resolving a type body, the compiler places the type's members in the lookup context outside the method's parameter and block scopes. Normal nearest-scope lookup applies:

- A parameter or local with the same spelling shadows the type member.
- If no nearer binding shadows an instance field, its bare name is lowered to `self.field`.
- If no nearer binding shadows an instance method, calling its bare name is lowered to `self.method(...)`. This applies to an overloaded name too: the bare name resolves to the type's `FnOverloadDclNode`, and the lowered member call selects the concrete candidate.
- `self.field` or `self.method(...)` explicitly selects the member when a lexical name shadows it.

Implicit `self` is therefore lowering performed after ordinary name resolution has selected an unqualified type member; it does not take precedence over lexical bindings.

## Visibility

Documented Cone visibility is spelling-based:

- A name declared in a module and beginning with `_` is private to that module.
- A type member beginning with `_` is private to its type.
- Other names are public.

The compiler enforces this on the paths that can reach a private name: `nameUseNameRes` reports `ErrorNotPublic` for a `_`-prefixed name reached through a module qualifier from outside its module, `importNameRes` skips private nodes when folding, and `fnCallLowerMethod` refuses a private member on a receiver that is not `self`. A declaration's visibility is also written once, from the spelling, into its `DclPrivate` bit when it joins its namespace, and generation reads the bit rather than the spelling — see "Symbols".

One consequence is deliberate and worth knowing: **visibility is checked against the spelling the caller used**, so a public overload name may legitimately select a private concrete candidate.

Visibility should belong to the original definition or declaration, while access is evaluated from the use site. A folded or renamed NameDef must not make a private definition public merely by changing its local spelling. The design must also decide whether an alias may deliberately narrow visibility.

## Include, import, and name folding

`include` contributes declarations to the current module. It does not introduce a namespace.

Plain `import math` binds the imported module as `math`; public members are intended to be accessed as `math::name`.

Documented folding supports:

- Selectively bringing a member into the importing namespace.
- Renaming while folding, such as importing `math3d::Point3` as `Point`.
- Folding all public names with `::*`.
- Folding any category of name, subject to the importing namespace's single collision domain.

Current compiler behavior is narrower:

- Plain module import and wildcard `::*` folding are parsed.
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

## Aliases

Current `typedef` creates a module-scoped structural alias for a type. Type resolution follows the alias to its underlying type.

The aspirational model generalizes aliases: a new NameDef may denote anything nameable. Alias chains should preserve each local binding for diagnostics and visibility while semantic operations can reach the final IR value. A type-valued alias remains structural; creating a distinct nominal type should use a separate construct.

Import folding/renaming is a namespace alias operation with an explicit source definition. Other aliases may bind expressions or declarations directly. The exact syntax and compile-time restrictions for general aliases remain open.

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

*Provenance: the rules are the author's; the as-built table is measured from
emitted LLVM IR and pinned by `llvmir` checks in the `core`, `generic`,
`module`, `struct` and `trait` groups.*

### The declaration facts

Every node that declares a symbol carries a `DclInfo` by value — `FnDclNode`,
`VarDclNode` (a global), `StructNode` and `ModuleNode` — and `inodeGetDclInfo`
is the one switch that knows which kinds those are. It holds the **owner**, a
pointer to the enclosing module or type node — the chain is walked, never
stored as a string — and five bits:

| Bit | Meaning | Written from |
| --- | --- | --- |
| `DclPrivate` | visible only within its owner | the leading `_`, once |
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
| visibility bit | export-table visibility (hidden or default; undefined until an export set exists) |
| supply: defined in this compile, or externally supplied | |
| naming regime: C-style or Cone-style | |
| calling convention, for C-style names | |

### Spelling

Rules for Cone-consumed names; C FFI names have their own (S5).

- **S1.** A symbol is a prefix naming the owner chain, then the declared name
  possibly mangled, then possibly a suffix qualifying its individuality.
  [differs: today the prefix is `<name>_` per owner, the name follows bare, and
  the suffix for an instance of a generic is a `:`-separated mangling of each
  parameter's type — which is where the type arguments show, because a
  parameter names them]
- **S2.** The owner chain is the enclosing modules, outermost first, then the
  enclosing types; a module never sits inside a type. **There is no package
  name.** The compiler knows only module names declared in source; a version
  slot, when one is filled, is a suffix on the top module.
- **S3.** A source file with no `mod` declaration contributes no module name,
  so its declarations carry no module prefix. The program's root is prefix-less
  on purpose, which is why `main` needs no special case.
- **S4.** Each owner in the chain is spelled the way its own symbol would be —
  its name possibly mangled, then its suffix if it has one. The only owner
  carrying a suffix is a generic type instance, whose suffix is its type
  arguments. [differs: a type in the chain contributes its declared name alone;
  an instance's arguments reach the symbol only through the mangled parameter
  types, where `self`'s type carries them]
- **S5. C FFI names.** Every module is flagged C-style or Cone-style. In a
  C-style module the owner chain contributes no prefix, no name is mangled and
  nothing carries a suffix, so such a module cannot declare a generic. The flag
  may carry a literal prefix — `SDL_` — prepended to every name, written by the
  author and never derived. The flag affects the symbol only; resolution is
  through the ordinary module, so a caller writes `sdl::Init`. Inbound (a C
  library's symbols) and outbound (a Cone declaration published to C) are one
  mechanism in two directions, and `main` is the existing outbound case.
  [differs: there is no module flag and no literal prefix; the regime is per
  declaration, from `extern`, and an `extern` inside a Cone module is spelled
  bare wherever it is declared]
- **S6. Vtables.** A vtable's owner chain is the implementing type's and its
  suffix is the trait, both spelled by these rules; a vtable list's owner chain
  is the trait's. [differs: a vtable is `<Impl>-><Trait>:Vtable` and its LLVM
  type `<Trait>:Vtable`, from the two types' declared names alone; every
  trait's list is the one literal `vtable-list`, which LLVM uniquifies as
  `vtable-list.1` for the second trait in a module]
- **S7.** Overloaded functions need no signature encoding: a concrete candidate
  already has a unique declared name in its namespace, and only that name
  reaches the symbol. Only an instance of a generic needs type arguments
  encoded.
- **S8.** An identifier may contain any character, in backticks, and may be
  Unicode; both reach the symbol table as quoted or raw-byte symbols. So the
  scheme needs an encoding for the name itself, not only a prefix. **Open:**
  which encoding; Rust v0's punycode-based one is the nearest prior art.

**The exact form is open.** What is decided in principle is that each
component is length-prefixed, which dissolves the separator question — with `_`
as both separator and identifier character, `a_b::c` and `a::b_c` spell one
symbol today — that a sigil opens the symbol in the space C reserves for
implementations, as `_Z` and `_R` do, and that the top module leaves a version
slot. The bytes are not decided, and neither is whether a program's prefix-less
symbols are emitted bare or encoded with an empty chain.

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
| a C-style declaration, inbound or outbound | external, unique |

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

[differs: external everywhere; `hidden` visibility on `_` names, which COFF
ignores; `linkonce any` on instances of generics and on vtables; a COMDAT of
selection kind `nodeduplicate` on every other definition]

The prior art that settled the derivation: C++ `static` is linkage, not access,
and a header-scoped `static` reached from an inline function is an ODR
violation; C++17 `inline` variables are the linkonce case only because no
single `.cpp` owns a header variable. Rust's author writes `pub` or not, and the
compiler derives linkage from what references the item. Cone has one object per
package, so "outside" means outside the object.

**Open:**

- **L4, the vtable list.** By L2 it is mergeable, but its contents are the
  implementers *this compile* saw, so two objects produce different bodies
  under one name and the linker keeps whichever it meets first. Probably one
  list per compilation unit, internal.
- **L5, private but reachable across a package boundary.** A private concrete
  candidate selected through a public overload name is ruled a compile error.
  A private helper called from a public generic or `inline` body, whose
  instance the importer emits, is not ruled: the importer may emit its own
  internal copy of the helper, but a private *global* reached that way has one
  owner and cannot be duplicated, so it is either forbidden from such bodies or
  accepted as external and hidden.
- **L6.** The compiler must be told whether it is producing an importable
  package; a `mod util` inside a program looks identical to a package's module,
  and `--library` today changes only the relocation mode. Under S3 a program's
  prefix-less symbols are internal, which is what makes prefix-less sound: a
  program's `@log` can no longer satisfy a package's reference to libm's `log`.

### As built

One row per kind of symbol. Linkage is LLVM's spelling — absent means
`external` — and the COMDAT is the selection kind of the one each definition
leads; a declaration leads none. Measured on the default x64 Windows triple,
where both selection kinds appear.

| Kind | Spelling today | Linkage · visibility · COMDAT |
| --- | --- | --- |
| root `fn`, public | `define i64 @plainPub(i64 %0) comdat {` | external · default · `nodeduplicate` |
| root `fn`, private | `define hidden i64 @_plainPriv(i64 %0) comdat {` | external · **hidden** · `nodeduplicate` |
| `fn main` | `define i32 @main() comdat {` — nothing special-cases it | external · default · `nodeduplicate` |
| root global: `mut`, `imm`, private | `@pubGlobal = global i64 5, comdat` · `@constGlobal = constant i64 7, comdat` · `@_privGlobal = hidden global i64 6, comdat` | external · default / hidden · `nodeduplicate` |
| struct method, static fn, private method | `@Pt_get` · `@Pt_make` · `define hidden i32 @Pt__hid(%Pt* %0) comdat {` | as the root rows |
| imported module's `fn` | `declare i64 @sub_subFn(i64)`; a private top-level `fn` or global leaves no symbol; a private overload candidate is `declare hidden double @modulesub__scaleFloat(double)` | external · default / hidden · none |
| imported module's global | `@sub_subGlobal = external global i64`; `imm` is `external constant` | external · default · none |
| method on a struct in an imported module | `declare i32 @sub_SubPt_get(%SubPt*)`; the private method **is** declared, `declare hidden i32 @sub_SubPt__hid(%SubPt*)`, because the privacy filter in `genlProgram` tests only the module's top-level node | external · default / hidden · none |
| the same file as root and as import | `define i64 @scaleInt(i64 %0) comdat {` as root; `declare i64 @modulesub_scaleInt(i64)` when imported — one declaration, two symbols, depending on which compilation the module was the root of | |
| instance of a generic `fn` | `define linkonce i64 @"pick:i64:i64"(i64 %0, i64 %1) comdat {` | **linkonce** · default · **`any`** |
| method of a generic type's instance | `define linkonce i64 @"Holder_tally:Holder:i64"(%Holder %0) comdat {` — the arguments come from `self`'s type, so `fn tally(self) i64` is told apart across instances | linkonce · default · `any` |
| trait default cloned into an implementer | `define i32 @Gauge_reading(%Gauge* %0) comdat {` — spelled exactly as an override written there, `@Dial_reading`; no suffix, since a copy is not an instance | external · default · `nodeduplicate` |
| synthesized drop function | `@Bundle_drop`; in an imported module `declare %void @sub_Bundle_drop(%Bundle*)`; for a generic instance `define linkonce %void @"Holder_drop:&Holder:i64"(%Holder* %0) comdat {` | as its type's methods |
| vtable | `@"Gauge->Meter:Vtable" = linkonce constant %"Meter:Vtable" { ... }, comdat` | linkonce · default · `any` |
| vtable list | `@vtable-list = linkonce constant [2 x %"Meter:Vtable"*] [...], comdat`; a second trait's is `@vtable-list.1` | linkonce · default · `any` |
| `extern` | `declare i32 @abs(i32)` — bare inside a module too | external · default · none |
| `extern system` | `declare dllimport x86_stdcallcc i32 @GetTickCount()` | external · default · none |
| string literal | `@string = internal constant [5 x i8] c"hello", comdat` | **internal** · default · `nodeduplicate` |
| anonymous `fn` | `define internal i32 @anon(i32 %0) comdat {` | internal · default · `nodeduplicate` |
| `inline` fn | no symbol | |
| overload name | no symbol; each candidate is spelled as an ordinary `fn` | |
| `stdio` | defined in every importer, since `stdio` is a generating module: `@stdio_print = global %IOStream zeroinitializer, comdat`, `define hidden %void @stdio_IOStream__appendInt(...) comdat {` — two such objects would clash | external · default / hidden · `nodeduplicate` |
| corelib's `extern fn malloc`, `free` from `genlFree`, `llvm.trap`, `llvm.sqrt.*` | `declare i8* @malloc(i64)` and so on — C and LLVM names, minted outside these rules | external · default · none |
| `a_b::c` and `a::b_c` | both `@a_b_c`; LLVM renames the second `@a_b_c.1`, which nothing will ever define | |
| an import cycle back to the root | the root is found by name, and each root declaration is defined once | |

Read on COFF: `nodeduplicate` is selection 1, `any` is selection 2, `internal`
becomes `Static`, and everything else — `hidden` included — is `External`.

The test runner reads the post-optimization `.ir`, where an unreferenced
`linkonce` instance and an inlined `anon` have already vanished; a symbol
assertion written against `.preir` can pass where the runner's fails.

## Known gaps between implementation and intent

- Overloading:
	- Overloading is implemented with `FnDclNode` and `FnOverloadDclNode` rather than with a general `NameDef`, so the concrete/overload split described above exists only for functions and methods.
	- A generic function may not declare an overload name; the parser reports that combination.
	- Extending a type's overload sets from an extension, generic candidates, and merging matching `extern` declarations with implementations remain deferred.
- Compile unit handling of duplicate, consistent type `extern` vs. value-specified names.
- Selective import folding and `as` renaming are documented but unimplemented.
- Nested named modules are documented but lack clear declaration syntax and parser support.
- General aliases beyond `typedef` are not implemented.
- Generic, macro, union, inheritance, and metaprogram namespace behavior is partly implemented, incomplete, or aspirational.
- Packages organize importable libraries but are not yet defined as a distinct namespace layer.
