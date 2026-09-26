# Cone design notes

Design context for compiler and language work. **The source code is the truth
for current behavior**; these notes explain how it works and why it is shaped
that way, and point at the code rather than reproducing it. They also describe
intended or incomplete behavior, always marked as such.

Page in the note you need. Do not load the folder.

## What Cone is for

**Two aims: performance, and agility.** Everything else serves one of them.

- **Performance** — the program is fast, and the programmer has the levers to
  make it faster. Memory technique is where the orders of magnitude are.
- **Agility** — the program stays changeable. Modularity and safety are both in
  service of this: modularity so a change stays local, safety so a change is
  caught when it is wrong rather than in production.

Stating it this way names the tension the public four — *fast, fit, friendly,
safe* — leave implicit. **Performance work usually costs agility** (hand-tuned
code is rigid) **and agility mechanisms usually cost performance** (indirection,
abstraction, bookkeeping). A language claiming both is claiming to reduce that
trade-off, and every note here has to hold that claim up.

**Attention is the scale both aims are priced in.** A safety, modularity or
performance mechanism that costs more attention than it saves is a bad trade
whatever it guarantees. [Expressiveness and Attention](expressiveness-and-attention.md)
carries that argument.

## Two kinds of note

```
doc/design/            the topic notes, one concern followed across the whole compiler —
                       safety, modularity, memory — plus the naming rules
compiler/c/doc/
  phases/              one note per compiler phase
  nodes/               what is true of every IR node, and per-node notes
  compiler/            conec as a piece of software — how it is built, and how it stays fast
  diagnostics/         how to find out what the compiler does, and how to say it is wrong
```

**A topic note follows one concern across every phase and node it touches.** A
**structure note** — phases, nodes, compiler, diagnostics — describes how this
compiler is built. The two kinds live apart for that reason: the topic notes sit
with the language, and the structure notes sit inside the compiler they describe
and are retired with it.

**Both open with the principles they own**, then the design those principles
rule over, then what of it is built. The three are separated by section, never
by annotation sprinkled through prose.

**What differs is whose principles they are**, and the old northstar test sorts
them: *if `conec` were rewritten from scratch, would this survive?* A topic
note's principles are about the **language**, and survive. A structure note's are
about **this compiler** — one node family per source pair, the linker's
inclusion granularity is the function — and are thrown away with it. Both are
real, and both rule over decisions downstream of them.

## Principles: own them, or inherit them

**A note states the principles it owns and references the ones it inherits.**
Writing out an inherited principle is how two notes come to disagree about it —
which has already happened, with the package model stated in full in both
[Modularity](modularity.md) and [module](../../compiler/c/doc/nodes/module.md).

**The owner is the note whose subject the principle is.** Symbol naming is a
rule about names, so [Names and Namespaces](names-and-namespaces.md)
owns it and [Generation](../../compiler/c/doc/phases/generation.md) carries only the lowering.
Attention as the scarce resource is about expressiveness, so that note owns it
and the rest point at it.

**Every principle names something it forbids or settles.** If you cannot say
what it would reject, it is decoration — cut it. *"Give programmers the levers"*
forbids nothing. *"The linker's inclusion granularity is the function, not the
object file"* settles a dozen choices downstream.

**A note that owns no principles writes no such section.** Absence by emptiness,
never by rule.

**A principle the author has not passed on is marked `[derived]`** — read from
the code and from a note's own prose rather than confirmed. Same inheritance rule
as the status tags: mark the section where that is true of all of it, override
inline where one differs. The marker goes when the author has reviewed it.

⚠ **The hazard `[derived]` exists for is not invention from nothing.** Most
principles are recoverable, because a position that rules on something has left
evidence in the shape of the code. **The hazard is promoting an implementation
accident to a principle** — asserting intent behind something that merely fell
out of how it was built. [assign](../../compiler/c/doc/nodes/assign.md) carries a worked example of
both kinds in one section.

**A principle in a topic note names the structure note that implements it.** A
mechanism stated in a topic with nothing named to carry it is a mechanism with no
owner.

⚠ **A new topic note asserts that a concern exists, and is the author's to
authorize.** Keeping its measured distances current is not.

## Where the aim and the code disagree

**That is a question, not automatically a defect.** These principles were largely
worked out *while* the language was being built — the author's own posts describe
going back to first principles mid-stream and finding that writing them down
deepened the understanding. So a divergence may mean the code has drifted, or may
mean the articulation moved ahead of it deliberately. Resolve it; do not assume
which side is wrong.

The author's writing is the source for the aims: `conesite/public/*.html`,
and the posts under `c:/src/progling/content/post/`.
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
| 1 | [Parse](../../compiler/c/doc/phases/parse.md) | lexer, parser, desugaring, module loading, parse-time namespaces |
| 2 | [Name Resolution](../../compiler/c/doc/phases/name-resolution.md) | binding names, deciding types from values, hooking and scopes |
| 3 | [Type Check](../../compiler/c/doc/phases/type-check.md) | *when* a declaration is checked — demand, marks, re-entry, size, circularity |
| 3 | [Type Check Reasoning](../../compiler/c/doc/phases/type-check-reasoning.md) | *what* the checks decide — coercion, overloads, casts, borrows, tuples |
| 4 | [Flow Analysis](../../compiler/c/doc/phases/flow.md) | moves, alias counting, drops, permissions, escape. Runs per function, inside phase 3 |
| 5 | [Generation](../../compiler/c/doc/phases/generation.md) | LLVM type lowering, allocation layout, pointer levels, output |

[Names and Namespaces](names-and-namespaces.md) sits with them: it is the
*rules* — what a name means, visibility, imports, aliases, overloading — as
against how name resolution implements them. It is not a phase, and the rules it
states are enforced from parse and type check as well; it lives here because it
is read beside the phase that carries most of them.

## Topics

**One note per concern the language takes a position on.** Each opens with the
principles it owns, carries the design they rule over, and states the measured
distance from it.

▸ **What earns a topic note is a position on a trade-off that could have gone
the other way** — not an aspiration.

⚠ **A correction, 12 September 2026.** This index claimed Performance stated an
aspiration rather than a position, and that it carried none of the decisions in
`conesite/public/fast.html`. **Both were wrong, and both came from reading the
row in this table instead of the note.** The note carries five explicit bets, a
what-is-free table, a what-costs table, and the strongest principle in the
folder: **no construct's cost is invisible at the point you write it.** ▸ **The
summary here was thin; the note was not.**

| Note | Serves | The position | The distance |
| --- | --- | --- | --- |
| [References and Regions](references-and-regions.md) | **both** | Memory strategy chosen per object, with safety preserved across all of them | mechanism built, two regions ship in core and a third, reference counting with weak references, as a library package; an arena and a generational pool ship as library values, not regions an allocation names; tracing GC is not written |
| [Performance](performance.md) | performance | Give knowledgeable programmers the levers for proven high-performance strategies | most levers unbuilt; what exists is the machinery making them cheap to add and free to skip |
| [Modularity](modularity.md) | agility | Every layer — block, function, type, thread, module, program — surfacing the same six strategies | composition, namespace and encapsulation broadly present; substitution, generativity and extensibility thin out above the type layer; no thread layer; the program layer has no namespace at all |
| [Safety](safety.md) | agility | Memory and type safety without a garbage collector, at no runtime cost | a scorecard: what is checked, what is not, and the four shapes the gaps take |
| [Expressiveness and Attention](expressiveness-and-attention.md) | **the scale, not an aim** | Programming as Lego assembly — small, uniform, opaque interfaces. Attention is the scarce resource both aims are priced in | the mechanisms meant to deliver it are the unbuilt ones: no thread layer so no actors, no module substitution, borrowing narrowed only by convention |

**References and regions is where the two axes meet**, which is why it is the
most distinctive thing in the language: one construct — a region-decorated,
permission-decorated reference — is simultaneously the performance lever and the
safety mechanism. If the claim to reduce the trade-off fails anywhere, it fails
there first.

*Not placed by this framing*: **fit** — "programs pack a lot of power for their
size, both as source files and as delivered executables" — reads as partly
performance and partly agility, and has no note. The "power for their size" half
is now argued in Expressiveness and Attention; whether what remains is a design
principle with content or a positioning claim is still worth deciding.

⚠ **Open, with a recommendation: three strategies, or six?**
[Modularity](modularity.md) enumerates three — complexity isolation,
interface-based substitution, multi-use generation. The author's concept vault
enumerates **six** — composition, namespace, encapsulation, substitution,
generativity, extensibility.

▸ **The recommendation is six, and the argument is that the three cannot classify
two of Cone's most distinctive mechanisms.** Name-folding is a *namespace*
operation, and delegated inheritance is name-folding applied to types — under the
three it has nowhere to sit. A trait's fields being a requirement the implementer
declares at position 0, rather than state it inherits, is a *composition* fact and
files under none of them. **The layers already agree at six on both sides** — block, function, type,
thread/concurrency, module/package, program/service — **so only the strategies
were compressed, and the note is a 3×5 rendering of a 6×6 matrix.**

▸ **What the three do well survives the move.** Isolation decreases complexity
while substitution and generation increase coupling — on six, that becomes
composition, namespace and encapsulation neutral-to-reducing, and substitution,
generativity and extensibility increasing. **Keep it as an observation about what
each strategy costs, not as the taxonomy.**

⚠ **Not yet passed on by the author.**

## The compiler

`conec` as software, as against what it does to a program.

| Note | Contents |
| --- | --- |
| [Architecture](../../compiler/c/doc/compiler/architecture.md) | One node family per source pair, the uniform per-phase function set, centralized dispatch, the one-way dependency, and where the boundaries are drawn |
| [Performance](../../compiler/c/doc/compiler/performance.md) | The arena, interning, memoization, and what is deliberately not optimized |

## By task

Most real work crosses phases. Start here instead.

| I want to… | Go to |
| --- | --- |
| add or change an operator | [Parse](../../compiler/c/doc/phases/parse.md), "Adding an operator" — six edits spanning parse, `corelib/` and generation |
| add a new IR node tag | [IR Nodes](../../compiler/c/doc/nodes/_index.md), "Adding a node tag" — every dispatch arm, and which of them report a missing one |
| change what syntax means | [Parse](../../compiler/c/doc/phases/parse.md), "What the parser leaves undecided", then [Name Resolution](../../compiler/c/doc/phases/name-resolution.md), "What it retags" |
| work out why a name will not resolve | [Name Resolution](../../compiler/c/doc/phases/name-resolution.md), "Hooking" onward; the rules are in [Names and Namespaces](names-and-namespaces.md) |
| work out why a value is or is not accepted | [Type Check Reasoning](../../compiler/c/doc/phases/type-check-reasoning.md), "The verdict vocabulary" and "Coercion" |
| change a call, a method, or overloading | [Type Check Reasoning](../../compiler/c/doc/phases/type-check-reasoning.md), "Calls, methods and overloads" |
| fix a double release, a leak, or a bad move | [Flow Analysis](../../compiler/c/doc/phases/flow.md), "Moves and counting" onward, then [Generation](../../compiler/c/doc/phases/generation.md), "The allocation header" |
| change ownership, borrowing, or lifetimes | [References and Regions](references-and-regions.md) for the model, then [Flow Analysis](../../compiler/c/doc/phases/flow.md) for what enforces it |
| know whether a safety property actually holds | [Safety](safety.md) — the scorecard, and why a clean compile proves less than it looks like |
| know what something costs at runtime | [Performance](performance.md) |
| add a file, a node family, or a phase | [Architecture](../../compiler/c/doc/compiler/architecture.md) |
| understand how a program is composed from pieces | [Modularity](modularity.md) |
| change modules, imports, or what a compile emits for each of them | [module](../../compiler/c/doc/nodes/module.md) — the model, and what it has not decided |
| work out why the compiler is slow | [Compiler Performance](../../compiler/c/doc/compiler/performance.md) |
| emit different LLVM, or fix a miscompile | [Generation](../../compiler/c/doc/phases/generation.md), "Pointer levels", before writing any cast, GEP, load or store |
| understand a node end to end | [IR Nodes](../../compiler/c/doc/nodes/_index.md), "Per-node notes", and `nodes/` |
| find out what the compiler is actually doing | [Measuring](../../compiler/c/doc/diagnostics/measuring.md) — probes, `--ir`, `--llvmir`, `--checktree` |
| add or change a diagnostic | [Error Codes](../../compiler/c/doc/diagnostics/error-codes.md) |
| add or update test coverage | [Test Suite](../../compiler/c/doc/diagnostics/test-suite.md) |
| find a built-in type, operator method, or intrinsic | `corelib/` — see the family map in [IR Nodes](../../compiler/c/doc/nodes/_index.md); `Option`, `Result`, `so` and `rc` are Cone source in `packages/core/src/core.cone`, and so are the intrinsics declared with `@intrinsic`, whose registry is `ir/stmt/intrinsic.c` ([intrinsic](../../compiler/c/doc/nodes/intrinsic.md)) |
| find `core` or `stdio`, or change where packages are found | `packages/` at the repository's root — [Module](../../compiler/c/doc/nodes/module.md), "The packages folder" |

## By language feature

The other two tables route by *compiler* structure. This one routes by the
language itself — start here when you know what the feature is called to a
programmer but not which phase or node owns it. The reference pages under
`doc/reference/` are the user-facing description; the notes are the
design behind it.

| Category | Reference pages | Design note |
| --- | --- | --- |
| **Lexical and basic form** | `reftoken` · `refterm` · `refbasics` · `ebnf` | [Parse](../../compiler/c/doc/phases/parse.md) |
| **Expressions and control flow** | `refexpr` · `refif` · `refwhile` · `refeach` · `refblock` · `refmatch` · `refflow` | [block](../../compiler/c/doc/nodes/block.md) · [if](../../compiler/c/doc/nodes/if.md) · [return](../../compiler/c/doc/nodes/return.md) |
| **Functions** | `reffunc` · `refmethod` · `refmethop` · `refclosure` · `reffnref` · `refcloref` | [fncall](../../compiler/c/doc/nodes/fncall.md) |
| **Core types** | `reftypes` · `refnumber` · `refstruct` · `refenum` · `reftuple` · `refarray` · `reftypealias` · `refvoid` | [struct](../../compiler/c/doc/nodes/struct.md) · [literals](../../compiler/c/doc/nodes/literals.md) |
| **Traits and polymorphism** | `reftrait` · `reftraitvar` · `refinherit` · `refvirtref` · `refgeneric` | [struct](../../compiler/c/doc/nodes/struct.md) · [generic](../../compiler/c/doc/nodes/generic.md) |
| **References, permissions, regions** | `refrefs` · `refptr` · `refborref` · `refperm` · `refpermlock` · `refweakref` · `refarrayref` · `refallocref` · `refalloccust` · `refregionglo` · `refmove` · `reflifefn` | [references](../../compiler/c/doc/nodes/references.md) · [References and Regions](references-and-regions.md) · [Flow Analysis](../../compiler/c/doc/phases/flow.md) |
| **Lifetime and construction** | `refinitdrop` · `reftypemanage` | [Flow Analysis](../../compiler/c/doc/phases/flow.md) · [vardcl](../../compiler/c/doc/nodes/vardcl.md) |
| **Modules and packages** | `refmodule` · `refinclude` | [module](../../compiler/c/doc/nodes/module.md) |
| **Safety and trust** | `refsafety` · `reftypesafe` · `reftrust` · `refintrinsic` | [Safety](safety.md) · [intrinsic](../../compiler/c/doc/nodes/intrinsic.md) |
| **Error handling** | `refexcept` · `refoption` · `refresult` | ⚠ **no note** |
| **Metaprogramming** | `refmacro` · `refmeta` | [generic](../../compiler/c/doc/nodes/generic.md) |
| **Concurrency** | `refconc` · `refconccomm` · `refconcio` · `refcorout` | ⚠ **no note; no thread layer exists** |
| **Collections** | `reftypecoll` | ⚠ **no note** |

⚠ **A reference page shows the language's *intended* shape, not only what is
built** — see "The language reference" below. Three categories above have no
design note at all, and the concurrency one has no implementation either.

**Congo — the build tool — is documented with the tool**, not here: it is not
`conec`, and a tool's design and user docs sit with the tool.
`tools/congo/README.md` covers its commands, the manifest, the registries, the
package layout, include files and what a build does; the compiler's side of the
contract is [module](../../compiler/c/doc/nodes/module.md), "A described build".

## Nodes

[IR Nodes](../../compiler/c/doc/nodes/_index.md) covers what is true of every node — tag groups,
header fields, the three sentinels, `--checktree`, the arms a new tag needs —
and carries the manifest for the per-node notes beside it.

| Node | Note |
| --- | --- |
| `FnCallNode` — calls, methods, operators, field access, indexing | [fncall](../../compiler/c/doc/nodes/fncall.md) |
| `StructNode` — struct, trait and enum | [struct](../../compiler/c/doc/nodes/struct.md) |
| `RefNode` — references, borrows, allocations, slices, virtual refs | [references](../../compiler/c/doc/nodes/references.md) |
| `VarDclNode`, `FieldDclNode`, `ConstDclNode` | [vardcl](../../compiler/c/doc/nodes/vardcl.md) |
| `ModuleNode`, `ImportNode`, `ProgramNode` — and the module/package/compilation-unit model | [module](../../compiler/c/doc/nodes/module.md) |
| `NameUseNode` — every appearance of a name | [nameuse](../../compiler/c/doc/nodes/nameuse.md) |
| `AssignNode` | [assign](../../compiler/c/doc/nodes/assign.md) |
| `CastNode` — `as`, `into`, `is`, and injected coercions | [cast](../../compiler/c/doc/nodes/cast.md) |
| `BreakRetNode` — `return`, `break`, `continue`, `blockret` | [return](../../compiler/c/doc/nodes/return.md) |
| `BlockNode` — blocks and loops | [block](../../compiler/c/doc/nodes/block.md) |
| `IfNode` — `if`, `elif`, `else`, and `match` | [if](../../compiler/c/doc/nodes/if.md) |
| literals — `nil`, numbers, strings, arrays, type literals | [literals](../../compiler/c/doc/nodes/literals.md) |
| `GenericInfo`, `GenVarDclNode`, `MacroDclNode`, cloning | [generic](../../compiler/c/doc/nodes/generic.md) |

## Diagnostics

| Note | Contents |
| --- | --- |
| [Measuring](../../compiler/c/doc/diagnostics/measuring.md) | How to find out what the compiler actually does, and how to read what it produced |
| [Error Codes](../../compiler/c/doc/diagnostics/error-codes.md) | Ranges, reporting, adding a code, when one code carries several causes, cascade suppression |
| [Test Suite](../../compiler/c/doc/diagnostics/test-suite.md) | Adding or updating coverage: the groups, choosing a scenario, what to assert |

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

## Implementation status

**`[built]` is the default and is never written.** A note describes the design;
where the code matches, nothing is marked, and the note reads as the standard it
is.

Two annotations, each terminating whatever it qualifies — a section title, a
paragraph, or a bullet, so the referent is never ambiguous:

- **`[planned]`** — decided, and not in the code.
- **`[differs: what the code actually does]`** — the design reads as ordinary
  prose and the divergence sits inside the brackets.

```
Regions are declared `region @move so:` with `alloc` and `free`
[differs: implemented as `struct so is RegionRef, Move`, a struct and not a module]
```

Mark at the coarsest level that is true, and override inline only where a child
differs. A largely-unbuilt section then costs one annotation, and so does a
largely-built one.

**This is not an exception to the no-changelog rule above.** `[differs]` states a
present gap between the design and the code. "Was `assert(0)`" is history and is
deleted; "[differs: a slice's elements are not finalized when its region frees
it]" is the current fact.

**There is no marker for code a note does not describe.** A note is an abstract
summary from one perspective and necessarily leaves implementation detail out. If
a detail matters to the design's integrity, state it in the note's own terms; if
it does not, leave it out. Its presence in the code already means a decision was
taken — the only open question is relevance, and relevance is answered by writing
it or not writing it.

## The language reference

`doc/reference/` shows the language's **intended** shape, not only what
is built. With no users yet, the breadth is what a reader needs to see.

Each page opens with an italic status note naming the exceptions — the form 31 of
the 60 pages already use:

> *Note: All of this capability is implemented except .len, multi-element
> segments, 'each' iteration, comparison, and pattern matching.*

Where a page is long enough that naming exceptions at the top no longer tells a
reader which paragraph they bite on, mark the affected content itself. **Every
code example that will not compile today is marked**, because examples are what a
reader copies.

**Neither the notes nor the reference ever carries scheduling** — no dates, no
priorities, no work-item references. `[planned]` is a fact about the code, not a
commitment about when it changes.

## Ripple: what a change here reaches

**"What lives elsewhere" is a read-more list, and this is not.** That section
answers *where do I go to learn about X*. This one answers *if X changes here,
what breaks*. The relations differ — read-more is roughly symmetric and stable,
ripple is directional and keyed to a specific fact — so they are separate
sections and neither substitutes for the other.

**A ripple entry names the fact, then its consumers.** Naming only a file is
worthless; any two notes in this folder are "related".

```
The symbol scheme — consumed by `compiler/c/doc/phases/generation.md` (lowering), by
`nameSymbol`, `nameType` and `genlLinkage`, and by
`doc/reference/refmodule.html` (name qualification).
```

**Name an area, never a precise location.** Same reason a code pointer names a
function and not a line: a ripple list is written by the fact's owner and points
at consumers that change without telling it. A fact plus an area survives a
refactor, because the search still finds it. A precise location rots silently.

**Ripple entries are required for `[planned]` and `[differs]` facts, and optional
for `[built]` ones.** A built fact has ground truth — if the notes drift, read the
code. A decided-but-unbuilt fact exists only in the notes, and its reference page
is marked `[planned]` too, so drift between two notes is unrecoverable rather than
merely wrong.

**The owner of a fact is the note whose SUBJECT it is**, never a note that
consumes it. Symbol naming is a naming rule, so `doc/design/names-and-namespaces.md`
owns it and `compiler/c/doc/phases/generation.md` and `compiler/c/doc/nodes/module.md` refer to it. Where no
existing note has a fact as its
subject, that is the signal the topic needs a note of its own — which makes a
topic-owned note a rare and earned thing rather than a default.

## Boundaries between notes are moment-in-time decisions

**Which note carries which material is decided under a particular focus, and it
is expected to move.** Working on packages makes it natural to write symbol
generation into the module note; once the focus leaves, that material belongs with
generation, referred to from module.

**Moving material and redrawing boundaries is encouraged — but propose it first.**
The relocation is agreed before it happens and recorded when it does, so a later
reader does not read a note as having quietly lost something.

## What a decided-design change records

Three things: **what rumination fed it, what prior art was surveyed, and what was
measured.** The provenance line near the top of each note already carries the
third.

**Research is required where a decision has prior art we do not know — not to
re-justify what is already settled.** A rule that demands a literature review for
every small change gets ignored, which is worse than not having it.
