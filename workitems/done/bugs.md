Defects with a known fix, needing no design decision and no major refactor.

**What belongs here.** All three must hold:

1. **The correct behavior is already clear.** Nobody has to decide what the
   language should do.
2. **The fix is local** — one function, or a few. No new IR node, no phase
   reordering, no subsystem refactor.
3. **It is a defect, not a missing feature.** "Crashes", "gives the wrong
   answer", "is spelled wrong" — not "is not built yet".

**What does not.** Anything whose fix waits on a language decision, anything
needing a refactor to fix properly, and unimplemented features. Where an item is
*part* bug and part open question, only the bug half lives here and the entry
says which work item holds the rest.

**Each entry carries what is needed to act**: how to see it, where it is, what
the fix is, and which test group the scenario lands in. An entry that cannot say
those things is not ready to be here.

---

# What has been fixed

Everything this file recorded at `50c6ba8` is fixed except the entries below,
and each fixed defect landed with the scenario that fails without it. Run
against a compiler built before the fixes, every one of those scenarios fails —
two of them by timing out, which is what a hang looks like from the runner.

Ten of the fixes have no scenario, and the reason is the same in each case:
nothing a source file can say reaches them. `iexpGetPermFlags` has no caller,
`ThreadBound` has no consumer, the two structural hashes and the dead
`cloneConstDclNode` and the dead `genlAddr` `FnDclTag` case change no output,
the vtable guard fires only if a compiler invariant broke, the two dead parser
parameters have no behavior, and the artifact and debug-metadata names are not
something the runner can assert today.

**Three of the fixes were bigger than the entry that described them**, and are
worth knowing about because each turned out to be a crash or a miscompile rather
than the tidy-up it was filed as:

- `genlAddr` tested a *type* tag where a tuple element's index is an expression
  tag, so `&t.0` never matched — and with the assert behind it compiled out of a
  Release build, control fell into the string-literal case and read the field
  access as an `SLitNode`. That is a segfault on one line of ordinary source.
  The general form of that failure mode is closed: [[compiler|Compiler]] audited all 22
  remaining `assert(0)` sites, converted every one to `errorUnreachable`, and
  fixed the three a source actually reaches.
- `genlConvert`'s surviving struct conversion used a mid-block `LLVMBuildAlloca`.
  Inside a loop that is a frame slot per iteration and `mem2reg` does not promote
  it, so `x into <struct>` in a long loop overflowed the stack.
- A stray `}` at global scope does not drive the block stack negative in any way
  that is read, which is what the entry suspected. What it did was worse:
  `parsePgm` was the one caller of the global-statement parser with no
  end-of-file check, so the rest of the main file was discarded in silence — no
  diagnostic, exit 0, an object emitted from half the source.

**One defect this pass found rather than inherited**, and fixed: a reference
type with no pointee — `fn f(s &)`, `&&`, `(&, i32)`, `*&`, `&[]`, `&[]&[]`, a
parameter after one of them, and a non-`self` parameter inside a method — parsed
cleanly and reached `genlType`, whose `Invalid vtype to generate` assert is
compiled out of a Release build and returns NULL. `LLVMPointerType` then
dereferenced it: access violation, no diagnostic, no exit code. `parseAmper`
accepts the spelling on purpose so a method can write `self &`, and `parseFnSig`
— which is the only thing that fills one in — fills in `Self` and inspects the
outer node's tag only. `refTypeCheck` and `arrayRefTypeCheck` now report
`ErrorNoRefType`, which covers all eight because every enclosing type reaches
them through `itypeTypeCheck` on its own pointee.

**One entry left on a premise that was wrong.** `.len` on a fixed-size array was
recorded here on the grounds that "the published array documentation says this
works". It does not: `coneref/refarray.html`'s own opening note lists `.len`
among what is unimplemented, and the body that describes it working is the
aspirational half the site warns about. Measured today, the compiler rejects it
cleanly with a correct exit code in every spelling. It is a missing feature, and
it has gone to [[collection-types|Collection Types]] with the two features the manual defines in
terms of it, `$-` and `each` over an array.

**One defect found while checking that**, and fixed: an array literal's elements
had to match the first element by `itypeIsSame`, with no supertype search, which
made an array literal the one construct that refused a variant where its union
was wanted — a variable initializer, a struct literal's field and an `if` branch
all accept one. They fold to a common supertype now and are coerced to it.

That turned out to sit on a wider hole in the fold itself: `structFindSuper` and
`structRefFindSuper` answered "these two variants share a base trait" and not
"one of these *is* the base trait the other extends" — which is what a third
value is asked about, once two have already widened the type in common to their
trait. So `if a {Circle} elif b {Rect} else {Circle}` failed where two branches
succeeded, in `if` and `break` as much as in an array literal. Both answer both
shapes now.

**Two things this pass surfaced and did not repair**, recorded where they belong
rather than here:

- `parseCloseTok`'s give-up return skips `lexDecrParens`, so an unterminated
  bracket leaks one from the open-delimiter count. Unobservable in the two
  unclosed-bracket scenarios, which recover by consuming to end of file.
  [[lexer-and-parser|Lexer and Parser]].
- `utf8IsLetter` is `utf8IsMultibyte(srcp) || isalpha(*srcp)`, so *every* byte
  at or above 0x80 — malformed ones included — is accepted as an identifier
  start, and multi-byte junk never reaches the bad-character path at all. That
  is [[lexer-and-parser|Lexer and Parser]]'s "UTF8 support for IsLetter", now with a reproducer:
  a line reading `a`, an invalid lead byte, and `a` is lexed as one identifier
  spanning the newline.

**One entry closed by a decision rather than by a fix.** `parseFieldDcl` rejected
the wrong permission — its check reduced to `== immPerm`, so it refused the one
permission that is legal and admitted four that are not — and the repair waited
on [[permissions|Permissions]] saying which permissions a field may carry. That answer
landed: `mut` or `imm`, on a field exactly as on a variable, with a field
defaulting to `mut` and a variable to `imm`. `parseDclPerm` now states the rule
once for both, which also applied the `defperm` that `parseFieldDcl` had always
ignored and retired `ro` on a variable along with the `ParseMayConst` flag that
existed only to allow it. The caveats Jon attached to that decision are in
[[permissions|Permissions]], not here.

**Two fixes are not assertable by the runner**, which is a gap in the runner
rather than in them. `--ir` naming its dump after the source compiled, and the
debug file name being the real source path, both produce a *filename* or *debug
metadata* — and a check may target only the post-optimization LLVM IR dump or a
run's stdout, and the runner never passes `--debug`. Either a check target for
artifact names or a `--debug` knob would close it. Carried by
[[design-notes-follow-on|Design notes follow-on]] rather than opened as its own item.

---

# Still open

Nothing. Both remaining entries were closed by a decision on 2026-08-21, and
what each decided is below so it is not re-derived.

## `structAddField` drops a duplicate field — closed, no defect

**`structAddField` is correct.** Two claims in the entry, neither of which
holds. `structTypeCheck` renumbers every field from zero during its field walk,
so the `index` the parser stamped is overwritten before anything reads it. And
`namespaceAdd` keeps the first node it finds and returns it rather than
replacing, so when the duplicate is skipped the namespace still names a field
that is in `fields`. Measured: a struct with a duplicated field name reports
`ErrorDupName` once, with no cascade, and nothing is generated because
`conec.c` only calls `genpgm` when `errors == 0`.

**The symptom the entry describes does exist, on a different path.** When a
struct extends a trait, `structTypeCheck` splices the trait's fields in and
calls `namespaceAdd` *after* inserting each one — so on a collision the clone
lands in `fields` while the namespace keeps the struct's own. `fields` then
holds two entries for one name, and a positional literal is told "Not enough
values specified on type literal", which blames the reader's literal for the
compiler's decision.

**It was left, deliberately, and this is why.** The obvious repair — report and
skip the insert — is unsound: `nodelistMakeSpace` pre-sizes the gap to exactly
`trait->fields.used` slots and the loop fills every one, so skipping leaves a
slot holding whatever `memmove` shifted there. Giving the clone a default value
does not work either, because the surplus field is at position 0 and the values
shift, so it is the *last* field that runs out. What is left is either removing
the struct's own duplicate afterwards — which needs the namespace repointed and
decides that the trait's field wins, a semantic choice on an error path — or a
sentinel for "this type's declaration failed", which is the general
`errorType`-for-types question `--checktree` also circles. The benefit is one
fewer message in a compile that already fails on a real error.

## `flowLoadValue` on array indexes — fixed, and it was worse than filed

The entry named `assignLvalInfo`, which no longer exists. Restated in current
terms: `assignFlow` read only its right side, and the left went to
`assignlvalrtype`, which checks write permission and borrow lifetime and is not
a value read. So **no value sub-expression on the left of an assignment was
ever flow-analyzed**, and the same expression was checked on the right and not
on the left. Measured three ways: an uninitialized index in `a[i] = 5`, a
moved-from value used as an index, and — the one that is not a missing
diagnostic — `*p = 5` on an uninitialized reference, which compiled clean and
emitted `load` then `store` through it. `swapFlow` read both of its sides all
along, so assignment and swap disagreed about one expression.

`assignFlowLvalReads` now reads an index and a dereference's operand, and
recurses through a field access. `array-flow-index` and `ref-flow` cover both
halves. **The base of a partial write is deliberately still not read** —
whether `a[i] = 5` reads `a` is a question about read-modify-write against
flow's whole-variable tracking, and [[use-escape-analysis-and-de-aliasing|Use, escape analysis and de-aliasing]]
carries it. `array-flow-index` pins that boundary as a non-diagnostic, so it is
asserted rather than assumed.
