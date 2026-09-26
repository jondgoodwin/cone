What Cone promises about memory and type safety, what the compiler actually
checks today, and the distance between the two.

**Read this before trusting any safety property.** The gap is large, and most of
it is invisible — an unenforced rule produces no diagnostic, so a clean compile
is not evidence the rule holds.

*Provenance: measured. Every "not checked" row below was confirmed by compiling
a program that violates it and observing no diagnostic; several are pinned by
test scenarios that assert the absence deliberately.*

## Principles

⚠ **1 and 2 are the author's stated position and are quoted. 3, 4 and 5 are
measured facts about the present state, not positions** — they belong here
because they rule what a reader may conclude from a clean compile, which is the
most consequential thing this note settles.

1. **The programmer is primarily responsible; the compiler is a teammate.** The
   stated position is that the compiler will never understand intended behaviour
   as well as the programmer, but brings "a rapid, disciplined rigor that
   fallible humans can never rival" — so the two working together is the point,
   not compiler omniscience.
2. **Safety and performance are not traded against each other.** The premise,
   following Rust, is that a smarter compiler identifies safety exposures
   "without sacrificing runtime performance and flexibility". What safety costs
   instead is **complexity** — reference semantics are more complex than pointer
   semantics precisely because of it, and the author says so plainly. ▸ **So the
   trade safety actually makes is against attention**, which is priced in
   [Expressiveness and Attention](expressiveness-and-attention.md), where the
   author's position is that Rust underprices exactly this cost. **Forbids** a
   safety mechanism paid for at runtime; **permits** one paid for in complexity,
   but requires the price to be named.
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
| use of an uninitialized variable | **yes** | `nameuseFlow`, but on a whole-function summary — "initialized on one branch" reads as initialized everywhere. An assignment's target is read only for the parts of it that are values — its index and its dereference — never for the base of a partial write |
| use after move | **yes** | `nameuseFlow`, same summary caveat |
| move out of a global | **yes** | `flowHandleMove`, and `flowResultMove` for a returned value |
| move out through a borrowed reference, any permission | **yes** | `flowHandleMove`, and `flowResultMove` for a returned value |
| write through a read-only reference | **yes** | `assignlvalrtype`, `swapFlow` — `MayWrite` only |
| write through an `imm` *field* | **yes** | `iexpGetLvalInfo`, taking the minimum of the field's permission and its container's |
| read through a reference lacking `MayRead` | **yes** | `flowLoadThroughRef`, from `derefFlow`, `fnCallArrIndexFlow` and `fnCallFldAccessFlow` — a pointer carries no permission and is not asked |
| a borrow stored into a longer-lived place | **yes** | `assignlvalrtype`, one site |
| a borrow returned from a function | **yes** | `returnFlowEscape`, one site |
| a borrow returned through a call, singly or as one of several values | **yes** | `fnCallFinalizeArgs` types the call with the narrowest argument borrow's scope, on a reference node or on a tuple's borrowed elements, which the two rows above then read |
| a borrow passed beside a `&mut &T` argument the callee could store it through | **yes** | `fnCallFlowStoredBorrow`, one site |
| a borrow coerced to another reference type — widened to a base trait's reference, or made a `&<Trait` | **yes** | `iexpCoerce` types the injected cast with a copy of the target reference type carrying the source borrow's scope, which the rows above then read |
| a borrow **laundered through a variable** | **no** | assignment does not carry scope onto the variable's declared type |
| a borrow **captured or stored in a field** | **no** | — |
| two parameter borrows with different lifetimes | **no** | there is no lifetime annotation syntax to express it; every borrow in a signature is taken to share one lifetime |
| aliasing of borrows | **no** | `borrowFlow` checks only that the borrowed place holds a value |
| freezing a borrow's source | **no** | documented; never implemented |
| array and slice bounds | **yes** | `genlBoundsCheck`, per dimension |
| **raw pointer** bounds | **no** | unchecked by construction |
| raw pointer deref / arithmetic gated by `trust` | **no** | `trust` is not a keyword and has no parse rule |
| allocation failure | **yes** | null test then `llvm.trap`, unless `?` asked for an `Option` |
| thread-safety of a shared reference | **no** | `RaceSafe` is populated and read nowhere; `ThreadBound` is now infected correctly and nothing consumes it either |
| release of an owning reference at scope exit | **partly** | leaks on a conditionally-moved variable, for arrays of owning references, and for an `Option` holding one (a `?+` allocation); a value moved out of on only some paths is freed but not finalized on the others |

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
three of the many places a reference can escape. The checks that exist are
correct, which makes the absences harder to notice.

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
of the checks it would switch off are switched on to begin with. The same holds
for the intrinsics whose misuse breaks memory safety (`finalize`, the slice
constructors, `readRaw`, `writeRaw`, `moveRaw`): the compiler's registry marks
each as needing `trust`, and nothing yet asks for it (`refintrinsic.html`).

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
  an enum variant is real.
- **Ordinary array and slice indexing is bounds-checked**, and allocation
  failure traps rather than returning null.
- **A variable is not read or borrowed before it holds something**, and not
  read or borrowed after its value moved away — on any path, because the check
  is conservative.
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
- **`assert` is a no-op in the release build**, so any internal consistency
  check written as one is absent from the shipped compiler. The 22 sites that
  meant *unreachable* now call `errorUnreachable` instead and do fire there; the
  ordinary value asserts still do not.
- **`--verify` is off by default.** No corpus scenario fails it today — the one
  shape that did, an empty phi from a valueless loop-as-expression, is fixed and
  covered — but nothing has run it over a program outside the corpus. Malformed
  IR is not a safety property of the language, but it is a way a "clean compile"
  lies.
- **`--checktree` checks IR well-formedness, not safety.** Passing it says
  nothing about any row above.

## What lives elsewhere

- The three axes and what each permits: [References and Regions](references-and-regions.md)
- What flow actually does and does not analyze: [Flow Analysis](../../compiler/c/doc/phases/flow.md)
- Bounds checks, traps and pointer levels: [Generation](../../compiler/c/doc/phases/generation.md)
- Re-measuring any row: [Measuring](../../compiler/c/doc/diagnostics/measuring.md)
