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
the three it has nowhere to sit. **A trait's fields being a requirement the
implementer declares at position 0, rather than state it inherits, is a
*composition* fact** and files under none of the three. The
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
| **type** | fields; an enum's spliced into its variants at compile time | members | members are private unless `pub`; an enum and its variants are one boundary | traits and virtual references, asserted with `is` or noticed structurally | generics; trait defaults cloned into implementers | ⚠ **unknown** — whether a type can gain methods outside its own declaration is not established |
| **thread** | **absent** | **absent** | **absent** | **absent** | **absent** | **absent** |
| **module** | the files of a folder, swept from the designated file the compiler is given, plus the submodules its subfolders and its one-file modules draw; an organisational subfolder's files, at any depth, join the enclosing module | yes — the folder, or a one-file module's file, names the module, and a submodule is reached by a path through its parent | names are private unless `pub`, and a submodule is private to its parent unless `pub` | module traits, asserted with `is` and checked where written; static, since a module is one instance | generic modules, `mod stack[T]`, instantiated as a generic type is — one instance per argument list, each with its own globals, `init` and `final` (a generic module's submodules are not built); module-trait defaults cloned into conforming modules | **absent** |
| **program / library** | linking; `extern` declarations — functions, methods, operators and globals defined in another object — each spelled by its module's naming, Cone or C (`@c`) | ⚠ **absent — the linker has one flat symbol space**, and nothing in a generated name carries the package | partial — a program's definitions are internal to its object, and a library built from a build description exports its public definitions and the private ones an expanded body reaches; a generic's instance — a generic module's globals included — is defined by every object using it and merged at link | **absent** | **absent** | **absent** |

⚠ **The six-column table shows something the three-column one could not.**
**Namespace at the program/library layer is absent** — a flat linker symbol space
with no package component in a generated name. **That is the separate-compilation
problem restated as a modularity gap**, and under the old three columns it was
invisible, because "namespace" was folded into isolation. **Composition and
namespace being separable at the module layer is the other thing it surfaces:**
the folder sweep is a composition mechanism, and an organisational subfolder
proves it — it composes a module's files and makes no namespace, while the
subfolder beside it that holds its own designated file makes one, and so does a
file that opens with `mod`.

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

Cone's modules carry namespace and encapsulation, and substitution through
module traits, asserted with `is`; generativity is still above them. Adding
substitution and generativity is explicitly seen as an opportunity — it "would
improve the versatility of modules, at some cost to complexity", letting a
program be configured by plugging in modules rather than by creating singleton
types. The first use is a program plugging into a framework — a shell, a web
server — through the framework's module trait.

**What Cone's modules actually are is not this note's subject.** The package as
unit of distribution and compilation, the module as a nesting namespace within
it, the folder sweep, `import` and `use`, what distinguishes a module from a type
— all of that is Cone's specific answer at this layer, and [module](../../compiler/c/doc/nodes/module.md)
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
export list; the declarations are the list. **An enum is one boundary with its
variants**: code anywhere inside its braces, or an extension's, sees every
variant's private members, since a closed enum is one type written in one place
(Jon, 23 Sep 2026).

**A file states its module and its dependencies first.** Every file the
compiler builds as a module opens with its `mod` line (Jon, 24 Sep 2026), and
the module's imports come right after it, ahead of everything else — so all of
them are in its designated file, since its other files have no `mod` line to
follow (Jon, 23 Sep 2026). That header — comments, the `mod` line, the imports —
is what a reader sees first and all Congo reads, and the compiler refuses a
first file without the line (`ErrorNoModDcl`) and an import below the header or
in another file (`ErrorImportLate`).

**Composition is compile-time flattening, and it is the same operation at two
layers.** A trait's fields are a requirement rather than state it hands over: the
implementer declares them itself, in the trait's order, at position 0, and takes
the trait's default methods as clones. That prefix — which `is` verifies at the
declaration — is what makes a by-value coercion to a same-size base a pure recast.
The one place fields are still spliced is an enum into its variants, whose layout
the compiler owns.

The author's term for it is **delegated inheritance**, and the insight driving
the symmetry goal is that it is *the same name-folding* a module `using` does:
"applying a similar name-folding capability to types yields the intriguing
delegated inheritance capability." Inheritance is treated as "pure composition
plus 'extra magic'"; Cone keeps composition and delegation, and moves
polymorphism out to traits.

**The type side is built, and the claim held where it was tested.** A field's
`use` clause folds members of its type in as names of the struct, with `as`,
`but` and one collision rule ([struct](../../compiler/c/doc/nodes/struct.md), "Name folding").
At the namespace the two folds are one operation: insertion, collision,
aliasing, and the alias node that binds a folded method is the binding record
the module fold needs. They differ in what the binding holds and in resolution
— a type fold reaches its target through a value, so a use of the name lowers
to an access path or shifts a call's receiver, which a module fold never does.
Same namespace, same rule, same node; different resolution.

**There are three reuse mechanisms on the type side, and the later two are where
the symmetry claim comes cheapest.** A field's clause delegates to a *part*;
`extends` over a concrete base enriches the *whole*, adding methods and no fields
([struct](../../compiler/c/doc/nodes/struct.md), "Enrichment"). Because it may not change the
fields, the enriching type and its base have one representation and their values
substitute for each other in both directions at no cost — so a base method
already takes exactly the right receiver, and folding one is the same degenerate,
one-instance case the module fold is: no dispatch to arrange and no second copy
of the state. ▸ **What it delivers is retroactive conformance, owned rather than
ambient**: an enrichment supplying the method a trait wants makes the base's own
values reach that trait, by being bound to the enriched name, and two rival
enrichments cannot collide, because each hangs off its own name and its own
vtable. That rests on conformance staying structural, so nominal `is` and
structural noticing are both kept on purpose. ▸ **What it costs** is a boundary
crossed knowingly: an enrichment is inside its base's encapsulation and reads its
private members, so a type that may be extended has its representation in its
contract. SemVer is the protection, and it holds because two versions of one
package may coexist in a binary.

**The third is what the other two were for, and it is the expression problem
answered.** A `use` in a type's body folds in a *sibling* — another type that
declared this type's base ([struct](../../compiler/c/doc/nodes/struct.md), "Sibling folding"). So a
base type plus one package's trigonometry plus another's logarithms become one
type, declared once, in the namespace of whoever needs it, with no package
modified and nobody's permission asked. It costs nothing beyond `extends`: the
shared base means a sibling's method already takes a receiver this type's values
substitute for, so the fold makes aliases and nothing else. ▸ **And the collision
is the feature.** Two enrichments of one base may each declare `span`, and neither
is wrong; the type that folds both settles it with `as` or `but` in its own
declaration. **That is the one place in the language where two independent
libraries disagreeing is resolvable by the party who needs both** — which is what
made reuse across package boundaries a language problem in the first place.

**`import` composes; the folder gathers.** `import` binds another module's name —
a sister found in the registry the enclosing module is, or a module loaded from a
path — and a `use` clause folds its public names into the importer, selected,
renamed or excluded as a global's clause does, and is the one spelling of an
import's fold [Jon 23 Sep]. Every binding it
makes carries a visibility of its own, so what a third module sees through this
one is what this one wrote `pub import` or `pub use` for. A submodule is never
imported, so its parent folds its names with a standalone `use` naming it, which
takes the same clause; a standalone `use` never names an imported module, whose
own import folds it [Jon 23 Sep]. A module's own files
are its folder's, and nothing in any file brings another in — so a tool handed
one file knows its module from the path alone, and a file's private names are
private to the module whose folder holds it. `include`, which injected a file's
global statements into the current module, is retired and reported
(`ErrorInclude`).

**Dependencies form a DAG, at every scale** [Jon 23 Sep]: modules inside a
package as well as packages. A module depends on what it imports, on what it
extends, and on each of its own submodules, so a part never leans on the whole
it is part of and a child never imports a name of its parent; what they share
moves into a sister both import. That is the hierarchical decomposition the
layer exists for, and it is what gives modules an order to initialise in:
dependencies first. Each module declares only its own `init` and `final`, and
the compiler stitches every module's together — every `init` in that order, the
root last, and every finalizer in the reverse — so no module sets up another's
state. Congo refuses a
loop from the header scan, and the compiler again for a direct run
(`ErrorImportLoop`).

| Boundary | Guaranteed | Enforced by |
| --- | --- | --- |
| module, name not `pub` | not reachable by qualified name from outside | `nameUseNameRes` |
| module, name not `pub` | not folded by a wildcard import | `importNameRes` |
| module, a fold not re-exported | not reachable from outside, and not folded on | `importFoldItem`, `fnCallNameResPath` |
| module tree | a sister is reached by name, never by a path that walked to her file | `parseImport` |
| modules and packages | no loop of dependencies — imports, `extends`, containment | `pgmModuleOrder`; Congo's `build_order` and `check_module_loops` |
| type, member not `pub` | not reachable except through `self` — or, for an enum and its variants, from code inside the enum's braces or an extension's | `fnCallLowerMethod`, `structEnumSeesPrivate` |
| type, field not `pub` | not settable from outside in a type literal — an enum's braces, and an extension's, being inside | `typeLitStructReorder`, `structEnumSeesPrivate` |
| any namespace | no duplicate name, whatever the kind | `namespaceAdd`, `modAddNamedNode` |

Visibility is checked against **the spelling the caller used**, which is why a
private concrete candidate may not join a public overload name
(`ErrorPrivOverload`): through the public spelling it would be reachable from
outside its owner. Both are private, or both public.

## The distance, honestly

**Separate compilation works only through a build description.** A
declaration's symbol is its path — enclosing modules, then enclosing types, then
the name — and **the root module contributes nothing to it**. Compiling
`modulesub.cone` directly emits `@scaleInt`, bare; compiling a `main.cone` that
imports it emits `@_CNvC9modulesub8scaleInt`, `modulesub.scaleInt`, and the two
never resolve. A library compiled from a build description is the way off that:
its root is named, so its symbols are spelled as its importers spell them, and
it exports what they need — its public definitions, and each private one an
`inline`, generic or macro body reaches — so a program compiled against the
include file for it links and runs (`module_build_link`); the include file
declares what the package defines with `extern` and no body, and each
declaration takes the package's Cone name. The library's compile generates that
include file from its own source ([module](../../compiler/c/doc/nodes/module.md),
"Generating the include file"). Congo drives it: `congo run` compiles each
package a program imports on its own, from a build description it writes, and
links the objects (`tools/congo/README.md`), each dependent compiled against
the include file its package's compile generated, `core`'s and `stdio`'s
included; what a package's root reaches in its submodules is declared there in
private nested module blocks.

The generation machinery, though, is not the missing part. An imported module's
bodies are emitted whenever it is flagged for generation, and **every module found
on the package search path is flagged** — until separate compilation lands, a
package is compiled into the object that imports it. So a compile that prints
emits `stdio.print` and full definitions for the `IOStream` methods, alongside
the caller, because `stdio` is a package in the repository's `packages/` folder.
An imported module found beside its importer is denied the flag. So what blocks a
multi-package program is the symbol rule and that one condition, not the absence
of a mechanism. **A module of the same tree is flagged**, which is why an import
between two sisters links and an import of a loaded module does not: what a
folder tree holds this compile defines.

The serialized interface is a separate and larger cost, and it buys build speed
rather than the ability to link at all. The author anticipated it — "it is not a
trivial effort to add the compiler the ability to ingest, preserve, and re-ingest
public interface information from source files."

**To the compiler, packages are a search path, not a unit.** The search path —
every `--path` folder, then the packages folder, where `core` and `stdio` are —
finds files, and `--safe=package` appears in the option help, but the compiler
knows no package: it reads no manifest and no version. The package as a unit is
Congo's: a folder with a `congo.toml` (name, `MAJOR.MINOR.PATCH` version, and
executable or library) and its source under `src/`, compiled on its own and
imported through its include file. What the compiler takes from Congo is a
**build description**: one package's modules and files and where each import
is, which the compiler checks against each file's `mod` line and never searches
beyond ([module](../../compiler/c/doc/nodes/module.md), "A described build").

**There is no thread layer.** Which of async/await, gothreads or actors Cone
adopts is an open question the author treats as unsettled across the field; the
stated leaning is actors, and structured concurrency is named as the missing
discipline — unstructured concurrency being "similar to GOTO."

## Hazards

- **A mixin brings fields in at a position**, so adding one shifts every later
  field index, and positional type literals move with it.
- **Two mixins of closed types each bring a discriminant field**, reported as a
  second tag rather than as a composition that cannot work. An open trait
  carries no discriminant, so any number of those compose.
- **A use of a module's name answers `isTypeNode` true** — `nameUseGroup`'s
  fallthrough for every declaration that is not a value, a macro or a generic
  parameter, not because a module is a type.

## What lives elsewhere

- Lookup, qualification, hooking and overloading: [Names and Namespaces](names-and-namespaces.md)
- Mixin expansion and trait inheritance, step by step: [struct](../../compiler/c/doc/nodes/struct.md)
- Instantiation and monomorphization: [generic](../../compiler/c/doc/nodes/generic.md)
- The symbol-naming rule in full: [Names and Namespaces](names-and-namespaces.md), "Symbols"; its lowering: [Generation](../../compiler/c/doc/phases/generation.md)
