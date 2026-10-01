Six literal forms across three source pairs: `nil`, integer, float and string in
`literal.c`; the array literal in `arraylit.c`, which **shares its node with the
array type**; and the type literal in `typelit.c`, which **shares its node with
a call**. A struct's value is constructed `new Point(1, 2)`, and a construction
its implicit field-wise init takes is lowered into a type literal; so is a
number's conversion, `u64.from(count)`. An enum's variant writes its literal in
brackets, `Some[x]`, and so may an allocation's value, `+Rc-mut Node[1]`; a
struct's in brackets anywhere else is refused.

**At a glance.** The parser builds them without deciding types. Name resolution
resolves each literal's type name, decides that `[…]` is an array literal, and
refuses it where it spells an array type. Type check types an integer literal and checks it fits, sizes
a string, checks array elements against each other, selects a construction's
init, and reorders a type literal's fields. Flow accounts for the values a
composite literal takes ownership of. Generation emits constants where it can.

*Provenance: read from source.*

## Shape

| Node | Tag | Payload |
| --- | --- | --- |
| `NilLitNode` | `NilLitTag` | none; `vtype` is a fresh void node |
| `ULitNode` | `ULitTag` | `uintlit` — also carries `true`/`false` and tuple element indices |
| `FLitNode` | `FLitTag` | `floatlit` |
| `SLitNode` | `StringLitTag` | `strlit` pointer into the lexer's arena, plus `strlen` |
| `ArrayNode` | `ArrayTag` **or** `ArrayLitTag` | `dimens`, `elems` |
| type literal | `TypeLitTag` | an `FnCallNode` — `args` are the field values, or a number conversion's one value; `FlagNew` when it came of a construction, `FlagAllocValue` when it is an allocation's bracketed value |

**`FlagUnkType`** is set only by `newULitNode`, only when the lexer gave no type
suffix. **There is no float equivalent** — a suffix-less float defaults to `f32`
concretely, and the lexer refuses a float literal past its type's range
(`ErrorFloatRange`; [Parse](../phases/parse.md)).

**`FlagLitNeg`** says `uintlit` is the negation of the digits written. It is set
only by `parsePrefix`'s fold (see Parse), and toggled, so a minus applied twice
clears it.

**The array node serves both a type and a literal.** The fill literal `[3; 7]`
has `dimens` `[3]` and `elems` `[7]`; the list form `[a,b,c]` has empty
`dimens`. The array type, written `Array[i32, 3]`, is the same node tagged
`ArrayTag`, `dimens` `[3]` and `elems` `[i32]`: name resolution builds it from
the bracketed call (`arrayTypeLower`; [Name resolution](../phases/name-resolution.md)).
Several sizes build nested nodes, each of one size, the first size the
outermost: `Array[i32, 2, 3]` is the node of `Array[Array[i32, 3], 2]`, so it is
that type, laid out and indexed (`a[i][j]`) as it is. The parser builds every
`[…]` as a literal and does not decide.

## Parse

Literal tokens map straight to constructors. `parseArrayLit` gathers
comma-separated expressions, and **if a `;` follows, swaps** — what it gathered
becomes `dimens` and a fresh list is gathered into `elems`.

A type literal is not built as one. A construction, `new Point(1, 2)`, is an
`FnCallNode` flagged `FlagNew` (`parseNew`): its `objfn` the type -- a name, a
path, type arguments in brackets -- and its `args` what is in the parentheses,
none when they are left off. A bracketed form is an `FnCallNode` with
`FlagIndex` from `parseSuffix`, and whether `Some[x]` is an index, an
instantiation, or a variant's literal is type check's. `parseArg` wraps
`name: value` in a `NamedValNode` in every argument list, so refusing the
wrapper everywhere else is type check's too: `fnCallTypeCheck`, once it knows
the call is not a type literal, `typeLitNewCheck` for a name a declared init
would be given, and `macroExpand` report each one through `namedValRefuseArgs`
(`ErrorNamedArg`). A function, method, closure or declared init's call, an
index and a macro use take arguments by position only, and flow analysis and
generation meet a `NamedValNode` only inside a type literal.

**A minus before a literal is folded into it.** `parsePrefix` negates a
`ULitTag`'s `uintlit` in place, two's complement, and a `FLitTag`'s `floatlit`,
and returns the literal instead of a negation call. The integer's bits alone
then cannot tell `-1` from `18446744073709551615`, so the fold also toggles
`FlagLitNeg`, and everything that reads the value as a number — the range check
and the float adoption below — reads the magnitude written and its sign.

## Name resolution

`litNameRes` resolves the literal's `vtype` name use — turning the `NameUseNode`
naming `i32` or `f64` into a resolved one. A string literal's `vtype` is still
`unknownType`, so this is a no-op for it.

**`arrayNameRes` is the one retag site**: `ArrayTag` becomes `ArrayLitTag` when
`elems[0]` is **not** a type node. Decided by the first element alone. A fill
form whose element is a type, `[3; i32]`, is the fill literal's spelling of an
array type, and is refused (`ErrorArrayTypeOld`, naming `Array[T, n]`); it stays
the `ArrayTag` it spells, so nothing after reports it again. A list of one type,
`[i32]`, stays an `ArrayTag` with no size, which type check refuses. In a
generic's template `[2; T]` is a literal until `cloneArrayNode` decides again on
the substituted element ([generic](generic.md)), and refuses it then the same way.

`namedValNameRes` resolves the *value* only — the name is deliberately not
bound, because it is matched against a field by symbol later.

## Type check

**An untyped integer literal takes the number type it is wanted as, wherever it
meets it, and no other literal is context-typed.** `litAdoptNumberType` is the
one rule: a `ULitTag` carrying `FlagUnkType` against an integer type takes that
type and drops the flag; against a float type it is replaced by an `FLitNode`
holding the full 64-bit magnitude written, negated when `FlagLitNeg` says so,
rounded once at the target's own precision. `litTypeCheck` applies it when `expectType` is a number type, and
`iexpCoerce` applies it to a literal that reaches coercion still untyped — an
argument to an overload set, a generic or an operator, which is type checked
before its callee is chosen. Every other literal is typed by
`itypeTypeCheck(&node->vtype)` alone.

**`Bool` is the one number type it refuses.** `Bool` is a 1-bit unsigned, so it
answers `UintNbrTag` like any other, but its only values are `true` and `false`
and a literal reaches it the way every other number does — through the `isTrue`
coercion in [Type Check Reasoning](../phases/type-check-reasoning.md),
"Coercion", which makes any non-zero value true. `litAdoptNumberType` returns 0
for it, so both of its callers fall through to that coercion and every position
agrees. `typemgmt_success` pins all of them — initializer, assignment, argument,
return value, struct-literal field, `if`, `not`, `and` and `or` — each with a
value whose low bit is 0, because `1` and `-1` read the same either way.
`true` and `false` are built carrying `Bool`, never `FlagUnkType`, so the rule
never reaches them.

**Adopting the type is what builds the constant at the right width.** The
alternative, converting from the `i32` default, materializes the constant at 32
bits first and widens what is left, which silently drops every bit above the low
32 — `i64`'s maximum stored as `-1`, `u64`'s as `4294967295`, `i64`'s minimum as
`0`, and `5000000000` as `705032704` on its way to an `i64` parameter or an
`f64`. `typemgmt_success` pins the cases that tell the two apart, in every
position a literal meets a type: initializer, assignment, argument, return value
and struct field, for `i64`, `u64`, `f32` and `f64`.

**A float literal widened to a wider float type is widened in place.** A float
literal is not context-typed: `0.5` is an `f32` wherever it is written, and it
reaches an `f64` by the implicit widening every `f32` value has. When
`iexpCoerce` finds that widening (`ConvSubtype`) and the value is a float
literal, `litWidenFloat` replaces it with an `FLitNode` of the wider type
holding the `f32` value widened, rather than wrapping it in a conversion node.
The value is exactly what the conversion generated, so no position's value
moves; what moves is that the result is still a literal, which the global,
static, parameter-default and typed-constant rules ask for ([vardcl](vardcl.md)),
so `imm g f64 = 0.5` is accepted as it is in a function body. It is still the
`f32` value: `imm g f64 = 0.1` holds `0.100000001490116…`, as a local, an
argument, a return value and a field do. `typemgmt_success` pins the positions
that require a literal, with values exact in `f32`, and pins that a global and a
local given the same literal agree.

**A named constant's use widened is folded the same way.** The manual has a
constant's name stand for its literal value, but the use is typed as the
constant is: `const K = 5` is an `i32`, and its use reaches an `i64` by
widening. When `iexpCoerce` finds that widening and the value names a
constant, `litWidenConst` follows the constant, and a constant whose value names
another, to its literal and replaces the use with a literal of the wider type,
leaving the constant's own literal for its other uses. An integer is
sign-extended from a signed type and zero-extended from an unsigned one, as the
conversion would, so `const B i8 = -128` reaches an `i64` as `-128` and
`const U u8 = -1` a `u32` as `255`; a float goes through `litWidenFloat`. A
widening holds every value of the narrower type, so nothing is range checked at
the wider one. The one value not yet checked is an untyped integer's `i32`
default, which generation checks (below): a constant whose value does not fit it
is not folded, so its use keeps the conversion and generation still refuses it
at the constant. Only widening folds: `const K = 300` then `imm b u8 = K` is a
type mismatch, as `i32` does not coerce to `u8`. `typemgmt_success` pins a
global, a static, a parameter default and a typed constant initialized from a
widened constant, with the edge values above.

**`FlagUnkType` is also read by `iexpMatches`**, which returns `ConvSubtype` for
an untyped integer literal against any number type. `iexpCoerce` adopts before
it asks, so this answers only the callers that ask without coercing: overload
resolution, struct field matching and the branch meet. It deliberately ignores
subtype direction "for user convenience".

**A literal nothing typed keeps its `i32` default, and must fit it.** Some
literals are reached by neither `litTypeCheck`'s `expectType` nor `iexpCoerce`:
the branches of an `if` or a block passed as an operand, or as an argument to
an overload set or a generic, which get no type because theirs chooses the
callee, and every literal whose type comes from nothing but itself — a
variable, global or constant declared without a type, an array literal's
elements. Such a literal still carries `FlagUnkType` when it is
generated, so the `i32` is final, and `litCheckDefaultRange`, called from
`genlExpr`, refuses one whose value does not fit it (`ErrorLitRange`) rather than
materializing it at 32 bits — `x + if v {5000000000;} else {1;}` would add
`705032704`. It is checked there because generation is the one place every such
literal is reached, in a body, a global's initializer and a constant's value
alike, after everything that could have typed it has run.
`typemgmt_genllvm_litrange` pins each position.

**A literal must fit the type it is given.** `litCheckRange` refuses an integer
literal whose value its integer type cannot hold (`ErrorLitRange`) rather than
materializing it at that width, which silently dropped every bit above it:
`300u8` and `mut n u8 = 300` stored `44`. It runs where the type is decided —
`litTypeCheck` for a literal whose suffix gave it one, `litAdoptNumberType` for
one taking the type it is wanted as — and at generation for the `i32` default.
It measures the magnitude written, recovered through `FlagLitNeg`: a signed
*N*-bit type holds magnitudes up to 2^(*N*-1)-1, or 2^(*N*-1) negated, so
`-128i8` fits and `128i8` does not; an unsigned type holds magnitudes up to
2^*N*-1 negated or not, because the manual has a minus on an unsigned literal
leave it unsigned, so `-1u8` is `255`, negation in its own width. `Bool` is
not asked, since no literal is built at it. The message quotes the literal as
written, digits and suffix, with a `-` for the folded minus; the typed message
gives the type's range. Reported once: the literal is left a zero of its type,
so a literal checked again, or a constant generated at each use, does not
repeat it. `typemgmt_typecheck_litrange` pins each position and the edges;
`typemgmt_success` and `lexical_literals` print the edges that fit.

An explicit conversion is not a literal meeting a type: `u8.from(300)` and
`300 into u8` convert the `i32` literal `300`, and keep its low bits as any
conversion does.

`slitTypeCheck` sets a string's type to an array of `u8` sized from `strlen`. A
string literal is also an lval.

**Array literal** — `arrayLitTypeCheck` requires the fill dimension to be a
literal constant, everywhere, an allocation's initial value included: an
array's size is part of its type, and a count chosen at run time belongs to a
`List`.

- **Fill form**: one dimension only; a `ULitTag` dimension is forced to `usize`;
  exactly one fill value.
- **List form**: not empty; the elements settle on one type by folding, the same
  way an `if` folds its branches — the first successfully typed element sets the
  type in common, and each later one either matches it or meets it at a
  supertype found by `itypeFindSuper`. An element already reported bad
  contributes nothing. Where a supertype was reached, a second pass coerces
  every element to it, because the array's element type and the values in it
  would otherwise disagree about size. So `[Circle[..], Rect[..]]` is an array
  of their enum, while `[1, 2u8]` is still refused — signed and unsigned have
  no type in common.

Either form's type is built by `newArrayNodeTyped`, already checked, so it
never passes through `arrayTypeCheck`. **The constructor gives it the element
type's move-ness itself**, as `arrayTypeCheck` does for a type written out, so `[new Fin(1), new Fin(2)]` moves exactly as `Array[Fin, 2]` does. Move-ness
is asked of `itypeIsMove`, not read off the element's flags, because a tuple
element carries no flag and moves when one of its own elements does.

Every diagnostic path sets `errorType`, so the literal never leaves the pass
untyped.

**Construction** — `typeLitNewCheck` takes a `FlagNew` call. Its type must be a
struct (`ErrorNewType` otherwise: a number converts with `from`, an enum's
variant keeps its brackets, an allocation keeps its `+` spelling). A generic
struct named bare, `new Box(5i64)`, has its type arguments inferred from the
values, as its literal's were (`genericSubstitute`). Its inits are the implicit
field-wise one and those it declares under the name `init`, one or an overload
set, each `fn init(self &new, ...)`. With none declared, the arguments are
checked against the fields they fill, as a literal's are; otherwise with no
expectation, as an overload set's are. Exactly one init must take them:
viability is counted as `fnSigViableCall` counts it -- the count, the defaults,
each argument passable -- for a declared one its parameters after `self`, for
the implicit one the fields in order, and neither is preferred, so none or
several is `ErrorInitNone`. Named arguments are the implicit init's alone; a
name no field has is `ErrorNamedArg`. The implicit init is lowered to the
struct's literal, retagged `TypeLitTag` with `FlagNew` kept, and checked as
below; a declared one stays an `FnCallTag` with `FlagNew`, its `objfn` the
init's name use, its arguments coerced to the parameters after `self` and the
defaults appended, and its `vtype` the struct, the call's value
([fncall](fncall.md), "Construction"). A declared init not `pub` is the type's
own (`ErrorNotPublic`).

**Type literal** — `typeLitTypeCheck` requires a concrete type, then builds a
struct's literal. **A struct's literal in brackets is `ErrorStructBracket`**,
naming `new Point(...)`, unless it came of a construction (`FlagNew`), is an
allocation's value (`FlagAllocValue`, set by `allocateTypeCheck`), or is a
variant's, which has a discriminant field to fill; refused at type check, so a
struct reached through an alias or a type parameter is refused as one named
directly, and the literal is still built so nothing after it reports again. A
number type written with brackets, `u64[count]`, is
`ErrorNbrBracket`, because a number's conversion is its method,
`u64.from(count)`. That call reaches this node another way: `fnCallNumberFrom`
([fncall](fncall.md)) checks its one value, has `typeLitNbrFromCheck` accept a
number (and for `Bool` a reference or pointer, as `castConvertsToBool` says),
and retags the call `TypeLitTag` with the number as its type, so it never passes
through `typeLitTypeCheck`. `typeLitStructReorder` walks the struct's fields
in declaration order and rewrites `args` to match: a `NamedValNode` is moved
into position; a missing field takes its default; a field flagged `IsTagField`
gets the variant's `tagnbr` **inserted** — which is how a variant's discriminant is
materialized, and why a constructor never writes one. Inserted where the field
sits, so an enum that placed its discriminant itself is served by the same walk. A
private field may not be given a value from outside the type, by position or by
name; leaving it to its default is allowed, because the default is the type's own
value rather than one the literal gives. Then a
positional pass runs each value through `iexpCoerce` against its field's type: **a
field takes a value on the same terms a variable initializer does**, a variant
standing in for its enum included. A value given by name is coerced inside its
`NamedValNode`, which then takes the value's type: the wrapper is neither an
lval nor a literal, so coercing it would refuse a string literal's borrow to a
slice and leave an untyped number literal at its default type.

`litIsLiteral` is the compile-time-constant predicate the global, parameter and
field-default rules use. It accepts a use resolved to a `ConstDclTag`, which is
what makes `imm g i32 = K` legal. It accepts a borrow (`BorrowTag` or
`ArrayBorrowTag`) of a string literal too: the text is a constant global, so a
reference to it, or a slice of it (its address and its length), is known before
anything runs. That is the borrow `borrowAuto` wraps a string literal in when a
`&[]u8` wants it, so `imm g &[]u8 = "text"` and a struct literal holding one as
a field are literal initializers; generation's `genlExpr` builds the slice with
instructions the builder folds to a constant aggregate. It accepts a borrow of
an array literal whose elements all satisfy it (`arrayLitIsLiteral`) on the same
terms, so `imm g = &[1, 2, 3]` is a literal initializer, and a borrow of a named
constant holding either (`borrowIsConstLit`), so `imm g = &K` is one too. It accepts a value
tuple whose values all satisfy it (`vtupleIsLiteral`), so
`mut g (i64, i64) = 1, 2` is one too: the `VTupleTag` arm of `genlExpr` builds
it by `insertvalue` of constants, which the builder folds to a constant struct,
as it does a struct literal of constants.

**A reinterpretation of a constant is a constant** (`litIsConstCast`): a
`CastTag` without `FlagConvert` (`as`, not `into`) whose target is a number or
a raw pointer and whose operand is a number literal, a `ConstDclTag` use, or
another such cast. `0usize as *T` is how a raw pointer starts out null, since
there is no null literal. The predicate reads the tree as written, because
`fieldDclTypeCheck` asks it before the default is type checked; the target is
found through name resolution's binding. A struct target is left out:
`genlRecast` reinterprets one through a stack slot, which a global's
initializer has none of. Type check still applies the same-size rule, and
`genlRecast`'s `bitcast`, `inttoptr` or `ptrtoint` of a constant operand is
folded by the builder into a constant (`ptr null` for zero). An array literal's
fill count is the one constant context that refuses it (`arrayLitDimIsConst`):
type check reads the count from a `ULitTag`, and a cast has not been generated
yet.

**A borrowed constant array literal is a constant, as a borrowed string literal
is.** Neither is an lval of a variable, so `borrowTypeCheck` asks
`borrowIsConstLit` before refusing its operand as a temporary, and gives the
borrow what a global constant has: `imm` and scope 0, the program's lifetime.
So `&mut [1, 2, 3]` is refused (`ErrorBadPerm`) as `&mut "text"` is, and a
function may return `&[2, 3, 5]`. An array literal with a computed element is
still a temporary (`ErrorBadLval`). The literal was typed from its elements
alone, the borrow expecting nothing of it, so `&[1, 2, 3]` wanted as a `&[]u32`
would be an `&Array[i32, 3]`: `iexpCoerce`'s `NoMatch` arm hands such a borrow to
`borrowConstLitCoerce`, which coerces the elements to the wanted element type by
`arrayLitCoerce`, rebuilds the borrow's type around the retyped literal, and
lets the match run again. Only a borrow of a literal written in place is
retyped; a named array is not coerced.

**A named constant is borrowed as its literal would be.** A use of a constant is
its value (refterm, "Named constants"), so `borrowIsConstLit` follows a
`ConstDclTag` use to the literal it holds: `&K` for `const K = [1, 2, 3]` is
`&[1, 2, 3]`, `imm`, the program's lifetime, and placed in a constant global
(`genlAddr` recurses into the value, [vardcl](vardcl.md), "Shape"). Not
retyped, as a named array is not: `const K Array[u32, 3]` borrows as an `&Array[u32, 3]`.
Any other constant, or a field or element of one (`&K.x`, `&K[1].y`, and a
method taking a borrowed `self` on one), is the borrow of a temporary, as its
literal's would be: `borrowRefusesConst` reports it once (`ErrorBadLval`) and the
reference is still typed, so nothing follows from it. An element of a constant
array, `&K[i]`, is not a part of one in this sense: `borrowReassocIndex` makes it
an index of `&K`. Nor is a place reached through a reference a constant holds
(`const R = &K`, then `&R[i]`): it is where the reference points, and is
borrowed on the reference's terms.

## Flow

Scalar literals are no-ops. The two composites take ownership of values:

`typeLitFlow` unwraps each `NamedValNode` and move-or-copies its value —
a field initialization is accounted exactly as a call argument would be.

`arrayLitFlow`:

- **List form**: every element gets its own holder, so each is move-or-copied.
- **Fill form**: a **move value may not be repeated** — `ErrorBadFill`,
  unconditionally, without trying to prove the count is 1. A counted reference
  may be, but the count must be a compile-time constant within `int16_t`, else
  `ErrorFillCount`. The amount is **n for an lvalue, n−1 for a temporary** — an
  lvalue still holds its own reference after the read, a temporary hands over
  the one it was born with.

The two codes are deliberately distinct: `ErrorBadFill` is a language rule;
`ErrorFillCount` is an implementation limit that should disappear when a fill
lowers to a loop.

## Generation

Scalars are LLVM constants; `nil` is `undef` of the empty struct. An integer
literal still carrying `FlagUnkType` is range-checked against its `i32` default
first (see Type check). The constant is built from `uintlit`'s bits truncated to
the type, which is the value itself once the range check has passed. `genlExpr`
truncates them itself before `LLVMConstInt`: LLVM takes only a value that fits
the type, and a release build of it keeps the excess bits of a negative
narrower literal unchecked, where folding a `zext` of the constant reads them.

**An array literal is emitted as a constant when every element is constant**,
and otherwise as an `undef` plus a chain of `insertvalue`. The constant form is
kept where possible because it is cheaper and it is **the only form usable
outside a function body**. Its address, which a borrow or a run-time index
takes, is that of an internal constant global made for the occurrence (the
`ArrayLitTag` case of `genlAddr`), as a string literal's is; one with a computed
element, which only an index reaches, is stored into an unnamed local instead.
A named constant's literal is reached the same way at each use that takes its
address, so each such use of a constant array makes a global of its own, as
each occurrence of a written literal does.

A type literal is the same `insertvalue` chain, with one special case: a
**nullable-pointer** enum has no struct at all, so the literal is either a null
pointer or the payload alone, with the tag discarded. A number's conversion is
`genlConvert` of its one value to the number type, the instruction `into`
emits ([cast](cast.md)), so `u64.from(x)` and `x into u64` are one conversion;
its value a literal, it is a literal too (`typeLitIsLiteral`), which a global
may take.

**A string literal emits a fresh global on every occurrence** — there is no
interning, and constant merging is not in the pass list.

**A string literal's global ends in a NUL its type does not count**, for C
compatibility: `"hello"` is an `Array[u8, 5]` and its global a `[6 x i8]`. The
`StringLitTag` case of `genlAddr` recasts the global's address to a pointer to
the literal's own array type, so a load, a copy and a slice's count all see the
text's bytes only; the terminator is reachable only through a pointer handed to
code that reads to it.

**So does a global variable initialized from a string literal.** It is not a
copy: its storage is the initialized data, so like the literal it gets the
terminating zero after the text, uncounted — `imm g = "hello"` and
`mut g Array[u8, 5] = "hello"` are each an `Array[u8, 5]` stored in a `[6 x i8]`, and a
`static` in a function body the same. `genlGloVarName` creates the longer
global and keeps in `llvmvar` its address recast to a pointer to the
variable's type, which is all any use sees; `genlGloVarGlobal` recovers the
global itself for its initializer, COMDAT, constness and linkage. Every Cone
store to a `mut` one is a store of the whole `Array[u8, n]` or an index the bounds
check holds below `N`, so the NUL is never overwritten — only a raw pointer,
which reads or writes past `N` at its own risk, reaches it. A global array
initialized from anything but a string literal, and a local copy of either,
is its type exactly and has no terminator.

## Hazards

- **Only an integer literal and an array literal are context-typed.** Every other literal is still
  adapted by coercion afterward, so `expectType` reads as more general than it is.
- **`uintlit` is not the value's number without `FlagLitNeg`.** Read alone, as
  a signed or an unsigned 64-bit value, it answers wrong for one class or the
  other: `18446744073709551615` read signed is `-1`, and `-1` read unsigned is
  u64's maximum. Anything new that reads a literal's value as a number must read
  the flag too.
- **An array literal's elements are coerced to the expected element type in
  two places, and only one of them sees the element type while checking.**
  Given an expected array type of its own length, `arrayLitTypeCheck` checks
  each element against the element type and coerces it there, so strings of
  different lengths share a `&[]u8` element and a nested literal is checked
  against the inner array type. A call's argument and a struct literal's field
  value are checked with no type expected, so the literal folds its elements
  among themselves first, and `iexpCoerce`'s `NoMatch` arm (`arrayLitCoerce`)
  coerces them afterward — which cannot rescue a fold that already failed:
  `f(["a", "bb"])` for an `Array[&[]u8, 2]` parameter is still refused (1046), where
  `["a", "b"]` is accepted. A literal of another length is never coerced.
- **`TypeLitTag` has no arm in `inodeTypeCheck`**, so it falls to the default,
  which reports `ErrorUnreachable` and stops. `typeLitNameRes` *is* dispatched,
  so an already-retagged literal in a cloned generic body can be name-resolved
  but not re-checked — and if that path is live, the compile aborts rather than
  skipping the check.
- **`cloneArrayNode` clones `elems` but shares `dimens`**, so a cloned fill
  literal shares its dimension node with the original.
- **A fill dimension that cannot be resolved silently becomes 0**, so a
  run-time-sized allocation's type claims a zero-length array. That path
  generates through the run-time fill loop, not the constant path.
- **`typeLitStructReorder`'s error recovery inserts fake zero values** typed as
  the field's type, so the coercion pass that follows passes on a value that is
  not real.
- **`newFakeULitNode` is dead code.**

## What lives elsewhere

- How an untyped literal is adapted: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- The literal-initializer rules for globals, parameters and field defaults: [vardcl](vardcl.md)
- What a fill literal's alias count means: [Flow Analysis](../phases/flow.md), "Moves and counting"
- The nullable-pointer enum: [struct](struct.md) and [Generation](../phases/generation.md)
- Where a type literal is retagged from a call, and a construction by a declared init: [fncall](fncall.md)
- What a declared init may do with its `self &new`: [Flow Analysis](../phases/flow.md), "An init's self"
