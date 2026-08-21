Every phase works on the same heterogeneous IR. This note covers what is true of
**all** nodes: how a node declares what it is, the header fields every node
carries, the three type sentinels, and what `--checktree` verifies. It is also
the manifest for the per-node notes.

What each phase *does* to these nodes is in the phase notes:
[Parse](../phases/parse.md), [Name Resolution](../phases/name-resolution.md),
[Type Check Phase](../phases/type-check.md) and
[Type Check Reasoning](../phases/type-check-reasoning.md),
[Flow Analysis](../phases/flow.md), [Generation](../phases/generation.md).

*Provenance: read from source, plus measured claims carried over from
[Type Check Phase](../phases/type-check.md). See [Measuring](../diagnostics/measuring.md).*

## 1. A node is its tag

`inode.h` is the truth for the tag list; do not mirror it here. What matters is
that **the tag encodes group membership in its high bits**, so a predicate is a
mask test rather than a list of tags:

| Bits | Constant | Predicate |
| --- | --- | --- |
| `0x0000` | `StmtGroup` | statements, returning no value |
| `0x4000` | `ExpGroup` | `isExpNode` — returns a typed value |
| `0x8000` | `TypeGroup` | `isTypeNode` — defines or names a type |
| `0xC000` | `MetaGroup` | `isMetaNode` — generic, macro, metaconditional |
| `0x2000` | `NamedNode` | `isNamedNode` — declares a name |
| `0x1000` | `MethodType` | `isMethodType` — a type that supports methods |

**Two of these predicates are not pure mask tests, and both matter.**
`isTypeNode` is the mask *plus* `itypeIsGenericType`, so that an unlowered
`Box[i64]` counts as a type before type check replaces it — without which
`*Box[i64]` reads as a dereference. `isMethodType` inherits that, being
`isTypeNode` and the bit. Separately, `isExpOrMacroNode` counts a parameterless
macro name as an expression: it is a meta node until type check expands it, so
any position deciding by node kind whether a statement can give a value has to
count it, or it is rejected before it gets the chance to expand.

Node structs are castable, and there are four headers, each extending the last:

| Header | Adds | Used by |
| --- | --- | --- |
| `INodeHdr` | the fields in section 2 | every node |
| `IExpNodeHdr` | `vtype` | every expression node |
| `ITypeNodeHdr` | `llvmtype` | most type nodes |
| `INsTypeNodeHdr` | `nodelist`, `namespace`, `dropfn` | types with methods — struct, number, enum, permission |

Most type nodes carry `vtype` because they build on `IExpNodeHdr`: the parser
cannot always tell a type from a value expression when it builds the node, so
both must have somewhere to put one.

**Two type nodes are the exception and will fault if you assume otherwise.**
`FnSigNode` and `VoidTypeNode` carry a bare `INodeHdr` — no `vtype`, no
`llvmtype`. Casting either to `IExpNode*` reads whatever follows the header.
`AbsenceNode`, which backs the three sentinels, carries `IExpNodeHdr` despite
being retagged into the type group.

**The source tree is the family map.** To find a node, go to the directory its
group names, then to the file its concept names — `enum NodeTags` in `inode.h`
lists every tag with a one-line comment, and the `.h` beside each `.c` carries
the struct:

| Directory | Holds |
| --- | --- |
| `ir/stmt/` | declarations and statements — module, fn, var, field, const, import, return, break, continue, swap |
| `ir/exp/` | expressions — nameuse, literals, fncall, block, if, logic, assign, borrow, allocate, cast, deref, sizeof, tuples, array and type literals |
| `ir/types/` | types — number, struct, array, reference, arrayref, pointer, fnsig, enum, permission, region, lifetime, typedef, tuple, void |
| `ir/meta/` | generics and macros |
| `ir/` | shared machinery — dispatch, namespaces, name and type tables, cloning, flow, `--checktree` |
| `corelib/` | **the built-in types themselves** — `corelib.c` declares the permissions and, as Cone source in `corelibSource`, the `so` and `rc` regions, `Option` and `Result`; `corenumber.c` builds the number types in C and hangs every operator method and intrinsic off them |
| `shared/` | diagnostics (`error.h` is the `ErrorCode` list), memory arena, file, options, timer, UTF-8 |

`corelib/` is the one most often looked for in the wrong place. A built-in
method such as integer `+` is **not** written in Cone text: `corenumber.c` calls
`iNsTypeAddFn` with a `newIntrinsicNode`. Only the region and option types are
Cone source.

## 2. The header every node carries

| Field | Type | Purpose |
| --- | --- | --- |
| `instnode` | `INode*` | the generic or macro instantiation that created this node, else NULL |
| `lexer` | `Lexer*` | source file — `url` and `source` |
| `srcp` | `char*` | start of the parsed token |
| `linep` | `char*` | start of the line holding `srcp` |
| `linenbr` | `uint32_t` | 1-based line number |
| `tag` | `uint16_t` | node kind plus group bits (section 1) |
| `flags` | `uint16_t` | node-specific flags |

**`flags` is not one namespace.** The same bit means different things on
different node families — `0x0020` is `IsMixin` on a `FieldDcl` and `SameSize`
among the type flags. Check every declaration family before claiming a bit. **A
collision has no diagnostic**: `0x0040` overlapping `HasTagField` stops type
checking every tagged union and reports nothing.

**A node built after parsing takes the lexer's *current* position**, which is
end of file. `newNode` reads `lex->tokp`, so an injected node points at nothing
unless `inodeLexCopy` (or the `copyNodeLex` macro) is called on it. Every phase
that injects nodes has to do this, and diagnostics are how you find out it was
forgotten.

**Anything added to `flags` needs the clone functions audited.** Generic and
macro instantiation clones declarations, and a clone carrying state it should
have cleared produces an instance that silently skips its own check — no
diagnostic anywhere. See `ir/clone.c` and the `clone*Node` functions.

## 3. The two walks mutate through double pointers

`inodeNameRes` and `inodeTypeCheck` both take `INode **`, because both phases
**replace** nodes rather than only annotating them. A `NameUseTag` becomes a
`VarNameUseTag` or a `TypeNameUseTag`; an `FnCallTag` becomes a `FldAccessTag`,
an `ArrIndexTag` or a `TypeLitTag`; a generic instantiation is replaced by the
instance it names.

Two consequences worth internalizing:

- **A phase that walks a declaration twice corrupts it.** That is why the
  `TypeChecked` mark is a correctness requirement, not an optimization — see
  [Type Check Phase](../phases/type-check.md), "The two marks".
- **A node tag is not stable across phases.** Code that runs after type check
  can rely on tags earlier phases produce having been replaced; code that runs
  during type check cannot.

`inodeTypeCheck` also takes `expectType`. Only `blockTypeCheck` and
`ifTypeCheck` consume it — see
[Type Check Reasoning](../phases/type-check-reasoning.md), "How an expected type reaches
an expression".

## 4. The three type sentinels

`corelib.c` builds three singletons that are all `AbsenceNode`s retagged
`UnknownTag`. They are distinguished by **identity**, never by tag, so a
comparison against one must be `==` and never a tag test:

| Singleton | Means | Effect on diagnostics |
| --- | --- | --- |
| `unknownType` | not inferred yet | none — a type may still be worked out |
| `noCareType` | the receiver does not care what type comes back | none |
| `errorType` | already reported as bad | **suppresses** follow-on complaints |

Conflating `errorType` with `unknownType` buys either spurious cascades or
masked real failures, which is why they are separate objects sharing a tag
rather than one object.

`errorType` is installed by an error path that has reported a diagnostic and has
no honest type to give the node. `itypeMatches` returns `EqMatch` when either
side is `errorType`, and `iexpMultiInfer` skips a branch carrying it, so
everything derived from an already-reported node stays quiet. `inodeIsError`
(`inode.c`) is the predicate for "an earlier diagnostic already covered this".

Where a whole subtree has to be replaced rather than one node retyped,
`newErrorNode` (`void.c`) builds an expression-shaped stand-in: a fresh
`AbsenceNode` carrying the replaced expression's source position, with
`errorType` as its value type. It is per-site rather than a singleton precisely
because it holds a position.

**A constructor must initialize every field it declares — not just `vtype`.**
Nodes come from `memAllocBlk`, a bump allocator that **never frees and never
zeroes**. An unset field holds arena garbage rather than NULL, which is why one
such defect faulted at an arbitrary address instead of at zero, and why
`--checktree` could not see it. A node that has not been type checked yet
carries `unknownType`.

This is one root cause behind several separate defects — an uninitialized
`genname`, `vtblidx`, `life` and `phiCnt` have each been found the same way. The
allocator is right for a short-lived compiler; the discipline it demands is that
**a partially-initialized struct is indistinguishable from a fully-initialized
one** until something reads the hole.

## 5. --checktree

`ir/checktree.c` walks the program after semantic analysis and reports
`ErrorBadTree` for any expression node with no value type or any block with no
statement list. It runs whether or not errors were reported, because a phase
that reports a bad program and returns early is exactly what leaves those holes.
`test/run.py` passes `--checktree` on every compile of every category.

It does not descend into type declarations: a type may refer to itself through a
reference, so that graph has cycles, and a type node's own `vtype` is not what
these defects leave empty.

## 6. Adding a node tag: every arm you must add

A tag is dispatched from a dozen `switch (node->tag)` statements spread over
five files. **Missing one used to be silent in the release build** — the
`default:` arms were `assert(0)`, which `NDEBUG` compiles out, so the node fell
through into whatever followed. They now call `errorUnreachable`, so a missing
arm aborts the compile with a source position instead. That turns a miscompile
into a bug report; it does not make the tag work. Work the list.

| File | Function | Add an arm when |
| --- | --- | --- |
| `ir/inode.h` | `enum NodeTags` | always — and get the group bits right, section 1 |
| `ir/inode.c` | `inodePrintNode` | always. Miss it and `--ir` silently prints nothing for the node |
| | `inodeNameRes` | the node survives parsing |
| | `inodeTypeCheck` | the node survives name resolution |
| | `inodeGetName`, `inodeIsDcl` | it declares a name |
| `ir/clone.c` | `cloneNode` | always — a tag this switch does not list is a gap the file's own comment flags |
| `ir/checktree.c` | `checkNode` | it has children to descend into |
| `ir/flow.c` | `flowLoadValue` | it can appear in a value position |
| | `flowHandleMove`, `flowIsLvalRead` | it can be an lval, or the source of a move |
| `genllvm/genltype.c` | `_genlType` | it is a type |
| `genllvm/genlexpr.c` | `genlExpr` | it produces a value |
| | `genlAddr` | its address can be taken |
| `genllvm/genlstmt.c` | `genlBlock` | it is a statement with its own control flow |
| `genllvm/genllvm.c` | `genlGlobalSyms`, `genlGlobalImpl` | it can appear in the global area |

Then: initialize `vtype` in the constructor (section 4), clear any analysis
marks in the clone function (section 2), and add a scenario in the owning test
group. If the node is injected rather than parsed, call `inodeLexCopy` on it.

## 7. Per-node notes

Most nodes need no note. A node earns one when its behavior is **spread across
phases** and is not recoverable by reading one file — when the parser builds one
shape, name resolution rewrites it, type check lowers it into something else,
and generation depends on that lowering. A node whose whole story is one
`*TypeCheck` function does not earn one; it earns a row here and a good comment
in its header.

Notes are one per **source pair** (`ir/exp/fncall.c` + `.h`), not one per tag,
because that is how the code already groups them: `literal.c` owns four tags and
`logic.c` owns three.

A per-node note answers these six questions, in this order, and links to the
phase notes for mechanism rather than restating it:

1. **Shape** — the struct, what each field means, and which fields are valid
   when.
2. **Constructors** — every function that builds one, and what each is for.
3. **Parse** — what syntax produces it, and what the parser leaves undecided.
4. **Name resolution** — what it binds, what it replaces this node with.
5. **Type check** — what it decides, what it lowers this node into, and what
   `vtype` ends up as.
6. **Flow and generation** — what flow injects or accounts for, and what
   generation requires.

| Node source | Note | Why it earns one |
| --- | --- | --- |
| `ir/exp/fncall.c` | [fncall](fncall.md) | one shape serves eight syntaxes; the largest function in the compiler decides which |
| `ir/types/struct.c` | [struct](struct.md) | struct, trait and union are one node; layout, inheritance, vtables and drops |
| `ir/types/reference.c`, `arrayref.c`, `ir/exp/borrow.c`, `allocate.c` | [references](references.md) | seven tags on one struct, across two node groups |
| `ir/stmt/vardcl.c`, `fielddcl.c`, `const.c` | [vardcl](vardcl.md) | three declaration nodes that differ mostly in what they lack |
| `ir/exp/nameuse.c` | [nameuse](nameuse.md) | four tags, retagged mid-pipeline; two lowerings and the move diagnostics |
| `ir/exp/assign.c` | [assign](assign.md) | mutability and ownership are enforced in flow, not type check |
| `ir/exp/cast.c` | [cast](cast.md) | three syntaxes plus two injected forms; generation re-checks what type check could not |
| `ir/stmt/return.c`, `break.h` | [return](return.md) | one struct serves four tags; placement rule and escape check live in three different phases |
| `ir/exp/block.c` | [block](block.md) | loops are the same node; `blockret` is injected by two phases |
| `ir/exp/if.c` | [if](if.md) | a flat alternating list, an identity-compared sentinel, and `match` lowered into it |
| `ir/exp/literal.c`, `arraylit.c`, `typelit.c` | [literals](literals.md) | the array node is also the array type; the type literal is also a call |
| `ir/meta/generic.c`, `macro.c`, `ir/clone.c` | [generic](generic.md) | no node of its own; cloning stands in for name resolution |

## 8. FnOverloadDcl

`FnOverloadDclNode` (see `ir/stmt/fndcl.h`) is the namespace binding for an
explicitly declared overload name. It holds only the common `INode` header, its
`namesym`, and an ordered `Nodes *overloads` vector of the concrete `FnDclNode`
candidates that declared they overload that name. It has no function type, body,
LLVM value, or generated symbol: every executable implementation remains a
separate `FnDclNode` bound to its own unique concrete name, and a call is always
lowered to the concrete node that was selected.

`FnDclNode` carries `Name *overloadsym`, which is the overload name this
declaration also answers to, or `NULL` when it has only its concrete name. That
field is what lets a cloned method (a generic instantiation or an inherited
trait default) rebuild its overload membership in the cloned type's namespace,
since the overload node itself is never copied.

Name resolution of an `FnOverloadDclNode` is an explicit no-op, because the
module or type that owns the candidates already walks each concrete `FnDclNode`;
walking its vector again would resolve the same function bodies twice. Type
checking only compares the candidates' already-checked signatures, to report two
candidates that would accept the same arguments.

How a call selects among the candidates is
[Type Check Reasoning](../phases/type-check-reasoning.md) section 7.
