# Compiler defect backlog

The residue of the test-suite survey: defects with a clear right answer, no
language decision attached, and no shared cause. Each belongs near an existing
work item rather than to this one; this page exists so that none of them is lost
between the survey that found them and the feature work that will touch them.

The three severe groups are elsewhere. [[Ownership memory safety]] and
[[Diagnose instead of crash]] are done and now live in `workitems/done/`;
[[Unenforced language rules]] is not. Full evidence for everything below is under
"Found while building the groups" in [[Add test suite]].

**Every entry below has been re-verified against the compiler by writing the
program and reading what came out.** Four were wrong as recorded, in the same
direction each time — the symptom was real and the stated cause was not — and
each is corrected in place with its evidence. Two turned out to be memory-safety
bugs rather than missing checks. Do the same before acting on what is left: the
check costs a minute and it has now changed the work six times.

**Most of these are pinned by an `xfail` scenario**, which means the suite fails
the day one is fixed and the fixer is told to remove the mark. They cannot be
fixed quietly and they cannot rot silently. The few that are not pinned are
marked below, and those are the ones worth doing first, because nothing is
watching them.

## Fixed

Six are done, with a scenario each. They are recorded here rather than deleted
because two of them were mis-diagnosed on this page and the correction is worth
more than the entry was.

| Defect | What it actually was | Now covered by |
| --- | --- | --- |
| Writing through `&mut [N; T]` | Two defects. `iexpGetLvalInfo`'s `ArrIndexTag` case took the permission from the reference only for `ArrayRefTag` and `PtrTag`, as recorded — and repairing that exposed a second: `genlAddr`'s `ArrIndexTag` case had no `RefTag` arm either, so the write then reached an `assert` a Release build drops, and crashed | `collection-success` |
| Slices cannot be compared | **Not the signature.** `newArrayRefTypeMethods` declares `==` over `unknownType`, but so does `newRefTypeMethods`, and reference comparison works — because `iNsTypeFindPtrMethod` selects a compiler-declared pointer method by comparing the two *arguments to each other* rather than to the deliberately generic parameter, and `ArrayRefTag` was missing from the tags taking that path. `genlexpr` then had to compare the two words instead of `icmp`-ing an aggregate | `collection-success` |
| A name-fold clash names the wrong file | **Every cross-module diagnostic named the wrong file**, not just a fold clash. `lexInject` recycled a popped `Lexer` block, and a node holds the `Lexer` it was built with and reads its url when a diagnostic is reported, so the second import rewrote the url out from under every node of the first. It also named `conec`'s output files after the wrong source | `module-typecheck-provenance` |
| `ErrorFewArgs` with a "too many" message | Exactly as recorded: the guard is `args->used > parms->used`. The message was right and the code was wrong | `generic-typecheck-infer` |
| `as` onto a struct target is unchecked | **A stack over-read, not a missing check.** `genlRecast` allocates the source's bytes, bitcasts, and loads the target's, so `n as Big` emitted a 24-byte load from an `alloca i32` — and passed `--verify`. `castBitsize` has no answer for a struct, so the size is now checked in `genlRecast`, where the data layout exists, under `ErrorRecastSize` | `typemgmt-genllvm-recast`, `typemgmt-success` |
| `each`'s increment reports at the closing brace | As recorded. The synthesized nodes are now positioned on the range expression | `each-typecheck` |

One latent crash was found on the way and fixed with them:
`newFnCallOpnameLower` was defined in `fncall.c` and declared in no header, so
the first caller outside that file got an implicit declaration returning `int`,
truncated the returned pointer to 32 bits and segfaulted.

## Wrong code generated

| Defect | Cause | Pinned by |
| --- | --- | --- |
| `&mut p.x` — a field borrow without parentheses | `parseAmper` (`parseexpr.c:321-326`) re-applies suffixes to the borrow, so it becomes a field access on `&mut p` carrying `FlagBorrow`. `fnCallTypeCheck`'s field branch ignores the flag and types the access as the field's own type, while `genlexpr.c` honours it and returns the field pointer. LLVM verification fails. The array-index path (`fncall.c:203-208`) has the fixup the field path lacks | `ref-field-borrow` |
| The nullable-pointer optimization | `genlTypeMeta` (`genllvm/genltype.c:174-207`) gives base and variants a bare pointer type while the variant initializer still stores through the variant's struct type. Verified: `--verify` reports "Stored value type does not match pointer operand type" on construction | `union-nullable-ptr` |

## Declared but unusable

| Defect | Cause | Pinned by |
| --- | --- | --- |
| No method on a generic struct can be called | `self` keeps the generic's type after cloning, so the call finds no candidate. Verified: two `ErrorInvType` "self parameter for a method must match" at the instantiation, then `ErrorNoCandidate` at the call | `generic-struct-method` |
| `&Box[T]` — a reference to a generic instantiation — does not parse | Bare `Box[T]` is a fine type; only the `&` form fails, with `ErrorNotTyped` then "Expected a type". Blocks any generic collection with `&self` methods | `generic-ref-instance` |
| Writing a trait field through `&<mut` is checked against the binding | `imm m &<mut Meter` is rejected while `mut m` is fine, where a plain `imm r &mut Rect` writes through happily | `trait-vref-lval` |
| A struct field of virtual-reference type cannot be assigned | A `&i32` field can | `trait-vref-lval` |
| The whole-value `` `&[]` `` operator method is unreachable | Verified. `&[]mut value` is the borrow *operator* and types as `&[]mut Struct` — a one-element slice of the struct — rather than dispatching. The method is not unreachable in general: `(&mut v).`&[]`()` calls it and returns the declared type. Only the operator syntax skips it | **not pinned** |
| `Bool[r]` and `Bool[p]` fail | **Re-scoped.** `typeLitNbrCheck` (`ir/exp/typelit.c:57`) requires the source of a number literal to be a number, so the `Type[value]` path rejects a reference *and* a pointer, while `r into Bool` and `p into Bool` both work. The page blamed pointers; references fail identically and have nothing to do with `PtrTag` | **not pinned** |

`trait-vref-lval`'s header comment is now stale: it says only the first of its
two diagnostics is reported, because flow analysis ran only while the global
error count was zero. That gate is per declaration now and both are reported.
Rewrite the comment when the scenario is fixed.

## Reported in the wrong place

- **A name-fold clash between two wildcard imports names the wrong file** —
  fixed, and it was much larger than recorded. See the table above.
- **`ErrorFewArgs` with a "too many" message** — fixed. See the table above.
- **`each`'s synthesized increment** — fixed. See the table above.

## Behavioral, and possibly intended

Recorded because the survey could not tell. Two are now settled; two still need
Jon and are listed under "What needs a decision" below.

- **`continue` inside an `each` hangs forever.** Confirmed by reading
  `parseEach`: the increment is appended to the end of the loop body
  (`parsefnflow.c:298-311`), so `continue` jumps over it and the loop variable
  never advances. `break` is fine. Unpinnable — the runner would have to hang to
  observe it. **No ruling needed**; what it needs is somewhere for the increment
  to live that `continue` reaches, which the parser rewrite has no notion of
  today.
- **A method cannot be called through a raw pointer.** **Re-scoped, and it is
  not only pointers.** See "What needs a decision".

## Found while fixing [[Ownership memory safety]]

Both were found by tracing something else, and neither belongs to ownership.

- **An array holding owning references is never released.** Confirmed by reading
  `flowScopeDealias` (`ir/flow.c:230-260`): it tests `reftype->tag == RefTag`, an
  array's type is `ArrayTag`, and the `else` branch asks
  `itypeGetDropFnDcl`, which returns NULL for anything but a struct. So every
  element leaks, however the array was built. `region-fill-count` records this in
  passing.
- **`itypeIsGenericType` tests a tag nothing ever assigns.** Confirmed:
  `GenericNameTag` appears in its own declaration (`inode.h:159`), three dispatch
  tables, and this test (`itype.c:328`) — and is never written to a node.
  `nameUseNameRes` gives a use of a `MacroDclTag` declaration `MacroNameTag`, and
  generics and macros share that declaration tag, so `MacroNameTag` is what the
  predicate should be testing. **Careful:** making it so would also let a macro
  call `MYMACRO[x]` satisfy `isTypeNode`, which is not a type. This is why the
  fallible-allocation lowering fails; see [[Regions]].

## What needs a decision

Four questions, with a recommendation for each. The first three are language
questions; the fourth is a routing question about this page.

### 1. Calling a method through a raw pointer, and through a reference

Jon has said a method should be callable through a raw pointer. Phase 1 was
asked to confirm whether that means synthesizing a reference from a pointer,
which `refptr.html` documents as needing `trust`. **It depends on the receiver,
and the answer splits the work cleanly:**

- A **value receiver** (`fn sum(self)`) needs only a dereference — a load,
  exactly what field access through a pointer already does. **No reference is
  manufactured and `trust` is not implicated.** `(*sp).sumValue()` compiles and
  runs today.
- A **reference receiver** (`self &`, `self &mut`) does require making a
  reference out of a pointer: a borrow with no region, lifetime or permission,
  which is the operation `refptr.html` spells `trust{ptr1 as &i32}`. **But that
  operation is already spellable and already unguarded**: `imm r &mut Point =
  &mut (*sp)` compiles clean today, outside any trust block, and generates valid
  LLVM — as do `*p`, `p[i]`, `p.x` and `sp.y = 9`. So the fix manufactures
  nothing the language does not already hand out; it is sugar over a borrow the
  programmer can write by hand.

  This matters for scheduling. Requiring `trust` for `p.method()` alone, while
  every other pointer operation stays unguarded, would be a lone island of
  strictness rather than a safety improvement. The `trust` design has to arrive
  for all of them at once, and when it does this call site is one of the many it
  will have to wrap, not a special case.

**And the gap is not confined to pointers.** `fncall.c:731-736` looks like it
retries method lookup after `derefInject` on the `RefTag` path — the comment in
`safety-typecheck-ptruse` says so — but the retry is **dead code**: it is gated
on `fnCallLowerMethod` returning 0, which means "this type supports no methods
at all", and the surrounding branch has already established that it does. So a
value-receiver method is not callable through a reference either:
`fn sum(self)` called as `rp.sum()` on a `&Point` reports `ErrorNoCandidate`.
`struct-methods.cone` never caught it because its `sum` has a `self &`
candidate too.

**Recommendation:** do both halves now. Repair the dead retry so it fires when
lookup found the name but no candidate accepted the receiver, and let it try a
deref and a borrow in turn — which is what the programmer would write by hand
and what the reference path is already trying to do. It needs no language
decision, it grants no capability pointers do not already have, and it fixes a
reference bug nobody had noticed.

The architectural question underneath — that a raw pointer can become a
reference at all, with no region, lifetime or permission, and that nothing
anywhere requires `trust` — is real and is bigger than this call site. It
belongs to the `trust` work rather than here; see [[Unenforced language rules]]
and `reftrust.html`. Fixing the lookup now does not make it worse and does not
make it harder to close later.

`safety-typecheck-ptruse`'s `methodThroughPointer` case must then be rewritten —
it asserts the gap as though it were the rule — and the working forms moved to
`safety-pointers` beside the field access they resemble.

**Status: the deref half is done; the borrow half is deferred and routed.**
`fnCallLowerMethod` re-selects against the dereferenced receiver when no
candidate accepted it as it stood, so a value receiver is now reachable through
`&T`, `&mut T` and `*T` alike, and the dead retry is gone. Nothing borrows on the
receiver's behalf: `self &` and `self &mut` stay out of a value's and a pointer's
reach, pinned by `struct-typecheck-methods` and `safety-typecheck-ptruse`.
`struct-methods` and `safety-pointers` run the working forms. The borrow half,
and the operator fall-through the deref half brings with it, are written up under
"Receiver adjustment" in [[Types. Pointers and Borrowed References]].

### 2. Branch inference does not unify two identical closure types

Confirmed: `imm f = if flag {&fn(x i32) i32 {x+1}} else {&fn(x i32) i32 {x+2}}`
reports "Branch's expression type inconsistent with other branches", while the
same thing with a declared type `imm f &fn(x i32) i32 = ...` compiles, and the
same shape with numbers compiles. Each anonymous function gets its own type node.

**Recommendation:** this is a defect, not a decision — coercion to a declared
type already proves the two types are the same type, so inference disagreeing
with coercion is an inconsistency rather than a policy. It belongs with
[[Types. Function and Closure]] and probably with whatever interns function
signature types. Confirm that reading with Jon before it is scheduled, since it
is the one place the survey's "possibly intended" hedge might still be right.

### 3. A parameterless macro name is not expanded in two of four positions

Confirmed exactly as recorded, with `macro TWO { 2 }`:

| Position | Result |
| --- | --- |
| initializer, `imm a = TWO` | works |
| right operand, `1 + TWO` | works |
| left operand, `TWO + 1` | `ErrorManyArgs` "Incorrect number of arguments vs. parameters expected" |
| final statement, `TWO` | `ErrorInvType` "A return value is expected but this statement cannot give one" |

Both failures are positions where the name is the *receiver* of something — the
object of an operator call, or the block's value — rather than an argument.

**Recommendation:** a defect, and `refmacro.html` should be read before it is
scheduled, since it may or may not claim a parameterless macro is a value
anywhere a value goes. Route to [[Macro and Inline]].

### 4. Does `closure-capture` belong on this page?

**No.** The backlog page is for defects with a clear right answer and no language
decision. Closure capture is an unimplemented documented feature —
`parseexpr.c:296` lifts the anonymous function's declaration to module scope
while parsing, so it never sees its enclosing locals — and [[Add test suite]]
already routed it to [[Types. Function and Closure]]. What is worth adding is a
line in *that* work item saying `closure-capture` pins it, so whoever implements
capture knows the suite will tell them.

### Also worth a decision, though smaller

`usize[p]` was recorded here beside `Bool[p]` as one defect. It is a different
question: `p into usize` fails too, so no permitted conversion is being made
unreachable — a pointer simply does not *convert* to an integer. `p as usize`
does work, by reinterpretation. So the question is whether taking a pointer's
address as a number should be a conversion as well as a reinterpretation, which
is a language decision and not a defect. Recommendation: leave it as
reinterpretation only, and drop it from this page.
