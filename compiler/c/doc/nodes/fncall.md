`FnCallNode` is the compiler's busiest node. One shape — `objfn`, `methfld`,
`args` — serves function calls, method calls, operator applications, field
access, array indexing, constructions (`new Point(1, 2)`), variants' literals,
number conversions, generic instantiation and macro calls. Type check is where
they separate.

**At a glance.** Built by `parseexpr.c` from several unrelated syntaxes. Name
resolution binds `objfn` and the arguments and **deliberately leaves `methfld`
alone**. Type check dispatches on the receiver's type, selects a candidate, and
retags the node into what it actually is. Flow moves or copies each argument.
Generation emits a call, a GEP, an `extractvalue`, or an intrinsic.

Read this before touching `fnCallTypeCheck`, which is the largest function in
the compiler — and the one most often proposed for splitting, because giving
these syntaxes distinct node shapes would remove most of what stage 3 has to
re-derive.

*Provenance: read from source, end to end through every dispatch arm.*

## Shape

| Field | Meaning |
| --- | --- |
| `objfn` | the callee, or the receiver of a method/field/index |
| `methfld` | the member after `.`, or the operator's interned name — a `NameUseNode` bound to nothing until the member is selected, or a `ULitTag` for a tuple element index, or NULL |
| `args` | argument list, or NULL. **The receiver is inserted at index 0** when a method is selected |
| `vtype` | the call's result type, established by lowering |

**`methfld` is what tells a call from an operator or member access.** `TWO + 1`
is objfn `TWO`, methfld `+`, one argument — the same shape `p.sum()` builds. The
flags carry the rest of what the source said:

| Flag | Means |
| --- | --- |
| `FlagIndex` | arguments were written in `[]` |
| `FlagBorrow` | this link is part of a borrow chain |
| `FlagVDisp` | virtual dispatch |
| `FlagLvalOp` | the operator needs an lval receiver (`++`, `--`, `<-`, op-assign) |
| `FlagOpAssgn` | an operator-assignment such as `+=` |
| `FlagOperator` | **the source wrote an operator, not a named member access** |
| `FlagNew` | a construction, `new Point(1, 2)`; kept on what type check lowers it to |
| `FlagTryNew` | beside `FlagNew`, a construction written `trynew`; its `methfld` holds the bound `Option` from name resolution to type check |
| `FlagAllocValue` | a struct's bracketed literal that a refused `+` allocation takes as its value, which type check refuses there with its own code (`ErrorPlusAlloc`) rather than `ErrorStructBracket` |

`FlagOperator` exists solely because the two are otherwise indistinguishable
after parsing, and one dispatch decision depends on knowing which — see Hazards.

## Constructors

| Function | For |
| --- | --- |
| `newFnCallNode` | a plain call |
| `newFnCallOpname`, `newFnCallOp` | an operator application; **both set `FlagOperator`** |
| `newFnCallOpnameLower` | an operator application positioned on an existing node rather than on wherever the lexer has reached; also sets `FlagOperator` |
| `newFnCallLower` | a plain call, positioned the same way. **Does not set `FlagOperator`** |
| `cloneFnCallNode` | instantiation |

Use a `*Lower` form for anything synthesized after its construct was parsed —
the lexer has moved on, and a node built with the plain constructors points at
end of file.

## Parse

Built from: a call `f(a)`, an index `a[i]`, a member access `a.b`, every binary
and unary operator, a generic instantiation `Box[i64]`, a managed reference
type `Rc[mut, Node]`, `?T` for `Option[T]`, a variant's literal `Some[x]` (or a
struct's, refused), a number's conversion `u64.from(count)`, a member access
like any other, and a construction, `new Point(1, 2)` or the allocation
`new Rc[mut, Node](1)`: `parseNew`, a term, takes `new`, the type (a name,
`.` paths and bracketed type arguments) and the parenthesized arguments, left
off when there are none, into one node flagged `FlagNew`; suffixes after the
parentheses apply to the value. `trynew` builds the same node, flagged
`FlagTryNew` as well, inside an `Option` node tagged `QuesTag`, as `?T`
parses, which name resolution takes apart (`allocateQuesNameRes`): the
construction replaces it, holding the bound `Option` in its `methfld`.
`parseDotCall`, `parseSuffix`, `parseArgs` and the whole precedence cascade
all build this node.

`<-` is an operator application too, its one argument what follows it: one
entry, or a `TupleNode` of several (`parseAppend`, `parseEntries`). An entry
is a value, or an `EntryNode` for the forms read nowhere else, `n of x`,
`fill x`, `k: v` and an `each` (`parseEntry`; [Parse](../phases/parse.md), "The list
after `<-`"). After a construction inside a comma list -- an argument, a named
value, an array literal's element, an entry -- `<-` takes one entry
(`parseContentsAfter`), so the comma stays the list's.

Nothing about which of those it is has been decided yet.

## Name resolution

`fnCallNameRes` resolves `objfn`, collapses the node if that turned out to be a
path, and resolves each argument. Resolving `objfn` first is what lets
`itypeIsGenericType` recognize an unlowered `Box[i64]` as a type, and
`itypeIsManagedRefType` an unlowered `Rc[mut, Node]`, which the
type-versus-value decisions elsewhere depend on. An index whose `objfn` is
bound to `Array` (`arrayTypeDcl`) is the array type, `Array[i32, 3]`, and is
replaced here, once its arguments are resolved, by the array type node it
names (`arrayTypeLower`, [literals](literals.md), "Shape").

**It never resolves `methfld` as a member** — selecting a member needs the
receiver's *type*, which does not exist yet — so a member name is a
`NameUseNode` name resolution never meets, since nothing else hands one to
`inodeNameRes`.

### The path collapse

**A period whose left side names a namespace is a path through it, not an
access to a value**: `math3d.Point3`, `Tally.make(1)`, `Tally.made`. The parser
cannot tell the two apart, so `fnCallNameResPath` decides as soon as `objfn` is
bound, and **what it binds to is the whole test** — a `ModuleTag` or a
`StructTag` is a namespace, anything else is a receiver. An operator, a tuple
index and a call with no member name are never paths.

The member is looked up with `namespaceFind` in that namespace, bound, stamped
`FlagQualified`, and the hop disappears:

| | Becomes |
| --- | --- |
| no arguments — `f32.pi`, `mymod.Gadget` | the bound name use, replacing the node outright |
| arguments — `Tally.make(1)` | a plain call: `methfld` moves to `objfn` and becomes NULL |

**Either way the shape handed on is the one an unqualified name of the same
declaration produces**, so nothing downstream learns a path was written. A
chain falls out of that for free: `mymod.Gadget.make(2)` parses innermost
first, so the inner hop has left a resolved type name in `objfn` before the
outer hop looks at it.

### The parallel reductions

**`xs.parallel().sum()`, `fold(...)` and `findFirst(...)` are rewritten here, by
shape**, ahead of the receiver's resolution: a method named `sum`, `fold` or
`findFirst` called directly on a `.parallel()` with no arguments
(`parallelReduceIs`, `ir/exp/pareach.c`). `parallel()` is an ordinary method of
core's `Array` and cursors, giving a view (`ParallelSlice`, or
the cursor itself where it has `len` and `at`); the reductions
run on the actors' workers, which core cannot reach, so they are the actors
package's `parSum`, `parFold` and `parFindFirst` (`reduce.cone`) and, once the
receiver and arguments are resolved, the node becomes the call
`actors.parSum(view, ...)` (`parallelReduceNameRes`: the function found in the
namespace of the actors package the module imports, bound and stamped
`FlagQualified` as a path's member is, the view first among the arguments). A
module that does not import `actors` is `ErrorParReduce`, as `parallel each`'s
`ErrorParRuntime`. After the rewrite it is a call of a generic function like any
other, which is why the closures take their parameter types from the bounds
(`genericClosureSig`, [generic](generic.md)). By shape alone, so a reduction on a
view held in a variable (`imm v = xs.parallel(); v.sum()`) is not rewritten and is
refused at type check, in `fnCallLowerMethodOn`'s method-not-found message
(`parallelViewNotFound`), as is `parallel()` of a type that has none (a chain of
iterator adapters, a deque's cursor): `ErrorParReduce`, saying what has one. A
number range has no view yet: `(lo < hi).parallel()` and `(lo <= hi).parallel()`
(an operator application of `<` or `<=` directly under a `.parallel()`) are refused
with the reason, `ErrorParReduce` (`parallelRangeNameRes`); ranges get one with the
new range syntax.

**Privacy is checked here**, against `dclInfoGetModule` of the base — so
`modulesyms.Gadget.make` is judged against `modulesyms`, one hop back, which is
the module that owns the type.

**So is a method of an abstraction.** Through a trait or an enum, a member that
is a method — or an overload name with a method among its candidates — is
reported `ErrorAbstractMeth`: the implementers and variants own the only
generated copies ([struct](struct.md), `nodelist`), so the path would name code
that does not exist. A static function through the same path is the
abstraction's own and passes, and the same name through a struct, a variant or
an implementer names that type's copy. `trait_nameres_method_path` pins it.

⚠ **This cannot wait for type check.** Name resolution itself asks `isTypeNode`
of an operand: `&mut mymod.Gadget` and `(mymod.A, mymod.B)` are settled by
`refNameRes` and `ttupleNameRes`, which run after this and need a resolved type
name to look at.

**What it does not reach** is a base that is not a namespace *yet* — an alias,
a number type, a generic instance, a generic parameter. Those arrive at type
check as member accesses. **An instance's path is collapsed there**, once the
receiver's check has made the instance: a generic module's by
`fnCallModuleInstancePath`, `stack[i64].push(3)`, and a generic type's by
`fnCallTypeInstancePath`, `List[i64].empty()`, which looks the member up in the
instance's namespace and applies the same privacy and abstract-method rules as
above, the privacy judged against the instance's module. A generic type's path
reaches only a function or an overload name. **A number type's path reaches
only `from`**, its conversion, `u64.from(count)`, lowered by
`fnCallNumberFrom` (Type check, stage 1). Any other member, and every other
base, is refused by stage 2's type receiver.

## Type check

`fnCallTypeCheck` in three stages. This is the map worth carrying.

**A construction (`FlagNew`) is handed whole to `typeLitNewCheck`** before any
stage ([literals](literals.md), "Construction"), which selects the init its
arguments call for and lowers it to the struct's literal or to a call of a
declared init (below, "Construction"); a managed reference type there makes
it an allocation, the `AllocateTag` node holding the value's construction or
a finished value ([references](references.md), "Allocation").

**Stage 1 — syntax, before the callee is known.**
A generic method given type arguments on a receiver, `h.pick[i32](6)` or
`h.pick[i32]`, first (`fnCallMethodTypeArgs`). It parses as the member access
`h.pick` indexed by the type arguments, with the call applied to that; a type is
never an index, so a member access with no arguments indexed by a type is taken
for this. The receiver is checked, the name looked up on its type, and the whole
of it lowered here to the one method call `h.pick(6)`, positioned on the member
access, with the instance the arguments name bound to the member
(`genericMethodInstance`) — finished here rather than handed back, since the
receiver is already checked. The type arguments are checked before the
instance is made, as a generic function's are, so a written instance of a
generic type, `h.pick[Box[i32]](...)`, is the type it names. A member that is not a generic method is
`ErrorNotTyped` at the first type argument. A base that is a type or a module,
or an instance of a generic one (`fnCallIsPathBase`), is a path rather than a
receiver and is left alone.
Macro call (only when `methfld` is NULL — with it set, the name is a receiver
and expands like any other value; a macro *method* named bare is first rewritten
to `self.name`); `<-` given a list of entries, an entry that is not a plain
value, or a construction's contents, which becomes the applications it stands
for (`contentsLower`, below, "The list after `<-`");
a managed reference type, `Rc[mut, Node]`, lowered into the `RefNode` it names
(`fnCallLowerManagedRef`, [references](references.md), "The managed reference
type") ahead of the struct-literal pass below, which would take its head for a
literal's struct; and a permission in the brackets of anything else, refused
(`fnCallRefusePermArg`). Then, for a member access by name that is not an operator, the **receiver is
checked ahead of the arguments** and its type asked what the name binds. **A
receiver that is a number type with the member `from`** — named directly,
through an alias, or as the argument a type parameter has in an instance — is a
conversion, lowered by `fnCallNumberFrom`: its one value checked with no
expected type (so an untyped literal keeps its `i32` default and is converted
from it), then the node retagged `TypeLitTag` with the number as its type,
which generation expands with `genlConvert`. No declaration of `from` exists;
the name is recognised on a number type. A wrong count of values, a value that
does not convert, or `from` named without a call is `ErrorNbrFrom`. Otherwise an
alias, for a macro method the type holds by folding, is resolved first, and the
receiver shifted to the field it was folded through (`structFoldReceiver`): a
macro method expands here, through `macroMethodTypeCheck`, with its arguments
still unchecked, as a macro's must be. A name bound to one method `FnDcl`, not
generic, gives its parameters after `self` as the arguments' expected types. So
does a function named directly, one `FnDcl` and not generic, whose name is then
checked ahead of the arguments too (a bare method name's first parameter is the
unwritten `self`), and a literal of a struct named directly and not generic,
each value expecting the field it will fill (`fnCallTypeLitField`). Then every
argument is checked, against that type where there is one, so an `if`, a block
or an array literal is coerced branch by branch as an initializer is; an
overload set's, an operator's and a generic's arguments get none, because their
types are what selects or infers the callee. **A closure literal among a
generic's or an overload set's arguments waits for the others** and is checked
after them (`fnCallClosureArgs`): its signature is what the other arguments leave
the callee's bound, or the overload its parameter count and written types pick
([closure](closure.md), "A closure given to a call"). A literal given to a parameter
that takes a borrowed callable, `&<fn(sig)`, is lent to it as a temporary of the
statement (`fnCallCheckClosureArg`). **A method name that is a field of the receiver's
type, with an argument list, is the call of what the field holds**: `t.profile(3.)`
becomes the call of the access `t.profile` (`fnCallFieldCall`), unless the field's
type cannot be called, which is `ErrorFldArgs`. Then generic substitution, which
may finish the node entirely.

**Stage 2 — make the callee knowable.**
Check `objfn`, *unless* it names an overload set — that one path deliberately
skips the ordinary name-use check, which leaves `nameUseTypeCheck` free to
reject an overload name everywhere else. Bail if `objfn` is already marked
`errorType`. Then rewrite the shapes that are not yet calls:

- **A type**, with `FlagIndex` → retag `TypeLitTag` and hand to
  `typeLitTypeCheck`, which builds a variant's literal, refuses a struct's
  (`ErrorStructBracket`, naming `new Point(...)`; at a `+` allocation's value,
  `ErrorPlusAlloc` instead), and refuses a number type, `u64[count]`, with
  `ErrorNbrBracket` naming `u64.from(...)`: its conversion is the method.
  Refused there, at type check, so a type reached through an alias or a type
  parameter is refused as one named directly.
- **A type**, with a member name → a path the collapse could not take, because
  the base is not a namespace until later: an alias, a number type's member
  other than `from`, a generic instance, a generic parameter. `ErrorUnkName`,
  naming what a path may pass through.
- **A type**, with neither, called `Point(1, 2)` → `ErrorInitCall`: a type is
  not called, its value is constructed with `new`.
- **A bare method or field name** (`FlagMethFld`, not `FlagQualified`) →
  rewrite to `self.method`, synthesizing a resolved `self` from parameter 0.
  Outside a method there is no `self`, so it is `ErrorUnkName`, "there is no
  self here to reach it through", as for a bare field name
  ([nameuse](nameuse.md), step 2): a static function's body, and a signature,
  which is checked with no function of its own around it. A member's name
  hides a module of the same name inside its type, so `mesh.Mesh` in a
  signature of a type with a method `mesh` arrives here, and `nameUseNoSelf`'s
  message names the two ways past it: import the module under another name, or
  reach it through the module's own name.
- **An overload set** → `fnCallLowerOverloadFn` type checks every candidate not
  yet analyzed (`fnCallDemandCandidates`, as a member name's are below), then
  picks the concrete candidate.
- **`FlagLvalOp`** → borrow the receiver as `&mut`, or hand an operator-assign
  on a method type to `fnCallOpAssgn`. A receiver that is already a reference
  (`fnCallIsRefReceiver`) is passed as it is, exactly as the reference arm of
  stage 3 takes a named method's receiver: its own permission is what candidate
  selection checks, not the permission of the binding that holds it. The `<-`
  list lowering holds such a receiver in its temporary unborrowed for the same
  reason.

  **An operator-assign is routed by what the receiver's type or its referent
  declares** (`fnCallOpAssgnMethodType`), so a reference to a method type goes
  to `fnCallOpAssgn` too, and stage 3's reference arm never sees it. That is
  what makes the derivation an operator-assign is entitled to — `a += b`
  rewritten to `a = a + b` where the type declares no `+=` — reachable through
  a reference as it is by value. `<-` is routed the same way (the parser flags
  it `FlagOpAssgn` too) but has no base operator to be rewritten to
  (`fnCallOpEqMethod` answers NULL), so a type that declares no `<-` is
  reported `ErrorNoMbr` under `<-`, as any missing operator method is.
  Stage 3's arm loses nothing by not seeing it:
  of the operator names, `refType` declares only the identity comparisons,
  `===` and `!==`.
- **`===` and `!==` on a receiver that is neither a reference, a slice, a
  virtual reference nor a pointer** → `ErrorSameNotRef`: identity asks about
  places, and it is not an operator a type declares, so the type's own
  namespace is never asked. `!==` is never derived from anything.
- **`!=` on a type that declares `==` and no `!=`** (`fnCallNeFromEq`) → the
  node is renamed to `==` and a `NotLogicTag` node takes its place in the
  tree, wrapping it; stage 3 then lowers the `==` like any other operator, and
  its answer is coerced to `bool` (through `isTrue` if need be). The type asked
  is a struct receiver's own, or the struct a reference refers to, through any
  number of references — `!=` on references compares the values, so
  `fnCallLowerRefCompare` lowers the renamed `==` exactly as it would a written
  one, and its diagnostics name `==`. A slice, directly or through references,
  has its `!=` derived the same way where its elements have a `==`, and so does
  an array, or a reference to one, compared with a slice; where the elements
  have none, the `!=` is refused under its own name. A pointer declares its own
  `!=`, on the pointer, and a virtual reference refuses it, so neither asks a
  referent. A type declaring its own `!=` keeps it, an enum's intrinsic pair
  is declared together, and a type declaring neither is reported missing its
  `!=`. A `==` that selects nothing is reported once, under `==`, and the `not`
  carries `errorType` on.

**Stage 3 — dispatch on the receiver's type tag.**

A `ULitTag` member is a number, not a name, and never reaches the lookups
below, which take one. `fnCallLowerRefIntField` takes it first: on a reference
or a pointer to a tuple it dereferences the receiver (`derefInject`) and lowers
the element with `fnCallLowerIntField`, so `r.0` is `(*r).0`, as `r.x` is
`(*r).x` for a struct; on a struct, a number, a slice, a virtual reference, or
a reference or pointer to anything but a tuple it is `ErrorNoMbr`. A tuple held
by value, an array and a function go on to the table's own rows.

| Receiver type | Goes to |
| --- | --- |
| `FnSigTag` | `fnCallFnSigTypeCheck` — a plain call |
| struct, number | fill in `()`/`[]`/`&[]` as `methfld` if absent, then `fnCallLowerMethod`; an index in set position on a type declaring `&[]` (`fnCallSetIndex`) takes `&[]` |
| `TTupleTag` | `fnCallLowerIntField` — element by literal index, its type read from the resolved tuple, since the receiver's `vtype` may be an alias naming it |
| `ArrayTag` | `fnCallArrIndex` under `FlagIndex`; a comparison with a slice to `fnCallArrayAsSlice`; else a method of core's `Array[T]` (`fnCallLowerSliceMethod`, below), `len` excepted |
| `ArrayRefTag` | index; `==`, `!=` or an ordering to `fnCallLowerSliceCompare`; else `fnCallLowerPtrMethod` against `arrayRefType`, else a method of core's `Array[T]` |
| `RefTag` | a key (`lifeIsKey`) refused for anything but `===` and `!==` (`ErrorKeyAccess`); else function-by-ref, array index, a comparison to `fnCallLowerRefCompare`, or `fnCallLowerPtrMethod`, then `fnCallLowerTraitMethod` and failing that `fnCallLowerMethod` (a reference to an array: a method of core's `Array[T]`) |
| `VirtRefTag` | fill in `()` as `methfld` if absent and not indexing, so `f(u)` calls the trait's `()` as ``f.`()`(u)`` does; `==`, `!=` or an ordering is `ErrorRefNoCompare`; else `fnCallLowerPtrMethod`, else set `FlagVDisp` and `fnCallLowerMethod`, whose selection (`fnSigViableCall`) takes only a method whose `self` permission the receiver's grants, and where `fnCallFinalizeArgs` lends an owning receiver as a borrowed virtual reference (`fnCallLendVirtOwner`) |
| `PtrTag` | the pointer's own operators first, then the value's fields and named methods |

**A name or an index reaches through every level of reference, before the
dispatch.** `fnCallReadThroughRefs` runs on the receiver as soon as it is
checked (twice in `fnCallTypeCheck`: after a member's receiver is checked, for
the parameters its arguments are wanted as, and after the callee is checked;
and in `fnCallMethodTypeArgs`). While the receiver is a `RefTag` (a borrow or an
owner) whose referent is a `RefTag`, `ArrayRefTag` or `VirtRefTag`, it injects a
dereference (`derefInject`), so `r.x`, `r.len()`, `r[i]` and `r.pick[T](v)` on a
`&&Pt`, `&&&Pt`, `&mut &Pt` or `&So[Pt]` see a receiver of one reference level,
exactly as the explicit `(**r).x` would. Not done for an operator (`FlagOperator`),
a call with no member name, or a borrowed or ranged index, whose receiver is the
borrow the parser put round it; and it stops at a key (`lifeIsKey`), a
lock-managed reference and a raw pointer, which keep the refusals they had. The
outermost level winning a name is that order: the only names a reference
declares are `refType`'s `===` and `!==`, operators, which this does not touch
and which `fnCallLowerPtrMethod(node, refType)` takes first; a struct's own
method is found on the struct before anything it holds (a fold cannot repeat a
name). **What it permits is what the explicit dereferences permit**: a borrow
read out of another keeps its own permission (`iexpLvalInfo` takes the last
step's), so `r.x = 9` through a `& &mut Pt` is allowed as `(**r).x = 9` is, and
refused through a `&mut &Pt` because the inner level is read-only. Only an owner
is held to the steps above it (a `&` lends only `&` of an owner,
`borrowOwnerLendRefused`): from the first step whose permission cannot write,
the dereference that reaches an owner is typed as a copy of the owner's type
with that permission (`clamp`), so a field stored or a `self &mut` candidate
through it is refused by the checks that already read the type. The `RefTag` branch's
`ownerread` retry remains for what this skips: an operator, or a call, on a
borrow of an owner.

**An array or a slice calls the methods core writes on `Array[T]`.** Neither
type declares methods (a slice's `len`, `maxlen`, `===` and `!==` are the
compiler's own, `arrayRefType`). A name those do not answer is looked up on the
instance of core's `Array[T]` for the element type (`fnCallSliceBodyOf`: a
`FnCallNode` for `Array[elem]`, type checked), and the call is lowered against
that struct (`fnCallLowerMethodOn`) with the array or slice as the receiver, as
the methods a list folds in from its `view()` are called. An array's `len` is
its type's, not a member of the value, and stays refused. A `self &mut` method
on an array borrows it mutably (`fnCallBorrowReceiver` takes an array).

**Text has three lowerings of its own, ahead of that dispatch.** `fnCallTextOf` asks
whether a receiver's type, through any references and owners, is `str` or a
struct that lends it (`structLentBody`). Then: (1) `==`, `!=` and the orderings
between two texts of different kinds replace each operand that lends by the call
of its lending method (`fnCallLentOperands`, `structLendView`), so the comparison
is `str`'s own operator on two borrows, and `String == So[str]` needs no operator
of either; (2) a borrowed range of text, `&s[a..b]`, `&s[a..]`, `&s[a...b]`, is a
call of the `slice`, `sliceFrom` or `sliceThrough` method (`fnCallLowerStrRange`,
asked once the receiver is checked, as the dispatch's first test of a range). The
receiver is the borrow `borrowReassocIndex` put around what is indexed; it is
dropped, the method borrowing its receiver itself, and what it borrowed is used
as it stands: the place, the reference where the place was reached through one
(the borrow's dereference is removed), or, for a temporary the borrow hid in a
local of the statement (`varDclTempValue`: the `s.view()` of `&s.view()[1..]`),
the expression it was, the local never declared. The methods refuse a bound
inside a character. A range of text not borrowed is `ErrorBadIndex`; (3) a byte index `s[i]` is the `[]` method of
`str`, found as any struct's is.

**A comparison on a reference compares what it refers to.** A reference reads
as its value everywhere else — `r.x`, `r.method()` — so `==`, `!=` and the four
orderings do too, and `===`/`!==` are what ask whether two references point to
the same place; `refType` and `arrayRefType` declare only those two, as
`EqIntrinsic` and `NeIntrinsic` on the address (on both words, for a slice).
Identity is selected by `iNsTypeFindPtrMethod`, which wants the two operands of
the same type, permission included, so `&i32 === &mut i32` is refused as no
candidate. `fnCallLowerRefCompare`:

- **One side a reference and the other a value** is read through only for a
  borrow of a value that copies freely (`fnCallBorrowTypeReadsThrough`: below),
  so `x > 1` and `x == v` compare the value the borrow lends, as `*x > 1`
  does. For any other referent it is `ErrorRefCompareMixed`, rather than read
  through on one side only, so `r == v` never says something `*r == v` does not
  (and a value is never moved out of a borrow to compare it). The mirror, a
  value on the left and a reference on the right, reaches the value's own
  operator, where the argument is read through by the same rule
  (`fnCallLowerMethod`) and otherwise refused as no candidate. A reference to
  an array with a slice on the right is the exception: it is compared as the
  slice it converts to (`fnCallArrayAsSlice`). A derived `!=` follows the same
  line (`fnCallNeFromEq`).
- **A referent that is a pointer, a reference or a slice is read through** on
  both sides, and the result compared as it would be by value: a pointer by its
  own operators, a reference by this same function again, a slice by
  `fnCallLowerSliceCompare`. The exception is text: an owner of it,
  `&So[str]` or `&Rc[str]`, whose referent is a reference, against a `&str`,
  whose referent is `str` itself, is read through on the owner's side only,
  and the comparison goes on between `So[str]` and `&str`. That is what
  `&str == &So[str]` already does by the `str` operator's taking the owner as
  its other side, so text compares alike in either order.
- **A referent whose type declares the operator** is asked first with the
  operands as written, so a method declared for references (`self &`,
  `other &T`) takes them unchanged. Only when no candidate matches are both
  operands dereferenced (`derefInject`, positioned on the comparison) and the
  value's operator selected by `fnCallLowerMethod`, as for `*a == *b`. An
  enum's compiler-declared `==` is reached that way, and so is its refusal,
  `ErrorEnumEquality`, for one with a variant that carries fields and declares
  no `==`.
- **Anything else is `ErrorRefNoCompare`**, whose message names `===` for `==`
  and `!=`: a referent with no such operator, a referent with no methods at all
  (an array, a function), and a trait other than an enum, whose comparison would
  be dispatched on the variant and is not built. A virtual reference is refused
  the same way in its own arm.

**A comparison of two slices compares their elements**
(`fnCallLowerSliceCompare`): equal when the counts are and each element is `==`
to its partner. A slice has no order, so an ordering is `ErrorRefNoCompare`.
The comparison is core's `mem.sliceEq[T](a &Array[T], b &Array[T]) bool`, a generic
function whose body is the loop: the node becomes a call of its instance at
the receiver's element type (`genericMethodInstance`), the receiver its first
argument, and `fnCallFinalizeArgs` converts the other side to that slice as it
does any slice argument, so an array, a reference to one or a string literal
is compared too; an array, a reference to one or a `&str` (a string literal is
one) on the left is converted the same way first (`fnCallArrayAsSlice`). A value pattern on a slice, `case
"box"`, is the same `==` (`castMatchValueTypeCheck`). The function is found
by its name and its package, as core's `TypeRecord` is: `sliceEqDclNameRes`
remembers its declaration when it is name resolved, held to that one
signature, and `sliceEqFn` hands it out.

The body compares each pair as `&a[i] == &b[i]`, which is a comparison of
references, so every element type gets the `==` this function already selects:
a number's or a `bool`'s built-in one (a float's IEEE `==`, so a NaN is unequal
to everything and -0.0 equals 0.0), a pointer's on the address, one a struct
declares on the value or on references, an enum's, and through a
reference or a nested slice, what it refers to. Nothing is copied. A body in
Cone rather than a loop generated of its own is what reuses that selection; it
is generated once per element type in each module that compares, an ordinary
generic instance, not expanded at each comparison. No element type is
compared with `memcmp`: equality is bitwise only for some of them (not a
float's, nor a struct's with padding, nor one whose `==` asks less than every
byte), and one loop serves them all.

Whether the elements can be compared is decided where the slices are, by
`fnCallSliceElemNoEq`, which asks what `fnCallLowerRefCompare` would of the
pair, so a refusal is `ErrorRefNoCompare` naming the element type rather than
an error reported inside core: a struct declaring no `==`, an enum with a
variant that carries fields and declares none, an array (whose comparison is not
built), a trait, a virtual reference.

**`==` on an enum with a variant that carries fields is a call of the enum's
`-eq`** (`fnCallLowerEnumEq`, reached from `fnCallLowerMethod` where the selected
`==` is the enum's `NoEqIntrinsic`): the tags are compared and then the variant's
own `==` ([struct](struct.md), step 8a, for how `-eq` is made). Each operand is
lent to it read-only where it lies (`fnCallLendEnumOperand`: a place is borrowed,
a temporary borrowed to the end of its statement, a reference passed as it is),
so an enum that moves is compared as one that copies is and nothing is moved into
the comparison; the node becomes a call of `-eq` with those two arguments. `!=`
is `not (a == b)`, as for any type declaring only `==` (`fnCallNeFromEq`). Where
no `-eq` was made, because a variant carries fields and declares no `==`, the
call is refused with `ErrorEnumEquality`, naming the first such variant and its
fields.

The permission a reference carries is enforced on the dereference, by flow, so
`==` through an `opaq` reference is `ErrorNoRead` while `===` on it is allowed.

**A raw pointer is the exception**: its operators are on the pointer (see the
deref retry below), so its `==` already asks about places, and `ptrType`
declares `===` and `!==` as synonyms for its `==` and `!=`.

**A method called on a plain reference to a trait dispatches on the variant**,
and `fnCallLowerTraitMethod` is what routes it there. Neither of the trait's own
declarations is callable — an abstract method has no body, and one with a body is
a default that was cloned into each variant — so selecting either left the call
naming a declaration with no symbol, which generation dereferenced as a null.
The route is the one [reftraitvar](../../../../doc/reference/reftraitvar.html)
describes: the tag says which variant, that selects its vtable, and the vtable
holds the method. It is built by coercing the receiver to `&<Trait`, which
already exists and already does the tag lookup, and then dispatching as any
virtual reference does — the compiler writing what a caller could write by hand.

**An open trait has no tag**, so `refvirtMatches` refuses that coercion, and the
refusal is reported rather than left to fail later. The same page states the rule
and the remedy: obtain a virtual reference first. **A field takes none of this**
— it lives in the trait's own layout, a prefix of every implementer, so
`fnCallLowerMethod` reaches it directly.

**A method called on a value typed as an enum is refused**, `ErrorEnumValueDispatch`,
in `fnCallLowerMethod` once a candidate is selected: a method of a trait reached
without `FlagVDisp`, other than an intrinsic such as a payload-free enum's `==`.
Only a by-value `self` can be called on a value, and which variant's clone runs
would have to be read from the tag at run time, which is built for a reference and
not for a value. Before, the call named the enum's own declaration, which is never
generated, and only LLVM's verifier or the linker caught it. On a variant's value
the call reaches that variant's clone and is fine.

### Selecting a candidate

`fnCallLowerMethod`: look the name up in the receiver's namespace, check
the visibility of **the binding the name reaches** (`inodeIsPrivate`), resolve
an alias to the method it stands for (`aliasDclResolve` — the binding for a
method the type holds by folding), **type check every candidate not yet
analyzed** (`fnCallDemandCandidates` — a later method of the caller's own type
is not, and its unchecked signature matched no reference receiver; see
[type check](../phases/type-check.md), "Demand"), then `iNsTypeFindMethod`,
which tests every candidate with `fnSigViableCall` and **alters nothing**. One
viable candidate is a match; two are `OverloadAmbiguous`. There is no ranking,
bar one fallback: a string literal lent as a temporary to a `&T`
([literals](literals.md)) is counted only when no candidate took the arguments
without it.
For a folded method the receiver is rewritten before any candidate is tried:
`structFoldReceiver` makes it the access to the field the name was folded
through, reborrowed with a reference receiver's permission, so selection,
borrowing and the permission checks see the receiver the method was declared
for. [struct](struct.md), "Name folding", has the rule. A name the receiver's
type does not bind is `ErrorNoMbr`, unless the type is an instance of a generic
type that has the member where its `where` clause holds: then it is
`ErrorWhereAbsent`, naming the clause (`genericReportAbsent`;
[generic](generic.md), "Constraints").

**A generic method named alone is selected as its instance.** A generic method
bound by more than function signatures may not declare an overload name, so a
name binding one binds it alone, and the candidate tried is the instance: the one already bound to the member — by
`fnCallMethodTypeArgs` for written type arguments, or by substitution for a
bare `pick[i32](6)` that became `self.pick` — which `genericIsInstanceOf`
recognizes in the generic's memo, else the one the call's arguments infer
(`genericMethodInstance`). Inference matches the arguments against the
parameters after `self`, since the receiver is not among them yet, so the
receiver tells it nothing. The instance is then selected like any method, and
the receiver dereferenced or borrowed to fit its `self`.

**A generic in an overload set is selected by its arguments, and instantiated
after.** Only a generic whose every type parameter is bound to a function
signature may name a set (`parseGenericOverloadVet`; any other is
`ErrorGenericOverload`). Its signature is never type checked, so
`iNsTypeFindMethodPass` asks `genericOverloadViable` in place of
`fnSigViableCall`: each parameter that is, or is a reference to, a type parameter
is given the type of its argument by `genericInferType` and the bound is
evaluated at it (`genericUnmetCondition`), and every other parameter is a copy of
its written type, checked, that the argument must coerce to. A closure literal
not yet checked is taken to fit its parameter, and what the others give is all the
test has (`fnCallClosureOverload` asks it too, so another argument can tell two
generic candidates apart before the literal is checked). Nothing is instantiated by
the test. The one candidate selected is then instantiated from the call's
arguments: a method by `genericMethodInstance` in `fnCallLowerMethod`, just
before the receiver is inserted, and a function by `genericSubstitute` in
`fnCallLowerOverloadFn`, which answers 0 so `fnCallTypeCheck` carries on with
the instance as an ordinary callee. Two viable candidates are
`OverloadAmbiguous`, and `iNsTypeAmbiguous` holds the first two for the error to
name.

Then the node is rewritten: the receiver is inserted at `args[0]`, `methfld`'s
name-use node is repurposed into `objfn` pointing at the selected function,
`methfld` becomes NULL, `vtype` becomes the signature's return type, and
`fnCallFinalizeArgs` coerces each argument to its parameter and appends
defaults. **Coercion happens once, after selection** — which is what lets
selection be a pure filter.

A field, rather than a method, retags the node `FldAccessTag` and injects a
deref on the receiver if needed. A folded copy of a field is reached through
the field it was folded through: `fnCallFieldAccess` makes the receiver the
access to that field — an access per hop, root first — and this node the
access to the copy on it, the nesting `p.left.fuel` written out produces. The
copy carries the index, type and permission of the field it stands for, so
generation and the permission read treat the last access as one to that field.

A private member (one not declared `pub`) is granted to a receiver that is the enclosing
method's own `self`, and to an access that a macro method's body wrote on *its*
`self` — the clone carries `FlagSelfRecv`, stamped by `cloneFnCallNode` at
expansion, since by then the receiver is the use site's expression. Any other
receiver is granted it when the function being checked is written in the module
that declares the receiver's type — `dclInfoGetModule` of `pstate->fn` and of the
type agree — or in an extension of the receiver's enum (`structSeesPrivate`,
`structEnumSeesPrivate`). Every other receiver gets `ErrorNotPublic`. An `isTrue`
a coercion injects is lowered with no state (`iexpCoerce` passes none), so only
`self` reaches a private one.

⚠ **Visible is not reachable through a vtable.** A call or field access with
`FlagVDisp` reads the member's slot, and a private member has none
([struct](struct.md), `structMakeVtable`), so `fnCallPrivateVtable` refuses it
with `ErrorPrivateVtable`, naming the member and the trait: the selected
candidate's privacy for a method, since a public overload name may select a
private one. It fires only where visibility let the name through, in the trait's
own module; a member already refused as `ErrorNotPublic` is not reported twice.
The call is lowered as usual after the report, so its type is the member's and
nothing around it reports a consequence; generation, which would index the
vtable with the unset `vtblidx`, never runs.

Three adjustments, two of them asymmetric on purpose:

- **The deref retry.** A receiver held through a reference still satisfies a
  method declaring `self` by value: `derefInject`, then select again. It runs
  *only* when no candidate matched at all, so a real ambiguity is still an
  ambiguity. **An operator's one argument is read through the same way**, when
  it is a borrow of a value that copies freely (`fnCallBorrowReadsThrough`: a
  `RefTag` whose region is the borrow region, not a key, not a lock
  permission, not fat, and whose referent is not `MoveType` and is a method
  type, other than a trait that is not an enum). The attempts, each only after
  the one before found no candidate: the receiver read through; receiver and
  argument both; the argument alone, which an operator-assign needs, its
  receiver being the `&mut` it takes; so `total + x`, `x + y`, `sum += x` and
  `3 <= x` select the value's operator. A candidate declared for references
  matches the operands as written before any of this, and a type that moves is
  never read through, so a borrowed `String` is refused where it was. An
  argument that is not a borrow of such a value, and an index (`FlagIndex`,
  which is not an operator), are left alone.
- **The borrow retry** (`fnCallBorrowReceiver`). A receiver held as a value
  reaches a method declaring `self &` or `self &mut`: it is probed as a `ro`
  borrow, then (for a receiver whose type declares `Immutable`, whose `self &`
  is `&imm`) an `imm` one, then a `mut` one, and the first that selects a
  candidate is made by `borrowMutRef`, so the permission check and the lifetime are a written
  `&mut v`'s — `ErrorBadPerm` for `&mut` of an immutable variable, and the
  borrow's scope carried into a returned borrow. It runs only after both
  selections above found nothing, so a by-value candidate is always preferred.
  A temporary is borrowed where it is (`borrowTempRef`), with the block's
  lifetime (`borrowTempScope`), and lives to its statement's end, as `&` of it
  does: `mk().get()`. **A pointer is never borrowed from** — a pointer receiver,
  and a dereference of one written out, are left as the deref retry left them.
  An ambiguity among the probed candidates is reported as one. **An owner
  receiver is borrowed too** (a `So[T]`, `Rc[T]`, `Arc[T]`, any reference that
  is not a borrow): it is a reference and so passed as it is, but any type's
  method may declare `self &So[Self]` (or `&Rc[Self]`, `&Arc[Self]`, ...), a
  borrow of an owner of its type — core's `str.cstr`, which a plain `&str`
  therefore has no candidate for, is one — and that is what the probe's borrow of
  the owner finds. The form is general: no owner kind is named in the compiler.
  `fnDclTypeCheck` allows it: a method's `self` is its type, a reference to it,
  or a borrow of an owner of it. Inside such a method `self` is a borrow of an
  owner, a reference to a reference, and a field or method named on it is
  read through to the value: `fnCallReadThroughRefs` dereferences it before the
  dispatch (a name), and the RefTag branch of `fnCallTypeCheck` once
  (`ownerread`) for an operator.
  Pinned by `typemgmt_owner_self` and `typemgmt_typecheck_owner_self`.
- **An operator on a pointer does not reach through.** `p + 2` offsets the
  pointer; `p * 2` is an error rather than becoming `(*p) * 2`. `FlagOperator`
  on a pointer receiver is what skips the retry, argument included. A
  reference's comparison is the other way round, reading through both
  operands, which `fnCallLowerRefCompare` does before `fnCallLowerMethod` is
  reached.

**An index in set position.** `x[i] = v`, `x[i].f = v` and an operator
changing `x[i]` or a field of it in place write to the element, so where
`x`'s type declares `&[]` the index is lowered as `&mut x[i]` is: the
assignment, or the `FlagLvalOp` call, marks the index at the root of the
place (`fnCallSetIndexRoot`, through field reads only) in `fnCallSetIndex`,
`fnCallTypeCheck` sets `FlagBorrow` on that node and names `&[]`, and an
assignment to the index itself stores through the reference it returns
(`derefInject`). A method called on `x[i]` that no candidate takes with the
read-only element is retried with the element `&[]` lends, the index lowered
again from its checked receiver and arguments, where the receiver may be
borrowed mutably (`fnCallIndexAsMut`). Rust chooses `IndexMut` the same way;
a dynamic arena's `ar[key] = v` is the case it was built for.

When nothing is selected, `fnCallNoCandidate` reports it. Two cases have a
message of their own. An indexed borrow `&x[i]` reaches `` `&[]` `` with the
read-only receiver `&x`, and where a `&mut` receiver would have been accepted
the message says the method takes `self &mut` and names `x[i]` and
`&mut x[i]` (`fnCallRefIndexWantsMut`, still `ErrorNoCandidate`). An operator
or an index given a `bool` where a candidate declares a number, `n + b` or
`list[b]`, is `ErrorBoolNotNbr` naming that number's `from`
(`fnCallBoolOperandWantsNumber`): a `bool` coerces to no number. The same probe
reports a `char` where a candidate declares a number, a number where it declares
a `char`, and a character literal of 128 or more where it declares a `u8`, as
`ErrorCharNotNbr` (`iexpCharNumberMismatch`). Neither probe changes anything;
the refusal is the same. Before any candidate is tried, a character literal that
is a binary operator's receiver, beside a `u8` argument, is retyped `u8`
(`litAdoptCharBesideByte`), so `'0' + digit` selects the byte's `+`.

### The list after `<-`

`contentsLower` (`ir/exp/contents.c`) takes apart a `<-` whose argument is a
tuple of entries or an `EntryNode`, or whose receiver is a construction,
`new` or `trynew` (`contentsIsAppend`). A construction's type is checked
first, unless it is a generic struct named bare, whose arguments infer it
(`contentsBuiltType`), since an array, an allocation of one, or anything else
lowers differently. `EntryNode` is one struct for the entry forms, the
tag telling them apart: `first` holds `n` or `k` (NULL for `fill`), `val` the
value. Its `first` is an expression resolved like any other, which a
`NamedValNode`'s name is not, so it is not that node. An `each` entry
(`EachEntryTag`) holds in `first` the loop `parseEach` built (below), and a
`YieldEntryTag` entry is that loop's last statement.

- **On a value**, the receiver is checked, borrowed `&mut` once (held as it is
  when it is already a reference) into a temporary, and each entry becomes
  statements of one block against that borrow: a value, one application
  `*recv <- value`; `n of x`, a block counting `n` (checked here as a `usize`,
  a constant one 0 through 4294967295, `ErrorRepeatCount`) with `x`'s
  application inside the loop, so it is evaluated once per value; `fill x`, a
  loop appending `x` until `(*recv).len() >= (*recv).capacity()`, which the
  receiver's type must declare (`ErrorFillSize`); `k: v`, one two-argument
  application, which needs a `<-` of two parameters after `self`
  (`ErrorPairAppend`). The loops are built after name resolution, so their
  `break` is joined to its loop here, and their variables carry the scope the
  block will give them. A tuple value appended is split again by the same
  path, so a tuple is never one value of a `<-` list.
  An `each` entry (`contentsEach`) becomes the loop's block itself, a statement
  beside the others. Its loop is the ordinary `each` of the parser
  (`parseEachLoop`): the source in a hidden variable, the pass variables, any
  `if` filter, and for a body the one statement, a `YieldEntryTag` entry whose
  `val` is the `yield`ed expression (`first` its key, for `yield k: v`) or, for a
  drain `each src`, a use of the pass variable a hidden name (`-item`) gives, with
  `drain` set. `eachLower` builds the loop as it does any other. The entry is
  lowered when the loop's body is checked, in `yieldEntryLower`, which finds
  the receiver (`recv`, set by `contentsEach`) and the pass variables' types:
  `*recv <- v`, `*recv <- (k, v)` for a pair (`ErrorPairAppend` when the
  collection's `<-` takes no pair), and for a drain `*recv <- *item` when the
  item is a borrow (a type that moves is `ErrorDrainItem`: a source that hands
  its items over, `drain()`, gives owned items, which are appended as they are).
  The receiver of a list holding an `each` is a variable like any other
  (`-recv`) rather than the operator's temporary, because flow holds a borrow
  across a loop's passes only in a variable that is a holder (`flowpath.c`,
  `pv->holder`); that is what refuses `xs <- each xs` (`ErrorFrozen`) as it does
  `each x in xs { xs.push(*x) }`. Name resolution numbers the loop's
  variables as if the blocks the lowering adds were already there
  (`EntryNode.nest`, set by `parseEntryNest`: 1, or 2 after a construction,
  which is first held in a variable of its own), so the scopes of the loop's
  variables agree with the ones `eachLower` gives. An array's contents refuse
  an `each` (`ErrorArrayContents`), whose count is not known.
- **After a construction**, the construction becomes a variable the contents
  are appended to, and the block's value: `{ mut built = new T(...); built <-
  contents; built }`, so the construction fills the variable in place. An
  allocation of a collection is this form too, the variable its reference and
  the contents appended through it.
- **After an array's construction**, `new Array[T, n] <- ...`, there is no
  `<-` to call, so the contents become an `ArrayLitTag` node typed as the
  array (`contentsLowerArray`). Every count is a constant, `fill` given what
  is left, and they add to the size exactly (`ErrorArrayContents`; a pair is
  `ErrorPairAppend`). A repeated value is copied, unchecked, once per element
  (`contentsCopy`) and each copy checked against the element type, so it is
  evaluated once per element and the move rule applies to each. Past
  `ArrayRepeatUnroll` (16) copies there are two, the second standing for every
  element after the first (the literal's `repeats`), which generation fills in
  a loop evaluating it once for each: a later copy would be checked and walked
  by flow exactly as the second is, so the second asks all of them would. One
  entry repeating a constant becomes the literal's fill form, one value stored
  into every element. Refused contents leave an error node.
  For an array type written with several sizes (its
  outermost node's `nsizes`, `arrayTypeLower`) the entries are its scalars,
  row-major: the size is the product of the sizes (at most 4294967295,
  `ErrorArrayContents`), each value is checked against the element type
  written first (a row given is `ErrorInvType`), and the literal records the
  levels its elements fill (its own `nsizes`), so generation stores them into
  the array seen as one run of scalars (`contentsArrayShape`). The nested
  spelling has no `nsizes`, so its entries are its rows. Several sizes over an
  element type that is itself an array, `Array[Array[u8, 4], 2, 3]`, mix the
  spellings, and take no contents (`ErrorArrayContents`). A generic's type
  argument reaches its instance without its shape (`arrayTypeUnshaped`, at
  the clone's substitution), since one instance serves both spellings of a
  type: through a parameter the entries are rows.
- **After an allocation of an array**, `new Rc[mut, Array[T, n]] <- ...`, the
  contents become the same literal, typed as the value type, and the
  allocation's value (`contentsLowerAlloc`, `typeLitNewFilled`), flagged
  `FlagAllocFill`: generation fills it in place after the region's and the
  permission's inits rather than evaluating it before `alloc`
  ([references](references.md), "Allocation"). `trynew` is taken the same way,
  so the contents are evaluated only on `Some`.
- **After any other `trynew`**, the contents would be appended through the
  `Option` it gives, which is not built: the construction is checked (so a
  `trynew` of a value type is its own `ErrorTryNewValue`), then the `<-` is
  refused once, `ErrorTryNewContents`, naming the spelling that works,
  `trynew` alone and an append on `Some`, and left an error node
  (`contentsRefuseTryNew`).

An `EntryNode` reached by `entryTypeCheck` sat where no `<-` took it apart,
only possible inside a tuple used as a value: `ErrorEntryPlace`. (A yield is the
exception: it is reached as its loop's statement and lowers there.)

### Construction

A construction by a declared init stays an `FnCallTag` flagged `FlagNew`: its
`objfn` the init's name use, its `args` the arguments after `self` (no node
stands for `self`), coerced and with the defaults appended by
`typeLitNewCheck`'s own pass rather than `fnCallFinalizeArgs`, which counts
from the first parameter, and its `vtype` the struct, which is the value the
construction has, though the init returns nothing. **An init is called by no
other path**: `fnCallFinalizeArgs` refuses every call whose callee is one
(`ErrorInitCall`), since called on a value it would write over one without
finalizing it. A value receiver never reaches it anyway -- nothing but an
init's own `self` has the `new` permission its `self` takes -- so what is
refused there is an init called on `self` inside another init.

### What the node becomes

`FnCallTag` (a real call, or a construction by a declared init), `FldAccessTag`, `ArrIndexTag`, `TypeLitTag`, a
generic instance, a macro expansion, a block of applications, or — for a
derived `!=` — a call wrapped in a `not` that replaces it in the tree. Anything after
type check that still sees an un-lowered `FnCallTag` with `methfld` set is
looking at a bug.

## Flow

Three entry points, by what the node became:

- `fnCallFlow` — first, when `objfn` names a variable (a function reference
  called through), `nameuseFlow` on it, so one never given a value is refused;
  then for each argument: `flowLoadValue`, then
  `flowHandleMoveOrCopy`. Arguments are moved or copied into the callee. A
  method's receiver that is an init's `self &new` is walked as a use through
  it (`flowNewSelfThrough`; [Flow](../phases/flow.md), "An init's self"). A
  construction by a declared init is walked as any call: its arguments; the
  value it yields is the call's.
- `fnCallArrIndexFlow` — the receiver, through `flowLoadThroughRef` because a
  reference to a fixed-size array and a slice are indexed with no dereference
  injected, and the index.
- `fnCallFldAccessFlow` — the receiver only, also through `flowLoadThroughRef`:
  a plain reference's field access had a dereference injected and `derefFlow`
  reads through that, but a virtual reference's did not, so this is where its
  `MayRead` is asked.

**`fnCallFlow` skips a call whose type is the error type.** Such a call
passed its function's flow gate, because what failed was reported elsewhere —
a field of a value returned by a function whose signature failed — but type
check gave up on it unlowered, so a field access still has no arguments.

## Generation

**A construction by a declared init is `genlNew`**: its arguments evaluated,
then the init called with the memory to fill as its first argument -- the
variable a declaration gives it (`genlLocalVar`), the allocation's value
(`genlallocref`), both through `genlNewInto`, or else a temporary alloca,
loaded afterward as the construction's value. A value holding traced
references is zeroed there before the init runs, and a temporary is a root.

`genlFnCall` evaluates every argument, then `genlFnCallInternal` picks: a call
through a deref, an indirect call through a reference or pointer value, virtual
dispatch (extract the object and vtable from the fat pointer, `structgep` the
slot, load, call), generator-level inlining for `FlagInline`, an ordinary call,
or an intrinsic. An atomic intrinsic is taken before that, by
`genlAtomicIntrinsic`, which reads its orderings from the call's own arguments;
`fnCallFinalizeArgs` has already held them to constants the operation allows
([intrinsic](intrinsic.md)).

**The switch for the intrinsics built in C dispatches on the LLVM type kind of
argument 0**, not on the Cone type — so a mutating intrinsic's receiver, which
arrives as an lvalue pointer, and a non-mutating one's, which arrives as a value,
land in the same branch and are told apart only by which intrinsic it is. What
the pointer points at — a number to add to, or a pointer to step and by what — is
read from argument 0's Cone type, which `genlFnCall` passes down as `selftype`,
as is the trait whose vtable a virtual dispatch reads. An
intrinsic declared in core with `@intrinsic` is taken first, by
`genlDeclaredIntrinsic`, and decided by its kind and the Cone type its instance
carries ([intrinsic](intrinsic.md)).

`FldAccessTag` splits on `FlagBorrow`: with it, `StructGEP` the receiver's
address; without it, load the **whole aggregate** and `extractvalue`. Getting
the flag wrong is not a type error.

`ArrIndexTag` splits the same way: without `FlagBorrow`, load the element
from its address; with it, the address is the value. **A borrowed index's
receiver is a reference, but the borrow node only at the root of the chain.**
`borrowReassocIndex` turns `&m[1][0]` into `((&m)[1])[0]`, each link
`FlagBorrow`: the inner link's receiver is the borrow `&m`, which generation
steps around to index `m`'s own place, while the outer link's receiver is the
inner link, a reference value holding the row's address, which `genlAddr`
and `genlSubslice` index through as through any reference to an array or
slice. The same holds where the receiver is a reference some call returned,
`&mut list[i][j]` for a list of arrays.

**A range index** (`FlagRange`, set by the parser's `parseIndexArgs`) is a slice
of part of an array or a slice, and exists only borrowed: `fnCallTypeCheck`
refuses it on any other receiver before a `[]` method could take its two ends
for two indices (`ErrorBadIndex`, naming `mem.sliceFromParts` for a pointer),
and `fnCallArrIndex` refuses it unborrowed, where it would copy or fill a
segment. Borrowed, its type is an `ArrayRefTag` slice with the permission and
scope an element's borrow gets, and `genlSubslice` builds `{&x[start], end -
start}` after checking `start <= end <= count` at run time (`end` is the count
when the range runs to the end, and one past what was written for `...`).

## Hazards

- **`methfld` is NULL after lowering.** Code written against the parsed shape
  breaks on the lowered one, and both exist during type check.
- **The node's tag is not stable.** It may become `FldAccessTag`, `ArrIndexTag`
  or `TypeLitTag` — all still `FnCallNode` structurally.
- **`args[0]` is the receiver after selection, and was not before.** Anything
  iterating arguments has to know which side of lowering it is on.
- **`FlagOperator` is the only record of what the source wrote.** Lose it and
  `p * 2` silently starts dereferencing.
- **One dispatch arm skips resolving the callee** — the overload-set path — so
  the invariant "objfn is type checked by stage 3" holds in most of the function
  and not all of it.
- **`fnCallLowerMethod` returns three values** — 1 lowered, 0 receiver has no
  methods so try another way, −1 already reported. Treating it as a boolean
  produces a duplicate diagnostic.
- **`fnCallArrIndex` must resolve the receiver's type the way its caller did.**
  It is reached only from the array, slice, reference and pointer arms above,
  and its own switch has to agree with that decision. Reading a reference's
  pointee tag raw rather than through `itypeGetTypeDcl` was the bug: `&Alias` to
  an alias of an array matched neither the array nor the slice arm, so a valid
  index kept `unknownType` and surfaced as a return-type mismatch elsewhere.

## What lives elsewhere

- Overload selection, coercion, and the verdict vocabulary: [Type Check Reasoning](../phases/type-check-reasoning.md), "Calls, methods and overloads"
- Why `methfld` cannot be resolved earlier: [Name Resolution](../phases/name-resolution.md), "Where it stops, and why"
- How the parser builds all these shapes: [Parse](../phases/parse.md)
