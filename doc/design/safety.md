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
| use of an uninitialized variable | **yes** | `nameuseFlow`, on the state along the walk in source order, the arms of an `if` joined: "initialized on one branch" reads as initialized after it, except for a variable that moves or has anything to do as it dies, which the path walk refuses where some path gave it no value. An assignment's target is read only for the parts of it that are values — its index and its dereference — never for the base of a partial write |
| use after move | **yes** | `nameuseFlow`, a move on any arm of an `if` counting after it; a use some path reaching it moved out — a loop's earlier pass — by the path walk (`dropRefuse`) |
| move out of a global | **yes** | `flowHandleMove`, and `flowResultMove` for a returned value |
| move out through a borrowed reference, any permission | **yes** | `flowHandleMove`, and `flowResultMove` for a returned value |
| move out of a field, leaving a struct with a hole in it | **yes** | `flowRefuseMoveField`, from `flowHandleMove` and `flowResultMove` |
| write through a read-only reference | **yes** | `assignlvalrtype`, `swapFlow` — `MayWrite` only |
| write through an `imm` *field* | **yes** | `iexpGetLvalInfo`, taking the minimum of the field's permission and its container's |
| read through a reference lacking `MayRead` | **yes** | `flowLoadThroughRef`, from `derefFlow`, `fnCallArrIndexFlow` and `fnCallFldAccessFlow` — a pointer carries no permission and is not asked |
| a borrow stored into a longer-lived place | **yes** | into a global or through a reference: `assignlvalrtype`, through `assignBorrowLifetimeCheck` (a borrowed parameter's borrow is the caller's, shorter-lived than a global), and the loan walk, for a place it reaches (what a parameter, or a copy of one, points at, stored there directly or by a call, which may store through every writable borrow it reaches, a struct's `&mut` field and one a struct handed by value holds included; an `Rc`'s referent), never a local a reference points at, whatever that local holds, and for a global given the caller's borrow some other way than as a borrowed parameter's own (read through a `&mut &T` parameter, held by a by-value one). Into a variable's own storage, a shorter borrow is legal: a variable's lifetime follows what it holds, and the laundering row below is how it is held to that |
| a borrow swapped into a longer-lived place | **yes** | `swapFlow`, the same check once in each direction: a swap stores both ways, so neither side may outlive a borrow it receives from the other |
| a borrow returned from a function | **yes** | `returnFlowEscape`, one site, looking into a returned `if`, `match` or block arm by arm. The lifetimes it reads: a borrow of a local, of a by-value parameter, of `self` by value or through an owner passed by value is the function's, and an owner handed back as a borrow is a borrow through it; a borrow through a borrowed reference is that reference's, the caller's for a borrowed parameter even through an immutable local copy of it (through a mutable one, that local's block); an `if`, `match` or block used as a value has its shortest-lived arm's. A borrow carried out in a variable assigned after its initialization, or inside another value, is the loan walk's: the laundering and field rows below; so is a borrow of a temporary, an owner made by a call and handed back as a borrow at once among them (the temporary row) |
| a borrow returned through a call, singly or as one of several values | **yes** | `fnCallFinalizeArgs` types the call with the narrowest argument borrow's scope, on a reference node or on a tuple's borrowed elements, which the two rows above then read |
| a borrow passed beside a `&mut` or `&uni` argument the callee could store it through | **yes** | the loan walk (`pwCallStores`): any writable borrow, a method's receiver included, of a place that can hold a borrow (`&T` itself, a struct with a borrow field, an `Option` or `List` of borrows, a slice of them) takes what the other arguments carry for a parameter whose lifetime may be held where it points. Into a place of the function's own that follows flow, as a store written out does: the call is legal, and the place used after a borrow it may hold has ended is refused there (`ErrorFrozen`); into one that may outlive the function, a borrow of the function's own storage, or of a lifetime the caller did not lend there, is refused at the call (`ErrorCallEscape`) |
| a borrow coerced to another reference type — widened to a base trait's reference, or made a `&<Trait` | **yes** | `iexpCoerce` types the injected cast with a copy of the target reference type carrying the source borrow's scope, which the rows above then read |
| a borrow **laundered through a variable** | **yes** | a variable's lifetime follows what it holds now, on each path (the loan walk): assigned a shorter borrow, it may use it while its source lives; kept past the end of its source's block and used there, it is refused at the source's end (`ErrorFrozen`); returned, or stored where it may outlive the function, while it may hold a borrow of the function's own storage, it is refused there (`ErrorEscape`) |
| a borrow **captured or stored in a field** | **yes** | a struct, `Option`, array, list or `Rc` holding a borrow carries its loans as a bare borrow does (the loan walk): held in a local and kept past the end of the borrow's source and used there, or handed on by a block past the end of its own variable, it is refused (`ErrorFrozen`); returned, or stored through a parameter or into a global, or handed to a call that may store it so, while it holds a borrow of the function's own storage, it is refused (`ErrorEscape`, `ErrorCallEscape`), and stored into a global while it holds a borrow the caller lent, too (`ErrorEscape`). Nothing is captured yet: an anonymous function reads no local of the function enclosing it |
| a borrow of a **temporary** kept past its statement | **yes** | as Rust: what a variable's initializer borrows, through a place, a recast or a tuple, array or variant literal (`imm r = &mk();`, `&mk().n`, `&*mkso()`, `(&mk(), 1)`), or the owner it lends as a borrow (`imm r &R = mkso();`), or what a block's final expression, an `if`'s arm or a `match`'s arm there borrows (Rust 2024's extension; given its value where the arm runs, drop-flagged), becomes a hidden local of the block (`varDclExtend`) and lasts to its end. Any other temporary, borrowed anywhere (`f(&mk())`, `&(a + 1)`, `mk().get()`), ends with its statement, and the loan walk roots a borrow of it there: held past the statement and used (`imm r = id(mkso()); r.n`, `imm r = id(&mk()); r.n`, Rust's E0716), it is refused at the temporary (`ErrorFrozen`); returned or stored away (`fn f() &R { mkso(); }`), it is refused (`ErrorEscape`). A lock's guard, the temporary a borrow through `Arc[Mutex, T]` reads through, is such a temporary, and holds its lock to the same end |
| two parameter borrows with different lifetimes | **yes** | named on a signature's references (`&'a T`, `&'a mut T`, `&'a Array[T]`; `'static` the global one), on a type's arguments there (`Option[&'a T]`), and on a struct's fields and uses (`struct Cursor['a]`, `Cursor['a]`; one name in a struct's fields may go undeclared), each name its own lifetime, the unnamed one more, none ordered against another but by a `where` clause (`'a >= 'b`, `'a == 'b`, transitive; a struct's own clause holds wherever it is used) or by what a borrow in the signature implies (`&'a Pair['b]` and `&'a &'b T` order `'b >= 'a`, Rust's implied bounds), never instanced. What a caller lends through a parameter is held in parts: what its reference points at, and what that holds, by lifetime for a struct declaring them (caller loans per part; loan-set entries tagged with a struct's lifetime where it is known exactly, `flowloan.h`). In the callee, the loan walk lets a part out only where the result holds a lifetime it flows to (`loanCallerApart`), and into what a parameter points at only where that holds one, field by field for a struct declaring lifetimes (`loanStoredApart`); at a call, the result's scope, its loans and the store check read of each argument only the parts flowing where they go (`lifeCarry`; `fnCallNarrowestBorrowScope`, `pwCall`, `pwCallStores`), so the result of a method of a local returning its struct's lifetime carries what the local holds, not the local; a call that may move a borrow between a struct's lifetimes leaves what the struct holds no longer known by lifetime (`pwCallMoves`); a `'static` parameter takes only a global borrow. A trait's implementation must promise at least what the requirement it meets promises (`lifeSigMeets`: a result holding no more, a parameter `'static` only where the requirement's is), and a function taken as a reference exactly what the signature it meets promises (`lifeSigsAgree`). A lifetime bound says what a type whose insides are unknown holds outlasts: on a generic function's type parameter, `[T + 'a]` or `where T + 'a`, the pair `'+T >= 'a` in its order, its argument's lifetimes all renamed `'+T` in the instance (`lifeRenamed`), never instanced per lifetime, so the body may store or return a `T` as `'a` and a call carries the argument wherever `'a` flows, a `'static` bound's argument checked global (`pwStaticArgs`); on a borrowed virtual reference, `&<Trait + 'a`, the type holding `'a` and a value returned or stored through a parameter as it holding no borrow not known to last `'a` (`pwBoundHolds`); an owning virtual reference, `So[Trait]`, bounded by `'static` as Rust's `Box<dyn Trait>` is, a value made one holding only global borrows (`loanNotStaticIn`). An unbounded parameter's argument inferred from a borrow carries no band into the instance, as a written one carries none (`lifeErased`). Not yet: a bound on a generic type's parameter, or another bound on an owning virtual reference |
| a dynamic arena's reference used with another arena, or on its own | **yes** | invariant lifetimes (`'=a`), checked in type check as brands compared by identity (`lifetime.c`, "Invariant lifetimes: brands"): a reference of one is a key, reaching nothing but through the arena of the same brand (`ar[key]`, `ErrorKeyAccess`); each call of a function whose result names one its parameters do not mints a fresh one, so two arenas never share a brand (`ErrorBrand`); a brand minted in a loop's pass is kept for no later pass (`ErrorBrandLoop`); a key's referent holds no borrow (`ErrorKeyBorrow`). The arena's `[]` is the key's pointer: no check at run time |
| aliasing of borrows | **partly** | a mutable borrow of a source reached as `uni` (a local) excludes every other borrow of it until its last use (the freezing row below); through a shared `mut` path aliasing is the design, and what it must not break is a shape: a reference into an enum's variant may be narrowed only through `uni`, `imm` or `mut1`, or through a borrow of a local made where it is matched, which freezes the local (`castSumInterior`). A borrow a call returns excludes as a borrow of each argument would (a receiver's mutable borrow two-phase), and a call's arguments exclude each other. A borrow held inside a value in a local excludes as the bare borrow would. Copies of one `&mut` borrow may still reach one place two ways, and an element of a resizable collection may still be borrowed through a shared path: the borrow freezes the path it was taken through, but a change through another reference to the collection is not seen (the collections declare `ShapeChanging`; the check that refuses it outright is not built) |
| freezing a borrow's source | **partly** | the loan walk (`flowpath.c`, `flowloan.c`, `ErrorFrozen`): a borrow held in a local, bare or inside a struct, `Option`, array or list the local holds (put there by building the value, by a store into it or through a reference to it, or by a call handed a `&mut` of it), freezes its source until the borrow's last use, on each path — for a source reached as `uni`, against a change, a move, a borrow that would conflict, the source's end, and, for a mutable borrow, a read; for one reached through a shared path (a `&mut`/`&ro` reference, a `Rc[mut, T]` owner), only against what would end it and an `&uni`/`&imm` borrow. A borrow a call returns freezes every argument as that argument's borrow would (`list[0usize]` keeps `list` loaned read-only), except that a container declaring `NoLoanMut` or `NoLoanRead` (the arena) keeps only its life (`a.alloc(v)`); an owning reference coerced to a borrow freezes its owner (to a `&uni`, as the borrow `&uni *o` built by type check, not a move), and a `&uni` reference lent where a `&` or `&mut` is wanted is reborrowed (`&mut *r`, built by type check) and so frozen, not moved; a call's arguments are checked against each other, a method receiver's mutable borrow two-phase; a holder whose `final` may read its borrow uses it as it dies (Rust's drop check; a collection's elements, behind a raw pointer, are taken as finalized only). Not yet: an element borrow through a shared path |
| array and slice bounds | **yes** | `genlBoundsCheck`, per dimension, and a range's start, end and count (`genlSubslice`): a failed check panics, naming the values it compared (`genlPanic`) |
| **raw pointer** bounds | **no** | unchecked by construction |
| raw pointer deref / arithmetic gated by `trust` | **no** | `trust` is not a keyword and has no parse rule |
| allocation failure | **yes** | null test then a panic naming the size asked for, unless `?` asked for an `Option` |
| what may cross threads | **partly** | the built-in marker `Sendable`, which `thread.start` and sync's channel types ask of what they carry (`ErrorNotSendable`): refused are a borrow of any permission (but an `imm`, `opaq` or `uni` borrow that lasts the whole program crosses, where a generic bounds its parameter `+ 'static` or an actor's parameter is written `'static`, if what it reaches does), an owner that may be aliased without a `RaceSafe` permission or in a region not declaring `ThreadSafe` (`Rc[imm, T]`, `Arc[mut, T]`), a traced reference, and a raw pointer, anywhere a value holds them, through owning references too (`refThreadBinds`, `itypeThreadBound`). A type declaring `Sendable` is taken on trust. Not checked: a `mut` global reached from several threads, what a started thread makes for itself (`Gc` on a thread), and anything crossing by a route that does not ask (a raw pointer cast, an `extern` call) |
| a value shared between threads reached only under its lock | **yes**, through a lock permission | `Arc[Mutex, T]`, `Arc[Rwlock, T]` (and `Rc[Rwcell, T]` on one thread): the reference reads, writes, lends and calls nothing on its value (`ErrorLockAccess`); a borrow through it takes the lock and holds it to the end of its statement, or of the block where a local's initializer made it, the guard it reads through dying there on every path (`borrowLockPlace`); the borrow is refused returned, stored outward or used after its guard (`ErrorEscape`, `ErrorFrozen`). A plain `Mutex` held as a value guards nothing the compiler sees. Not prevented: deadlock |
| release of an owning reference at scope exit | **partly** | once, on the paths that still hold it: a variable moved, hollowed or given a value on only some paths carries a drop flag the release tests, as does one stored over. A temporary is released at the end of the statement that made it (of an `if` or `while` condition, or of the right operand of `and` or `or`), newest first. A temporary a variable's initializer extends is released at the end of its block, as a local is. Leaks for the rest of an array one element was moved out of, and for a temporary that a raw pointer made from it may outlive. One a struct, an enum, a tuple or an array holds, however deep, is released with it, and one a module's global holds by its module's finalizer, which `finalAll()` runs |

## The four shapes the gaps take

Grouping them is more useful than the list, because each shape fails the same
way wherever it appears.

**1. A rule with a representation but no consumer.** The data is computed and
nothing reads it. `MayAliasWrite`, `MayIntRefSum` and `IsLockless` are set on
every permission and consulted nowhere (`RaceSafe` and a region's `ThreadSafe`
are the thread check's since it was built). These look like working machinery
in a grep and are inert.

**2. A rule enforced at some sites and not others.** Borrow lifetime was the
worst case: the scope is recorded on every bare borrow and checked at three
sites, a return, a store and a call, and a borrow carried through a variable
or inside a value escaped them. The loan walk now reads what a value holds at
the same sites, and the gate walks every function that has one.

**3. A rule enforced on one walk rather than every path.** Initialization and
move state live on the declaration and follow the walk in source order, the
arms of an `if` joined after it, and a loop walked once. So a move in one arm
counts as a move after the `if`, conservatively, and what a loop's later pass
sees is not there. Where a variable's state may differ by path, the path walk
follows every path — for its drops (a drop flag where the paths disagree) and
for the uses a loop's earlier pass leaves without a value — as it does for
freezing; a function without such a variable is not walked twice.

**4. A guard that does not exist yet.** `trust` is the whole of this. The
compiler has no `trust` keyword, so a program using one fails as an unknown
name. The operations `trust` is meant to gate — raw pointer dereference,
indexing and arithmetic — all compile with no guard anywhere, which means none
of the checks it would switch off are switched on to begin with. The same holds
for the intrinsics whose misuse breaks memory safety (`finalize`, the slice
constructors, `readRaw`, `writeRaw`, `moveRaw`, the atomic operations): the compiler's registry marks
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
  failure panics rather than returning null: the program ends, saying what
  failed and where, and nothing runs after it.
- **A variable is not read before it holds something**, and not read or
  borrowed after its value moved away — on any path, because the check is
  conservative. A variable never given a value may be borrowed, so a method can
  fill it in, and a field the method left unset can then be read through the
  borrow.
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
- Bounds checks, the panics they end in, and pointer levels: [Generation](../../compiler/c/doc/phases/generation.md)
- Re-measuring any row: [Measuring](../../compiler/c/doc/diagnostics/measuring.md)
