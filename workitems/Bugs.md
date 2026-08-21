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

**Two things this pass surfaced and did not repair**, recorded where they belong
rather than here:

- `parseCloseTok`'s give-up return skips `lexDecrParens`, so an unterminated
  bracket leaks one from the open-delimiter count. Unobservable in the two
  unclosed-bracket scenarios, which recover by consuming to end of file.
  [[Lexer and Parser]].
- `utf8IsLetter` is `utf8IsMultibyte(srcp) || isalpha(*srcp)`, so *every* byte
  at or above 0x80 — malformed ones included — is accepted as an identifier
  start, and multi-byte junk never reaches the bad-character path at all. That
  is [[Lexer and Parser]]'s "UTF8 support for IsLetter", now with a reproducer:
  a line reading `a`, an invalid lead byte, and `a` is lexed as one identifier
  spanning the newline.

**Two fixes are not assertable by the runner**, which is a gap in the runner
rather than in them. `--ir` naming its dump after the source compiled, and the
debug file name being the real source path, both produce a *filename* or *debug
metadata* — and a check may target only the post-optimization LLVM IR dump or a
run's stdout, and the runner never passes `--debug`. Either a check target for
artifact names or a `--debug` knob would close it. Carried by
[[Design notes follow-on]] rather than opened as its own item.

---

# Still open

## `parseFieldDcl` rejects the wrong permission

**Measured.** `struct A { imm x i32 }` is `Error 1013`; `uni` and `opaq` fields
compile clean.

```c
if (permdcl != (INode*)mutPerm && permdcl == (INode*)immPerm)
```

`immPerm` is never `mutPerm`, so this reduces to `== immPerm` — it rejects
exactly `imm` and admits everything else. Almost certainly `!=` was meant in the
second clause. `parseFieldDcl` also takes a `defperm` argument that its body
never reads.

**Fix waits on [[Permissions]]** to say which permissions are legal on a field —
flipping the condition inverts which one is rejected, so the mechanical fix
needs that answer first. Listed here because the *defect* is not in doubt.

The neighbouring half of this is now fixed, which raises the stakes: a `ro` field
used to be writable because `iexpGetLvalInfo` compared node pointers, and it now
asks the permission whether it may write. So a field's permission is read for
something real, and what it should *mean* is a question [[Permissions]] owes an
answer to rather than a note.

## `.len` on a fixed-size array is rejected

**Measured.** *(from [[Constant literals]], where it was the file's one entry
labelled "Bug")*

```cone
imm bigarray = [1,2,3]
imm length = bigarray.len       // Error 1025: Invalid operation on an array
```

The published array documentation says this works, so the correct behavior is
not in doubt.

**Sizing, honestly:** this is a small addition rather than a repair, which is why
this pass left it. `len`/`maxlen` are registered as `CountIntrinsic` only on the
array *reference* type (`corenumber.c`), and generation extracts field 1 of the
slice fat pointer. `ArrayTag` is in the type group with neither `NamedNode` nor
`MethodType`, so a fixed-size array has no namespace to hold a method at all, and
`fnCallTypeCheck`'s `ArrayTag` arm handles only `FlagIndex`. The length is a
compile-time `ULitTag` in `dimens`, so it can fold to a constant.

**Do not work this twice:** [[Collection Types]] carries
"`.len` and `maxlen` for arrref and array" as feature work — the same gap from
the other side.

## `structAddField` drops a duplicate field after indices are assigned

On a duplicate name it reports and returns without adding to `fields`, while the
parser has already assigned `index` over the original numbering.

**The symptom this was filed with does not hold.** `structTypeCheck` renumbers
every field from zero during its field walk, so the parser's numbering is
overwritten before anything reads it and a positional literal cannot shift. What
is left is a type carrying a name in its namespace with no field behind it in
`fields`, reachable only after `ErrorDupName` has already fired — so nothing is
generated from it.

**What wants deciding is whether that matters.** Either the entry closes as
"harmless after a diagnostic", or the answer is that a type whose declaration
failed should not be walked further at all — which is the question `errorType`
and `--checktree` already ask, and belongs with them rather than here.

## `flowLoadValue` may be missing on array indexes in assignment

*(from [[Collection Types]], where it read
"assignLvalInfo needs to invoke flowLoadValue on array indexes?")*

Recorded with a question mark by its author, and **the function it names no
longer exists** — there is no `assignLvalInfo` in the tree. The lval-read logic
now lives in `flowIsLvalRead`, and assignment's flow pass in `assignFlow` /
`assignlvalrtype`. Someone has to work out what the entry now refers to before it
can be sized at all. Kept because a missing `flowLoadValue` on an index
expression would be a real hole, and losing the observation is worse than
carrying a stale one.
