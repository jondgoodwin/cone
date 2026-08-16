# Unenforced language rules

Rules the language documents, the compiler's data structures anticipate, and
nothing checks. Found by building the test suite; the evidence is under "Found
while building the groups" in [[Add test suite]].

These are separated from the other defects because none of them is a bug in the
ordinary sense. In each case the code does what it was written to do; what is
missing is a decision that was never made, or a check that was never written
against a decision that was. **Every one of them needs a language answer before
it needs a patch**, which is why they are not in [[Diagnose instead of crash]] or
[[Ownership memory safety]].

Each also has the same testing property, and it is an uncomfortable one: **the
suite cannot assert an absent check.** A scenario proves the compiler rejects
something; there is no way to write "and it should have rejected this too" except
as an `xfail`, which requires the construct to fail somehow already. Where a rule
is simply unenforced, the corpus records it by *establishing the opposite* — the
`safety` group compiles pointer arithmetic with no trust block anywhere, which is
the only honest way to pin the absence of a rule.

## `imm` is not enforced at all

```cone
imm n = 3
n = 5            // compiles clean
```

So does `p.x = 5` on an `imm p`, and `r.x = 5` through an `imm r = &p`, and
`p[1] = 99` through an `imm p *i32`. There is no immutability check on a variable
anywhere in the compiler. `ErrorNoMut` exists and is reachable, but only through
*reference permissions* — never through the mutability of the binding.

This is the largest gap the survey found, and probably the one with the widest
consequences: `imm` appears throughout the reference manual, the examples and the
playground, and every use of it currently means nothing. Related:
[[Permissions]].

## Module-level privacy is not enforced

`importNameRes` (`ir/stmt/import.c`) folds **every** named node of an imported
module, private ones included, and `nameUseNameRes` resolves `mod::_privateName`
without complaint. Privacy exists as `LLVMHiddenVisibility` in genllvm and as the
`ErrorNotPublic` check on methods and fields — that is all.

The consequence is a crash rather than a leak, because codegen skips private
declarations in a non-generating module and the call site then uses a null
`llvmvar`. Commit `34ca637` fixed one path of this (private *function candidates*
reached through a public overload name, which `test/cases/module/module-imports`
now pins as a regression test) and left the others.

The decision: reject cross-module private access at name resolution, which is
probably the intended rule, or generate symbols for everything reachable. The
first makes the crash impossible; the second makes privacy advisory. Related:
[[Names and Namespaces]], [[Using and Module Name-folding]].

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
