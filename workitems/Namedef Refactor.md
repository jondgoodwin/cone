**Scope under review; re-derive before starting.** What
[[Packages and Separate Compilation]] and delegated inheritance actually require
is narrow — a namespace entry that is a *binding*: local spelling, visibility
bit, the value it refers to, a link to its origin, and room for code-generation
provenance. Everything below assumes something larger.

Three reasons to re-derive rather than execute as written:

- **It bundles separable refactors.** Replacing tag-group classification in
  `isExpNode`/`isTypeNode`, merging the name-use tag variants, and migrating
  declaration data onto the binding are each worth deciding on their own merits.
  Only the binding record is on the critical path for folding and visibility.
- **Stages 3–5 carry a dual representation across behavior-preserving steps**,
  where duplicated `namesym`, `vtype` and `value` stay live behind a
  compatibility accessor. No test can distinguish those intermediate states,
  which sits badly against the rule that a change lands with a case that fails
  without it.
- **The pipeline prerequisite has been overtaken.** [[Analysis re-factor]] built
  the per-node states for type check and deliberately left name resolution as one
  eager pass; the plan below still asks for both to be interleaved as a
  precondition.

**Better first target: structs.** Name-folding into a type does not exist yet, so
the binding record can be built beside the clone-based mixin without regressing
anything, then applied to module folding — which is a replacement of working
behavior. `FnOverloadDclNode` is already a proto-binding to generalize from.

1. Create namedef node (name, type/constraint, value, generic?, owner?) and convert compliant nodes accordingly
2. 	1. Switch exp/type detection algorithm to not use nodetype as signal, thereby fixing nameuse variants etc.
3. 	2. See [[names-and-namespaces|Names and Namespaces]]
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
	- Introduce `FnOverloadDclNode`. Initially it refers directly to concrete `FnDclNode`s so this stage does not depend on the larger NameDef migration.
	- Support overload-set names in module and type namespaces.
	- Initially, name resolution may resolve the overload spelling to the `OverloadSet` and the existing type-checking pass may select and lower exactly one matching concrete function. The recursive pipeline refactor below will subsequently move selection into the interleaved resolution of the call.
	- Reject overload names outside call position. Apply the same machinery to operators.
	- Replace `FnDclNode.nextnode` and all declaration-order or best-match behavior. Verify direct concrete calls, unique overload matches, no matches, ambiguous exact/coerced matches, methods, and operators.
	- See [[Overload Refactor]] for the requirements and ordered two-phase implementation plan.

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

**The per-node states this asks for now exist for type check**, built by
[[Analysis re-factor]]: `TypeChecking` and `TypeChecked` on every declaration, read
rather than refused, so a forward reference analyzes what it names, finished work
is not repeated, and a true cycle reports `ErrorCircular` or `ErrorNoSize`
instead of recursing. `design/phases/type-check.md` is the design and the corpus has the
tests. Two things that item deliberately did *not* do and this one may still
want: name resolution stays one eager pass with a global gate, because binding a
name needs the declaration to exist rather than to be analyzed; and no `Failed`
state was added, so a declaration already reported still reports again at every
later use.

**How far `fnCallTypeCheck` still is from the four-step choreography above.**
Measured by instrumenting its front end and compiling all 122 corpus sources.
Demand made the *guarantee* available; the dispatch was not restructured to use
it, so three gaps remain and each maps onto a step above.

- **Step 1 is skipped for the very case it describes.** `calleeIsOverload` peeks
  at `dclnode->tag` and then does *not* type check the callee. That path fires
  21 times, and in all 21 the callee had not been checked by anything else — 0
  of 21 arrived already checked. So the ~50 lines below it run in two modes,
  callee-resolved and callee-unresolved, with nothing marking which. The
  downstream `self.method` test confirms it: 2946 arrivals with a checked
  declaration, 22 without. An invariant held by convention rather than by
  construction is what the [[Analysis re-factor]] revert was about.
- **Step 2 runs in the wrong order.** Arguments are type checked *above* the
  line that resolves the callee, so argument checking cannot see parameter types
  and no expected type can be pushed down into an argument. Recorded from the
  coercion side in [[Type Inference and Coercion]].
- **The dispatch leads with syntax, not with the declaration.** The first two
  decisions — macro, and `<-` on a value tuple — are pattern matches on
  `objfn->tag` and `methfld`'s namesym, taken before anything is analyzed. At
  entry the callee's declaration is already `TypeChecked` in 3736 cases and not
  in 41, so the front end cannot rely on it and does not try.

**What would collapse the front end.** Resolve `objfn` first, macros excepted —
they must not have arguments checked before substitution — and then dispatch on
what the *declaration is* rather than on what the *node looks like*. That is
item 1.1's "do not use nodetype as signal" applied to the call node, and it
composes with [[Lexer and Parser]] giving `()` and `[]` distinct node shapes:
fewer syntaxes per shape makes the leading tests smaller, and resolve-first
makes the rest answer from the declaration instead of re-deriving from syntax.
Neither alone is enough.

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
