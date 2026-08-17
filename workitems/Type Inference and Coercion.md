
Bugs:
- Mut background = Rgba[]  implicit typing does not work
- Successful coercion of Some[Variant1] to Option[VarTrait]
- Untyped integer, for example `var x u32 = 5`:
	- [https://cone.jondgoodwin.com/play/index.html?gist=e2da8485600ad7b4140e270d1b284950](https://cone.jondgoodwin.com/play/index.html?gist=e2da8485600ad7b4140e270d1b284950)
	- - [https://cone.jondgoodwin.com/play/index.html?gist=bafee1a55aa7850a68162b91c9d69083](https://cone.jondgoodwin.com/play/index.html?gist=bafee1a55aa7850a68162b91c9d69083)


### Inference never consults the expected type, so a valueless variant must always spell its type argument

Measured. `Some` can infer its type argument from the value it wraps; `None` has
no value, so its argument can only come from the expected type — and generic
inference does not look there:

| Spelling, with `mut a Option[i32] = …` | Result |
| --- | --- |
| `None[i32][]` | works — and is what `union-success` uses throughout |
| `None[]` | `ErrorInvType` "Could not infer all of generic's type parameters" |
| `None` | `ErrorNotTyped` — a bare generic name is a type, not a value |
| `None[i32]` | `ErrorNotTyped` — same, an instantiation is still a type |
| `Some[3]` | works: `T` comes from the argument |

Also fails as an argument: `wantOpt(None[])` where the parameter is declared
`Option[i32]`, though `wantOpt(None[i32][])` is accepted.

So `Option[T]`'s empty side is **reachable for every `T`** — the earlier claim
that it could not be spelled anywhere was wrong — but it can only be reached by
writing the type argument out, which is exactly what an `Option` is meant to save
you. Every use in the corpus is spelled in full for this reason.

**What it needs is bidirectional inference**, which is this section's subject
rather than a defect with a local fix: when a generic's arguments do not
determine all its parameters, the expected type should supply the rest. The
narrow case — the expected type is an instantiation of the union that declares
the variant, so its arguments are the variant's — may be affordable on its own,
and is worth costing before the general form is attempted. The neighbouring
entries below ("Test for i32 -> ?i32 coercion in structs", "Coerce => Build
TypeLit node, putting in enum and type as constraint") are the same problem seen
from the coercion side.

Two related findings measured with it, both confirmed on a tagged union so
neither belongs to the nullable-pointer layout: a variant literal **does not
coerce to its union in a struct literal's field**, though the same coercion in a
variable initializer is accepted.

### Branch inference cannot meet two references differing only in permission

Measured by [[Compiler defect backlog]]. `itypeFindSuper` is what `iexpMultiInfer`
falls back on when the branches of an `if` are not already the same type, and it
has arms for numbers, structs, `RefTag` and `VirtRefTag` — and none for
`ArrayRefTag` or `PtrTag`. Identical slices meet only because `itypeIsSame`
catches them one line earlier.

Worse, the `RefTag` arm it does have gives up whenever the permissions differ,
so `if c { &mut a } else { &a }` reports "Branch's expression type inconsistent
with other branches" even though coercion accepts a `&mut` wherever a `&` is
wanted. **Inference and coercion disagree about whether these two types have a
supertype** — the same inconsistency that turned out to be behind the closure
defect below, arriving from the permission side instead of the parameter side.

What it needs is the meet of two permissions, which belongs to [[Permissions]]
and is recorded there; this side is then the missing `itypeFindSuper` arms.

**Fixed, and worth knowing because the shape recurs:** branch inference used to
reject two structurally identical anonymous function types. `fnSigEqual` compared
each parameter's `VarDclNode` rather than its type, so it compared parameter
declarations by node identity — which no two separately written signatures can
satisfy. Coercion to a declared type worked the whole time, because `fnSigMatches`
beside it extracted the type. Anonymous function types now compare structurally,
per Jon's ruling. Interning function signature types the way reference types are
canonicalised through `typetblFind` was considered and deliberately deferred; it
was not needed for that defect, and would make the whole class impossible rather
than fixed.

### Inference & Type checking

Test for i32 -> ?i32 coercion in structs
- If samesize basest trait, with tag as 1st field, loop through derived variants to find one whose 2nd field’s type matches (but no other)
- Coerce => Build TypeLit node, putting in enum and type as constraint

Coercion of Numbers/String literals: both expMatches and expCoerces get involved to set type for numbers and to borrow for literal strings
