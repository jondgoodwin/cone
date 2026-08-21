What Cone promises about memory and type safety, what the compiler actually
checks today, and the distance between the two.

**Read this before trusting any safety property.** The gap is large, and most of
it is invisible — an unenforced rule produces no diagnostic, so a clean compile
is not evidence the rule holds.

*Provenance: measured. Every "not checked" row below was confirmed by compiling
a program that violates it and observing no diagnostic; several are pinned by
test scenarios that assert the absence deliberately.*

## Key principles

1. **The programmer is primarily responsible; the compiler is a teammate.** The
   stated position is that the compiler will never understand intended behaviour
   as well as the programmer, but brings "a rapid, disciplined rigor that
   fallible humans can never rival" — so the two working together is the point,
   not compiler omniscience.
2. **Safety and performance are not traded against each other.** The premise,
   following Rust, is that a smarter compiler identifies safety exposures
   "without sacrificing runtime performance and flexibility". What safety costs
   instead is **complexity** — reference semantics are more complex than pointer
   semantics precisely because of it, and the author says so plainly.
3. **A clean compile proves less than it looks like it proves.** Several rules
   the language documents are enforced nowhere.
4. **Where a rule is unenforced, the corpus records it by establishing the
   opposite** — a scenario that compiles a violation clean. So a scenario that
   starts failing may be one a fix correctly invalidated.
5. **The safe/unsafe boundary is not yet drawn.** `trust` does not exist, and
   the operations it would gate are unguarded already.

## The scorecard

| Property | Checked? | Where, or why not |
| --- | --- | --- |
| use of an uninitialized variable | **yes** | `nameuseFlow`, but on a whole-function summary — "initialized on one branch" reads as initialized everywhere |
| use after move | **yes** | `nameuseFlow`, same summary caveat |
| move out of a global | **yes** | `flowHandleMove` |
| write through a read-only reference | **yes** | `assignlvalrtype`, `swapFlow` — `MayWrite` only |
| write through a `ro` *field* | **no** | the check compares by pointer identity against a singleton a written permission never is |
| **read** through a reference lacking `MayRead` | **no** | `MayRead` is never consulted as an access check anywhere |
| a borrow stored into a longer-lived place | **yes** | `assignlvalrtype`, one site |
| a borrow returned from a function | **yes** | `returnFlowEscape`, one site |
| a borrow **passed as an argument** | **no** | `fnCallFlow` never looks at scope |
| a borrow **laundered through a variable** | **no** | assignment does not carry scope onto the variable's declared type |
| a borrow **captured or stored in a field** | **no** | — |
| a borrow across a function boundary | **no** | there is no lifetime annotation syntax to express it |
| aliasing of borrows | **no** | `borrowFlow` is an empty function |
| freezing a borrow's source | **no** | documented; never implemented |
| array and slice bounds | **yes** | `genlBoundsCheck`, per dimension |
| **raw pointer** bounds | **no** | unchecked by construction |
| raw pointer deref / arithmetic gated by `trust` | **no** | `trust` is not a keyword and has no parse rule |
| allocation failure | **yes** | null test then `llvm.trap`, unless `?` asked for an `Option` |
| thread-safety of a shared reference | **no** | `RaceSafe` is populated and read nowhere; `ThreadBound` infection is unreachable |
| release of an owning reference at scope exit | **partly** | leaks on `continue`, on intervening scopes under `break`, on a conditionally-moved variable, and for arrays of owning references |

## The four shapes the gaps take

Grouping them is more useful than the list, because each shape fails the same
way wherever it appears.

**1. A rule with a representation but no consumer.** The data is computed and
nothing reads it. `RaceSafe`, `MayAliasWrite`, `MayIntRefSum` and `IsLockless`
are set on every permission and consulted nowhere. `lifeMatches` exists and is
called from nowhere. `VarDclNode.flowflags` is zeroed twice and never read.
These look like working machinery in a grep and are inert.

**2. A rule enforced at some sites and not others.** Borrow lifetime is the
worst case: the scope is recorded correctly on every borrow, and checked at
exactly two of the many places a reference can escape. The check that exists is
correct, which makes the absence harder to notice.

**3. A rule enforced on a summary rather than a path.** Initialization and move
state live on the declaration and are never saved or restored, so they describe
the whole function rather than a program point. A move in one arm of an `if`
marks the source moved for the other arm and everything after. This is a
deliberate conservative approximation — it leaks rather than double-frees — but
it means "the compiler accepted it" and "this program is correct" are further
apart than usual.

**4. A guard that does not exist yet.** `trust` is the whole of this. The
compiler has no `trust` keyword, so a program using one fails as an unknown
name. The operations `trust` is meant to gate — raw pointer dereference,
indexing and arithmetic — all compile with no guard anywhere, which means none
of the checks it would switch off are switched on to begin with.

**`trust` is meant to be narrow.** It is framed as a remedy for compiler
over-reach — constraints that are "sometimes overzealous, preventing behavior
that may actually be safe, but which the compiler does not have the insight to
confirm" — not as a general escape. Inside it: pointer arithmetic, calls into
unsafe-language externals, casting references to other types.

## Deliberate non-goals

Two things a reader might expect and should not:

- **Compile-time-proven multi-owner data structures are an explicit non-goal.**
  The idea was worked through and rejected: the value-add is not worth the cost
  to the language, the compiler and compile times, for the sake of a few data
  structures that an isolated unsafe API handles just as well.
- **Refinement types are refused.** Data invariants are carried by privacy
  instead — structural matching deliberately skips private fields, so a type's
  invariants are protected by not being matchable from outside.

## What is guaranteed today, honestly stated

If you want a short answer to "what does a clean compile buy me":

- **Types are sound in the ordinary sense** — a value of a declared type has
  that type's representation, coercions are explicit or checked, and the tag on
  a union variant is real.
- **Ordinary array and slice indexing is bounds-checked**, and allocation
  failure traps rather than returning null.
- **A variable is not read before it holds something**, and not read after its
  value moved away — on any path, because the check is conservative.
- **Nothing is freed twice by the ordinary paths.** The release machinery
  errs toward leaking.

And that is close to the whole list. In particular a clean compile does **not**
establish that references do not dangle, that mutation is not aliased, that data
does not cross threads unsafely, or that memory is released.

## Hazards

- **An unenforced rule produces no diagnostic**, so its absence has to be looked
  for rather than noticed. The scorecard above is the current answer; re-measure
  before relying on any row.
- **Adding a check may break scenarios that assert the gap.** The corpus records
  unenforced rules by compiling a violation clean. A scenario that starts
  failing may be one your fix correctly invalidated — read it before "fixing"
  it.
- **`assert(0)` is a no-op in the release build**, so internal consistency
  checks are absent from the shipped compiler.
- **`--verify` is off by default**, and the compiler does emit invalid LLVM IR
  in at least one shape. Malformed IR is not a safety property of the language,
  but it is a way a "clean compile" lies.
- **`--checktree` checks IR well-formedness, not safety.** Passing it says
  nothing about any row above.

## What lives elsewhere

- The three axes and what each permits: [References and Regions](references-and-regions.md)
- What flow actually does and does not analyze: [Flow Analysis](../phases/flow.md)
- Bounds checks, traps and pointer levels: [Generation](../phases/generation.md)
- Re-measuring any row: [Measuring](../diagnostics/measuring.md)
