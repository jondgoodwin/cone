What a language feature costs is the attention it demands, and Cone's design
trades on that. This note carries the author's position on expressive power,
cognitive load, and why the balance falls where it does.

**The aim** is that programming feels like plugging together Lego blocks — a
simple assembly operation rather than a complex algorithmic calculation. **The
distance** is that the mechanisms meant to deliver it are the ones not yet built:
no thread layer, so no actors; module substitution and generativity absent; and
borrowing not yet narrowed by anything but convention.

The framing is the author's, from a conversation of 7 September 2026. It is a
statement of design intent, offered by him with a caveat attached: *"some of
which I'm likely to rethink over the course of the next year or two."*

*Provenance: the author's stated design; the current-state claims read from
source. Nothing here is measured, because none of it is built.*

⚠ **This note has no separate Principles section because it is entirely
principles.** Every heading below states a position the author holds, quoted,
and what it rules over. **Attention is the scale the other topic notes are
weighed on**, so this one is read alongside whichever aim is in question rather
than instead of one.

## The scarce resource is attention, not safety

> *"The nature of the limitation of both AI and human beings: the scarceness of
> attention and the complexity of multifocal problems. My mind and yours get
> overwhelmed by huge context and complexity and contradictions in that. Any time
> you can express conceptually how to solve a problem in simpler ways, you're
> going to think more clearly, solve more quickly, have fewer problems."*

This is the criterion the other aims are judged against. A safety mechanism, a
modularity mechanism or a performance lever that costs more attention than it
saves is a bad trade whatever it guarantees.

**It is explicitly not an argument for minimalism.**

> *"I'm not arguing for the simplest possible language, because the problem with
> the simplest possible language is that you then have to express complex ideas
> with lots of moving parts. The balance of a language is to find the right
> balance between simplicity of expression and simplicity of creating the program
> itself. A good language is complex enough that it gives you enough working
> parts to do very interesting things quickly and put them together neatly."*

So the target is not a small language. It is a language whose parts compose
without the composition itself becoming the hard part.

## Lego is the design image, and it is a claim about interfaces

> *"How can I convey the richness of the programming constructs in the simplest
> possible ways… so that programming is like plugging together Lego blocks and
> becomes a simple assembly operation rather than a complex algorithmic
> calculation sort of language."*

**Lego composes because its interfaces are uniform and its pieces opaque.** Every
stud is the same stud, and nothing about a brick's interior participates in the
joint. What that demands of a language is small, uniform, opaque interfaces —
and it is the same demand [Modularity](modularity.md) makes when it asks for the
strategies to look alike at every layer.

**The counter-example is instructive.** Rust's connectors are parameterised — by
lifetimes, by trait bounds — so no two pieces snap together the same way and the
joint is where the thinking goes. Generic-heavy library composition is the shape
this note is steering away from.

## Safety is priced, not assumed

The author does not treat safety as a free good:

> *"I don't hold it with the same religious conviction as the Rust people do,
> because I don't think they see safety accurately. It's a religious piece, and
> they are not looking at the cost of the safety mechanisms, and the fact that we
> need things that go beyond those mechanisms. And unsafe Rust is at the
> foundation of the way the language works — it can't operate without unsafe
> Rust."*

He flags the reasons as a topic in their own right and has not written them down.
[Safety](safety.md) carries what Cone actually promises and checks; this note
carries only that the promise is costed.

## Borrowing is deliberately narrow [planned]

> *"The whole borrow-checker thing, the restrictions, the way that we have to
> refactor code because of that type system — that's the level of complexity of
> cognition, of problem-solving, that we want to avoid as much as possible. For
> that reason I'm generally much more interested in restricting the use of
> borrowing to a very narrow set of circumstances."*

The mechanism behind the objection is that **borrow-checker failures are
frequently non-local**: the error is reported in one place and resolved by
restructuring ownership somewhere else. That is the multifocal problem in its
purest form, and it is why the cost is attention rather than keystrokes.

**Narrowing borrowing creates a debt, and actors are how it is paid.** Something
must carry the aliasing load: copying costs performance, and reference counting
and tracing both cost latency that the millions-of-objects engine requirement
rules out. State owned by an actor, with messages transferring rather than
sharing, means most aliasing questions never arise to be asked.

▸ **So actors are load-bearing for the simplicity argument, not only the
concurrency one.** [References and Regions](references-and-regions.md) carries
what borrowing does today.

## Actors, and why they rate highly [planned]

> *"What I like about actors is how much simpler it makes talking about
> concurrency and designing around concurrency, and the modularness of it, as
> compared to tasks and other mechanisms."*

Rated by the author as having *"huge potential in simplifying concurrent design
paradigms and building things more quickly."* Pony-like actors and Rust-like
capabilities are named together as serving performance and agility at once —
*"all of these are about high-performing systems that are also high-functioning
systems."*

**There is no thread layer at all today**, which [Modularity](modularity.md)
records as the largest single hole in the layer table.

## The evidence offered that expressiveness pays

> *"When we moved a search product from C# to Rust, we dramatically reduced code
> size because of capabilities we were able to leverage."*

One data point, from the author's own experience, and the only empirical claim in
this note.

## Ripple: what a change here reaches

- **The Lego criterion** — consumed by [Modularity](modularity.md) (the uniform-
  interface aim across layers) and by any decision about generic or trait-bound
  surface area.
- **Narrow borrowing** — consumed by [References and Regions](references-and-regions.md),
  and by the permissions and regions material in `phases/type-check.md` and
  `ir/flow.c`.
- **Actors carrying the aliasing load** — consumed by [Performance](performance.md)
  (the latency argument against refcounting and tracing) and by whatever design
  note the thread layer eventually gets. There is none today.
- **The attention criterion** — consumed by every northstar note, since it is the
  scale the other aims are weighed on.

## What lives elsewhere

- What modularity is for, and the six strategies: [Modularity](modularity.md)
- What Cone promises about safety and what it checks: [Safety](safety.md)
- The performance levers and why they are architectural: [Performance](performance.md)
- Borrowing, permissions and regions as they stand: [References and Regions](references-and-regions.md)
