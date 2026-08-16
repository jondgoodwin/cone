# Unenforced language rules

Rules the language documents, the compiler's data structures anticipate, and
nothing checks. Found by building the test suite; the evidence is under "Found
while building the groups" in [[Add test suite]].

These are separated from the other defects because none of them is a bug in the
ordinary sense. In each case the code does what it was written to do; what is
missing is a decision that was never made, or a check that was never written
against a decision that was. **Most of them need a language answer before they
need a patch**, which is why they are not in [[Diagnose instead of crash]] or
[[Ownership memory safety]].

**Two of the six entries below have already turned out to be wrong**, both in the
same direction: a rule that looked unenforced was enforced, and the reason it
looked otherwise was worth more than the entry. Module-level privacy needed no
language answer, because `refmodule.html` had already given one; it is closed.
`imm` is checked, and what hid the check was flow analysis being gated on the
global error count. **So verify each entry against the current compiler before
acting on it, and read the reference manual before deciding a rule needs a
ruling.** The survey that produced this page was reading a compiler that has
since moved, and some of it was misread at the time.

Each also has the same testing property, and it is an uncomfortable one: **the
suite cannot assert an absent check.** A scenario proves the compiler rejects
something; there is no way to write "and it should have rejected this too" except
as an `xfail`, which requires the construct to fail somehow already. Where a rule
is simply unenforced, the corpus records it by *establishing the opposite* — the
`safety` group compiles pointer arithmetic with no trust block anywhere, which is
the only honest way to pin the absence of a rule.

## ~~`imm` is not enforced at all~~ — it is; the real defect is that any earlier error silences it

**The original entry was wrong, and how it was wrong is the finding.** It said
there is no immutability check on a variable anywhere in the compiler. There is,
and there has been since 2021 (`bce6fbb`): `assign.c:162` reads the binding's own
permission through `iexpGetLvalInfo`, and all three of the cases the entry listed
as compiling clean are rejected with `ErrorNoMut`:

```cone
imm n = 3
n = 5              // ErrorNoMut
imm p = P[1]
p.x = 5            // ErrorNoMut
imm r = &p
r.x = 5            // ErrorNoMut
```

Two of the entry's examples are not defects at all. `imm r = &mut p; r.x = 5`
compiles, and should: the binding is immutable, the referent is not. And
`p[1] = 99` through an `imm p *i32` compiles because a pointer bypasses
permission checks entirely, which is `trust`'s subject and not `imm`'s.

**What is real is why the survey saw nothing.** `ErrorNoMut` on an assignment is
a *flow* diagnostic, and `fndcl.c:181` runs `blockFlow` only when `errors == 0`.
So one error anywhere earlier in the compile — of any stage, in any function —
silences immutability checking for everything after it:

```cone
fn first() i32 { nosuchname }   // ErrorUnkName
fn second() i32 {
  imm n = 3
  n = 5                          // reported by nothing
  n
}
```

The same gate means **two `imm` violations in one file report only the first**.
`design/Test Suite.md` already documents this as a hazard for writing scenarios;
what it does not say is that it makes a whole class of rule conditionally
enforced in ordinary use. `ErrorMove`, the lifetime check in `assign.c` and
everything else reported from the flow pass are behind the same gate.

That is the defect worth fixing, and it is one decision rather than several:
whether flow analysis should run on a function whose own type check succeeded,
regardless of what failed elsewhere. Related: [[Permissions]],
[[Analysis re-factor]].

## ~~Module-level privacy is not enforced~~ — enforced

**Closed by [[Diagnose instead of crash]].** It was listed here on the belief
that it needed a language answer first — reject the access, or generate symbols
for everything reachable and make privacy advisory. The answer was already
published: `refmodule.html` says a name beginning with an underscore "may not be
referenced outside the module", twice, once for modules and once for types. So
there was a check to write and no decision to make, and the crash made it urgent.

`importNameRes` no longer folds private names, and `nameUseNameRes` rejects
`mod::_privateName` with `ErrorNotPublic`. A private *candidate* reached through a
public overload name is deliberately untouched — the program never names it — and
`test/cases/module/module-imports` still pins that. `module-nameres` pins the
rejection.

This entry is the reason to check the reference manual before assuming a rule
needs a ruling. Related: [[Names and Namespaces]],
[[Using and Module Name-folding]].

## Only one lifetime rule is enforced

`assign.c:176-179` checks an lval's scope against the scope of the reference
being stored into it. That is the whole of lifetime enforcement.

- **Returning a borrowed reference to a local is not diagnosed.**
  `fn escapes() &i32 { mut a = 3; &a }` compiles clean.
- **Slices skip the check entirely.** `assign.c:175` gates it on
  `rvaltype->tag == RefTag && lvaltype->tag == RefTag`, and `ArrayRefTag` on
  either side falls through — so a slice may outlive the array it borrows. One
  missing tag test, pinned by `test/cases/collection/collection-flow-escape` as
  an `xfail`.
- **Freezing the source of a borrow** is documented and unimplemented; the source
  variable stays fully usable.

Lifetime *annotations* (`&'a i32`) have no parser syntax at all, which
`reflifefn.html` states accurately. Related:
[[Types. Pointers and Borrowed References]].

## Two permission flags are populated and never read

`permission.h:29` declares `RaceSafe` — "may be shared with or sent to another
thread" — and `corelib.c:51-56` sets it on `uni`, `imm` and `opaq` while
withholding it from `mut`, `ro` and `mut1`, matching `refconccomm.html`'s
sendability prose exactly. `IsLockless` is set on all six. **Neither is read
anywhere in the tree.**

The concurrency design is one flag check away from being partially enforceable,
and that check does not exist. Worth knowing before [[Concurrency Threads]] is
picked up: the hard part is already done and was left disconnected.

## Eleven diagnostics are declared and never raised

`ErrorDupImpl`, `ErrorNotFn`, `ErrorNoElse`, `ErrorNoVtype`, `ErrorNotLval`,
`ErrorAddr`, `ErrorNoFlds`, `ErrorBadAlloc`, `ErrorNoDbl`, `ErrorBadSlice`,
`WarnCopy`. `python test/run.py --coverage` derives this list by scanning `src/`,
so it stays accurate.

Each is either a check someone intended to write, or a leftover. Deciding which,
per code, is cheap and worth doing once: **deleting is now safe**, because
`ErrorCode` values are explicit, so a deletion leaves a gap rather than
renumbering its neighbours. Until they are resolved they hold R6.4's coverage
figure eleven codes below what it could reach.

`ErrorNoElse` is the instructive one. The exhaustiveness diagnostic everyone
expects it to be is actually `ErrorInvType`, "if requires an 'else' clause (or
exhaustive matches) to return a value" (`ir/exp/if.c:181`) — so the code is not
merely unused, it was superseded and never removed.

## Reserved words, before the features that need them

None of `throw`, `catch`, `panic`, `assert`, `yield`, `spawn` or `actor` is a
keyword. A program declaring all seven as functions compiles clean under
`--checktree --verify`. Implementing [[Error Handling]] or any concurrency work
item therefore breaks user code that no current test would catch.

Reserving the words ahead of the features costs one lexer table edit and one
sweep of the corpus, and it gets more expensive with every scenario and example
written. This is the cheapest item on this page and the only one with a deadline.
