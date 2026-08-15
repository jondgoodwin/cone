# Overload Refactor

## Requirements

The compiler does not yet have `NameDef`. This refactor therefore uses the
declaration nodes that exist today:

- A concrete function or method name resolves to its `FnDclNode`.
- An overload name resolves to an `FnOverloadDclNode`, which contains the
  `FnDclNode` candidates declared for that overload name.

The completed refactor must satisfy these requirements:

1. Every concrete function or method has a namespace-unique concrete name. The
   concrete name is its stable source identity, can be used directly as a
   function value, and supplies the base name used for generated symbols.
2. A function or method may additionally declare a different overload name:

   ```cone
   fn intersect_bool overload intersect(...)
   ```

   `intersect_bool` maps to the concrete `FnDclNode`; `intersect` maps to an
   `FnOverloadDclNode` containing that node and the other concrete declarations
   that overload `intersect`.
3. The old representation must be removed completely. `FnDclNode` must no
   longer have `nextnode`, namespace entries must not use the first
   `FnDclNode` as the head of a hidden linked list, and no lookup, call,
   trait, cloning, or generation code may traverse such a chain.
4. An overload name may be used only as the callee of a function, method, or
   operator call. Taking its address, storing it as a function value, or using
   it in another expression is an error. The concrete name remains available
   for those uses.
5. Concrete names and overload names share the namespace's normal collision
   rules. A concrete name must be unused. An overload name must be unused or
   already map to an `FnOverloadDclNode`. The concrete and overload spellings
   on one declaration must differ. Two candidates with the same accepted
   parameter signature are an error.
6. Candidate testing must not modify the call. It may determine whether each
   argument can be passed to the corresponding parameter, including default
   arguments and permitted implicit coercions, but it must not insert a cast,
   borrow, or default argument until one candidate has been selected.
7. All candidates are tested:
   - Exactly one matching candidate selects that concrete `FnDclNode`.
   - No matching candidate is a compile error.
   - More than one matching candidate is an ambiguity error.

   Exact matches are not ranked above coercible matches, fewer coercions do not
   win, declaration order does not break ties, and return types do not
   participate. A caller resolves an ambiguity by calling a concrete name or
   explicitly converting arguments so only one candidate accepts them.
8. After selection, the call IR points directly to the selected `FnDclNode`.
   Existing argument finalization then inserts coercions and default arguments
   once. Flow analysis and LLVM generation never receive an unresolved
   `FnOverloadDclNode` as a callee.
9. Global functions, static functions, methods, and operators use the same
   overload representation and unique-selection rule. A method's receiver is
   the first argument when candidates are tested. Pointer/reference operators
   must not retain their separate first-match loop.
10. A trait method remains one named requirement. A concrete type satisfies it
    with either a directly named `FnDclNode` of the required signature or the
    sole matching candidate in an `FnOverloadDclNode`. Vtables and structural
    matches record the selected concrete `FnDclNode`, never the overload node.
11. Cone's existing visibility rule remains in force: a spelling beginning
    with `_` is private. Visibility is checked when the caller looks up the
    spelling it actually uses. A public overload name may therefore select a
    candidate whose concrete name is private; the caller did not look up that
    private concrete spelling.
12. Generic functions may not declare an overload name in this implementation.
    The parser must report that combination rather than partially supporting
    it.

## Design and implementation plan

The work is split into two ordered phases. Phase 1 changes only the compiler's
internal representation: existing same-named method source continues to
compile and uses the existing best-match behavior. Phase 2 changes the language
surface and selection rule: concrete names become unique, overload names become
explicit, global overloads are enabled, and ambiguity becomes an error.

### Phase 1: Replace overload chains without changing Cone behavior

1. **`test/overload/phase1-methods.cone`,
   `test/overload/phase1-operators.cone`, and `test/test.cone`**
   - Add focused, committed characterization fixtures before changing C code.
     Cover the existing same-named `Point.add` methods, value and reference
     receivers, different arities, pointer/reference operators, the embedded
     stream `<-` methods, trait conformance, and virtual calls.
   - Keep each focused fixture small enough to compile independently. Preserve
     `test/test.cone` as the broad smoke input rather than making it the only
     overload test.
   - Record commands and expected success in `test/overload/README.md`. These
     fixtures remain in the repository and are run in Phase 2 and future
     overload PRs; they are not temporary comparison files.
   - Build `conec`, compile all three inputs, and retain their AST and LLVM IR
     outputs outside the repository as the Phase 1 baseline.

2. **`src/c-compiler/ir/stmt/fndcl.h` and
   `src/c-compiler/ir/stmt/fndcl.c`**
   - Add `FnOverloadDclNode` with `INodeHdr`, `Name *namesym`, and
     `Nodes *overloads`. The vector contains ordered `FnDclNode *` candidates;
     the overload node has no function type, body, LLVM value, or generated
     symbol.
   - Add its constructor and printer in `fndcl.c`.
   - Remove `FnDclNode.nextnode`, its initialization in `newFnDclNode`, and its
     reset in `cloneFnDclNode`. A cloned `FnDclNode` is an independent concrete
     declaration and gains overload membership only when inserted into the
     cloned type's namespace.

3. **`src/c-compiler/ir/inode.h` and
   `src/c-compiler/ir/inode.c`**
   - Add `FnOverloadDclTag` beside `FnDclTag` as a named statement node and add
     printer dispatch to the new overload printer.
   - Add explicit no-op name-resolution and type-check dispatch cases for the
     overload node. The owning module or type already walks each concrete
     `FnDclNode`; walking the vector again would process function bodies twice.

4. **`src/c-compiler/ir/instype.h` and
   `src/c-compiler/ir/instype.c`**
   - Change `iNsTypeAddFnDict` so the first method for a spelling remains a
     direct `FnDclNode` binding. When a second same-named method arrives,
     replace that namespace entry with an `FnOverloadDclNode` containing the
     first and second methods. Append later methods to its `overloads` vector.
   - Preserve the current error when the existing binding is not an overloadable
     method or the new declaration is not a method.
   - Remove the loop that finds the end of `FnDclNode.nextnode` and the
     assignment that links the new declaration to it.
   - Change `iNsTypeFindBestMethod` and `iNsTypeFindVrefMethod` to accept the
     namespace's `INode *` binding. Each function handles one direct
     `FnDclNode` or iterates an `FnOverloadDclNode.overloads` vector.
   - Preserve the current Phase 1 selection rules exactly: the first exact
     method match wins, otherwise the lowest existing numeric match score wins;
     virtual-reference matching still chooses the first exact signature.
   - Add a corresponding binding-based pointer/reference method finder here so
     `fncall.c` no longer needs its own representation-specific candidate loop.
     Preserve that loop's existing first-acceptable-candidate behavior in this
     phase.

5. **`src/c-compiler/ir/exp/fncall.c`**
   - Accept either `FnDclTag` or `FnOverloadDclTag` when member lookup is
     followed by a call. Continue accepting `FieldDclTag` only for field
     access.
   - Pass the uncast namespace binding to the revised finders, then lower the
     call to the returned concrete `FnDclNode` as today.
   - Remove the direct `nextnode` traversal from `fnCallLowerPtrMethod`; use the
     Phase 1 pointer/reference finder in `instype.c`.

6. **`src/c-compiler/ir/types/struct.c`**
   - Pass namespace bindings, rather than assumed chain heads, to
     `iNsTypeFindVrefMethod` during trait reconciliation, structural matching,
     and vtable construction.
   - Continue storing the selected concrete `FnDclNode` and its `vtblidx`.
   - Keep cloning concrete methods through `iNsTypeAddFn`; its revised
     dictionary insertion reconstructs overload nodes and never copies an
     overload vector that points into the source type.

7. **Phase 1 verification, then `design/IR Nodes.md` and
   `design/Names and Namespaces.md`**
   - Rebuild `conec`; compile the Phase 1 fixtures and `test/test.cone`; compare
     AST and LLVM IR with the baseline; and run the native smoke result where
     supported. This phase is not complete if syntax, selected methods,
     diagnostics, or generated symbols change.
   - Search all compiler C sources for `nextnode` and require zero remaining
     overload-chain fields or traversals.
   - Only after those checks pass, document `FnOverloadDclNode`, the temporary
     direct-single/grouped-multiple namespace representation, and the fact that
     every executable implementation remains a separate `FnDclNode`.
   - Do not change conesite in Phase 1 because user-visible syntax and behavior
     have not changed.

### Phase 2: Add explicit overload names and require one matching candidate

1. **`test/overload/*.cone`, `test/overload/README.md`, and
   `test/test.cone`**
   - Before changing C code, convert the focused methods and operators to
     unique concrete names plus `overload <name>`, and add global/static
     function overloads and direct calls through concrete names.
   - Add positive fixtures for one exact match, one coercible match, default
     parameters, value/reference receivers, operators, trait satisfaction by a
     direct concrete name, and trait satisfaction by one overload candidate.
   - Add one-file negative fixtures for no match, multiple exact matches, exact
     plus coercible ambiguity, multiple coercible matches, overload use as a
     value/address, duplicate concrete names, collision between an overload
     name and another binding, equal candidate signatures, a generic overload
     declaration, and trait mismatch.
   - Document the expected diagnostic text for every negative fixture so the
     same cases can be rerun after later parser, type-coercion, trait, and
     NameDef changes.

2. **`src/c-compiler/parser/lexer.h` and
   `src/c-compiler/parser/lexer.c`**
   - Add `OverloadToken` and intern `overload` as a reserved keyword.

3. **`src/c-compiler/ir/stmt/fndcl.h` and
   `src/c-compiler/ir/stmt/fndcl.c`**
   - Add `Name *overloadsym` to `FnDclNode`; `NULL` means the declaration has
     only its concrete name.
   - Initialize, clone, and print `overloadsym`.
   - Keep `FnOverloadDclNode` as the namespace binding for every explicit
     overload name, including a set that currently has only one candidate.

4. **`src/c-compiler/parser/parsefnflow.c`**
   - After parsing the required concrete function name and any generic
     parameters, parse the optional `overload` keyword and following identifier
     or backtick operator name before the signature.
   - Store the two spellings in `namesym` and `overloadsym`; diagnose a missing
     overload spelling, equal concrete/overload spellings, an anonymous
     overload, and a generic declaration with an overload name.
   - Remove acceptance of same-spelled declarations as implicit overloads; the
     parser now produces two independent names for namespace insertion.

5. **`src/c-compiler/ir/stmt/module.h`,
   `src/c-compiler/ir/stmt/module.c`, and
   `src/c-compiler/parser/parsemod.c`**
   - Add a module function insertion path that always adds the concrete
     `FnDclNode` to `mod->nodes`, binds its unique `namesym`, and, when
     `overloadsym` is present, finds or creates the separate
     `FnOverloadDclNode` binding and appends the concrete node.
   - Add each newly created module overload node to `mod->nodes`. Module
     printing then shows the binding, and `importNameRes` can fold it during a
     wildcard import because that code enumerates `sourcemod->nodes`.
     `inodeNameRes` and `inodeTypeCheckAny` use the no-op overload cases, so
     candidates are not processed twice.
   - Replace `parseFnOrVar`'s direct `modAddNode` call for functions with this
     function-specific insertion path.
   - Rewrite the embedded `stdiolib` `<-` methods with unique private concrete
     names and `overload `<-`` declarations.
   - Remove the old module duplicate-name path only for a valid existing
     overload-node binding; all cross-kind and concrete-name collisions remain
     errors.

6. **`src/c-compiler/parser/parsetype.c`,
   `src/c-compiler/ir/instype.h`, and
   `src/c-compiler/ir/instype.c`**
   - Change type insertion to bind every method/static function under its
     concrete `namesym` and separately add it to the node at `overloadsym`.
   - Create an `FnOverloadDclNode` on the first explicit overload declaration;
     append later candidates after checking the binding kind and signature.
   - Remove Phase 1's promotion of duplicate same-named `FnDclNode` bindings.
     Two concrete declarations with the same name now produce the normal
     duplicate-name error instead of forming an overload.
   - Replace `iNsTypeFindBestMethod`'s early return for the first exact match,
     `bestnbr` score comparison, and declaration-order fallback with an
     all-candidate scan that returns a concrete node only when exactly one
     candidate is viable.
   - Replace the pointer/reference first-match behavior with the same
     all-candidate decision, while retaining its pointer type-identity rules.

7. **`src/c-compiler/ir/types/fnsig.h` and
   `src/c-compiler/ir/types/fnsig.c`**
   - Add a non-mutating call-viability function that checks arity, required
     versus defaulted parameters, receiver compatibility, and whether every
     explicit argument can be passed using the permitted coercions.
   - Return only viable/not viable for overload selection. Remove use of the
     numeric `1`/`2+` match score when selecting overloads; it must no longer
     encode a preference for exact or fewer-coercion candidates.
   - Keep exact signature comparison as a separate operation for duplicate
     candidate detection and trait/vtable requirements.
   - Leave actual cast, borrow, and default-argument insertion in
     `fnCallFinalizeArgs`, after selection.

8. **`src/c-compiler/ir/exp/nameuse.h`,
   `src/c-compiler/ir/exp/nameuse.c`,
   `src/c-compiler/ir/inode.h`, and
   `src/c-compiler/ir/inode.c`**
   - Add the transitional resolved-name-use handling needed to retain an
     `FnOverloadDclNode` until the enclosing call is checked; do not assign the
     set a fabricated function type.
   - Permit that resolved use only in callee position. Report an error when an
     overload name is borrowed, stored, returned, or otherwise checked as an
     ordinary expression.
   - Ensure printing and semantic dispatch distinguish the overload binding
     from the concrete `FnDclNode` selected later.

9. **`src/c-compiler/ir/exp/fncall.c`**
   - Add global/static overload-call handling as well as method/operator
     handling.
   - Resolve the receiver and explicit argument types, test every candidate
     with the non-mutating viability function, and report no-match or
     ambiguity unless exactly one candidate succeeds.
   - Rewrite the callee/member name use to the selected concrete `FnDclNode`,
     then call existing argument finalization once.
   - Remove every remaining exact-match preference, coercion-score preference,
     declaration-order tie break, and pointer/reference first-match path.

10. **`src/c-compiler/ir/types/struct.c`**
    - During trait reconciliation, accept either a direct concrete method or
      exactly one overload candidate whose signature equals the substituted
      trait requirement.
    - Preserve the current default-method rule: clone a trait default only when
      the implementing namespace has no binding; an existing binding with no
      compatible concrete candidate is a conformance error.
    - Store only the selected `FnDclNode` in vtable and structural-match data.
      Reconstruct overload membership from each clone's `overloadsym`.

11. **`src/c-compiler/corelib/corenumber.c`**
    - Give each compiler-created overloaded intrinsic method a unique private
      concrete `namesym` and assign its existing operator spelling to
      `overloadsym`.
    - Keep genuinely non-overloaded helpers, including `len`, `maxlen`, and
      other directly named operations, as ordinary `FnDclNode` declarations.
    - Remove reliance on repeated same-spelled `iNsTypeAddFn` calls to create
      an implicit overload chain/group.

12. **`src/c-compiler/genllvm/genllvm.c`**
    - Generate functions only for concrete `FnDclNode`s and assert that an
      `FnOverloadDclNode` never reaches function generation.
    - Stop adding parameter-signature mangling merely because a concrete
      function is a method; its new concrete source name is already unique.
      Retain mangling required for generic instantiations.
    - Verify that a lowered overload call uses the selected concrete node's
      `llvmvar` and generated name.

13. **`src/c-compiler/shared/error.h` and the C call sites listed above**
    - Add distinct diagnostics for malformed overload declarations,
      concrete/overload name collisions, duplicate candidate signatures, no
      matching candidate, ambiguous candidates, generic overload declarations,
      and overload names used outside call position.
    - Remove reuse of `ErrorNotPublic` for a missing or non-matching method;
      visibility errors, lookup failures, no-match errors, and ambiguity errors
      must be distinguishable.

14. **Final verification, then `design/IR Nodes.md`,
    `design/Names and Namespaces.md`,
    `conesite/public/coneref/reffunc.html`,
    `conesite/public/coneref/refmethod.html`,
    `conesite/public/coneref/refmodule.html`, and
    `conesite/public/coneref/refmethop.html`**
    - Rebuild `conec`; compile `test/test.cone` and every positive fixture;
      require every negative fixture to fail with its expected diagnostic and
      source location.
    - Generate AST and LLVM IR and confirm every overload call is lowered to a
      concrete name, each concrete symbol is unique, and no overload node
      reaches generation. Run representative native results and compile the
      WebAssembly target so success is not based on parsing/type checking alone.
    - Search the compiler for `nextnode`, numeric best-match selection,
      declaration-order fallback, and raw candidate traversal outside the
      overload implementation; require all old overload techniques to be gone.
    - Only after verification passes, update the design documents from the
      temporary Phase 1 representation to the final concrete-name plus
      overload-name representation.
    - Replace conesite's obsolete same-name, `+`, and `|` syntax and its
      best-match/declaration-order descriptions with `fn concrete overload
      shared`, direct concrete-name use, call-only overload names, and the
      exactly-one-viable-candidate rule. Update method/operator examples to use
      the implemented syntax.
