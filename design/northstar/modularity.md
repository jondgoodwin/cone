Modularity is a first-order goal for Cone, ranked by its designer alongside
expressiveness and a powerful type system — not a consequence of having modules.

**The aim** is that every layer of the language surfaces the same three
modularity strategies, so that types, modules and threads look alike rather than
each inventing its own. **The distance** is that Cone today has all three
strategies at the function and type layers, only the first at the module layer,
and no thread layer at all.

The framing below is the author's, from *Modularity in Programming*
(`ProgLing/plingsite/content/post/modularity-in-programming.md`). That post
carries the general argument; this note carries what it means for Cone.

*Provenance: principles from the author's stated design; the current-state
claims read from source, with the separate-compilation gap measured.*

## The three strategies

Every modularity mechanism, at every layer, is one of these:

| Strategy | What it is | Benefit |
| --- | --- | --- |
| **Complexity isolation** | a black box: interior logic invisible from outside, interaction through a public interface | reduces cognitive load; changes stay local, so an evolving system stays stable |
| **Interface-based substitution** | components that differ inside but comply with one interface, so they interchange | plug-and-play versatility |
| **Multi-use generation** | one abstracted component generating many specialized ones | development productivity |

**The tension between them is the design content.** Isolation *decreases*
complexity and fragility. Substitution and generation *increase* coupling, and
therefore complexity and fragility. A language cannot maximize all three, and
each layer's design is a position on that trade.

The same caution applies to drawing boundaries: high cohesion and low coupling
is the goal, but over-fragmenting in anticipation of future complexity
overshoots — inter-component coupling costs grow faster than the intra-component
complexity avoided.

## The layers, and where Cone stands

Ordered smallest to largest. **Languages agree in the small and diverge in the
large**, which is why the interesting decisions are at the bottom of this table.

| Layer | Isolation | Substitution | Generation |
| --- | --- | --- | --- |
| **control block** | yes — single entry, single exit, private locals released at exit | n/a by design — promote it to a function instead | n/a |
| **function** | yes | via function references | via generics |
| **type** | yes — `_`-prefixed members are private | via traits and virtual references | via generics; via trait default methods cloned into implementers |
| **thread** | **absent** — no thread layer exists yet | — | — |
| **module** | yes — `_`-prefixed names are private, namespaces are qualified | **absent** | **absent** |
| **program / library** | via `extern` and the C ABI | — | — |

**The stated goal is to close the two gaps by making the layers symmetric** —
"to make modularity for types, modules and threads look the same, including how
to support name-folding (delegated inheritance) the same way for types and
modules." The open work items on module generics, polymorphic modules and region
modules are that goal, not unrelated features.

## Modules: namespaces today, more intended

There are two families of module system, and Cone is currently the simpler one:

- **Modules as namespaces** — a module is a dictionary of types, functions,
  globals and macros, and namespace management is as far as most languages go.
- **SML-inspired modules** — signature, structure and functor, extending modules
  to subtype and parametric polymorphism; some descendants make modules
  first-class values.

Cone is a namespace system with the isolation strategy only. Adding substitution
and generativity to modules is explicitly seen as an opportunity — it "would
improve the versatility of modules, at some cost to complexity", letting a
program be configured by plugging in modules rather than by creating singleton
types.

**The package is the unit, and a module is a namespace within it.** A package is
the unit of distribution, the semantic unit and the compilation unit; it
correlates to one top-level module, and modules nest beneath that by path. A
module may span source files, each file belonging to exactly one module. Package
dependencies form a DAG. The shape is .NET's: the assembly is what ships and
what compiles, and the namespaces inside it are free to nest.

**The package is the compilation unit** — "the only sensible approach that
allows multiple source files in the same module to refer to entities in each
other is to compile all of a module's source files together, at the same time,"
and a package is what holds those files. There are no header files:
declarations are inferred from definitions, and building a package emits a
serialized public interface for importers to read.

**What distinguishes a module from a type is state, not namespace.** Both carry
a namespace, and the aim is for both to carry the same namespace machinery. A
module's state is global and singleton, reached at a fixed address by functions
that take no `self`; a type's is per-instance, reached through `self`. That is
why a module cannot nest inside a type, and why a module holding one type is
simply a type at the package's top level.

**Cone today is file-per-module, with no package at all.** That is the largest
single distance between the modularity aim and the code.
[module](../nodes/module.md) carries the model in full, and what it has not yet
decided.

## What Cone has today, concretely

**A namespace is one uniqueness domain** whatever a name refers to — a module
cannot hold a type and a function of the same name.

**Encapsulation is spelling, not a keyword.** A leading `_` makes a name private
to its module or its type. It is visible at every use site, needs no export
list, and costs no syntax.

**Composition is compile-time flattening, and it is the same operation at two
layers.** `extends` and `mixin` are one mechanism — a synthetic mixin field at
position 0 — so a trait's fields become a prefix of the implementer's layout and
its default methods are cloned in. That prefix property is what makes a by-value
coercion to a same-size base trait a pure recast.

The author's term for it is **delegated inheritance**, and the insight driving
the symmetry goal is that it is *the same name-folding* a module `using` does:
"applying a similar name-folding capability to types yields the intriguing
delegated inheritance capability." Inheritance is treated as "pure composition
plus 'extra magic'"; Cone keeps composition and delegation, and moves
polymorphism out to traits.

**`import` composes; `include` does not.** `import` loads a file as a module in
its own right and binds its name; `::*` folds its public names into the
importer. `include` injects a file's global statements into the *current*
module, producing no module and no namespace — so an included file's private
names are private to the including module.

| Boundary | Guaranteed | Enforced by |
| --- | --- | --- |
| module, `_` name | not reachable by qualified name from outside | `nameUseNameRes` |
| module, `_` name | not copied by a wildcard import | `importNameRes` |
| type, `_` member | not reachable except through `self` | `fnCallLowerMethod` |
| type, `_` field | not settable from outside in a type literal | `typeLitStructReorder` |
| any namespace | no duplicate name, whatever the kind | `namespaceAdd`, `modAddNamedNode` |

Visibility is checked against **the spelling the caller used**, so a public
overload name may legitimately select a private concrete candidate — the set is
public, the member is not, and calling through the set is the way in.

## The distance, honestly

**Separate compilation does not work.** A declaration's symbol is the module
prefix plus enclosing type names plus the source name — and **the main module's
prefix is empty**. Compiling `mymod.cone` directly emits `@scaleInt`; compiling
a `main.cone` that imports it emits `@mymod_scaleInt`, and the two never
resolve. **A program spanning modules cannot be linked today.**

The generation machinery, though, is not the missing part. An imported module's
bodies are emitted whenever it is flagged for generation, and `stdio` is flagged
— a compile that prints emits `@stdio_print` and full definitions for the
`IOStream` methods, alongside the caller. Every other imported module is denied
the flag by a `strcmp` on its filename. So what blocks a multi-package program
is the symbol rule and that one condition, not the absence of a mechanism.

The serialized interface is a separate and larger cost, and it buys build speed
rather than the ability to link at all. The author anticipated it — "it is not a
trivial effort to add the compiler the ability to ingest, preserve, and re-ingest
public interface information from source files."

**Packages are a search path, not a unit.** `--pkg-path` finds files and
`--safe=package` appears in the option help, but there is no package in the
language — no manifest, no versioning, and nothing that makes a set of source
files one compiled, distributable thing.

**There is no thread layer.** Which of async/await, gothreads or actors Cone
adopts is an open question the author treats as unsettled across the field; the
stated leaning is actors, and structured concurrency is named as the missing
discipline — unstructured concurrency being "similar to GOTO."

## Hazards

- **`include` and `import` look alike and are not.**
- **A mixin brings fields in at a position**, so adding one shifts every later
  field index, and positional type literals move with it.
- **Two mixins can each bring a discriminant field**, and nothing rejects it.
- **A module name resolves as a type name use** — the retag falls through to
  `TypeNameUseTag` by default, not because a module is a type.

## What lives elsewhere

- Lookup, qualification, hooking and overloading: [Names and Namespaces](../phases/names-and-namespaces.md)
- Mixin expansion and trait inheritance, step by step: [struct](../nodes/struct.md)
- Instantiation and monomorphization: [generic](../nodes/generic.md)
- The symbol-naming rule in full: [Generation](../phases/generation.md)
