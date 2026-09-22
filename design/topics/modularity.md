Modularity is a first-order goal for Cone, ranked by its designer alongside
expressiveness and a powerful type system — not a consequence of having modules.

**The aim** is that every layer of the language surfaces the same modularity
strategies, so that types, modules and threads look alike rather than each
inventing its own. **The distance** is that the first three strategies are
broadly present, the last three thin out sharply above the type layer, and there
is no thread layer at all.

The framing below is the author's, from *Modularity in Programming*
(`c:/src/progling/content/post/modularity-in-programming.md`) and his concept
vault. That material carries the general argument; this note carries what it
means for Cone.

*Provenance: principles from the author's stated design; the current-state
claims read from source, with the separate-compilation gap measured.*

## Principles — the six strategies — [derived]

⚠ **This note enumerated THREE until 12 September 2026** — complexity isolation,
interface-based substitution, multi-use generation. **The author's concept vault
enumerates six, and so does his recent writing** (*"six strategies learned once
and applied six times, instead of thirty-six unrelated features"*). **The six are
adopted here and the change has not yet been passed on by him.**

▸ **Why six rather than three.** The three cannot classify two of Cone's most
distinctive mechanisms. **Name-folding is a *namespace* operation**, and
delegated inheritance is name-folding applied to types, built as such — under
the three it has nowhere to sit. **`extends` and `mixin` being one mechanism, a synthetic field at
position 0, is a *composition* fact** and files under none of the three. The
three were a coarsening that dropped exactly the categories Cone innovates in.

Each strategy builds on the ones before it:

| Strategy | What it is | Benefit |
| --- | --- | --- |
| **Composition** | a component is assembled from parts, carrying some ordering arrangement | the whole is built rather than written |
| **Namespace** | a component's parts get unique names by which they are referenced | reach without collision |
| **Encapsulation** | interior detail — algorithm, private state — is hidden, shrinking the surface available for coupling | reduces cognitive load; changes stay local |
| **Substitution** | components differ inside but comply with one interface, so they interchange | plug-and-play versatility |
| **Generativity** | one abstracted component generates many specialized ones | development productivity |
| **Extensibility** | a reusable component can be enriched after the fact | reuse without forking |

**The tension is the design content, and it does not run evenly across the six.**
Composition, namespace and encapsulation are neutral or *reducing* on coupling.
**Substitution, generativity and extensibility *increase* it**, and therefore
increase complexity and fragility. A language cannot maximize all six, and each
layer's design is a position on that trade.

The same caution applies to drawing boundaries: high cohesion and low coupling
is the goal, but over-fragmenting in anticipation of future complexity
overshoots — inter-component coupling costs grow faster than the intra-component
complexity avoided.

## The layers, and where Cone stands

Ordered smallest to largest. **Languages agree in the small and diverge in the
large**, which is why the interesting decisions are at the bottom of this table.

| Layer | Composition | Namespace | Encapsulation | Substitution | Generativity | Extensibility |
| --- | --- | --- | --- | --- | --- | --- |
| **control block** | statements in sequence, blocks nested | locals | single entry, single exit; locals released at exit | n/a by design — promote it to a function | n/a | n/a |
| **function** | the block it holds, and the calls in it | parameters and locals | body invisible; the signature is the interface | function references | generics | overload sets ⚠ *unconfirmed reading* |
| **type** | fields; `extends` and `mixin` flattened at compile time | members | members are private unless `pub` | traits and virtual references | generics; trait defaults cloned into implementers | ⚠ **unknown** — whether a type can gain methods outside its own declaration is not established |
| **thread** | **absent** | **absent** | **absent** | **absent** | **absent** | **absent** |
| **module** | **[planned]** — the folder walk makes a module span files; today a module *is* one file | yes — this is what a module is today | names are private unless `pub` | **absent** — module traits are planned | **absent** — generic modules are planned | **absent** |
| **program / library** | linking; `extern` and the C ABI | ⚠ **absent — the linker has one flat symbol space**, and nothing in a generated name carries the package | partial — a program's definitions are internal to its object, but what a package exports is undecided | **absent** | **absent** | **absent** |

⚠ **The six-column table shows something the three-column one could not.**
**Namespace at the program/library layer is absent** — a flat linker symbol space
with no package component in a generated name. **That is the separate-compilation
problem restated as a modularity gap**, and under the old three columns it was
invisible, because "namespace" was folded into isolation. **Composition at the
module layer being *planned* rather than absent is the other thing it surfaces:**
the folder walk is a composition mechanism, not a namespace one.

**The stated goal is to close the gaps by making the layers symmetric** —
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

Cone's modules carry namespace and encapsulation, and nothing above them. Adding
substitution and generativity is explicitly seen as an opportunity — it "would
improve the versatility of modules, at some cost to complexity", letting a
program be configured by plugging in modules rather than by creating singleton
types.

**What Cone's modules actually are is not this note's subject.** The package as
unit of distribution and compilation, the module as a nesting namespace within
it, the folder walk, `import` and `use`, what distinguishes a module from a type
— all of that is Cone's specific answer at this layer, and [module](../nodes/module.md)
owns it. This note is modularity as a discipline: what the strategies are, which
layers surface them, and where Cone stands against that.

⚠ **This paragraph replaced a full restatement of the package model on 12
September 2026.** It was written out here and again in `module.md`, in
near-identical language, with nothing saying which won. **That duplication is
the reason the own-or-inherit rule exists.**

**At the module layer Cone today is file-per-module, with no package at all.**
That is the largest single distance between the modularity aim and the code.

## What Cone has today, concretely

**A namespace is one uniqueness domain** whatever a name refers to — a module
cannot hold a type and a function of the same name.

**Encapsulation is the default, and `pub` is the one keyword that opens it.** A
name is private to its module or its type unless its declaration says `pub`,
so the interface of a namespace is exactly what its author declared it to be,
and the default is the smallest interface rather than the largest. There is no
export list; the declarations are the list.

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

**The type side is built, and the claim held where it was tested.** A field's
`use` clause folds members of its type in as names of the struct, with `as`,
`but` and one collision rule ([struct](../nodes/struct.md), "Name folding").
At the namespace the two folds are one operation: insertion, collision,
aliasing, and the alias node that binds a folded method is the binding record
the module fold needs. They differ in what the binding holds and in resolution
— a type fold reaches its target through a value, so a use of the name lowers
to an access path or shifts a call's receiver, which a module fold never does.
Same namespace, same rule, same node; different resolution.

**`import` composes; `include` does not.** `import` loads a file as a module in
its own right and binds its name; `.*` folds its public names into the
importer. `include` injects a file's global statements into the *current*
module, producing no module and no namespace — so an included file's private
names are private to the including module.

| Boundary | Guaranteed | Enforced by |
| --- | --- | --- |
| module, name not `pub` | not reachable by qualified name from outside | `nameUseNameRes` |
| module, name not `pub` | not copied by a wildcard import | `importNameRes` |
| type, member not `pub` | not reachable except through `self` | `fnCallLowerMethod` |
| type, field not `pub` | not settable from outside in a type literal | `typeLitStructReorder` |
| any namespace | no duplicate name, whatever the kind | `namespaceAdd`, `modAddNamedNode` |

Visibility is checked against **the spelling the caller used**, which is why a
private concrete candidate may not join a public overload name
(`ErrorPrivOverload`): through the public spelling it would be reachable from
outside its owner. Both are private, or both public.

## The distance, honestly

**Separate compilation does not work.** A declaration's symbol is its path —
enclosing modules, then enclosing types, then the name — and **the root module
contributes nothing to it**. Compiling `modulesub.cone` directly emits
`@scaleInt`, bare; compiling a `main.cone` that imports it emits
`@_CNvC9modulesub8scaleInt`, `modulesub.scaleInt`, and the two never resolve.
**A program spanning modules cannot be linked today.**

The generation machinery, though, is not the missing part. An imported module's
bodies are emitted whenever it is flagged for generation, and `stdio` is flagged
— a compile that prints emits `stdio.print` and full definitions for the
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
- **Two mixins of closed types each bring a discriminant field**, reported as a
  second tag rather than as a composition that cannot work. An open trait
  carries no discriminant, so any number of those compose.
- **A use of a module's name answers `isTypeNode` true** — `nameUseGroup`'s
  fallthrough for every declaration that is not a value, a macro or a generic
  parameter, not because a module is a type.

## What lives elsewhere

- Lookup, qualification, hooking and overloading: [Names and Namespaces](../phases/names-and-namespaces.md)
- Mixin expansion and trait inheritance, step by step: [struct](../nodes/struct.md)
- Instantiation and monomorphization: [generic](../nodes/generic.md)
- The symbol-naming rule in full: [Names and Namespaces](../phases/names-and-namespaces.md), "Symbols"; its lowering: [Generation](../phases/generation.md)
