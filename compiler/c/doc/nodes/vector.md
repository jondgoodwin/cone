`VectorNode` is the SIMD vector type, `Vector[T, N]`: N lanes of one number type, lowered to
LLVM's `<N x T>`. It is a type of the compiler's own, as the array type is, and unlike it a
type that **has methods**: its operators and lane methods are made when the type is checked,
as intrinsics, and its conversions and construction are type literals that generation
expands. There is no `Vector` struct in Cone source (a number parameter on a generic type is not
built), so everything about it lives in two files, `ir/types/vector.c` and
`genllvm/genlvector.c`.

**At a glance.** Name resolution lowers the bracketed call on `Vector` into the node
(`vectorTypeLower`). Type check checks the lane type and the count, makes the mask the type's
comparisons give, and fills the namespace with the operators its lane type allows
(`vectorTypeCheck`, `vectorMethods`). Method selection finds them as it finds a number's. Four
things are not a method and are lowered in `fncall.c`: a number beside a vector, `.from`,
`select`, and an index. Generation expands each intrinsic on the whole `<N x T>`.

*Provenance: read from source and measured, October 2026.*

## Shape

| Field | Meaning |
| --- | --- |
| `dimens` | one node, the number of lanes: a `ULitNode` once checked (1 through `VectorMaxLanes`, 64), a use of a generic's value parameter in a template |
| `elems` | one node, the lane type: an integer or float type, or `bool` |
| `mask` | what a comparison of these lanes gives: a `Vector[bool, N]` of its own, or the vector itself when it is one. NULL until checked, and **NULL for a vector refused** (a lane type or count that was not allowed), which is how later code knows it was reported |
| namespace, `nodelist` | an `INsTypeNode`: the operators and methods `vectorMethods` made. Each node has its own; none is shared |

`isMethodType` is true of the tag (`nodeTagFacts`: a type, no name, supports methods), and
`isNamedNode` false. `inodeGetName` answers `Vector` for diagnostics. Type identity is structural
(`vectorEqual`: the same lane type and count, as `arrayEqual` is), so two written `Vector[f32, 4]` are
one type, and a vector is a copy type with no flags of its own: the lane type is a number.

## Constructors

- `newVectorNode`: empty, for `vectorTypeLower` and `cloneVectorNode`.
- `cloneVectorNode`: a clone made for a generic's instance has **no namespace of its own copied
  from the template's**: `iNsTypeInit` makes a fresh one, the checked flags are cleared, and the
  clone is checked as a new type is. Copying the dictionary would have made every instance share
  its template's methods and mask.
- `vectorNewConvert`: a `TypeLitTag` node whose type is the vector (or an array) and whose
  arguments are what generation converts: see below.

## Parse

None of its own. `Vector[f32, 4]` parses as an index of the name `Vector` by two arguments, the call
shape every generic is written in.

## Name resolution

`Vector` is a name every module reaches (`stdlibInit` binds `vectorTypeDcl`, a struct with no
fields and the flags of an opaque declaration, as it does `Array`), so a module cannot declare its
own. `fnCallNameRes` hands the call to `vectorTypeLower` when its head names it, which builds the
`VectorNode` from the lane type and the count and refuses any other shape (`ErrorVectorArgs`: one
argument, three, a count first, a bare number). `Vector` alone, with no brackets, is refused where
it is checked as a type (`nameUseTypeCheckType`, the same code).

## Type check

`vectorTypeCheck` refuses a count that is not an integer literal from 1 through 64
(`ErrorVectorLanes`) and a lane type that is not an integer, float or `bool` type (`ErrorVectorElem`;
`char` is a code point, not a number), then builds the mask and the methods. An error leaves `mask`
NULL and the namespace empty, and the contents of a construction of that type say nothing more.

`vectorMethods` gives a vector the methods its lane type has:

| Lanes | Operators and methods |
| --- | --- |
| float | `+ - * / %`, unary `-`, `== != < <= > >=`, `sqrt sin cos`, `floor ceil round fma`, `abs`, `min max` (lane-wise), `sum product min max` (reductions) |
| integer | the above less `sqrt sin cos floor ceil round fma` (and `abs` for an unsigned lane), and `~ & \| ^ << >>` |
| `bool` (a mask) | `~ & \| ^ == !=`, `any all` |

Each is a `FnDclNode` of `FlagMethFld | FlagPub` whose value is the same `IntrinsicNode` the
scalar has (`AddIntrinsic`, `SDivIntrinsic` for a signed lane, `SLtIntrinsic`, and so on) or one of
the reductions and mask tests (`VecSumIntrinsic` ... `VecAllIntrinsic`, built in C beside the number
types' rather than declared in `core`, because the type has no Cone declaration to hang a generic
over). `-` is two overloads, as a number's. `abs`, `min` and `max` with one argument, `floor`, `ceil`,
`round` and `fma` are not Vec* kinds: they are the *declared* number intrinsics (`AbsIntrinsic` ...
`FmaIntrinsic`, [intrinsic](intrinsic.md)) with the vector type as the node's `typearg`
(`vecAddLanewise`), so `genlNumberIntrinsic` lowers them for a number and a vector alike. **New vector
maths goes this way**: a declared number intrinsic given to the vector by `vecAddLanewise`, not a new
Vec* kind. `intrinsicDclTypeCheck` never sees that vector `typearg` (it is set on a method, not
inferred at a call of the declared intrinsic), so no check runs there: a number of the wrong type
beside a vector is caught by `vectorBroadcastOperand` instead (`ErrorVectorMixed`, 1555). `fma`'s
parameters are named `y` and `z` on a vector as on a number. `min` and
`max` are an **overload set by argument count**: the reduction (`VecMinIntrinsic`, no argument) and the
lane-wise form (`MinIntrinsic`, one vector), each a private candidate joining the public name, so
`v.min()` is a number and `v.min(w)` a vector. A comparison returns the mask. Because they are
ordinary methods, `fnCallLowerMethod` selects them, and a vector's `+=` is rewritten to `+` as a
number's is.

What `fncall.c` lowers itself, each in a hook of `fnCallLowerMethodOn` or the dispatch:

- **A number beside a vector**, `v * 2.0` and `2.0 * v` (`vectorBroadcastOperand`; also the numbers
  among the arguments of `v.min(0.0)`, `v.max(0.0)` and `v.fma(2.0, w)`, where the receiver is the
  vector). The number is
  coerced to the lane type (a literal takes it, a number of another type is `ErrorVectorMixed`) and
  wrapped in a type literal of the vector type whose one argument it is, so both operands are vectors
  by selection. A mixed pair of vectors is reported by `vectorNoCandidate`.
- **`m.select(a, b)`** (`vectorSelectLower`): its two arguments are a vector of any lane type of the
  mask's count, which no signature says, so the call is checked here and becomes a type literal of
  three arguments: the mask, `a`, `b`.
- **`Vector[T, N].from(x)`** and `Array[T, N].from(v)` (`vectorFromCall`), tried before the number
  types' `from`: an array of the lanes, a vector of the same count, or the vector for an array. The
  call becomes a type literal with the one argument.
- **`v[i]`** (`fnCallArrIndex` with `vectorIndex`): an `ArrIndexTag` of the lane type. A mask's lane
  and a borrow or range of a lane are `ErrorVectorIndex`.
- **A vector's contents**, `Vector[f32, 4]() <- 1.0, 2.0, ...` (`contentsLowerVector`): lowered as the
  array of the same lane type and count whose contents they are (`contentsArrayLit`: every form an
  array's contents take, a repeated value evaluated once for each lane), converted by a type
  literal. So the vector's contents are the array's, by construction.

A type literal of a vector or array type is never a literal (`typeLitIsLiteral`), so a global
cannot be initialized with a vector: there is none to write.

## Flow and generation

Flow has nothing of its own: a vector is a number-sized copy value, and a type literal of it is
an expression whose arguments are read. `genlType` gives `LLVMVectorType`; size and alignment are
LLVM's (a vector is aligned as its whole is, a count that is not a power of two is padded up),
which `mem.sizeof` and a struct's layout read. `genlFnCallInternal` sends an intrinsic whose first
argument is an LLVM vector to `genlVectorIntrinsic` (genlvector.c). What differs from a scalar:

- an integer `/` and `%` compare the divisor with zero and, for signed lanes, the dividend with the
  type's minimum beside the divisor with -1, reduce each to one flag (`llvm.vector.reduce.or`), and
  branch once to a panic (`PanicDivide`, `cone_panicDivide` in conestd) before the `sdiv`;
- a float `%` is `frem` on the whole vector, with no check, as a scalar float's is;
- `abs`, lane-wise `min` and `max`, `floor`, `ceil`, `round` and `fma` are LLVM's own intrinsics
  overloaded on the vector (`llvm.fabs.v4f32`, `llvm.minnum`, `llvm.smin`, `llvm.round`,
  `llvm.fma`...), chosen from the lane type by `genlNumberIntrinsic` (genlexpr.c), the code a number's
  call uses;
- a shift compares each lane's amount with the width and selects, as `genlShift` does a scalar's;
- `sum` and `product` of floats are `llvm.vector.reduce.fadd` and `fmul` with no `reassoc` flag, so
  they add in lane order, started at -0.0 and 1.0; `min` and `max` of floats are `reduce.fmin` and
  `fmax`;
- a type literal converts: an array to a vector and back by a store and a load of the vector's slot
  (a mask, whose lanes are one bit in a vector and a byte in an array, lane by lane), a vector to a
  vector lane by lane (`fptosi.sat` and `fptoui.sat` for a float into an integer), a mask's
  select (`LLVMBuildSelect`), and a number put in every lane (insert and shuffle).
- an index is the lane's address in the vector's memory (`genlVectorLane`, bounds checked against
  the count with `genlBoundsCheck`), so `v[i] = x` stores a scalar. The optimizer turns that into
  an insert where the vector lives in a register.

## Hazards

- **A node's namespace is its own.** `cloneVectorNode` and `newVectorNode` each make one; sharing
  a template's would share its mask and methods.
- **A vector refused has `mask == NULL`.** Code that goes on from a refused type (the contents
  lowering does) must test it, or it reports the same lane type again for every lane.
- **The scalar's edge cases are not all built for the scalar yet.** A vector panics on a zero
  divisor and a minimum divided by -1, and saturates a float converted to an integer, as ruled
  on 28 October; the scalar number types still divide and convert by LLVM's rules. Until they are
  built, `x / y` and `Vector.from` disagree where the lanes' edge cases arise.
- **A vector argument of an `@c` function is passed as LLVM passes it,** not through the struct
  plan of "C-named functions and the C ABI" (generation.md): LLVM's x86 back end gives it C's
  convention for `__m128`, which `vector_c_abi` checks against the platform's C compiler.
