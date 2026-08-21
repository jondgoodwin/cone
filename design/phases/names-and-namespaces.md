This note is the **rules**: what a name means, how lookup, visibility,
qualification, imports, aliases and overloading are supposed to behave, and
where the compiler does not yet match. Parts of it describe intended rather than
current behavior, and say so.

[Name Resolution](name-resolution.md) is the **mechanism** — how the walk
implements these rules, what it retags, and where it stops. Change a rule here;
change how it is carried out there.

A namespace maps each of its names, each with its own spelling in that namespace, to one binding. A namespace has a single uniqueness domain for its names, regardless of whether the names refer to a module, type, value, function, field, method, macro, generic, or other kind of declared name. This is essentially true of overloaded functions or methods as well.

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
| `src/c-compiler/ir/name.c` | Defines well-known interned names and constructs module/type-prefixed generated variable and function names. |
| `src/c-compiler/ir/inode.c` | Dispatches the name-resolution pass by IR node tag. Start here when a new node kind must participate in name resolution. |
| `src/c-compiler/ir/clone.c` | Rebinds generic/macro parameters during cloning and repairs resolved declaration references in cloned `NameUse` nodes. |

### Parsing and module namespaces

| C file | Name/namespace capability |
| --- | --- |
| `src/c-compiler/parser/parseexpr.c` | Parses unqualified, relative-qualified, and root-qualified name paths plus dotted member names. |
| `src/c-compiler/parser/parsemod.c` | Parses module-level declarations, `include`, `import`, and wildcard folding; loads/reuses modules, establishes module hooks, and assigns generated-name prefixes. |
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

A NameDef therefore records code-generation ownership or provenance separately from the namespace that lexically owns the binding. This determines whether the current compilation unit emits a concrete definition, treats the name as externally supplied, keeps a definition local/static, or emits a coalescible generic instantiation that the linker may merge with equivalent instantiations from other compilation units.

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

The compiler enforces this on the paths that can reach a private name: `nameUseNameRes` reports `ErrorNotPublic` for a `_`-prefixed name reached through a module qualifier from outside its module, `importNameRes` skips private nodes when folding, and `fnCallLowerMethod` refuses a private member on a receiver that is not `self`.

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
