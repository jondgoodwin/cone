Create namedef node (name, type/constraint, value, generic?, owner?) and convert compliant nodes accordingly
	1. Switch exp/type detection algorithm to not use nodetype as signal, thereby fixing nameuse variants etc.
	2. See [[Names and Namespaces]]
### Requirements

The refactor should separate the act of binding a name from the kind of thing bound to that name. A `NameDef` is the canonical declaration/binding record: it owns the interned name, an optional declared type or constraint, the bound value, optional generic information, and code-generation ownership/provenance. Its containing namespace or lexical scope owns the binding separately. Specialized information that is not common to all bindings (such as variable permissions and flow state, function linkage and LLVM state, field indexes, or a type's layout and member namespace) remains on the bound value or on purpose-specific metadata rather than being copied into `NameDef`.

All source constructs that introduce a resolvable name should use this binding abstraction, including variables and parameters, constants, fields, functions and methods, generic parameters, macros, type aliases, named types, and modules. Compiler-created temporary and core-library names should follow the same model. Parser and lowering code may still construct specialized value/type nodes, but namespace tables, scope hooks, cloning, and name uses should refer to their `NameDef`, not directly to a heterogeneous declaration node.

Name resolution must produce one stable result regardless of how the name is reached:

- An unqualified name resolves through lexical scopes and then enclosing namespaces.
- A qualified path resolves each component through a value that exposes a namespace (initially modules and named types), with a clear error when an intermediate component is not a namespace.
- Import/name folding exposes the same definition in another namespace without losing its original owner, visibility, or identity.
- A name use resolves to a `NameDef`; it must not be retagged as `TypeNameUse` versus `VarNameUse` by inspecting the declaration node's tag. Type, value, macro, generic, and namespace use are semantic roles determined from the resolved definition and the context in which it is used.

After name resolution, every ordinary name reference remains a `NameUse` node pointing to the selected NameDef. The referenced definition or its IR value explicitly exposes whether it is usable as a type, runtime value, callable, generic, macro, namespace, or other semantic kind. The compiler must not infer that role from the numeric category containing the name-use or declaration tag. This makes aliases and folded names independent of the concrete kind of IR node they ultimately reference.

Expression/type classification must therefore stop depending on the high bits or ordering of `NodeTags`. Concrete tags may continue to drive node-specific traversal and lowering, but predicates such as `isExpNode` and `isTypeNode`, and checks such as `iexpTypeCheckAny` and `itypeTypeCheck`, must use an explicit semantic property/interface of the resolved node or definition. This must support parser-ambiguous syntax and aliases without requiring separate name-use variants, while still diagnosing a value used as a type or a type used where a runtime value is required.

Aliases are ordinary definitions whose value is another nameable value. `x = 3` can therefore bind any value, while a type-valued alias is structural and resolves through to the underlying type (the present `typedef` behavior). Alias chains must preserve the alias definition for diagnostics, ownership, and visibility while allowing semantic operations to reach the final value. A future nominal-type declaration, if needed, should be a distinct operation rather than an implicit property of an alias.

Namespace policy belongs at the definition level:

- Visibility must be represented and checked consistently for qualified access and name folding; it should not be inferred ad hoc from the payload node.
- Code-generation ownership/provenance must identify whether the current compilation unit emits the definition, refers to an externally supplied definition, keeps a static definition local, or emits a coalescible generic instantiation. This is distinct from the namespace or lexical scope that contains the NameDef.
- Duplicate-name handling must be centralized. An `extern` declaration and an implementation of the same function name/signature must not silently create two definitions.
- Overloading must be an explicit property of a name binding (for example, an overload set), rather than a `nextnode` chain attached to the first function payload. Only permitted declaration kinds may contribute candidates, and duplicate signatures remain errors.

The migration must update every consumer of the current declaration shapes: module/type namespace insertion, local scope hooking, `inodeGetName`, `NameUseNode`, generic parameter capture and substitution, cloning, type inference/checking, flow analysis, LLVM generation, printing, and import folding. Source location should remain available on both the definition and its bound construct as needed for useful duplicate-definition and invalid-use diagnostics.

The refactor is complete when a name has one canonical definition object; qualified, folded, aliased, generic, overloaded, and local references all resolve through it; no expression/type decision relies on a declaration tag group; and existing forward references, shadowing, method/field lookup, type aliases, generic instantiation, and code generation continue to work.

### Recommended name and namespace refactor stages

Before the first behavioral change, add focused characterization cases for current method calls, operators, qualified names, shadowing, aliases, imports, duplicate names, and `extern` declarations. Each stage below must leave the compiler buildable and must add focused positive and negative cases for the behavior it changes.

1. **Implement the new overload model first.**
	- Add the `overload` declaration syntax and require every concrete function or method to have its own unique name.
	- Introduce an `OverloadSet` IR node. Initially it may refer directly to concrete `FnDclNode`s so this stage does not depend on the larger NameDef migration.
	- Support overload-set names in module and type namespaces.
	- Initially, name resolution may resolve the overload spelling to the `OverloadSet` and the existing type-checking pass may select and lower exactly one matching concrete function. The recursive pipeline refactor below will subsequently move selection into the interleaved resolution of the call.
	- Reject overload names outside call position. Apply the same machinery to operators.
	- Replace `FnDclNode.nextnode` and all declaration-order or best-match behavior. Verify direct concrete calls, unique overload matches, no matches, ambiguous exact/coerced matches, methods, and operators.

2. **Introduce declaration and semantic accessors without changing representation.**
	- Centralize access to a declaration's name, declared type/constraint, value, namespace, visibility, and code-generation provenance.
	- Add explicit semantic capability queries for type, runtime value, callable, macro, generic, namespace, and overload set.
	- Route namespace insertion, lookup, overload handling, and other high-value consumers through these accessors while the existing declaration layouts and tag groups remain intact.
	- This stage is behavior-preserving and creates the compatibility boundary used by every later stage.

#### Pipeline prerequisite between stages 2 and 3

Before NameDefs replace the current lookup targets, refactor the separate whole-tree name-resolution and semantic passes into a recursively interleaved process. Resolving a node must be able to request the semantic resolution of the child nodes or referenced declarations it depends on, then finish resolving or lowering the outer node with that information available.

For a call through an overload name, this choreography is:

1. Resolve the callee spelling to its overload-set binding.
2. Recursively resolve the argument expressions and the candidate signatures sufficiently to know their types.
3. Select the only matching concrete function or report no-match/ambiguity.
4. Point the call at the concrete definition before returning from resolution of the call node.

The same mechanism should address other cases where name resolution currently runs before it has enough semantic information. It must retain explicit per-node states such as unresolved, resolving, and resolved so forward references are supported, completed work is not repeated, and true dependency cycles produce controlled diagnostics rather than recursion. This pipeline change is broader than NameDef representation and should have its own design and focused tests, but stages 3–5 depend on it.

3. **Introduce canonical NameDefs at lookup boundaries.**
	- Add `NameDef` with its spelling, optional type/constraint, referenced IR value, source/origin link, visibility, and code-generation provenance.
	- Make every `Namespace` entry and scoped `Name.node` hook point to a NameDef rather than directly to a heterogeneous declaration node.
	- Create NameDefs for all existing declarations at their current construction/insertion points, but leave their duplicated `namesym`, `vtype`, and `value` fields temporarily intact.
	- Provide one compatibility accessor that unwraps a NameDef to its IR value so untouched name-resolution, type-checking, flow, and generation code continues to work.
	- Change `OverloadSet` candidates from concrete function nodes to concrete function NameDefs.

4. **Unify name uses and semantic classification.**
	- Make all resolved ordinary references remain `NameUseTag` nodes pointing to NameDefs.
	- Remove `TypeNameUseTag`, `VarNameUseTag`, and declaration-tag inspection from name resolution.
	- Have each use context validate the referenced definition's explicit semantic capabilities.
	- Replace `isExpNode`/`isTypeNode` decisions that rely on tag-group arithmetic with the new capability queries, retaining concrete tags only for node-specific dispatch.
	- Verify type aliases, value uses, calls, overload calls, qualified paths, constructors, generics, macros, and invalid cross-role uses.

5. **Move declaration data into NameDef one family at a time.**
	- Migrate variables and constants first, then fields, concrete functions/methods and overload sets, aliases and named types, modules, and finally generic/macro parameters and compiler-created names.
	- For each family, move only common binding data into NameDef; keep specialized state on the referenced IR node.
	- Update cloning, printing, type inference, flow analysis, LLVM generation, mangling, and diagnostics for that family before beginning the next.
	- Remove duplicated declaration fields only after no compatibility consumer reads them.
	- Remove the NameDef compatibility unwraps, old name-use tags, tag-group classification, raw declaration pointers in namespace/hook APIs, and other temporary paths once the final family has migrated.
	- Assert that every resolvable name reaches exactly one NameDef and every overload set contains concrete NameDefs.

**NameDef completion gate:** At the end of stage 5, the NameDef refactor itself is complete and independently verifiable. The remaining stages apply the model to broader name and namespace behavior and should be coordinated with the designs on which they depend.

6. **Implement canonical `extern` merging and code-generation provenance alongside compilation-unit design.**
	- Merge equivalent `extern` declarations and one implementation into a single NameDef.
	- Diagnose incompatible declarations and multiple implementations.
	- Use provenance to distinguish definitions emitted here, externally supplied definitions, local/static definitions, and link-coalescible generic instantiations.
	- Verify emitted LLVM linkage and symbols, not only successful compilation.
	- Coordinate this stage with the package/module/file and compilation-unit model, since those rules determine which unit owns and emits a definition.

7. **Implement aliasing, visibility, and import folding alongside module/import design.**
	- Preserve `typedef` behavior while making aliases point through NameDefs.
	- Implement selective import folding and `as` renaming by creating a new local NameDef that refers to the original IR value and retains its origin.
	- Enforce the receiving namespace's single collision domain and the original definition's visibility.
	- Ensure generated identity and code-generation provenance do not change merely because a definition is aliased or folded.
	- Coordinate this stage with nested modules, packages, file lookup, qualified paths, and the final import syntax.

8. **Finish nested generic, macro, and lexical namespaces alongside their semantic designs.**
	- Give generic and macro declarations ordinary NameDefs in their containing namespace.
	- Represent parameters, compile-time variables, and body scopes as nested NameDef scopes using the same lookup machinery as functions and blocks.
	- Preserve current shadowing and implicit-`self` behavior.
	- Coordinate final specialization ownership, overload participation, and macro-expansion hygiene with the generic and macro refactors rather than deciding them solely as part of NameDef.

9. **Apply the namespace model to extensions and other deferred namespace creators.**
	- Allow extensions to contribute members and overload candidates only after extension ownership, collision, visibility, and compilation-unit rules are specified.
	- Apply the same NameDef and single-domain invariants to nested modules, packages, inheritance forwarding, and any other construct that introduces or augments a namespace.
	- Treat each feature as a separate, testable semantic change rather than reopening the completed NameDef representation.

After each stage, run its focused cases plus the complete Cone smoke input. For stages affecting ownership or linkage, inspect generated output as well as compilation success.

### Overloading requirements

Overloading must preserve the namespace invariant that one spelling maps to one NameDef. It does so by distinguishing concrete callable definitions from the compiler-only name used to select among them.

Every concrete function or method must declare a namespace-unique name. For example:

`fn intersect_bool overload intersect ...`

This creates or uses two distinct NameDefs in the same namespace:

- `intersect_bool` refers directly to the concrete function IR node. It is the function's stable source identity and the basis for its generated linker name.
- `intersect` refers to an overload-set IR node containing references to all concrete function or method NameDefs that declare `overload intersect`.

The overload-set node owns no implementation and is not itself a callable function value. During recursive name/semantic resolution of a call, lookup of `intersect` first finds its overload-set NameDef. The resolver then obtains the argument and candidate-signature types, selects one concrete definition, and points the call at it before resolution of the call node finishes. An overload-set use is valid only in the callee position of a function or method call. It cannot be used as a first-class function value or have its address taken. A programmer who needs a function value must use the unique concrete name.

Once argument types are available within that recursive resolution, the call resolver examines every candidate in the overload set. A candidate matches when its parameter signature accepts the call arguments, including any permitted implicit coercions. Selection obeys a deliberately strict rule:

1. Exactly one matching candidate selects that concrete NameDef.
2. No matching candidate is a compile error.
3. More than one matching candidate is a compile error.

There is no ranking of exact matches over coerced matches, no ranking by number or quality of coercions, and no declaration-order tie breaking. The programmer resolves ambiguity by naming a concrete function directly or by explicitly coercing arguments until only one candidate accepts them. Return types do not participate in selection.

After selection, the call IR must refer directly to the chosen concrete function or method NameDef; later flow and generation phases do not operate on the overload set. Methods use the same mechanism, with the receiver participating in signature matching. Operators also use this mechanism and ordinarily appear at call sites through their overload name.

Access control is determined by the overload-set NameDef used by the caller. Its concrete candidates do not undergo a second visibility filter: private unique names may implement a public overload set because callers never resolve those private names. Code generation must still ensure such implementations are link-reachable wherever the public overload name is callable.

The initial implementation should keep overload sets closed to declarations in their owning namespace. Extending a type's overload set is intended but deferred to the extension design, which must define ownership and collision behavior. Generic overload candidates and merging equivalent `extern` declarations with implementations are also deferred, but the overload-set representation must be able to accommodate them without returning to declaration-linked chains.

### Overloading design and implementation plan

#### Feasibility and PR strategy

The first overload stage can be implemented and verified before pushing or merging. It is large enough that one PR is possible but not ideal: syntax, namespace storage, call lowering, operators, traits, core-library intrinsics, LLVM naming, and smoke coverage all change together.

The safer approach is two PRs:

1. **Preparatory PR: isolate candidate handling without changing Cone behavior or syntax.** Hide the current `FnDclNode.nextnode` traversal and signature matching behind callable-binding helpers. Route method calls, pointer operators, trait reconciliation, structural trait matching, and vtable construction through those helpers. Existing same-named method declarations and best-match behavior remain intact. The existing smoke program must compile exactly as before.
2. **Overload behavior PR: introduce the final syntax, `OverloadSet`, and strict unique-match rule.** Replace the linked-list representation and old declaration syntax atomically. Add module-level overloading, unique concrete names, call-only overload names, operators, and the trait rules below. Update core-library declarations and test sources in the same PR so the repository is never left with broken overload behavior.

This split keeps both commits mergeable and makes the second PR primarily a storage/semantic replacement behind already-centralized APIs. The later recursive name/semantic pipeline refactor will move candidate selection from the current type-checking pass into recursive call resolution; it is not required to prove the overload language behavior in this PR.

#### Transitional IR design

Until NameDef exists, add a dedicated `OverloadSetNode`:

```c
typedef struct OverloadSetNode {
    INodeHdr;
    Name *namesym;
    Nodes *candidates; // FnDclNode pointers initially; NameDef pointers after stage 3
} OverloadSetNode;
```

Each concrete `FnDclNode` gains `Name *overloadsym`, which is `NULL` when the function does not participate in an overload set. `nextnode` is removed after all legacy consumers have migrated.

For:

```cone
fn intersect_bool overload intersect(...)
```

the namespace contains:

- `intersect_bool -> FnDclNode`
- `intersect -> OverloadSetNode[intersect_bool, ...]`

The concrete function remains in the module/type's ordered implementation list so it is type checked and generated normally. A type's overload-set node may remain namespace-only and be reconstructed from its concrete functions when the type is cloned. A module overload set should also be retained in the module's node list so wildcard import folding can expose the overload name.

The transition may use a dedicated overload-name-use tag so ordinary expression type checking does not pretend an overload set has one function type. That temporary tag disappears when all name uses are unified under NameDef.

#### Insertion and collision rules

Adding a concrete function or method performs two independent insertions:

1. Insert its required unique concrete name. Any existing binding is a duplicate-name error.
2. If `overloadsym` is present, find or create the overload-set binding and append the concrete function.

The overload spelling may be absent or bound to an `OverloadSetNode`; any field, type, module, variable, concrete function, macro, or other binding under that spelling is a collision error. The concrete spelling and overload spelling may not be the same.

Once candidate signatures have been resolved, duplicate accepted signatures in one overload set are declaration errors. This catches indistinguishable candidates before a call site happens to exercise them.

Generic overload candidates are rejected in this first implementation. They can be enabled when generic specialization and overload participation are designed together.

#### Call resolution

Candidate matching must be observational until a unique candidate has been selected: testing a candidate may inspect possible implicit coercions but must not inject casts, borrows, default arguments, or otherwise mutate the call.

For a global/function call:

1. Name resolution points the callee name use at the `OverloadSetNode`.
2. Type checking resolves all explicit argument types.
3. The overload resolver tests every candidate signature.
4. Zero matches reports no matching overload.
5. More than one match reports ambiguity, without ranking exact matches, coercions, or declaration order.
6. Exactly one match rewrites the callee name use to the selected concrete `FnDclNode`.
7. Existing argument finalization performs coercions and inserts default arguments exactly once.

Method and operator resolution follow the same algorithm, with the receiver treated as the first argument. Pointer/reference operator lookup must use this shared resolver rather than retain its current separate first-match loop.

An overload name outside callee position is an error. Direct use of the concrete name remains a normal function value and may be called, borrowed, or stored where its signature permits.

#### Trait and virtual dispatch rules

Traits do not declare overload sets. Every trait method has one unique requirement name and one concrete signature after any trait generic parameters have been substituted.

When reconciling a trait requirement against a complying type, lookup of the trait method's name may produce:

- A concrete method: it satisfies the requirement only if its signature matches.
- An overload set: it satisfies the requirement only if exactly one candidate has the required signature.
- No binding: inherit the trait's default body when one exists; otherwise report the missing method.
- Any existing binding with no matching method: report a conformance error even when the trait has a default body. A default does not repair an incompatible binding already declared by the type.

The selected concrete method, not the overload set, is recorded in vtable implementation data. Trait mixin/default cloning must preserve these same rules. Generic trait matching uses the substituted concrete signature; broader generic overload candidates remain deferred.

#### File-ordered preparatory PR

1. **`src/c-compiler/ir/types/fnsig.h` and `fnsig.c`**
	- Separate the yes/no question "can this signature accept these arguments?" from the current numeric match-quality score.
	- Add non-mutating helpers for function and method viability and exact trait-signature compatibility.
	- Keep compatibility wrappers implementing today's score so this PR does not change selection behavior.
	- Correctly centralize arity/default-parameter checks now duplicated in call paths.

2. **`src/c-compiler/ir/instype.h` and `instype.c`**
	- Add opaque helpers to iterate the callable candidates represented by a namespace binding.
	- Move legacy chain append, best-method selection, and exact trait-method lookup behind these helpers.
	- Keep `nextnode` as the backing representation only for this PR.

3. **`src/c-compiler/ir/exp/fncall.h` and `fncall.c`**
	- Replace direct `nextnode` traversal in normal method calls and pointer/reference operator calls with the new helpers.
	- Keep today's best-match result and lowering unchanged.

4. **`src/c-compiler/ir/types/struct.c`**
	- Route trait mixin reconciliation, vtable construction, structural matching, and inherited-method checks through the callable-binding helpers.
	- Preserve current default-method behavior in this preparatory PR.

5. **`test/test.cone`**
	- Do not change syntax.
	- Ensure existing overloaded `Point.add`, operators, trait methods, and virtual calls continue compiling.

The preparatory PR is complete when no file outside the callable-binding implementation traverses `FnDclNode.nextnode`, and the existing compiler, smoke input, generated AST, and generated LLVM output remain unchanged except for irrelevant ordering or formatting.

#### File-ordered overload behavior PR

1. **`src/c-compiler/parser/lexer.h` and `lexer.c`**
	- Add `OverloadToken` and reserve the `overload` keyword.

2. **`src/c-compiler/ir/stmt/fndcl.h` and `fndcl.c`**
	- Add `overloadsym`.
	- Initialize, clone, and print the optional `overload name`.
	- Remove `nextnode` once all users have moved to candidate helpers.

3. **New `src/c-compiler/ir/overload.h` and `overload.c`**
	- Define `OverloadSetNode`, its constructor, candidate insertion, printing, validation, candidate iteration, strict call selection, and exact trait-signature selection.
	- Keep candidate testing non-mutating.
	- Diagnose overload collisions, duplicate candidate signatures, no match, ambiguity, and non-call use.

4. **`src/c-compiler/ir/inode.h`, `ir.h`, and `inode.c`**
	- Add transitional overload-set and overload-name-use tags.
	- Include the new interface and dispatch printing, name resolution, and semantic validation.
	- Make overload-set nodes produce no runtime value or generated code.

5. **`src/c-compiler/ir/clone.c`**
	- Clone module overload sets where needed.
	- Ensure cloned type functions reconstruct type-local overload sets from their copied `overloadsym` values rather than retaining pointers into the source type.

6. **`src/c-compiler/ir/stmt/module.h` and `module.c`**
	- Add function-specific insertion that registers the concrete function and then creates/appends its overload binding.
	- Retain a newly created module overload set in `mod->nodes` for printing and import folding.
	- Apply the namespace-wide collision rules.

7. **`src/c-compiler/parser/parsefnflow.c`**
	- Parse `fn concrete_name overload overload_name(signature...)`.
	- Store the overload spelling separately from the concrete spelling.
	- Reject an overload spelling on anonymous functions and, initially, generic functions.

8. **`src/c-compiler/parser/parsemod.c`**
	- Register global functions through the new module function insertion path.
	- Update the embedded `stdio` source so each `<-` method has a unique private concrete name and uses `<-` as its overload name.

9. **`src/c-compiler/parser/parsetype.c`**
	- Register concrete methods/static functions under their unique names and their optional overload sets.
	- Reject `overload` declarations inside traits; a trait method name remains a single conformance requirement.

10. **`src/c-compiler/ir/instype.h` and `instype.c`**
	- Replace chain-backed candidate helpers with `OverloadSetNode` handling.
	- Insert concrete type members and overload bindings independently.
	- Remove legacy best-rank and chain logic.

11. **`src/c-compiler/ir/exp/nameuse.h` and `nameuse.c`**
	- Recognize an overload-set declaration during name resolution and retain a distinct transitional overload name use.
	- Prevent ordinary variable/function type propagation from treating the set as one function.

12. **`src/c-compiler/ir/types/fnsig.h` and `fnsig.c`**
	- Remove match-quality scoring from overload selection.
	- Expose only viability for call selection and exact compatibility for trait/vtable reconciliation.
	- Ensure candidate checks do not perform coercion; actual coercion remains in final argument processing.

13. **`src/c-compiler/ir/exp/fncall.h` and `fncall.c`**
	- Resolve global overload calls as well as methods.
	- Count all viable candidates and require exactly one.
	- Rewrite the call to the selected concrete function before invoking existing final argument coercion/default insertion.
	- Use the same path for operators and pointer/reference operators.
	- Reject overload names used as values or addresses.

14. **`src/c-compiler/ir/types/struct.c`**
	- Rebuild overload sets when cloning or inheriting concrete methods.
	- Reconcile each trait requirement against either a direct concrete method or exactly one overload candidate.
	- Preserve the agreed default rule: inherit only when the implementing namespace has no binding; an incompatible existing concrete/overload binding is an error.
	- Store only selected concrete methods in vtables and structural-match results.

15. **`src/c-compiler/corelib/corenumber.c`**
	- Give every compiler-created intrinsic method a stable unique private concrete name.
	- Associate operator implementations with their operator overload spelling.
	- Preserve direct non-overloaded helpers such as `len`, `maxlen`, and special methods where appropriate.

16. **`src/c-compiler/genllvm/genllvm.c`**
	- Stop signature-mangling ordinary concrete methods merely because they are methods; their source-level concrete names are now unique.
	- Retain only the mangling still required for generic instantiations.
	- Generate no symbol for `OverloadSetNode`.
	- Verify that calls selected through an overload set target the concrete function's `llvmvar`.

17. **`src/c-compiler/shared/error.h`**
	- Add dedicated diagnostics for invalid overload declaration, overload-name collision, duplicate accepted signature, no candidate, ambiguous candidates, and overload-name-as-value.

18. **`test/test.cone` and focused overload fixtures**
	- Rewrite existing same-named `Point.add` methods with unique concrete names and `overload add`.
	- Rewrite user-defined operators with unique concrete names and operator overload names.
	- Add global function overloads, direct concrete calls, uniquely matching implicit coercion, method calls, operators, and trait conformance through both concrete and overload-set bindings.
	- Add small committed negative fixtures for ambiguity, no match, value/address use, duplicate concrete names, overload-name collision, duplicate signatures, and trait mismatch.

19. **`CMakeLists.txt` and `Cone.vcxproj`**
	- Add `overload.c` and `overload.h` to both supported build descriptions.

No change should be needed in LLVM expression generation: by generation time every overload call must already point to a concrete `FnDclNode`. Any need for LLVM generation to inspect an overload set is a failed lowering invariant.

#### Verification before push

1. **Baseline before editing**
	- Build current `conec`.
	- Compile `test/test.cone` with `--verify`, `--ir`, and `--llvmir`.
	- Preserve the diagnostics and generated artifacts for comparison.

2. **Build validation after each PR**
	- Configure and build the existing CMake target against LLVM 13.
	- Build the checked-in Visual Studio project when practical because the new source file must be represented in both systems.

3. **Positive compile matrix**
	- Compile the full updated `test/test.cone`.
	- Compile focused programs covering module functions, methods, operators, direct concrete calls, default parameters, one viable implicit coercion, traits satisfied by concrete names, traits satisfied by overload candidates, and inherited defaults when no binding exists.
	- Generate AST and LLVM IR and confirm overload-set names disappear from lowered calls while concrete function names remain distinct.

4. **Negative diagnostic matrix**
	- Compile each negative fixture separately and assert a nonzero exit.
	- Check for the intended diagnostic and source location for: no candidate; two exact candidates; exact plus coercible candidate; two coercible candidates; overload used as a value/address; duplicate concrete name; collision with a non-overload binding; duplicate accepted signature; trait overload with no matching candidate; and an incompatible binding suppressing a trait default.

5. **Generated-code validation**
	- Inspect LLVM IR to verify that concrete methods/functions have stable unique symbols without old signature-based method mangling.
	- Confirm operators and overload calls invoke the expected concrete symbol.
	- Run the generated native smoke executable where the existing compiler/link path supports it, and verify representative overload calls produce distinguishable expected values rather than merely compiling.
	- Compile the WebAssembly target to ensure overload lowering is target-independent.

6. **Regression searches**
	- Confirm `nextnode`, best-match scoring, and direct overload-chain traversal are absent.
	- Confirm only `OverloadSetNode` owns candidate lists.
	- Confirm no generation or flow-analysis code accepts an unresolved overload-set name.

The overload behavior PR is ready to push only when every positive fixture compiles, every negative fixture fails for the intended reason, the broad smoke input compiles for native and WebAssembly targets, generated calls name the selected concrete symbols, and the preparatory/current behavior regressions remain covered.