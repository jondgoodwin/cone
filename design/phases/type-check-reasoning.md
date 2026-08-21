Type check has two halves. [Type Check Phase](type-check.md) covers
*when* a declaration is checked — demand, the two marks, re-entry, size,
circularity. **This note covers what the checks decide**: how an expected type
reaches an expression, how an inferred type comes back, and how coercion,
overload resolution, casts, borrows and tuples each reach a verdict.

Read this when a value is accepted where you expected a rejection, when a
diagnostic names the wrong thing, or before changing anything that decides
whether two types fit.

*Provenance: read from source; which nodes consume `expectType`, and that
overload selection is unranked, were checked against every candidate path. See
[Measuring](../diagnostics/measuring.md).*

## 1. Key principles

1. **An expected type flows down as an argument; an inferred type flows back as
   `vtype`.** One direction each, no unification variables, no backtracking.
2. **Deciding and rewriting are separate.** `iexpMatches` returns a verdict and
   changes nothing. `iexpCoerce` is the only function that turns a verdict into
   an injected node.
3. **Coercion does not report a type mismatch.** `iexpCoerce` returns 0 and the
   caller writes the diagnostic, which is why the same mismatch reads
   differently at an argument, an assignment and a return. It does report two
   other things itself: an operand that is not an expression node at all, and
   whatever `fnCallLowerMethod` reports on the `ConvByMeth` path.
4. **Overload selection filters, it does not rank.** Exactly one viable
   candidate is a match; two are an ambiguity. There is no best-match score and
   no preference order to memorize.

## 2. The three sentinels are the levers

Every decision below turns on which sentinel is in play. They are singletons
compared by identity, never by tag — see "The three type sentinels" in
[IR Nodes](../nodes/_index.md) for why they are three objects sharing one tag.

| Sentinel | As an expected type | As a found type |
| --- | --- | --- |
| `unknownType` | infer — tell me what you are | not checked yet |
| `noCareType` | statement position, discard the value | never |
| `errorType` | never | already reported; matches anything, silently |

`iexpCoerce` returns success immediately for `unknownType` and `noCareType`, so
"no expectation" costs nothing and injects nothing. `itypeMatches` returns
`EqMatch` when either side is `errorType`, and `iexpMultiInfer` drops a branch
carrying it out of the type-in-common entirely. That is the whole cascade
suppression mechanism.

## 3. How an expected type reaches an expression

`inodeTypeCheck` takes `expectType`, but **only two node kinds consume it**:
`blockTypeCheck` and `ifTypeCheck`, the two nodes that unify several branches
into one value. Everything else ignores the parameter and is coerced *after* it
has been checked.

So the normal path is `iexpTypeCheckCoerce`:

1. `inodeTypeCheck(pstate, from, totype)` — check the expression, passing the
   expectation down in case it is a block or an `if`.
2. Return success at once if the expectation is `noCareType`.
3. Report `ErrorNotTyped` and **return success anyway** if what came back is not
   an expression node — a deliberate lie, so one untyped subexpression does not
   provoke a second complaint from every enclosing node.
4. `iexpCoerce(from, totype)`.

`litTypeCheck` also takes `expectType` and ignores it. An untyped literal is
not typed by its context here; it is typed by the coercion in section 5.

## 4. The verdict vocabulary

`TypeCompare` (in `itype.h`) is what every matcher returns, ordered from free to
expensive:

| Verdict | Means | What `iexpCoerce` injects |
| --- | --- | --- |
| `NoMatch` | incompatible | nothing; returns 0 |
| `EqMatch` | same type | nothing |
| `CastSubtype` | compile-time upcast | `newRecastNode` |
| `ConvSubtype` | runtime upcast | `newConvCastNode` |
| `ConvByMeth` | convertible by calling a method | an `isTrue()` call |
| `ConvBorrow` | convertible by auto-borrowing | a borrow node |

`SubtypeConstraint` is the second half of the question — *how much conversion
machinery is available at this site*:

| Constraint | Site | Severity |
| --- | --- | --- |
| `Monomorph` | generic monomorphization | no constraints |
| `Virtref` | virtual reference | relaxed |
| `Regref` | regular reference | ordered prefix required |
| `Coercion` | ordinary value coercion | most restrictive |

`iexpCoerce` always asks with `Coercion`; `iexpMatches` otherwise forwards
whatever constraint its caller passed. The looser constraints exist for
reference and generic matching, which ask the same matchers a weaker question —
`structMatches`, for one, re-enters `iexpMatches` with `Monomorph` on its
monomorphization branch.

## 5. Coercion

`iexpMatches` layers expression-level fallbacks on top of pure type subtyping,
**in this order**, stopping at the first that answers:

1. `itypeMatches(totype, fromtype, Coercion)` — the type-only question,
   dispatched by the *target* type's tag to `nbrMatches`, `structMatches`,
   `refMatches`, `arrayMatches`, `fnSigMatches` and the rest.
2. **Target is `Bool`**: look for an `isTrue` method on the source type.
   This branch returns in both arms, so a `Bool` target never reaches the two
   fallbacks below.
3. **Source is an untyped integer literal** (`ULitTag` with `FlagUnkType`) and
   the target is any number type: `ConvSubtype`. Deliberately not a subtype
   check — a literal goes wherever the author wrote it.
4. **Auto-borrow** (`borrowAutoMatches`): can a borrow of the source produce the
   target reference?

`iexpCoerce` then switches on the verdict and injects, copying source position
onto every node it creates with `inodeLexCopy` so the diagnostic still points at
the author's text.

`fnSigMatches` is the one matcher whose variance is easy to get backwards:
**parameters are contravariant** (it flips `to` and `from`), **the return type is
covariant**, and the overall verdict is the most expensive of all the parts.

## 6. Unifying branches

`blockTypeCheck` and `ifTypeCheck` fold one branch at a time: coerce the branch
with `iexpTypeCheckCoerce`, then fold its type into the type in common with
`iexpMultiInfer`. `blockTypeCheck` reaches both through the
`iexpMultiCoerceInfer` wrapper; `ifTypeCheck` calls the two directly and is
otherwise the same fold. A block folds its final expression *and* every `break`
registered against it; an `if` folds one branch per condition.

`ifTypeCheck` carries three obligations a block does not: each condition is
coerced to `Bool` (which is where implicit `.isTrue` reaches a conditional), a
closed-variant `is` match that covers everything is rewritten into the `else`
by `ifExhaustCheck`, and a value-producing `if` with no `else` is `ErrorNoElse`.

`iexpMultiInfer` does three different things depending on the expectation:

- **`noCareType`**: everything matches, nothing is inferred.
- **`unknownType`**: the first branch sets the type in common; each later branch
  must be the same type or have a supertype in common with it, found by
  `itypeFindSuper`. No common supertype is `ErrorInvType`, reported on the
  branch.
- **a real type**: every branch is matched against it directly, and the type in
  common becomes the expected supertype as soon as two branches differ.

**A supertype discovered late forces a second pass.** When the expectation was
`unknownType` and the fold ended at `CastSubtype` or `ConvSubtype`, the branches
checked before the supertype was known were coerced to the wrong target. So
`blockTypeCheck` re-runs `iexpCoerce` over every break and the final expression,
and `ifTypeCheck` re-runs it over **the last statement of each branch block**,
not the block itself — generation requires the branch to stay a block node.
Anything added to a block's or an `if`'s value paths has to be reachable from
that second loop too.

## 7. Calls, methods and overloads

`fnCallTypeCheck` is the largest function in the phase, and serves several
syntaxes that all parse to the same node shape. Read it as three stages.

**Stage 1 — syntax, before the callee is known.** Macro call, `<-` on a value
tuple, argument type checking, generic substitution. `methfld` is what
distinguishes a call from an operator or member access: `TWO + 1` is objfn
`TWO`, methfld `+`, one argument.

**Stage 2 — make the callee knowable.** Type check `objfn`, unless it names an
overload set — that one path deliberately skips the name-use check, so the
overload name stays rejected everywhere except here. Then rewrite the shapes
that are not yet calls: a type becomes a constructor (`FlagIndex`) or its `init`
method; a bare method name becomes `self.method`.

**Stage 3 — dispatch on the receiver's type tag** to `fnCallFnSigTypeCheck`,
`fnCallLowerMethod`, `fnCallLowerPtrMethod`, `fnCallArrIndex` or
`fnCallLowerIntField`.

### Selecting a candidate

`iNsTypeFindMethod` walks every candidate, tests each with `fnSigViableCall`,
and **alters nothing**. Viability is: no more arguments than parameters, the
receiver passable as parameter 0, every argument `iexpMatches`-compatible with
its parameter, and every unsupplied parameter carrying a default. Two viable
candidates is `OverloadAmbiguous` and an error, not a tie-break.

**Coercion happens once, after selection**, in `fnCallFinalizeArgs`: coerce each
argument to its parameter, then append defaults for what was not supplied. This
ordering is the reason selection can be a pure filter.

Two adjustments are worth knowing because they are asymmetric on purpose:

- **The deref retry.** A receiver held through a reference or pointer still
  satisfies a method declaring `self` by value: `fnCallLowerMethod` calls
  `derefInject` and selects again. It runs *only* when no candidate matched at
  all, so a genuine ambiguity is still reported as one. Nothing is borrowed on
  the receiver's behalf — a method wanting `self &mut` stays out of reach of a
  value.
- **An operator written on a pointer does not reach through.** `p + 2` offsets
  the pointer because `ptrType` declares `+`; `p * 2` is an error rather than
  quietly becoming `(*p) * 2`. `FlagOperator` is what records that the source
  wrote an operator, and the retry is skipped when it is set on a pointer
  receiver.

Visibility is checked against **the spelling the caller used**, so a public
overload name may legitimately select a private concrete candidate.

## 8. Casts and `is`

Three syntaxes, all `CastNode`:

| Source | Node | Meaning |
| --- | --- | --- |
| `x as T` | `newRecastNode` | reinterpret the bits |
| `x into T` | `newConvCastNode` (`FlagConvert`) | convert the value |
| `x is T` | `newIsNode` | is this the runtime type? |

**Reinterpret requires identical bit size** (`castBitsize`), except to a struct,
which is unchecked. **Convert** permits number to number, ref/ptr to ref/ptr,
virtual reference to reference, `SameSize` struct to struct, and anything
`castConvertsToBool` allows to `Bool`. A ref-to-ref conversion drops
`FlagConvert` on the spot — it is a bitcast after all. Everything else is
`ErrorInvType`. A slice deliberately does not convert to an integer: length and
data address are both candidates and both are spelled better already.

`castIsTypeCheck` — reached from the `is` keyword, which `parseCmp` handles at
comparison precedence — decides only whether a **downcast specialization** is
possible, and it needs a discriminant to do it: without `HasTagField` on the
source struct, or a virtual reference match, it refuses. Regions must be
identical — two regions are represented differently in memory, so no recast
exists between them, not even into a borrow. Region and permission downcast
covariantly while the structure is contravariant.

## 9. Borrows: where type check stops

`borrowTypeCheck` establishes the reference *type* and nothing about its
lifetime. In order: reassociate an index chain; refuse a temporary with its own
message rather than "must be lval", because every operand a borrow refuses is
refused for that one reason; auto-deref a suffixed borrow through a reference;
extract lval, permission and scope with `iexpGetLvalInfo`; infer the value type
from the lval; check the requested permission with `permMatches`; build the
`RefNode` carrying `borrowRef` as its region and the lval's scope.

**The scope it records is checked by flow analysis, not here — and not at the
borrow site either.** `borrowFlow` is an empty function. The two places a
borrowed reference can outlive what it points at are storing it and returning
it, and those are the two places checked, both during flow.
[Flow Analysis](flow.md) owns that rule; do not restate it here.

## 10. Tuples and multi-value assignment

`assignTypeCheck` dispatches on whether each side is a `VTupleTag`, which is why
there are exactly four helpers — it is a two-by-two:

| lval | rval | Helper |
| --- | --- | --- |
| single | single | `assignSingleCheck` |
| tuple | tuple | `assignParaCheck` — parallel assignment, element by element |
| tuple | single | `assignMultRetCheck` — a call returning several values |
| single | tuple | `assignToOneCheck` |

The assignment's own type is the rval's type, so an assignment is usable as an
expression.

## 11. Hazards

- **`iexpCoerce` returns 0 silently on a type mismatch**, but not on its other
  two failures. A caller that forgets to report a mismatch gets a wrong program
  with no diagnostic; a caller that reports unconditionally can double up on the
  non-expression case. Grep for callers before changing its contract.
- **`iexpTypeCheckCoerce` returns 1 on an untyped operand.** Success there means
  "do not complain again", not "this type checked".
- **The `Bool` branch of `iexpMatches` short-circuits** the untyped-literal and
  auto-borrow fallbacks. Adding a fallback below it will not apply to `Bool`.
- **Adding a value path to a block means adding it to two loops** — the fold and
  the re-coercion pass in section 6.
- **`litTypeCheck` accepts `expectType` and ignores it.** The parameter reads as
  though literals are context-typed. They are not.
- **A node injected during type check takes the lexer's position**, which is end
  of file by then. Call `inodeLexCopy` or the diagnostic points at nothing.

## 12. What lives elsewhere

| Question | Note |
| --- | --- |
| When is a declaration checked, and what may it answer mid-check? | [Type Check Phase](type-check.md) |
| What may type check assume is already bound? | [Name Resolution](name-resolution.md) |
| Ownership, moves, aliasing | [Flow Analysis](flow.md) |
