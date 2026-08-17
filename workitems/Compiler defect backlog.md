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

**Every one still open is pinned by an `xfail` scenario**, which means the suite
fails the day one is fixed and the fixer is told to remove the mark. They cannot
be fixed quietly and they cannot rot silently. The two that were not pinned were
done first for exactly that reason — nothing was watching them — and are in the
table below.

## Fixed

Eleven are done, with a scenario each. They are recorded here rather than
deleted because five of them were mis-diagnosed on this page and the correction
is worth more than the entry was. Two of the last three were: one entry was two
defects with one cause, and another was two defects with two causes, of which
only one is fixed.

| Defect | What it actually was | Now covered by |
| --- | --- | --- |
| Writing through `&mut [N; T]` | Two defects. `iexpGetLvalInfo`'s `ArrIndexTag` case took the permission from the reference only for `ArrayRefTag` and `PtrTag`, as recorded — and repairing that exposed a second: `genlAddr`'s `ArrIndexTag` case had no `RefTag` arm either, so the write then reached an `assert` a Release build drops, and crashed | `collection-success` |
| Slices cannot be compared | **Not the signature.** `newArrayRefTypeMethods` declares `==` over `unknownType`, but so does `newRefTypeMethods`, and reference comparison works — because `iNsTypeFindPtrMethod` selects a compiler-declared pointer method by comparing the two *arguments to each other* rather than to the deliberately generic parameter, and `ArrayRefTag` was missing from the tags taking that path. `genlexpr` then had to compare the two words instead of `icmp`-ing an aggregate | `collection-success` |
| A name-fold clash names the wrong file | **Every cross-module diagnostic named the wrong file**, not just a fold clash. `lexInject` recycled a popped `Lexer` block, and a node holds the `Lexer` it was built with and reads its url when a diagnostic is reported, so the second import rewrote the url out from under every node of the first. It also named `conec`'s output files after the wrong source | `module-typecheck-provenance` |
| `ErrorFewArgs` with a "too many" message | Exactly as recorded: the guard is `args->used > parms->used`. The message was right and the code was wrong | `generic-typecheck-infer` |
| `as` onto a struct target is unchecked | **A stack over-read, not a missing check.** `genlRecast` allocates the source's bytes, bitcasts, and loads the target's, so `n as Big` emitted a 24-byte load from an `alloca i32` — and passed `--verify`. `castBitsize` has no answer for a struct, so the size is now checked in `genlRecast`, where the data layout exists, under `ErrorRecastSize` | `typemgmt-genllvm-recast`, `typemgmt-success` |
| `each`'s increment reports at the closing brace | As recorded. The synthesized nodes are now positioned on the range expression | `each-typecheck` |
| The whole-value `` `&[]` `` operator method is unreachable | Exactly as recorded, and the reason is that the whole-value form never becomes a call. `&mut v[i]` parses to a `FlagIndex\|FlagBorrow` `FnCallNode`, which `fnCallTypeCheck` names `` `&[]` `` and dispatches; `&[]mut v` parses to a `RefNode` that `arrayRefNameRes` retags `ArrayBorrowTag`, so it reached `borrowTypeCheck` and fell to the "a one-element slice!" branch without dispatch ever being consulted. `borrowTypeCheck` now asks first, and where the value's type declares `` `&[]` `` and a candidate accepts the receiver, retags to a plain borrow and wraps it in the call — so the operator form becomes the `(&mut v).`&[]`()` that always worked | `struct-methods` |
| Writing a trait field through `&<mut` is checked against the binding, and a struct field of virtual-reference type cannot be assigned | **One line, and neither defect was where the reading put it.** `iexpGetLvalInfo`'s `FldAccessTag` case read its `vtype` through `((StarNode*)lval)->vtexp`, and `StarNode` carries an `llvmtype` that `FnCallNode` does not — so the field it lands on is `methfld`, not `objfn`. Its `VirtRefTag` test was therefore asking whether the **field** was a virtual reference, never the object. That is the whole of both symptoms: a `&<Shape` *field* had its permission replaced by the permission of the reference it holds, so assigning it was refused; and a field reached *through* a virtual reference never took the reference's permission at all, so it kept the binding's. No `DerefTag` is involved either way — `derefInject` rewrites only `RefTag` and `PtrTag`, so a virtual receiver is never dereferenced, which is why the plain-reference controls worked | `trait-success`, `trait-flow-vref` |
| No method on a generic struct can be called | **The recorded cause was right and was half of it.** A method's `self` is declared as a use of `Self` (`parsetype.c:350`), and `CloneState` has a `selftype` field that repoints exactly that — which `struct.c` fills for a trait mixin and generic instantiation never did. It is filled in `cloneStructNode` rather than at the instantiation site, because the copy has to exist before its members are cloned. That alone left the call crashing on an access violation: `genlGlobalSyms` and `genlGlobalImpl` walk a generic *function's* instances through `memonodes` but skip a generic *type* outright, so an instance's methods were given no LLVM symbol and the call loaded a null. Nothing downstream of the rejected declaration had ever run, so a third gap surfaced with them and is filed below: an operator inside a generic method still does not resolve | `generic-success` |
| A generic instantiation is not a type inside a composite type | **Half of what was filed as "`&Box[T]` does not parse", and the recorded cause was right.** `itypeIsGenericType` tested `objfn->tag == GenericNameTag`, a tag nothing assigns, so `isTypeNode` said no to `Box[i64]` — and name resolution asks exactly there when it decides between a type and the expression sharing its syntax, so `*Box[i64]` read as a dereference and `[2; Box[i64]]` as an array literal. The discriminator was already in the tree: a generic is a declaration carrying a `GenericInfo`, which `genericSubstitute` has always used, and a macro cannot reach the test at all. One thing had to move with it: `inodeTypeCheck` marks a type node walked-once, and an instantiation is a type that this pass *replaces*, so marking it stranded the flag and made a second use of one node — a match pattern and the variable it declares share one — report a recursive type | `generic-success` |
| `Bool[r]` and `Bool[p]` fail | **The conversion they were said to disagree with did not work either.** `typeLitNbrCheck` rejected a ref/ptr source exactly as recorded, but `r into Bool` and `p into Bool` only *type checked*: `genlConvert` has no ref/ptr case and Bool is a 1-bit `UintNbrTag`, so the number arm read `((NbrNode*)fromtype)->bits` off a `RefNode` and emitted `trunc i32* to i1`. `--verify` rejects it; nothing had ever run it. The Bool rule now lives once, in `castConvertsToBool`, which both paths ask, and `genlConvert` lowers ref/ptr to Bool as the null test `isTrue` already generated for the implicit coercion of a pointer | `typemgmt-success`, `typemgmt-typecheck-convert` |

The same line held a second misread that no source can reach today: it took the
field's own permission through a `VarDclNode*` cast, and `FieldDclNode` puts
`perm` two pointers earlier, so the read fell past the end of the node. The
"downgrade if the field is immutable" rule it implements has never fired, and
still cannot: `imm` is the one permission `parseFieldDcl` (`parsetype.c:141`)
rejects, so the only value the downgrade tests for is the only one a field may
not be given. It now reads the right field of the right struct, so the rule will
work the day the permission is allowed — and the guard that rejects it is worth
a second look, since it is written `permdcl != mutPerm && permdcl == immPerm`,
where the first half can never decide anything.

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
| An operator inside a method of a generic type is rejected | Newly reachable, and newly filed: until generic methods worked at all, no method of a generic type was ever type checked. `count = count + 1i64` and `1i64 + 2i64` both report `ErrorNoMbr` "Method or field `+` not found", at the method and again per instantiation. Measured at the failing lookup: the receiver the operator is dispatched on is a `VarNameUse` of `self` typed as the instance, not the operand written — the shape `fnCallTypeCheck`'s "rewrite to `self.method`" branch (`fncall.c:664-688`) produces when it folds a bare method name into a call that already exists. The same operator in a generic *function*, and in a non-generic struct's method, both work | `generic-method-operator` |
| `&Box[T]` — a reference to a generic instantiation — does not parse | **Re-scoped: it was two defects, and the other one is fixed.** "Only the `&` form fails" was wrong — `*Box[i64]`, `[2; Box[i64]]` and `?Box[i64]` failed too, all with the same pair of diagnostics, because an instantiation lost the type-versus-value vote at name resolution. That half is fixed; see the table above. `&Box[i64]` never reaches that vote: it parses as an index applied to `&Box`, since a borrow re-applies suffixes to itself. What is left is a precedence rule — `&` followed by a name that resolves to a generic has one possible reading — and it is the same `parseAmper` re-application `ref-field-borrow` pins, so the two should be decided together | `generic-ref-instance` |

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
- **`itypeIsGenericType` tests a tag nothing ever assigns** — fixed. See the
  table above. The worry recorded here, that generics and macros share
  `MacroDclTag` and so cannot be told apart, was unfounded: they share nothing.
  A generic is an ordinary `FnDclNode` or `StructNode` carrying a `GenericInfo`,
  which is how `genericSubstitute` has always recognized one, and a macro has a
  `MacroDclTag` declaration of its own whose uses are tagged `MacroNameTag`.
  Neither the flag nor the representation change this entry feared was needed.
  The claim that this is why fallible allocation fails did **not** hold:
  `?+rc-mut 5` still dies on the same access violation, byte for byte, against
  the fixed compiler.

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
`struct-methods` and `safety-pointers` run the working forms. The deref half
briefly reached operators too — `p * 2` on a `*i32` compiled and meant
`(*p) * 2` — and Jon has since ruled that operations on a pointer are on the
pointer and not the deref, so `p * 2` is `ErrorNoCandidate` again while
`sp.sum()` is not. The borrow half, and that ruling, are written up under
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

**Status: fixed, and interning had nothing to do with it.** Jon ruled that
anonymous function types compare structurally. Instrumenting `refIsSame` and
`fnSigEqual` — three readings of the code had guessed wrong before anything was
measured — put the divergence in one line of `fnSigEqual`: it compared each
parameter with `itypeIsSame(*nodes1p, *nodes2p)`, and a signature's parameter is
a `VarDclNode`, not a type. `itypeGetTypeDcl` guards that with an `assert` a
Release build drops, so it returned the declaration unchanged, and `itypeIsSame`
has no `VarDclTag` case: two distinct declarations fell to `default` and compared
unequal. So the function did not "ignore parameter names" as its shape suggests —
it compared parameter declarations by node identity, which only signatures
literally sharing parameter nodes can satisfy. Everything else already worked:
the measurement showed the return types same, the permissions same (both `opaq`
wrappers, unwrapped by `permIsSame`), and the regions same. `fnSigEqual` now
extracts the parameter's type with `iexpGetTypeDcl`, as `fnSigParmsEqual` and
`fnSigMatches` beside it already did. `closure-success` selects between two
anonymous functions with the type inferred, with it declared, and with the two
branches' parameters named differently, and calls each result.

**`fnSigVrefEqual` has the same line, was left alone, and has since been
measured. It is a real defect, and a bigger one.** `fnsig.c:109` still compares
parameter declarations by identity, so a trait requiring a method with any
parameter beyond `self` **cannot be satisfied at all**:

```cone
trait Scaler { fn scaled(self &, factor i32) i32 }
struct Box { w i32
             fn scaled(self &, factor i32) i32 { w * factor } }
fn useScaler(v &<Scaler) i32 { v.scaled(3) }
// useScaler(&b) -> ErrorInvType "Expression's type does not match declared parameter"
```

The self-only control in the same file conforms fine, which is the whole of why
the corpus never saw it: **every trait method requirement in `trait-success.cone`
takes only `self`** — `fn area(self &)`, `fn reading(self &)`, `fn step(self &mut)`
— and `fnSigVrefEqual` skips the first parameter, so the loop compares nothing
and returns a match. The blind spot in the coverage has exactly the shape of the
bug.

**This contradicts published documentation rather than extending it.**
`refvirtref.html`: "the fact that both types implement all its methods means they
are compliant with this trait." No caveat about parameters — and every example on
that page happens to take only `self`, so the manual demonstrates only the cases
that work. Fixing it is therefore a defect repair toward the documented rule, not
the trait-behavior decision it was cautiously filed as. Needs Jon's nod because
it widens what conforms, and a `trait` scenario with a parameterized requirement,
which the group has never had.

**Status: fixed, and code generation turned out to be sound underneath it.**
Jon settled the design question two ways, and both are now written into the
comment at `fnsig.c` so the rule stops being folklore:

- **Parameters must match exactly.** A virtual reference dispatches through a
  typed vtable slot, so the machine signatures have to line up.
- **No implicit coercion in this comparison.** Coercion is a convenience at
  calling, and is only appropriate for type comparison where subtyping is
  correctly in play — which it is not between a trait requirement and a type's
  implementation of it.

So the repair is the same one line and the same shape as `fnSigEqual`'s above,
and deliberately no wider. Measured before it was changed, by instrumenting the
loop rather than reading it: the two compared parameters were `VarDclTag` (8195)
and distinct nodes, `itypeIsSame` on them as they stood returned 0, and
`itypeIsSame` on `iexpGetTypeDcl` of each returned 1 with both types `IntNbrTag`
(45056). One measurement line printed for the whole compile, which is the
self-only trait comparing nothing, exactly as predicted. `fnSigVrefEqual` now
extracts the parameter's type, as `fnSigEqual`, `fnSigParmsEqual` and
`fnSigMatches` beside it already did.

**The risk was that nothing downstream of the check had ever run** — conformance
for a parameterized requirement had always failed, so the vtable slot, the
argument marshalling and the dispatch were all unexercised. They were correct
first time: no code generation change was needed, `--verify` is clean, and the
LLVM IR shows the slot typed with the requirement's parameters
(`%"Scaler:Vtable" = type { i32 (i8*, i32)* }`), a reference parameter lowering
to a pointer.

`trait-success` now runs a parameterized requirement through a virtual reference
with two implementers whose answers differ for the same argument, a
two-parameter requirement and one whose parameter is a reference, and asserts
every returned value. Its new IR check pins the slot types and the symbol in each
implementer's slot, which is the half of "exactly" a run cannot observe.
`trait-typecheck-vref` gained the three rejections that must not start passing —
a parameter of the wrong type, a reference where a value is required, and one
parameter too many — and each was verified to report identically against the
pre-fix and post-fix compilers, so widening what conforms widened nothing else.

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

**Status: fixed, and it was three positions in two places, not two in one.**
`refmacro.html` turned out to document only `macro max[a, b]` and never to
mention the parameterless form at all, so Jon ruled it: a parameterless macro's
name expands wherever a value is expected. The page now says so.

The two failures had nothing in common but the symptom. The operand case fires
in `macroCallTypeCheck` — measured, with the arity it saw: `expected=0 args=1`.
`a + b` builds `FnCallNode{objfn: a, methfld: the operator, args: [b]}`, the same
shape as calling `a` with one argument, and `fnCallTypeCheck` routed to the macro
path on `objfn` alone. `methfld` is what tells a call from an application, and the
route is now gated on it being absent. Parentheses never helped because they
leave no node behind.

The block case never reaches the macro path at all. `MacroNameTag` sits in
`MetaGroup`, so `isExpNode` is false for it, and `fnImplicitReturn` runs *before*
type check: it saw a last statement that was not an expression and reported
`ErrorNoRet` before anything could expand. That is the whole of why `return TWO`
worked and bare `TWO` did not — the parser had already built the `ReturnTag` in
one case and not the other. `blockTypeCheck` carries the same test, which is why
`if c {TWO} else {3}` failed too as a fourth position nobody had listed. Both now
use `isExpOrMacroNode`, and both downstream paths type check the node before
asking whether it is an expression, so the name expands and then answers.

Macros *with* parameters are untouched: `MAX[1]` and bare `MAX` report exactly
what they did. `MAX + 1` now reports "expects arguments to be provided" plus its
follow-on rather than the arity of a call nobody wrote, which is what bare `MAX`
already said.

`generic-macro` runs all eight value positions and prints a different constant
from each, so the `.out` proves which expansion produced which number rather than
that it merely type checked. `generic-typecheck-macro` and the group's
`cases.toml` both recorded the two positions as deliberately left out; both said
so as though it were the rule, and both are gone.

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

**Status: left undecided on purpose, and now pinned as it stands.** Fixing
`Bool[p]` deliberately did not widen into it: the type-literal path asks the
conversion rule only for Bool, so `usize[p]` still reports exactly what it
reported before, in `typemgmt-typecheck-convert`. Whoever takes the decision has
a scenario to change rather than a silence to notice.
