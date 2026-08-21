# Cone design notes

Design context for compiler and language work. **The source code is the truth
for current behavior**; these notes explain how it works and why it is shaped
that way, and point at the code rather than reproducing it. They also describe
intended or incomplete behavior, always marked as such.

Page in the note you need. Do not load the folder.

```
design/
  northstar/    what Cone is trying to be, and how far the compiler currently is
  phases/       one note per compiler phase, plus the naming rules they implement
  nodes/        what is true of every IR node, and per-node notes
  compiler/     conec as a piece of software — how it is built, and how it stays fast
  diagnostics/  how to find out what the compiler does, and how to say it is wrong
```

**The test for `northstar/`: if `conec` were rewritten from scratch, would this
note survive?** The other four folders describe *this* compiler — its phases,
its nodes, its construction, its tooling — and would be thrown away with it. A
northstar note states what the language is aiming at.

Each has two halves: **the aim**, and **the current distance from it**. The
second is the actionable half, and it is measured rather than asserted.

**Where the aim and the code disagree, that is a question, not automatically a
defect.** These principles were largely worked out *while* the language was
being built — the author's own posts describe going back to first principles
mid-stream and finding that writing them down deepened the understanding. So a
divergence may mean the code has drifted, or may mean the articulation moved
ahead of it deliberately. Resolve it; do not assume which side is wrong.

The author's writing is the source for the aims: `conesite/public/*.html`
outside `coneref/`, and the posts under `ProgLing/plingsite/content/post/`.
Where a note states an aim, it credits the post that argues for it — those carry
the general case, and the note carries what it means for Cone.

## By phase

```
parse  ->  name resolution  ->  type check  ->  generation
                                    |
                                    +--> flow analysis, per function
```

| Phase | Note | Owns |
| --- | --- | --- |
| 1 | [Parse](phases/parse.md) | lexer, parser, desugaring, module loading, parse-time namespaces |
| 2 | [Name Resolution](phases/name-resolution.md) | binding names, deciding types from values, hooking and scopes |
| 3 | [Type Check](phases/type-check.md) | *when* a declaration is checked — demand, marks, re-entry, size, circularity |
| 3 | [Type Check Reasoning](phases/type-check-reasoning.md) | *what* the checks decide — coercion, overloads, casts, borrows, tuples |
| 4 | [Flow Analysis](phases/flow.md) | moves, alias counting, drops, permissions, escape. Runs per function, inside phase 3 |
| 5 | [Generation](phases/generation.md) | LLVM type lowering, allocation layout, pointer levels, output |

[Names and Namespaces](phases/names-and-namespaces.md) sits with them: it is the
*rules* — what a name means, visibility, imports, aliases, overloading — as
against how name resolution implements them. It is not a phase, and the rules it
states are enforced from parse and type check as well; it lives here because it
is read beside the phase that carries most of them.

## Northstar

**Two aims: performance, and agility.** Everything else serves one of them.

- **Performance** — the program is fast, and the programmer has the levers to
  make it faster. Memory technique is where the orders of magnitude are.
- **Agility** — the program stays changeable. **Modularity** and **safety** are
  both in service of this: modularity so a change stays local, safety so a
  change is caught when it is wrong rather than in production.

Stating it this way names the tension the public four — *fast, fit, friendly,
safe* — leave implicit. **Performance work usually costs agility** (hand-tuned
code is rigid) **and agility mechanisms usually cost performance**
(indirection, abstraction, bookkeeping). A language claiming both is claiming to
reduce that trade-off, and that claim is what the notes below have to hold up.

| Note | Serves | The aim | The distance |
| --- | --- | --- | --- |
| [References and Regions](northstar/references-and-regions.md) | **both** | Memory strategy chosen per object, with safety preserved across all of them | mechanism built, two regions ship; the strategies that motivate it — arena, pool, tracing GC — are not written |
| [Performance](northstar/performance.md) | performance | Give knowledgeable programmers the levers for proven high-performance strategies | most levers unbuilt; what exists is the machinery making them cheap to add and free to skip |
| [Modularity](northstar/modularity.md) | agility | Every layer — block, function, type, thread, module — surfacing the same three strategies | all three at function and type; only isolation at module; no thread layer; separate compilation does not work |
| [Safety](northstar/safety.md) | agility | Memory and type safety without a garbage collector, at no runtime cost | a scorecard: what is checked, what is not, and the four shapes the gaps take |

**References and regions is where the two axes meet**, which is why it is the
most distinctive thing in the language: one construct — a region-decorated,
permission-decorated reference — is simultaneously the performance lever and the
safety mechanism. If the claim to reduce the trade-off fails anywhere, it fails
there first.

*Not placed by this framing*: **fit** — "programs pack a lot of power for their
size, both as source files and as delivered executables" — reads as partly
performance and partly agility, and has no note. Whether it is a design
principle with content or a positioning claim is worth deciding.

Each note states an aim and then the measured distance from it. The second half
is the actionable one.

## The compiler

`conec` as software, as against what it does to a program.

| Note | Contents |
| --- | --- |
| [Architecture](compiler/architecture.md) | One node family per source pair, the uniform per-phase function set, centralized dispatch, the one-way dependency, and where the boundaries are drawn |
| [Performance](compiler/performance.md) | The arena, interning, memoization, and what is deliberately not optimized |

## By task

Most real work crosses phases. Start here instead.

| I want to… | Go to |
| --- | --- |
| add or change an operator | [Parse](phases/parse.md), "Adding an operator" — six edits spanning parse, `corelib/` and generation |
| add a new IR node tag | [IR Nodes](nodes/_index.md), "Adding a node tag" — every dispatch arm, and which of them report a missing one |
| change what syntax means | [Parse](phases/parse.md), "What the parser leaves undecided", then [Name Resolution](phases/name-resolution.md), "What it retags" |
| work out why a name will not resolve | [Name Resolution](phases/name-resolution.md), "Hooking" onward; the rules are in [Names and Namespaces](phases/names-and-namespaces.md) |
| work out why a value is or is not accepted | [Type Check Reasoning](phases/type-check-reasoning.md), "The verdict vocabulary" and "Coercion" |
| change a call, a method, or overloading | [Type Check Reasoning](phases/type-check-reasoning.md), "Calls, methods and overloads" |
| fix a double release, a leak, or a bad move | [Flow Analysis](phases/flow.md), "Moves and counting" onward, then [Generation](phases/generation.md), "The allocation header" |
| change ownership, borrowing, or lifetimes | [References and Regions](northstar/references-and-regions.md) for the model, then [Flow Analysis](phases/flow.md) for what enforces it |
| know whether a safety property actually holds | [Safety](northstar/safety.md) — the scorecard, and why a clean compile proves less than it looks like |
| know what something costs at runtime | [Performance](northstar/performance.md) |
| add a file, a node family, or a phase | [Architecture](compiler/architecture.md) |
| understand how a program is composed from pieces | [Modularity](northstar/modularity.md) |
| change modules, imports, or what a compile emits for each of them | [module](nodes/module.md) — the model, and what it has not decided |
| work out why the compiler is slow | [Compiler Performance](compiler/performance.md) |
| emit different LLVM, or fix a miscompile | [Generation](phases/generation.md), "Pointer levels", before writing any cast, GEP, load or store |
| understand a node end to end | [IR Nodes](nodes/_index.md), "Per-node notes", and `nodes/` |
| find out what the compiler is actually doing | [Measuring](diagnostics/measuring.md) — probes, `--ir`, `--llvmir`, `--checktree` |
| add or change a diagnostic | [Error Codes](diagnostics/error-codes.md) |
| add or update test coverage | [Test Suite](diagnostics/test-suite.md) |
| find a built-in type, operator method, or intrinsic | `corelib/` — see the family map in [IR Nodes](nodes/_index.md) |

## Nodes

[IR Nodes](nodes/_index.md) covers what is true of every node — tag groups,
header fields, the three sentinels, `--checktree`, the arms a new tag needs —
and carries the manifest for the per-node notes beside it.

| Node | Note |
| --- | --- |
| `FnCallNode` — calls, methods, operators, field access, indexing | [fncall](nodes/fncall.md) |
| `StructNode` — struct, trait and union | [struct](nodes/struct.md) |
| `RefNode` — references, borrows, allocations, slices, virtual refs | [references](nodes/references.md) |
| `VarDclNode`, `FieldDclNode`, `ConstDclNode` | [vardcl](nodes/vardcl.md) |
| `ModuleNode`, `ImportNode`, `ProgramNode` — and the module/package/compilation-unit model | [module](nodes/module.md) |
| `NameUseNode` — every appearance of a name | [nameuse](nodes/nameuse.md) |
| `AssignNode` | [assign](nodes/assign.md) |
| `CastNode` — `as`, `into`, `is`, and injected coercions | [cast](nodes/cast.md) |
| `BreakRetNode` — `return`, `break`, `continue`, `blockret` | [return](nodes/return.md) |
| `BlockNode` — blocks and loops | [block](nodes/block.md) |
| `IfNode` — `if`, `elif`, `else`, and `match` | [if](nodes/if.md) |
| literals — `nil`, numbers, strings, arrays, type literals | [literals](nodes/literals.md) |
| `GenericInfo`, `GenVarDclNode`, `MacroDclNode`, cloning | [generic](nodes/generic.md) |

## Diagnostics

| Note | Contents |
| --- | --- |
| [Measuring](diagnostics/measuring.md) | How to find out what the compiler actually does, and how to read what it produced |
| [Error Codes](diagnostics/error-codes.md) | Ranges, reporting, adding a code, when one code carries several causes, cascade suppression |
| [Test Suite](diagnostics/test-suite.md) | Adding or updating coverage: the groups, choosing a scenario, what to assert |

## Conventions

Filenames are kebab-case. Code pointers name a file and a function, **never a
line number** — and never a section number in another note; quote its heading
instead. Both rot silently and nothing checks them. Each note states its
provenance near the top: measured, or read from source.

**Rationale is normative, not historical.** Explain a shape against what a
reader would otherwise assume — the industry-standard alternative, the obvious
simpler design, the intuition the name invites. Do not narrate what the project
tried and reverted; nobody reading a note needs the changelog, and a decision
recounted as an episode reads as drama rather than argument.

**That includes the note's own history.** A note is not a changelog of itself.
It never says what it used to list, what has since been fixed, or when a claim
was checked — git holds all three, and a reader asking how the compiler behaves
is not asking any of them. **When a note's claim stops being true, delete the
claim.** Do not leave it annotated as fixed, and do not replace a hazard with a
sentence saying it is gone: an absent hazard is the normal state and says
nothing. The same goes for present behavior described as a delta — "now reports",
"no longer asserts", "used to be `assert(0)`" all state the current fact in a
form that decays the moment the previous state is forgotten. Write the fact.

The same goes for names. A reader assumes things are named well, so a note
explains a name **only where it is not doing its job intuitively** — and then
against the reading it wrongly invites, not against a name it used to have.

**A design note says how a subsystem works. A work item says what to do about
it.** The dependency runs one way: **a work item may point at a design note; a
design note never points at a work item.** That keeps a note from going stale
when an item is closed, renamed or emptied — and it means finishing an item
prompts the question of which notes, tests and reference pages now need
updating, which only works if the pointers run that direction.

So a note states a hazard as a fact about the current mechanism and says what
would settle it. It does not say who owns fixing it.

The change discipline — that a code change is not finished until its notes are —
is in `CLAUDE.md`.
