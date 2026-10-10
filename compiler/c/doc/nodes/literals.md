Seven literal forms across three source pairs: `nil`, `null`, integer, float and
string in `literal.c`; the array literal in `arraylit.c`, which **shares its node with the
array type**; and the type literal in `typelit.c`, which **shares its node with
a call**. A struct's value is constructed `new Point(1, 2)`, and a construction
its implicit field-wise init takes is lowered into a type literal; so is a
number's conversion, `u64.from(count)`. An enum's variant writes its literal in
brackets, `Some[x]`; a struct's in brackets is refused. An allocation,
`new Rc[mut, Node](1)`, constructs its value the same way, in the region.

**At a glance.** The parser builds them without deciding types. Name resolution
resolves each literal's type name, decides that `[…]` is an array literal, and
refuses it where it spells an array type. Type check types an integer literal and checks it fits, sizes
a string, checks array elements against each other, selects a construction's
init, and reorders a type literal's fields; where a constant is required, it
folds an expression of constants into the literal it computes. Flow accounts
for the values a composite literal takes ownership of. Generation emits
constants where it can.

*Provenance: read from source.*

## Shape

| Node | Tag | Payload |
| --- | --- | --- |
| `NilLitNode` | `NilLitTag` | none; `vtype` is a fresh void node |
| `NullLitNode` | `NullLitTag` | none; `vtype` is `nullLitType` until the raw pointer type it is wanted as replaces it (below) |
| `ULitNode` | `ULitTag` | `uintlit` — also carries `true`/`false` and tuple element indices |
| `FLitNode` | `FLitTag` | `floatlit` |
| `SLitNode` | `StringLitTag` | `strlit` pointer into the lexer's arena, plus `strlen` |
| `ArrayNode` | `ArrayTag` **or** `ArrayLitTag` | `dimens`, `elems`; `repeats`, how many elements each of `elems` fills, NULL for one each; `nsizes`, a type's written sizes or a literal's scalar levels (below) |
| type literal | `TypeLitTag` | an `FnCallNode` — `args` are the field values, or a number conversion's one value; `FlagNew` when it came of a construction, `FlagAllocValue` when it is a `+` allocation's bracketed value, which is refused |

**`FlagUnkType`** is set only by `newULitNode`, only when the lexer gave no type
suffix. **There is no float equivalent** — a suffix-less float defaults to `f32`
concretely, and the lexer refuses a float literal past its type's range
(`ErrorFloatRange`; [Parse](../phases/parse.md)).

**`FlagLitNeg`** says `uintlit` is the negation of the digits written. It is set
only by `parsePrefix`'s fold (see Parse), and toggled, so a minus applied twice
clears it.

**The array node serves both a type and a literal.** The list form `[a,b,c]`
has empty `dimens`; the fill form, one value stored into every element, has
`dimens` `[3]` and `elems` `[7]`, and is built only by an array's contents
repeating a constant, `new Array[i32, 3] <- fill 7` ([fncall](fncall.md), "The
list after `<-`"): the source spelling `[3; 7]` is retired. A list form an
array's contents build may carry `repeats`, the one place an element stands
for several: `new Array[i32, 100] <- fill f()` is `elems` `[f(), f()]` with
`repeats` `[1, 99]`, the second copy filling every element after the first.
`elems` then counts fewer than the array's size, which is its type's. The array type,
written `Array[i32, 3]`, is the same node tagged
`ArrayTag`, `dimens` `[3]` and `elems` `[i32]`: name resolution builds it from
the bracketed call (`arrayTypeLower`; [Name resolution](../phases/name-resolution.md)).

**`nsizes` is the written shape, and only contents read it.** Several sizes,
`Array[u8, 2, 3, 4]`, build three nested nodes, the outermost given `nsizes`
3; every other array type has 0, the nested spelling included. Equality,
matching, layout, `sizeof`, printing and naming never read it, so the two
spellings are one type everywhere; it decides only that a construction's
contents are the scalars (`u8`, row-major) rather than the rows
([fncall](fncall.md), "The list after `<-`"). A generic's type argument loses
it at substitution (`arrayTypeUnshaped`), since one instance serves both
spellings. On a literal, `nsizes` is how many levels of its type its elements
are the scalars of: `new Array[i32, 2, 3] <- 1, 2, 3, 4, 5, 6` is a list form
of six `i32`s typed `Array[i32, 2, 3]`, `nsizes` 2, and its fill form's
`dimens` counts scalars. Such a literal arrives typed, and
`arrayLitTypeCheck` and `arrayLitCoerce` leave it alone, since its count of
elements does not give its type.
Several sizes build nested nodes, each of one size, the first size the
outermost: `Array[i32, 2, 3]` is the node of `Array[Array[i32, 3], 2]`, so it is
that type, laid out and indexed (`a[i][j]`) as it is. The parser builds every
`[…]` as a literal and does not decide.

## Parse

Literal tokens map straight to constructors. `parseArrayLit` gathers
comma-separated expressions, and **if a `;` follows, swaps** — what it gathered
becomes `dimens` and a fresh list is gathered into `elems` — only so that name
resolution can refuse `[n; x]` in both its meanings (below).

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
`elems[0]` is **not** a type node. Decided by the first element alone. `[n; x]`
is refused either way: with a type for x, `[3; i32]`, it is the old spelling of
an array type (`ErrorArrayTypeOld`, naming `Array[T, n]`), and stays the
`ArrayTag` it spells, so nothing after reports it again; with a value it is the
retired fill literal (`ErrorFillLiteral`, naming `new Array[T, n] <- fill x`).
A list of one type, `[i32]`, stays an `ArrayTag` with no size, which type check
refuses. In a generic's template `[2; T]` is neither until `cloneArrayNode`
decides again on the substituted element ([generic](generic.md)), and refuses it
then the same way.

`namedValNameRes` resolves the *value* only — the name is deliberately not
bound, because it is matched against a field by symbol later.

## Type check

**An untyped integer literal takes the number type it is wanted as, wherever it
meets it, and no other literal is context-typed but `null` (below).** `litAdoptNumberType` is the
one rule: a `ULitTag` carrying `FlagUnkType` against an integer type takes that
type and drops the flag; against a float type it is replaced by an `FLitNode`
holding the full 64-bit magnitude written, negated when `FlagLitNeg` says so,
rounded once at the target's own precision. `litTypeCheck` applies it when `expectType` is a number type, and
`iexpCoerce` applies it to a literal that reaches coercion still untyped — an
argument to an overload set, a generic or an operator, which is type checked
before its callee is chosen. Every other literal is typed by
`itypeTypeCheck(&node->vtype)` alone.

**`bool` is the one number type it refuses.** `bool` is a 1-bit unsigned, so it
answers `UintNbrTag` like any other, but its only values are `true` and `false`
and a literal reaches it the way every other number does — through the `isTrue`
coercion in [Type Check Reasoning](../phases/type-check-reasoning.md),
"Coercion", which makes any non-zero value true. `litAdoptNumberType` returns 0
for it, so both of its callers fall through to that coercion and every position
agrees. `typemgmt_success` pins all of them — initializer, assignment, argument,
return value, struct-literal field, `if`, `not`, `and` and `or` — each with a
value whose low bit is 0, because `1` and `-1` read the same either way.
`true` and `false` are built carrying `bool`, never `FlagUnkType`, so the rule
never reaches them. `char` is refused too: an integer is no code point until it
is converted, `char.from(65)`, so `litAdoptNumberType` returns 0 for it as well,
and a literal wanted as a `char` is `ErrorCharNotNbr` from `iexpCoerce`.

**A character literal is a `char`, and is a `u8` only where a `u8` is wanted.**
The lexer types it `char` (a 32-bit unsigned by tag, `charType`) and the parser
marks it `FlagCharLit`; it is never `FlagUnkType`, so `litAdoptNumberType` does
not touch it. `litAdoptCharAsByte` retypes a literal so marked whose value is
below 128 when `u8` is wanted: `litTypeCheck` with an expected type,
`iexpCoerce` for one that arrives untyped by context, and `fnCallLowerMethod`
for a binary operator's receiver beside a `u8` argument. It drops the mark, so
what it became is an ordinary `u8` literal. A value of 128 or more wanted as a
`u8` is refused (`litCharRefusedAsByte`, reported by `iexpCharNumberMismatch`).
The mark is on the literal as written: a named constant holding one is a
`char`, which `typemgmt_typecheck_char` pins.

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
leave it unsigned, so `-1u8` is `255`, negation in its own width. `bool` is
not asked, since no literal is built at it. The message quotes the literal as
written, digits and suffix, with a `-` for the folded minus; the typed message
gives the type's range. Reported once: the literal is left a zero of its type,
so a literal checked again, or a constant generated at each use, does not
repeat it. `typemgmt_typecheck_litrange` pins each position and the edges;
`typemgmt_success` and `lexical_literals` print the edges that fit.

An explicit conversion is not a literal meeting a type: `u8.from(300)`
converts the `i32` literal `300`, and keeps its low bits as any conversion
does.

**A string literal is a borrow of its text, `&imm str`.** `slitTypeCheck` types
it as a `RefTag` to `str` (named, as a written `&str` is, so a generic given the
type as an argument copies a name and not the struct) with the `imm` permission,
the `borrowRef` region and global scope (0): it lives for the whole program, so it
crosses threads under the static-borrow rule. It is a value, a fat `{ptr, usize}`
built from the address of its constant and `strlen`; see "A literal and its
neighbours" below for what it converts to.

**A literal and its neighbours.** Six conversions meet a literal, none of them
the literal's own business but each decided from its tag:

- `&str` to `&Array[u8]`, any `&str` and not only a literal: `arrayRefMatchesRef` answers
  `CastSubtype` (a recast; the two share a layout) for a borrow-region fat `str`
  reference and a slice of `u8` whose permission the reference's meets
  (`imm` to `ro`, but not `mut`). It is never `&Array[u8]` to `&str`, which is
  `as`. `fnCallLowerRefCompare` takes a `&str` on the left of `==` against a
  slice as that slice (`fnCallArrayAsSlice`).
- A literal fills a byte array exactly its length (`slitMatches`, `slitCoerce`,
  asked at the head of `iexpCoerceShape` and answered by `iexpMatches`): the
  node is retyped in place to `Array[u8, strlen]` (`slitAsArray`), whose
  generation is the constant array of the text, no NUL counted. Any other length
  or element type is the ordinary mismatch.
- A literal wanted as an owner of `str`, in any region (`slitIsStrOwner`): the
  node becomes the `AllocateTag` node `new R[str](lit)` builds, typed as the owner
  type wanted, its region and permission that type's. Only the literal node
  itself is taken this way; a variable or a call of type `&str` is not, and
  needs `new`.
- A literal wanted as a struct that declares a static function
  `fromLiteral(text &str) Self` (`slitFromLiteralFn`): the node becomes the call
  of it on the literal, built lowered (`newFnCallLower`, `fnCallFinalizeArgs`)
  since a coercion has no type check state to check a call with. That is how
  collections' `String` takes a literal wherever one is wanted. It is the type's
  statement that a literal may stand for it, so the function must be public and
  the type not generic; a literal is the only thing taken, as for an owner.
  Core's `cstr`, C's `const char *`, declares one (`new cstr(bytes as *u8)`), so a
  literal is a C string wherever one is wanted: the node is the inline call, and
  the pointer it holds is the literal's own global, whose NUL the type does not
  count. A `&str` that is not a literal is not taken (`ErrorCPtrConv`: it promises
  no NUL). A literal that holds a NUL byte inside is refused as a `cstr`
  (`ErrorCStrNul`, `slitCStrCheck`, called from `slitCoerce` and `slitBorrowCoerce`,
  at the literal): C reads a string up to its first NUL, so it would see only the
  text before it. The check is on the conversion, so it knows one type, `cstr`
  (`cstrTypeDcl`); the same literal as a `&str`, a `String` or an owner of text
  is taken as it is, and an explicit `cstr.fromLiteral(lit)` or `cstr.fromPtr(p)`
  is a call with a `&str` or a pointer, not a conversion, and says nothing.
- A literal wanted as a read-only borrow, `&T`, of such a struct (`slitBorrowMatches`)
  is lent as a temporary: the node becomes the borrow `&T.fromLiteral(lit)`, built
  and checked as that borrow written out is (`slitBorrowCoerce`: the call
  unchecked under a `BorrowTag` node, then `borrowTypeCheck`). So the temporary is
  a temporary of the statement or, in a local's initializer, extended to the
  block's end (`varDclExtendTemp`), is finalized there, and a borrow of it kept
  longer is refused by the loan walk as the written borrow's is. Nothing in it
  knows a type: `Path`, `String` and `cstr` are `T` alike. The borrow needs
  the type check state (its scope, the extension), which a coercion does not
  carry, so it is made one level up, by `iexpCoerceIn`, where the state is at
  hand: a call's arguments (`fnCallFinalizeArgs`), an init's arguments in
  `new T(...)` (`typeLitInitArgs`) and `iexpTypeCheckCoerce`'s places
  (initializer, assignment, ...), and only in a function's body (scope 2
  and up). A bare `iexpCoerce` meets it as no match (`iexpCoerceShape`), so every
  other place -- a global, a constant, a field's default, a
  returned value, a value for the implicit init's field, an `if` arm -- keeps the
  ordinary mismatch. **A parameter's default is the exception, made at the
  call:** `varDclTypeCheck` leaves a string-literal default as the literal when
  `slitDefaultDeferred` says the parameter wants a read-only `&T`, or a `T` by
  value, with `T` declaring `fromLiteral` (the default is still a constant, the
  literal), and `fnCallFinalizeArgs`, appending the defaults of the arguments a
  call omits, clones the literal (`cloneSLitNode`) and coerces the clone with
  `iexpCoerceIn` as it does an argument written there. So each call that takes
  the default has a temporary of its own, finalized at its statement's end
  (or the value, given to the callee), and a call that gives the argument
  makes none. A call made where no statement exists (a global's initializer)
  is refused, with the reason. A default that writes (`&mut T`) is refused as
  any literal is. Only a permission that cannot write is lent: `&mut T`,
  `&uni T` and a lock's are refused by `slitBorrowRefused` with
  `ErrorLitBorrowWrite`, since a write to a temporary is lost. **Overloads:**
  selection counts this conversion only as a fallback. `iNsTypeFindMethod`
  asks the call once without it, and again with it (`slitBorrowFallback`,
  `slitBorrowOffered`) only when no candidate took the arguments and one is a
  literal, so a candidate that takes the literal as the `&str` it is is never
  ambiguous with a `&T` one (a dictionary's `&K` and `&str` indexes). Two
  candidates that need the conversion are ambiguous as any two are. The
  selection of a declared init in `new T(...)` falls back the same way
  (`typeLitNewChecked`: only when neither a declared init nor the implicit one
  took the arguments), with the same refusals. The implicit init and the
  compiler-declared operators on pointers do not offer it.
- A written borrow of a literal, `&"text"` or `&[]"text"`, retypes the literal as
  the array (`borrowTypeCheck`), so the borrow is a reference to it as before:
  `&"text"` the array behind the text (no NUL counted), which `&"text" as *u8`
  takes the address of, and `&[]"text"` the slice. It is the way a literal
  reaches a C parameter that is a `*u8` rather than a `cstr`: a buffer, or a
  pointer that may be null. The literal as an array is also an lval.

A string literal wanted as a `&Array[u8]` is a `CastTag` recast of the `StringLitTag`
node, and `litIsLiteral` accepts that (`litIsTextAsBytes`), so a global's,
constant's, field's or parameter's default may be one.

**A `null` takes the raw pointer type it is wanted as, and no other type.** It
is built with `nullLitType`, an absence node distinct by identity, which says
"any raw pointer, not yet which". `nullLitTypeCheck` gives it an expected type
that is a pointer at once; with no expectation it waits, and every coercion
judges it first (`litAdoptNullType`, at the head of `iexpCoerce`): a `PtrTag`
target becomes its `vtype`; any other target, or none at all (`unknownType`,
as an initializer with no declared type gives), is `ErrorNullNotPtr`, and its
`vtype` becomes `errorType` so nothing that uses it reports again. A reference,
a function reference among them, is never null, and an `Option`'s absence is
`None`, so neither takes one. `iexpMatches` answers `EqMatch` for any pointer
and `NoMatch` for anything else, which is what overload and init selection ask.
Two places have no coercion to wait for and type it themselves: a
comparison, from the other operand (`fnCallTypeNullOperands`, before the
dispatch, since a pointer's `==` requires both sides' types to be the same),
where a `null` receiving any other call is refused; and `null as *T`, from the
target (`castTypeCheck`). Generic inference skips a `null` argument, so the type parameter
comes from another argument or is named. Flow is the backstop: a `null` it
reaches still untyped is refused there (`flowLoadValue`'s case). It is a constant
(`litIsLiteral`), so it may be a global's, static's, constant's, field's or
parameter's default, but never an array's count (`arrayLitDimIsConst`).

**Array literal** — `arrayLitTypeCheck` checks the list form; an array's
contents arrive already typed as the array (`contentsLowerArray`), the fill
form among them, and are not checked again. Its fill-form arm is reached only
by a fill literal a macro's or generic's clone refused, after the refusal:
one constant dimension, forced to `usize`, and exactly one value.
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

**Construction** — `typeLitNewCheck` takes a `FlagNew` call. A managed
reference type makes it an allocation, an `AllocateTag` node holding the
construction of its value type or a finished value of it
([references](references.md), "Allocation"); `trynew` on any other type is
`ErrorTryNewValue`. An array's construction is its
contents, after `<-`, which `contentsLower` takes before this is reached, so
one here has none (`ErrorArrayContents`); so is an allocation of an array
with contents, which `contentsLower` hands its literal (`typeLitNewFilled`). Otherwise its type must be a struct
(`ErrorNewType` otherwise: a number converts with `from`, an enum's variant
keeps its brackets). One positional argument whose type is the struct itself
is a finished value, which needs no construction (`ErrorNewFinished`); an
allocation takes one as its value instead. A generic
struct named bare, `new Box(5i64)`, has its type arguments inferred from the
values, as its literal's were (`genericSubstitute`). Its inits are the implicit
field-wise one and those it declares under the name `init`, one or an overload
set, each `fn init(self &new, ...)`. With none declared, the arguments are
checked against the fields they fill, as a literal's are; otherwise with no
expectation, as an overload set's are. Viability is counted as
`fnSigViableCall` counts it -- the count, the defaults, each argument passable
-- for a declared one its parameters after `self`, for the implicit one the
fields in order. **A viable declared init is preferred to the implicit one**,
which is then not asked; among the declared ones exactly one must be viable,
and the implicit one is the choice only when none is. So none viable, or
several declared ones, is `ErrorInitNone`. Named arguments are the implicit
init's alone, which is how it is reached whatever the struct declares; a name
no field has is `ErrorNamedArg`. The implicit init is lowered to the
struct's literal, retagged `TypeLitTag` with `FlagNew` kept, and checked as
below; a declared one stays an `FnCallTag` with `FlagNew`, its `objfn` the
init's name use, its arguments coerced to the parameters after `self` and the
defaults appended, and its `vtype` the struct, the call's value
([fncall](fncall.md), "Construction"). A declared init not `pub` is its module's
(`ErrorNotPublic` outside it, `structSeesPrivate`), as is giving a private
field a value in the literal.

**Type literal** — `typeLitTypeCheck` requires a concrete type, then builds a
struct's literal. **A struct's literal in brackets is `ErrorStructBracket`**,
naming `new Point(...)`, unless it came of a construction (`FlagNew`), is a
refused `+` allocation's value (`FlagAllocValue`, set by `allocateTypeCheck`,
which refuses the allocation itself, `ErrorPlusAlloc`, naming the `new` form),
or is a
variant's, which has a discriminant field to fill; refused at type check, so a
struct reached through an alias or a type parameter is refused as one named
directly, and the literal is still built so nothing after it reports again. A
number type written with brackets, `u64[count]`, is
`ErrorNbrBracket`, because a number's conversion is its method,
`u64.from(count)`. That call reaches this node another way: `fnCallNumberFrom`
([fncall](fncall.md)) checks its one value, has `typeLitNbrFromCheck` accept a
number (and for `bool` a reference or pointer, as `typeLitConvertsToBool` says),
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
lval nor a literal, so coercing it would refuse a string literal's conversion to a
slice and leave an untyped number literal at its default type.

`litIsLiteral` is the compile-time-constant predicate the global, parameter and
field-default rules use. It accepts a use resolved to a `ConstDclTag`, which is
what makes `imm g i32 = K` legal. It accepts a borrow (`BorrowTag` or
`ArrayBorrowTag`) of a string literal too: the text is a constant global, so a
reference to it, or a slice of it (its address and its length), is known before
anything runs. The literal itself, which is that address and length, is a literal
too, alone or recast to a `&Array[u8]` (`litIsTextAsBytes`), so `imm g &Array[u8] = "text"`
and a struct literal holding one as a field are literal initializers;
generation's `genlExpr` builds the `{ptr, usize}` with instructions the builder
folds to a constant aggregate. It accepts a borrow of
an array literal whose elements all satisfy it (`arrayLitIsLiteral`) on the same
terms, so `imm g = &[1, 2, 3]` is a literal initializer, and a borrow of a named
constant holding either (`borrowIsConstLit`), so `imm g = &K` is one too. It accepts a value
tuple whose values all satisfy it (`vtupleIsLiteral`), so
`mut g (i64, i64) = 1, 2` is one too: the `VTupleTag` arm of `genlExpr` builds
it by `insertvalue` of constants, which the builder folds to a constant struct,
as it does a struct literal of constants.

**A reinterpretation of a constant is a constant** (`litIsConstCast`): a
`CastTag` without `FlagConvert` (an `as`) whose target is a number or
a raw pointer and whose operand is a number literal, a `null`, a `ConstDclTag`
use, or another such cast. One to a number of a constant number is folded into a
literal where a constant is required (below), so what reaches this test there
is the one to a pointer, whose operand may itself have been folded:
`(BASE + 16usize) as *u8`. A struct target is left out:
`genlRecast` reinterprets one through a stack slot, which a global's
initializer has none of. Type check still applies the same-size rule, and
`genlRecast`'s `bitcast`, `inttoptr` or `ptrtoint` of a constant operand is
folded by the builder into a constant (`ptr null` for zero). The count of
`n of x` in an array's contents is the one constant context that refuses it
(`contentsConstCount`): type check reads the count from a `ULitTag`, and a cast
has not been generated yet.

**A borrowed constant array literal is a constant, as a borrowed string literal
is.** Neither is an lval of a variable, so `borrowTypeCheck` asks
`borrowIsConstLit` before refusing its operand as a temporary, and gives the
borrow what a global constant has: `imm` and scope 0, the program's lifetime.
So `&mut [1, 2, 3]` is refused (`ErrorBadPerm`) as `&mut "text"` is, and a
function may return `&[2, 3, 5]`. Before it asks, the borrow folds the array
literal's elements (`litFoldConst`, below), so an element computed from
constants alone, `&[R | G, B]`, is a constant too, in a function body as
anywhere else. An array literal with any other computed element is
still a temporary, borrowed as one is: to its statement's end, or, where a
local's initializer extends it, to its block's ([vardcl](vardcl.md),
"Temporaries an initializer extends"). The literal was typed from its elements
alone, the borrow expecting nothing of it, so `&[1, 2, 3]` wanted as a `&Array[u32]`
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

### Folding a constant expression

**Where a constant is required, an expression of constants is folded into the
literal it computes** (`litFoldConst`, `literal.c`), once the value is type
checked and coerced, and before `litIsLiteral` is asked: a named constant's
value (`constDclTypeCheck`), a global's, a static's and a parameter's default
(`varDclTypeCheck`), a field's default (`fieldDclTypeCheck`), and the elements
of an array literal a borrow takes (`borrowTypeCheck`). The result is an
ordinary `ULitNode` or `FLitNode` placed where the expression was, so
generation, `--ir`, the uses of the constant and every `litIsLiteral` caller
see a literal and nothing else changes for them. Nowhere else folds: an
expression in a function body is generated as written, and LLVM folds it.

**What folds**, from the leaves up, each over number literals and named
constants (followed through any constant naming another):
- a call of a number type's operator method whose body is an intrinsic
  (`litFoldOp`): `+ - * / %`, `& | ^ << >>`, unary `-` and `~`, the six
  comparisons (a `bool`), and the `isTrue` a coercion to `bool` injects; the
  operators on `bool` itself among them;
- `not`, `and` and `or` over `bool` constants;
- a number's conversion, `T.from(x)` (a `TypeLitTag` of a number type) and the
  `FlagConvert` cast a coercion's widening injects, by `litFoldConvert`, which
  does what `genlConvert` generates;
- a reinterpretation with `as` to a number of the same size, by
  `litFoldRecast`: the same bits read as the other type. One whose bits read as
  a float are a NaN or an infinity is left a cast, which `litIsConstCast` still
  accepts and generation folds, since no float literal holds either;
- inside an array literal, a struct's or a variant's literal (through its
  `NamedValNode`s), a value tuple, and a borrow of an array literal, each
  element on its own.

Anything else is left as it is: a call of a function, a method like `sqrt`, a
variable. The caller then reports the value as not a constant, as it did before
any folding, unless the fold reported why part of it has no value, or its type
is `errorType` (its type check reported it); `litFoldConst` answers 1 for both,
so one cause is reported once.

**A fold has the run-time meaning of the operator on its type.** Integers are
read through `FlagLitNeg`'s rule (`litExtend`: the low bits of the type,
sign-extended for a signed type), so `-1u8` is 255 and `-128i8` is -128.
Division and remainder truncate toward zero, `>>` is arithmetic on a signed
type and logical on an unsigned one, and `<<` discards the bits shifted out, as
`LLVMBuildShl`'s result does. A float operation is computed at its own width,
an `f32` as a C `float`, never as a double rounded afterward, and an `f32`
literal is first rounded to the `f32` it is generated as (its `floatlit` holds
the double written: `0.1f32` is the `f32` nearest 0.1). An untyped integer
operand is the `i32` it defaults to, and is checked against it then
(`litCheckDefaultRange`).

**What the run time gives no value is refused**, each at the operator and once,
the node then left a zero of its type (`litFoldNoValue`, as `litCheckRange`
leaves an out-of-range literal) so nothing folded from it reports again:
- `ErrorConstOverflow`: an add, subtract, multiply, divide or negation whose
  exact result the integer type cannot hold, the smallest value divided by or
  taken the remainder of by -1 (LLVM's `sdiv` and `srem` both have no value
  there), a float result past the type's finite range, a float converted to an
  integer that cannot hold its truncated value (`fptosi`/`fptoui` poison), and
  an `f64` converted to an `f32` past its range. The bitwise operators cannot
  overflow, and `-` on an unsigned constant is negation in its own width, as on
  an unsigned literal;
- `ErrorConstDivZero`: a division or remainder by zero, integer or float;
- `ErrorConstShift`: a shift by the width or more, or by a negative amount of a
  signed type. The one refusal here of something the run time gives a value: a
  run-time shift that far is 0, or the sign for `>>` on a signed type
  ([Generation](../phases/generation.md), `genlShift`).

**A constant defined in terms of itself.** A use of a named constant is checked
(`litConstUnsettled`): a chain of constants reaching one still under type check
has come back round, and is reported `ErrorCircular` at the use and replaced by
a zero of its type ([Type check](../phases/type-check.md), "Circularity").

## Flow

Scalar literals are no-ops. The two composites take ownership of values:

`typeLitFlow` unwraps each `NamedValNode` and move-or-copies its value —
a field initialization is accounted exactly as a call argument would be.

`arrayLitFlow`:

- **List form**: every element gets its own holder, so each is move-or-copied.
  An array's contents repeating a value are this form, the value's expression
  copied into each element, so a variable repeated is moved by the first and
  gone for the second, and a counted reference copied in gains a holder per
  element, by the ordinary rules. A copy the literal's `repeats` repeat is
  walked once, as the second element, and the holder flow injects around it
  is added on each pass of the loop that fills its elements.
- **Fill form**: one constant, which moves nothing and counts nothing; it is
  only read.

## Generation

Scalars are LLVM constants; `nil` is `undef` of the empty struct, and `null` the
null constant of its pointer type. An integer
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
each occurrence of a written literal does. Every element null is
`zeroinitializer`, built without a value for each element.

**An array's contents that repeat a value fill a variable in place**
(`genlArrayLitInto`, from `genlLocalVar`), never as one aggregate value:
LLVM's instruction selection takes an aggregate store apart element by
element, and at a hundred thousand elements crashes doing so. A literal with
`repeats`, or a fill form of more than `ArrayRepeatUnroll` (16) elements, is
stored element by element and run by run (`genlArrayRun`): a listed value is
one store; a constant repeated is generated once and stored by one `memset`
when it is null, or a loop otherwise; a computed value is generated inside the
loop, so evaluated once for each element. Anywhere else, a literal with
`repeats` and a computed element is filled the same way into an unnamed local,
and its value loaded from there; with only constants it is the constant
array, each repeated value standing for each element it fills.

**A literal of scalars (`nsizes` above 1) is stored into the array seen as
one run of them**: `Array[u8, 2, 3, 4]` is filled through `[24 x i8]` at the
same address, which is the same memory row-major, so every entry form is a run
as above, and a null fill one `memset` over all of it. A variable always takes
it in place. As a value elsewhere, all constants build the constant of the
nested type, rows cut from the scalars in order (`genlArrayConstRows`), every
one null `zeroinitializer`; otherwise it is filled into an unnamed local and
loaded (`genlArrayLitScalars`).

**An allocation's contents are filled in the region's memory**
(`genlallocref`, `FlagAllocFill`; [generation](../phases/generation.md),
"An allocation runs in one order"), by the same runs, each store taking the
write barrier in a traced region.

That chain, and a type literal's below, is built in a slot of memory, part by
part, where the value is more than 64 bytes: `genlAggCopies`
([generation](../phases/generation.md), "Large aggregates").

A type literal is the same `insertvalue` chain, with one special case: a
**nullable-pointer** enum has no struct at all, so the literal is either a null
pointer or the payload alone, with the tag discarded. A number's conversion is
`genlConvert` of its one value to the number type, the instruction a
coercion's conversion emits ([cast](cast.md)); its value a literal, it is a
literal too (`typeLitIsLiteral`), which a global may take.

**A string literal emits a fresh global on every occurrence** — there is no
interning, and constant merging is not in the pass list.

**A string literal's global ends in a NUL its type does not count**, for C
compatibility: `"hello"` is five bytes and its global a `[6 x i8]`. The
`StringLitTag` case of `genlAddr` makes the global and returns its address; as a
value (`genlExpr`) the literal is that address with the count `strlen`, which
leaves the NUL out, and taken as an array it is recast to a pointer to the
array type, so a load, a copy and a slice's count all see the text's bytes
only; the terminator is reachable only through a pointer handed to code that
reads to it.

**An owner of `str` keeps a NUL after its bytes too.** The allocation that copies a
`&str` into a region (`genlAllocate`, a body whose reference carries its length)
asks the region for the header, the bytes and one more byte, and stores a zero
there after the copy, in every region, so `So[str]`, `Rc[str]` and the rest are as
much a C string as the literal they may have been copied from (`s.cstr()` on a
`So[str]`, which calls `cstr.fromOwned`).
The count leaves the byte out. A `String` keeps its own (collections), and
`freeze` carries it into the `So[str]` it makes.

**So does a global byte array initialized from a string literal.** It is not a
copy: its storage is the initialized data, so like the literal it gets the
terminating zero after the text, uncounted — `mut g Array[u8, 5] = "hello"` is an
`Array[u8, 5]` stored in a `[6 x i8]`, and a `static` in a function body the
same (`genlGloVarHasNul` asks that the literal has been taken as an array). A
global given a literal as it is, `imm g = "hello"`, holds the `&imm str`, the
`{ptr, usize}` of the literal's own global, and is refused on a GPU target
as any global holding a reference is. `genlGloVarName` creates the longer
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
- **An `f32` literal's `floatlit` is the double written, not the `f32`.** It is
  rounded when generated, so anything that computes with it rounds it first
  (`(double)(float)`), as `litWidenFloat` and the fold's `litNbrRead` do; read
  raw, `f64.from(0.1f32)` folded to the double 0.1.
- **The fold computes floats with the host's C arithmetic.** That is the
  target's IEEE result only where the host rounds each `float` and `double`
  operation once, as x64's SSE does; a `conec` built for x87's extended
  precision would round twice.
- **A fill decides its form before the fold.** `contentsLowerArray` makes the
  fill form only of a value that is already a literal, so `<- fill R | B` in a
  constant is a list of copies, or a run with `repeats`, each copy folded on its
  own: still a literal, but one element per copy rather than one value.
- **An array literal's elements are coerced to the expected element type in
  two places, and only one of them sees the element type while checking.**
  Given an expected array type of its own length, `arrayLitTypeCheck` checks
  each element against the element type and coerces it there, so strings of
  different lengths share a `&Array[u8]` element and a nested literal is checked
  against the inner array type. A call's argument and a struct literal's field
  value are checked with no type expected, so the literal folds its elements
  among themselves first, and `iexpCoerce`'s `NoMatch` arm (`arrayLitCoerce`)
  coerces them afterward — which cannot rescue a fold that already failed:
  `f(["a", "bb"])` for an `Array[&Array[u8], 2]` parameter is still refused (1046), where
  `["a", "b"]` is accepted. A literal of another length is never coerced.
- **`TypeLitTag` has no arm in `inodeTypeCheck`**, so it falls to the default,
  which reports `ErrorUnreachable` and stops. `typeLitNameRes` *is* dispatched,
  so an already-retagged literal in a cloned generic body can be name-resolved
  but not re-checked — and if that path is live, the compile aborts rather than
  skipping the check.
- **`cloneArrayNode` clones `elems` but shares `dimens`**, so a cloned fill
  form shares its dimension node with the original.
- **An array's contents are one copy of a repeated value per element**, so a
  large array repeating a computed value is that many expressions, checked and
  generated each; only a constant repeated alone stays one value.
- **`typeLitStructReorder`'s error recovery inserts fake zero values** typed as
  the field's type, so the coercion pass that follows passes on a value that is
  not real.
- **`newFakeULitNode` is dead code.**

## What lives elsewhere

- How an untyped literal is adapted: [Type Check Reasoning](../phases/type-check-reasoning.md), "Coercion"
- The literal-initializer rules for globals, parameters and field defaults: [vardcl](vardcl.md)
- An array's contents, and the list after `<-` they are written with: [fncall](fncall.md), "The list after `<-`"
- The nullable-pointer enum: [struct](struct.md) and [Generation](../phases/generation.md)
- Where a type literal is retagged from a call, and a construction by a declared init: [fncall](fncall.md)
- What a declared init may do with its `self &new`: [Flow Analysis](../phases/flow.md), "An init's self"
