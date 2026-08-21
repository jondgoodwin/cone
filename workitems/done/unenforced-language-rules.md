# Unenforced language rules

Rules the language documents, the compiler's data structures anticipate, and
nothing checks. Found by building the test suite; the evidence is under "Found
while building the groups" in [[add-test-suite|Add test suite]].

These are separated from the other defects because none of them is a bug in the
ordinary sense. In each case the code does what it was written to do; what is
missing is a decision that was never made, or a check that was never written
against a decision that was.

**Every entry has now been verified against the compiler, and every check that
could be written has been.** What is left on this page is waiting on features,
and each piece of it has been written into the work item that will build that
feature — [[types-pointers-and-borrowed-references|Types. Pointers and Borrowed References]] for lifetimes,
[[concurrency-threads|Concurrency Threads]] for the permission flags,
[[copy-and-alias-vs-move-semantics|Copy & Alias vs. Move Semantics]] for `WarnCopy`, and [[analysis-re-factor|Analysis re-factor]]
for the one analysis gate still global. Read this page for how the compiler got
here; look there for what to do next.

The survey that produced this page was reading a compiler that has since moved,
and much of it was misread at the time. Three entries turned out to be wrong in
the same direction — a rule that looked unenforced was enforced, and the reason
it looked otherwise was worth more than the entry. A fourth named the wrong set
of words. **The lesson that kept repeating: verify against the compiler before
acting, and read the reference manual before deciding a rule needs a ruling.**
Three times the manual had already given the answer being sought from Jon.

Each also has the same testing property, and it is an uncomfortable one: **the
suite cannot assert an absent check.** A scenario proves the compiler rejects
something; there is no way to write "and it should have rejected this too" except
as an `xfail`, which requires the construct to fail somehow already. Where a rule
is simply unenforced, the corpus records it by *establishing the opposite* — the
`safety` group compiles pointer arithmetic with no trust block anywhere, which is
the only honest way to pin the absence of a rule. `ref-flow-return` uses the same
device for the one lifetime case that is still not reached.

## ~~`imm` is not enforced at all~~ — closed: it was, and the gate that hid it is gone

**The original entry was wrong, and how it was wrong is the finding.** It said
there is no immutability check on a variable anywhere in the compiler. There is,
and there has been since 2021 (`bce6fbb`): `assign.c` reads the binding's own
permission through `iexpGetLvalInfo`, and all three of the cases the entry listed
as compiling clean are rejected with `ErrorNoMut`.

Two of the entry's examples are not defects at all. `imm r = &mut p; r.x = 5`
compiles, and should: the binding is immutable, the referent is not. And
`p[1] = 99` through an `imm p *i32` compiles because a pointer bypasses
permission checks entirely, which is `trust`'s subject and not `imm`'s.

**What was real is why the survey saw nothing.** `ErrorNoMut` on an assignment is
a *flow* diagnostic, and `fnDclTypeCheck` ran `blockFlow` only when the global
`errors` count was zero. One type-check error anywhere earlier in the compile, in
any function, silenced immutability checking, `ErrorMove` and the lifetime checks
for every function after it.

`fnDclTypeCheck` now records the error count on entry and runs `blockFlow`
whenever the function's own signature and body added nothing to it. A function
that type checked is analyzed regardless of what failed elsewhere.
`core-flow-gate` pins this, and is the one scenario in the corpus that
deliberately mixes a type-check failure with flow diagnostics.

**The entry's own example was inaccurate and is worth recording as such.** It
used `fn first() i32 { nosuchname }` to make the earlier error. `ErrorUnkName` is
a name-resolution diagnostic, and `conec.c` returns before type checking whenever
name resolution reported anything — so that program never reached the flow gate
at all, and never would have. Demonstrating the gate needs an earlier error that
is specifically a *type-check* error in a function *body*.

**`module.c` had the same defect one level up, and it is fixed too.**
`modTypeCheck` type checks every declaration's signature first, then every body,
and used to skip the body pass entirely unless the signature pass was
error-free — so one malformed signature suppressed every body diagnostic in the
module. It now records `FlagSigError` on a declaration whose own signature
failed, and pass two skips only those. A declaration with a broken signature is
still not checked in its body, because every use of a parameter whose type never
resolved would report again and name nothing the author can act on.

Removing the gate outright was measured first: it left the suite green, with
`--checktree` silent everywhere, and changed exactly one scenario — three extra
follow-on diagnostics from bodies checked against failed signatures. Skipping
only the failed declaration removes the cross-declaration part of that. The part
that remains is *within* one declaration, where a failed signature check leaves
`unknownType` rather than `errorType` and the follow-ons are not suppressed.
**Converting those paths to `errorType` is the remaining piece and belongs to
[[analysis-re-factor|Analysis re-factor]]**, which will be in all of them anyway; doing it as its own
project would mean auditing every signature-phase error path blind.

`FlagSigError` is `0x0100`. A module's declaration list holds type declarations
as well as functions and variables, so the bit has to be free in both the
declaration and the type flag blocks — an earlier attempt at `0x0040` collided
with `HasTagField` and silently stopped type checking every tagged union.

**One gate is deliberately untouched**, and is the reason a type-check or flow
scenario still cannot contain a name-resolution error: `conec.c` returns after
name resolution if it reported anything. Type checking a tree with unresolved
names is a different and much larger question, and also [[analysis-re-factor|Analysis re-factor]]'s.

Related: [[permissions|Permissions]], [[analysis-re-factor|Analysis re-factor]].

## ~~Module-level privacy is not enforced~~ — closed: enforced

**Closed by [[diagnose-instead-of-crash|Diagnose instead of crash]].** It was listed here on the belief
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
needs a ruling. Related: [[names-and-namespaces|Names and Namespaces]],
[[using-and-module-name-folding|Using and Module Name-folding]].

## ~~Only one lifetime rule is enforced~~ — closed: three are, and the fourth needs lifetime tracking

The entry was right that `assign.c` held the whole of lifetime enforcement, and
right about both of its concrete cases. **Neither needed a ruling.**
`reflifefn.html` had already published the rule: a borrowed reference is
restricted "from living beyond the scope they were created in", and a violation
"result[s] in a compiler error". So this was two checks to write, not a decision
to make — the third time on this page that the manual already held the answer.

- **Slices skipped the check entirely.** `assign.c` gated it on both sides being
  `RefTag`, so an `ArrayRefTag` fell through and a slice could outlive the array
  it borrowed. A slice borrows exactly as a reference does and carries the same
  scope, so both tags now reach the check. `collection-flow-escape` was the
  `xfail` that pinned this, and is now a passing `reject`.
- **Returning a borrowed reference to a local was not diagnosed.**
  `fn escapes() &i32 { mut a = 3; &a }` compiled clean. `returnFlow` in
  `ir/stmt/return.c` now reports `ErrorEscape` for a returned borrow whose scope
  is a local's, walking a returned value tuple element by element.
  `ref-flow-return` pins it.
- **Freezing the source of a borrow** is documented and unimplemented; the source
  variable stays fully usable. Untouched, and still open — it is a rule about
  what the *borrowed-from* variable may do while the borrow is alive, which is
  not a lifetime comparison at all.

**A latent defect found while doing this:** `newRefNode` never initialized
`RefNode.scope`, so every reference type node except the ones `borrowTypeCheck`
built carried whatever the allocator last left there. Both lifetime checks read
that field. It is now initialized to 0 — global lifetime — at construction.

**What the return check does not reach**, established rather than claimed in
`ref-flow-return`: a borrow laundered through a variable. Scope lives on the type
node the borrow expression produced, and an assignment does not carry it onto the
variable's declared type, so `mut r &i32; r = &local; r` returns clean.

**That remainder is feature work, and it has moved.**
[[types-pointers-and-borrowed-references|Types. Pointers and Borrowed References]] now carries a section recording what
is enforced, how a lifetime is represented, the scope numbering, and why each
remaining hole needs work above it rather than a check. Two of its existing
bullets turn out to be exactly this problem — "Should borrowed ref vtype be
cloned? So, scope value is distinguishable?" is the enabling question, and "Add
lifetime borrowed ref check on vardcl" is the check that becomes tractable once
it is answered. Nothing about lifetimes is left on this page.

## Two permission flags are populated and never read

**Verified twice, unchanged, and nothing to do until concurrency work starts.**

`permission.h` declares `RaceSafe` — "may be shared with or sent to another
thread" — and `corelib.c` sets it on `uni`, `imm` and `opaq` while withholding it
from `mut`, `ro` and `mut1`, matching `refconccomm.html`'s sendability prose
exactly. `IsLockless` is set on all six. **Neither is read anywhere in the tree**:
the only occurrences are the two definitions and the six `newPermNodeStr` calls.

The concurrency design is one `permGetFlags` test away from being partially
enforceable, and that check does not exist — because there is nothing yet to
check. A flag test needs a construct that sends a reference to another thread,
and no such construct parses; `spawn` and `actor` are reserved words as of the
entry below, not implemented ones.

Deleting the flags would be wrong in a way deleting the eight dead diagnostics
was not: those were superseded, with another code already reporting the
condition. These encode a live design decision that matches published prose, and
re-deriving which permissions are sendable later would cost far more than the two
lines cost now.

**The finding has been written into [[concurrency-threads|Concurrency Threads]] itself**, since that
is the file that will be open when it matters. This entry stays open only in the
sense that a feature has not been built.

## ~~Eleven diagnostics are declared and never raised~~ — closed: one left, and it is waiting on a feature

Triaged per code by writing the program each was supposed to catch and reading
what the compiler actually said. **Eight were superseded and are deleted, two
named their condition exactly and are now raised, one remains.**
`python test/run.py --coverage` derives the list by scanning `src/`, so it cannot
go stale: it now reports 56 of 59 codes covered, against 52 of 65 before.

Deleting was safe because `ErrorCode` values are explicit, so each deletion
leaves a numbered gap rather than renumbering its neighbours. Each gap carries a
comment in `error.h` naming what took the code's place, so the next reader does
not re-derive it.

| Deleted | What actually reports the condition |
| --- | --- |
| `ErrorDupImpl` 1008 | `ErrorDupName` — "Global name is already defined" |
| `ErrorNotFn` 1018 | `ErrorNoMbr` — calling a non-callable is a missing `()` member |
| `ErrorNoVtype` 1030 | `ErrorBadTree`, which `--checktree` raises for a node left with no type |
| `ErrorNotLval` 1032 | `ErrorBadLval` — "Expression must be lval" |
| `ErrorAddr` 1033 | `ErrorBadLval`, for the same reason |
| `ErrorNoFlds` 1035 | `ErrorNoMbr` |
| `ErrorNoDbl` 1037 | nothing — no construct requires a `::` that could be missing |
| `ErrorBadSlice` 1047 | nothing — `&[]` over a non-array is a one-element slice by design (`borrow.c`), so there is no bad-slice condition |

Two were not superseded so much as *unclaimed*: the condition had a check and the
check was reporting under a generic code, against this repository's rule that a
diagnostic gets its own `ErrorCode` and that unrelated conditions stay
distinguishable. Both now raise the code that names them:

- **`ErrorNoElse` 1028.** `if.c` reported "if requires an 'else' clause (or
  exhaustive matches) to return a value" as `ErrorInvType`. This was the entry's
  own instructive example, filed as superseded; it was in fact the right code
  going unused. `union-typecheck-match` asserts it.
- **`ErrorBadAlloc` 1036.** `region.c` reported all six `_alloc` and `init` shape
  complaints as `ErrorInvType`. They are now 1036. "Not a valid region" stays
  `ErrorInvType`, because a non-struct region is a type error rather than an
  allocation one. `region-typecheck-region` and `region-typecheck-init` assert it.

**`WarnCopy` 3003 is the one left, and it is deliberately kept.** It names an
unsafe copy of a `CopyMethod` or `CopyMove` typed value; neither name exists
anywhere in the tree — the only occurrence of either is that comment in
`error.h`. So this is not an unwritten check against existing machinery: the
machinery is absent too, and the intent lives in
[[copy-and-alias-vs-move-semantics|Copy & Alias vs. Move Semantics]] as "'Copy' method: use w/ allocated
references, assignment, fn call, etc."

That makes it a different category from the eight deleted above. Those were
superseded — another diagnostic already reported their condition. This one has no
substitute because its condition cannot arise yet, and `--coverage` says so
accurately. The argument for deleting it is only that an empty "raised by
nothing" list would make any future entry a red flag rather than noise-plus-one;
weighed against losing the intent from where an implementer will look, keeping it
wins.

## ~~Reserved words, before the features that need them~~ — closed, and the list was not the one in this entry

The entry was right that none of `throw`, `catch`, `panic`, `assert`, `yield`,
`spawn` or `actor` was a keyword, and right that implementing [[error-handling|Error Handling]]
or any concurrency work would break user code no test would catch.

**What it missed is that `reftoken.html` publishes a reserved-keyword list, and
that list overlaps this entry's by exactly one word.** The manual reserves 28
keywords "and may not be used as identifiers". Ten of them had no token in the
lexer at all and compiled clean as function names: `async`, `baseurl`, `context`,
`local`, `new`, `selfmethod`, `using`, `wait`, `yield` — and `this`. So the page
was proposing to reserve six words the manual had never claimed while nine words
it *had* claimed went unreserved.

**Fifteen words are now reserved**, per Jon's ruling: the manual's nine
unimplemented ones plus this entry's six, which `refexcept.html`,
`refcorout.html` and `refconccomm.html` describe as the syntax of planned
features. `reftoken.html` has been updated with the six it was missing.

**`this` and `self` are deliberately excluded, and finding out why was the point
of checking.** Both are on the manual's reserved list, and both already work:
`self` is a method's first parameter, and `this` is the value of a `with` block —
verified by compiling one. Reserving them as "not implemented yet" would have
broken a working, documented feature. A word whose feature already exists is
implemented, not reserved.

The lexer maps each reserved word to `ReservedToken`, reports `ErrorReserved`
where it was written, then releases the name so the rest of the compile treats it
as the ordinary identifier the author meant. That reports each word once, at its
first appearance, and leaves the file otherwise diagnosed as it would have been.
`lexical-reject-reserved` pins all fifteen.

The corpus sweep the entry expected to be expensive was one rename: `local` in
`region-typecheck-coerce`. It would only have grown with every scenario and
example written, which is why this was the item with a deadline.
