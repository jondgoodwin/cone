How the compiler's own source is organized, and the rules for adding to it.

This is about the compiler as a piece of software — as against
[the phases](../_index.md), which are about what it does to a program. Read it
before adding a node, a phase, or a file.

*Provenance: read from source.*

## Key principles

1. **One node family per source pair.** `ir/exp/fncall.c` and `.h` own
   `FnCallNode` entirely — its struct, constructors, and its behavior in every
   phase.
2. **A node exports one function per phase, named uniformly.** `newXNode`,
   `cloneXNode`, `xPrint`, `xNameRes`, `xTypeCheck`, `xFlow`. If a phase does
   nothing to a node, the function is absent rather than empty.
3. **Dispatch is centralized; behavior is not.** One `switch (tag)` per phase in
   a shared file routes to the per-node function. Adding a node means adding
   arms to those switches — nothing else changes.
4. **Each walk carries its own state struct**, and they are deliberately not
   merged.

## The layout

| Directory | Holds | Depends on |
| --- | --- | --- |
| `parser/` | lexer, parser | `ir/` |
| `ir/` | the node definitions and the two semantic walks | `shared/` |
| `ir/exp/`, `ir/stmt/`, `ir/types/`, `ir/meta/` | one pair per node family — 16, 12, 14 and 3 files | `ir/` |
| `corelib/` | the built-in types, in C and in Cone source | `ir/` |
| `genllvm/` | LLVM lowering | `ir/`, LLVM |
| `shared/` | diagnostics, arena, files, options, timers, UTF-8 | nothing |

The dependency runs one way: `shared/` knows nothing, `ir/` knows `shared/`,
everything else knows `ir/`. **`genllvm/` is the only directory that includes
LLVM headers** — the front end has no LLVM dependency at all, which is what
would make a second back end possible.

`ir.h` is the aggregating header: a node file includes it and gets every node
type. That is why per-node headers can refer to each other's structs without an
include graph to maintain.

## The node contract

A node family's header declares the struct; its `.c` declares every function
that acts on it. Four header macros layer the common fields — `INodeHdr`,
`IExpNodeHdr` (adds `vtype`), `ITypeNodeHdr` (adds `llvmtype`),
`INsTypeNodeHdr` (adds a namespace and method list) — so a node opts into
exactly what it needs and stays castable to the shallower forms.

**The uniform function set is the modularity contract.** Because every node
exports the same names, the central dispatchers are mechanical, and a reader
looking for "what does type check do to a borrow" knows the answer is
`borrowTypeCheck` in `ir/exp/borrow.c` without searching.

The full list of dispatchers a new tag must be added to is in
[IR Nodes](../nodes/_index.md), "Adding a node tag" — and the reason that list is
worth having is that **a missing arm passes silently in some of them.** In
`inodeNameRes`, `inodeTypeCheck`, `inodeGetName`, `flowLoadValue`, `_genlType`,
`genlExpr`, `genlAddr` and `genlGlobalImpl`, the `default:` arm calls
`errorUnreachable`, which reports and stops; each used to be `assert(0)`, which
`NDEBUG` compiled out of the Release build. The rest let a tag they do not list
through, some of them deliberately.

## Walk state

| Struct | Carried by | Lives in |
| --- | --- | --- |
| `NameResState` | name resolution | `ir/ir.h` |
| `TypeCheckState` | type check | `ir/ir.h` |
| `FlowState` | flow analysis | `ir/flow.h` |
| `CloneState` | instantiation | `ir/clone.h` |
| `GenState` | generation | `genllvm/genllvm.h` |

**They are separate on purpose.** One merged struct would compile perfectly well
and would hand every `*NameRes` function an `fn` it must never read. Two structs
make that a compile error instead of a convention — the same reasoning that puts
`llvmtype` on `ITypeNodeHdr` rather than on every node.

## Where the boundaries are drawn, and why

**`corelib/` is the line between compiler-defined and Cone-defined.** The number
types are built in C, with every operator method hung off them as an intrinsic.
The regions `so` and `rc`, plus `Option` and `Result`, are **Cone source
compiled at startup**. That split is not arbitrary: a type needs C when the
compiler must know its identity (a number type is referenced by name from a
dozen places), and can be Cone source when it only needs to exist.

**`shared/` has no upward dependency**, which is what lets the arena, the
diagnostics and the option parser be used from the parser and the back end
alike.

**The front end does not know about LLVM.** Every LLVM handle lives behind a
field (`llvmtype`, `llvmvar`, `typeinfo`) that the front end sets to NULL and
never reads.

## Hazards

- **Adding a field to a node means auditing its clone function.** A clone that
  carries state it should have cleared produces an instance that silently skips
  its own check.
- **Adding a flag bit means checking every node family that uses that word.**
  `flags` is not one namespace.
- **A node's tag is not stable across phases**, so a file that acts on a node
  must know which side of lowering it runs on.
- **`ir.h` aggregating everything means a header change rebuilds the world.**
  That is the accepted cost of not maintaining an include graph.
- **`genllvm/` reaching back into front-end mutation** would break the one-way
  dependency. Generation reads; it does not decide.

## What lives elsewhere

- Every dispatcher a new tag must be added to: [IR Nodes](../nodes/_index.md)
- What each phase does: [the phase notes](../_index.md)
- Interning, memoization and the arena: [Compiler Performance](performance.md)
- Contributor conventions — style, comments, commit discipline: `CLAUDE.md`
