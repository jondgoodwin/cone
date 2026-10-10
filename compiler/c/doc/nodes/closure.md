A closure literal is **one node, `ClosureNode`, that lives from the parser until
type check replaces it** with the construction of a struct the compiler writes
(the *hidden struct*). Nothing after type check meets one, `--checktree` reports
one that is left, and that is the whole of the design: the literal's meaning is
a struct, a method and a call that already mean what they mean.

**At a glance.** `parseClosureFn` and `parseClosureArrow` (`parseexpr.c`) build
it. Name resolution (`closureNameRes`) resolves its parts and notes what its
body names of the code around it. Type check (`closureTypeCheck`) builds the
hidden struct, tries the body to find the permissions, and turns the literal into
`new Hidden(...)`. A name inside the body that names something the closure
holds is rewritten to the field's access as the body is checked
(`closureUse`).

*Provenance: read from source and measured by the `closure` group of the test
suite and `packages/closurelib`.*

## The two forms

```
fn (u f32) [ribs, mut n = 0, w = window.handle()] f32 { ... }     // the full form
x => x * 2          (a, b) => a.len() < b.len()          () => 5   // the short form
```

The full form is parameters, an optional **state list** in brackets (a bracket
never starts a type, so it cannot be read as the return type), an optional
return type and a block. A parameter's type is optional where a signature is
expected. A state entry is a field with an initializer; written without one,
`[ribs]`, its initializer is the variable of that name in the code around. The
short form is parameter names only (`ErrorClosureForm` otherwise), one expression
or a block, no list and no written return type. `=>` is the lexer's
`FatArrowToken`. The short form is read in `parseTerm` where a name or a
parenthesised list is followed by `=>`; the body is `parseSimpleExpr` and an
assignment on it, so a comma after it belongs to the list the closure is in.

`&fn(x i32) i32 { ... }` is not this node: it is an anonymous function lifted to
module scope as it is parsed (`parseAmper`), a function reference that sees no
local.

## Shape

`ClosureNode` (`ir/exp/closure.h`):

| Field | Meaning |
| --- | --- |
| `sig` | a `FnSigNode` of the parameters; `unknownType` for a type not written, and for a return type not written |
| `state` | a `VarDclNode` per entry: name, permission (`mut` or `imm`), initializer in `value` |
| `body` | a block (the short form's expression is wrapped in one) |
| `captures` | name resolution: the variables of the code around that the body names, each borrowed |
| `outerself` | name resolution: the `self` of the method it is written in, where the body names a member bare |
| `outer`, `outerscope` | name resolution: the literal it is written in, and the block scope it stands at |

## Name resolution

`closureNameRes` resolves the state's initializers and the parameters' types in
the code around the literal, before the closure's own names are hooked. It then
hooks the parameters and the state entries over the names around and resolves the
body, with `NameResState.closure` set and `loopblock` cleared (a `break` does not
leave a closure).

**What is the closure's own is told by scope, not by walking.** Every local has
the block scope it was declared at. The literal records the scope it stands at;
a variable declared deeper is its body's, and so are its parameters and state
entries (checked by membership, since a parameter's scope is 1). Any other local
a name binds to is of the code around it: `closureNoteUse`, called at the end of
`nameUseNameRes`, adds it to `captures` of every literal it is outside of,
innermost out, which is how a closure inside a closure borrows through the outer
one. A global and a function's `static` are reached by name and are not borrowed.

**A bare member name makes the method's `self` a capture.** Inside a method, a
bare field or method name means `self.name` (`nameUseTypeCheck`), and the
closure's own `()` has a `self` of its own. So the name binding to a member
records the enclosing method's `self` (`selfName->node`, hooked by the method) as
captured and as `outerself`, and the lowering to `self.name` reaches the member
through it (`closureSelfParm`). This is what lets a closure written in a method
use that struct's private fields and methods, and call them from wherever it
is passed: privacy is the module's, and the hidden struct is joined to the module
of the code around it.

`pstate->declaring` is the name of the local whose initializer is being resolved
(`varDclNameRes`). A name inside a closure that is that name and bound to nothing
is `ErrorClosureSelf`: a closure cannot call itself, and recursion uses a named
function. (`selfmethod` stays reserved.)

## Type check

`closureTypeCheck`, in the order it does things:

1. **The signature expected**: a function reference's, `&fn(...)`, when the
   position is a parameter of that type; the bound of a generic parameter, set in
   `closureHint` by the call (below); none elsewhere. Parameters written without
   a type take theirs from it (`ErrorClosureParm` if there is none, or the counts
   differ). A literal that holds and borrows nothing, given where a function
   reference is wanted, is lifted to a plain function and is `&` of it. One that
   holds or borrows is refused there, saying so (`ErrorClosureForm`); `&<fn` is
   what takes it.
2. **The state's values** are checked where the closure is made
   (`iexpTypeCheckCoerce` with no expectation, so a literal takes its default
   type); each value's type is its field's.
3. **The permissions are found by trying.** Whether a borrow is `&` or `&mut`,
   and whether `()` takes `self &` or `self &mut`, is what the body does. The
   body is checked as a clone, its diagnostics counted and not printed
   (`errorSilent`), first with the weakest permissions; if that fails, with
   every borrow the variable allows `&mut` (a variable is borrowed `&mut` only
   if it is `mut`) and `self &mut`; if that passes, each `&mut` borrow is taken
   back in turn while the body still checks, and `self` last, which stays `&mut`
   while any borrow is. A failure that survives the most permissive guess is the
   body's own, and the real check reports it. This asks the type checker itself
   what the body needs, so a method called on a captured variable, an assignment
   through one and a nested closure's needs are all found with no analysis of
   their own. A clone is made of the literal before anything is checked
   (`cloneClosureNode`).
4. **The hidden struct** (`closureBuild`) is a `StructNode` named `closure#N`,
   joined to the module of the enclosing function and added to its nodes so that
   it is generated. Its fields are the state entries, then a field for each
   captured variable holding `&` or `&mut` of its type. Its one method is `()`,
   `pub`, a `FnDclNode` whose `closure` field (a `ClosureInfo`) says what the
   body holds; its parameters are the literal's after a `self` of the struct, its
   body the literal's block. It is checked as any struct is, the method from the
   members queue.
5. **The literal becomes `new Hidden(state values, &variables)`**, with its
   arguments already checked (`typeLitNewArgsChecked`).

**A name in the body is a field.** `nameUseTypeCheck` asks `closureUse` first: a
name use bound to a state entry or a captured variable, in a function that has
a `ClosureInfo` naming it, is replaced by `self.field`, and by the place the
borrow points at, `*self.field`, for a captured variable. Type check replaces the
node through the pointer to it, as it does for a bare field.

**The return type** is the written one, the expected signature's, or read off the
paths. Read off, the signature holds `void` while the body is checked, and
`closureImplicitReturn` and `closureReturnTypeCheck` (called from `fnDclTypeCheck`
and `returnTypeCheck` for a function whose `ClosureInfo` says `retinfer`) set it
from the first return the check meets and hold the others to it. An `if` whose
branches disagree says `ErrorClosureRet`, in the closure's words, from the one
place that finds it (`iexpMultiInfer`, told by `closureInferring`). A body with
nothing to hand back returns nothing.

## A closure given to a call

A closure literal among a call's arguments is not checked with the others when the
callee is generic or an overload set (`fnCallTypeCheck`): it waits for the rest,
which say what its signature is, then `fnCallClosureArgs` checks it.

- **A generic.** `genericClosureSig` finds the signature bound of the type
  parameter the argument's parameter is (or is a reference to), reads the other
  type parameters off the other arguments (`genericInferType`), and clones the
  bound with them. A parameter that is `&F` or `&mut F` is given the literal
  lent as a temporary, a borrow of it, as `&make()` is.
- **An overload set.** The literal is considered only against the overloads whose
  parameter there is callable (a function reference's signature, or a generic
  parameter bound by one, in a generic that may join the set only because every
  type parameter of it is so bound), so a number's overload never takes it; among
  those, the generics the call's other arguments leave viable
  (`genericOverloadViable`), the ones taking as many parameters as it writes; and among several, the ones
  whose callable has exactly the types the literal writes. One left is chosen;
  more than one is `ErrorClosureOverload` naming them and saying to write the
  parameter's types; none is `ErrorClosureOverload`. The body is checked against
  none of them.

## A callable used dynamically

`&<fn(sig)`, `&<mut fn(sig)`, `So[fn(sig)]` and `Rc[fn(sig)]` take anything with a
pub `()` of that signature. The signature is **a trait the compiler writes**
(`fnSigCallTrait`, `ir/types/fnsig.c`): a `StructNode` flagged `TraitType` whose
`callsig` is the signature and whose one method is a `()` of it, with a `self` of
`&` or `&mut` (`callmut`). It is the compiler's own detail, never named in a
message: the language has one `&<fn(sig)`. It exists once per signature and `self`,
found by `fnSigEqual`, and it is in no module: it is named for the symbol spelling
of its signature (a long one by its hash), which is what lets two objects' vtables
for one signature be one symbol. Everything after it is the virtual reference of a
trait that already exists.

- **Where it is made.** `refvirtTypeCheck` replaces a signature target by the
  trait, choosing its `self` from the reference's permission: one that may write
  (`&<mut`, `So`, `Rc[mut, ...]`) is the `self &mut` trait, any other
  (`&<`, `So[imm, ...]`) the `self &` one. `fnCallLowerManagedRef` makes
  `So[fn(sig)]` a virtual reference as it does for a trait. The signature is
  written bare in a type argument or an index (`parseIndexArg`, `parseFnBound`), as
  a term where a type is expected (`parseTerm` with `intype`), and as an alias's
  target (`parseAlias`). A bare signature held by value (`Applied[fn(i32) i32]`
  with a field `f F`) is `ErrorNoSize`, a function signature being no value;
  `So[F]` and `&<F` with it as the argument are the callable references.
- **What meets it.** `structMapVtableImpl` maps a struct's `()` as it does any slot
  and, for a callable trait, asks `fnSigCallSelfFits`: a reference that only reads
  takes a `self &` (or `imm`, `opaq`) and one that may write also a `self &mut`; a
  `self` by value or `&uni` fits neither (call once is later). A `&<mut fn` is not
  lent as a `&<fn`, because the callable behind it may change.
  A trait's own method is held to `fnSigVrefSelfFits` ([struct](struct.md)), a
  closure's too: a literal that changes state has `self &mut`, and is refused
  (`ErrorVtableSelf`) where the trait's method says `self &`, under every holder.
  `fnSigCallRefusal` says which of these refused a coercion, in the author's
  words (`ErrorCallablePerm`), at an argument (`fnCallTypeCheck`), an
  initializer (`varDclTypeCheck`) and an owner's making.
- **A plain function.** `refvirtMatchesRef` accepts a reference to a function
  whose signature `fnSigEqual`s the trait's. Generation (`genlConvert`) makes the
  data pointer the function's own code pointer and the vtable (`genlFnStubVtable`,
  once per trait and object, `Vtable.llvmfnvtable`) a one-slot table holding a stub
  of the slot's type that calls its first argument as the function with the rest.
  It carries no type record, which only an owner reads, and an owner is never
  made of a function.
- **A literal.** A closure literal given as an argument to a parameter of type
  `&<fn(sig)` is lent as a temporary of the statement (`fnCallCheckClosureArg`,
  also for the overload a literal picks and, through `genericParmBound`, for a
  generic's parameter that names a type parameter in the signature); the borrow
  carries the parameter's permission, so a literal that changes a variable is a
  `self &mut` struct behind a `&` and is refused by the callable's own message. A
  literal anywhere else is not lent: to keep one it is moved into an owner.
- **An owner.** `new So[fn(sig)](c)` (`typeLitNewCallable`) takes one value, checks
  it against the signature's hint if it is a literal, allocates that value's own
  type and converts the owner to the callable's. **A callable that holds nothing**
  (a zero-size struct) is allocated as any other: the allocation of a value with no
  size asks the region for one byte (`genlallocref`; `allocateZeroSizeOk` lets the
  value check pass only here), and it is freed as any block is, with nothing
  special at its death. **A plain function** (`new So[fn(sig)](&f)`) is held by a
  struct of the compiler's, `closureFnHolder`: one field, the function reference,
  and a `()` that calls it, made as a closure's struct is and owned like it; a
  function of another signature is `ErrorCallableUse`. Whether the closure may be kept is the loan walk's, as for
  any owning virtual reference: one that borrows a local is refused by the rule
  that such an owner holds only global borrows.
- **A callable field.** `t.profile(3.)` where `profile` is a field is rewritten
  to the call of the field's access (`fnCallFieldCall`), as `(t.profile)(3.)`,
  after checking that the field's type can be called (`fnCallTypeCallable`); one
  that cannot is `ErrorFldArgs`, saying what the field holds. A field and a method
  never share a name, so there is nothing to choose between.
- **A generic taking `&F`.** A callable whose `()` takes `self &mut`, given to a
  parameter taken as `&F`, is refused at the caller's argument before an instance is
  made (`genericCallablePermCheck`, `ErrorCallablePerm`).

The call is the ordinary virtual dispatch: `f(x)` on a callable reference reaches the
trait's `()` (`fnCallTypeCheck`, `VirtRefTag`) through the vtable.

## A literal meets a trait with one required method

A closure literal given where a trait is wanted fills the trait's method when the
trait has exactly one *required* method (a method without a body; private ones
count: a generic bound requires them) and no field (`closureTraitSig`). A method
with a body is a default: the hidden struct inherits each, cloned in as an
implementer's are (`structInheritDefaults`, called by `closureBuild` from
`ClosurePlan.trait`, set by the places below through `closureTrait`), so the
defaults call the literal's body through the required method. The struct does
not *declare* the trait (no `traits` entry, so no `structCheckTraitReqs`); it
meets it structurally, as before. One exception: a body that needs `self &mut`
under a trait method that says `self &` gets no defaults, since the literal is
refused for that anyway (below) and defaults reading through `self &` would add
their own failures to its. Only a literal, at the point the trait is
expected, so no struct of the author's meets anything differently. The literal is
checked as one given to a signature: the signature is the method's without its
receiver, and `closureMethod` (beside `closureHint`) says the hidden struct's one
method is named for the trait's method, not `()` (`ClosurePlan.method`,
`ClosureInfo.method`). The places a trait is wanted:

- **A generic bound by the trait** (`[S Shape](shape &S)`): `fnCallClosureArgs`
  asks `genericClosureSig` (a signature bound) and then `genericParmTraitBound`.
  The instance's `S` is the hidden struct, which meets the bound structurally.
- **A borrowed reference to the trait** (`&<Shape`, `&<mut Shape`): lent as a
  temporary, as a callable reference is (`fnCallLendParm`).
- **An owner** (`new So[Shape](literal)`): `typeLitNewCallable`, the callable's
  making, for any trait when the one argument is a literal.

A trait with other than one required method, or with a field, is `ErrorClosureTrait`,
naming what it has (`fnCallClosureTraitRefused`). A parameter count, a written
parameter type or a written return type that is not the method's is
`ErrorClosureParm`, naming the method. **The method's `self` follows the body**,
as `()`'s does: a literal whose body changes state has `self &mut`. It is held to
the trait's `self` as any implementer is (`fnSigVrefSelfFits`): where the trait's
method says `self &` it is refused, under every holder -- `&<Shape`, `&<mut Shape`,
`So[Shape]`, `[S Shape]` by `&S`, `&mut S` or value -- as `ErrorVtableSelf`, in
the literal's words (`fnSigVrefSelfRefusal`: it needs `self &mut` because it
changes state it holds or borrows; declare the trait's method `self &mut`, or do
not change it). The vtable cases reach it through `structMapVtableImpl` and
`fnSigCallRefusal`, the static one through `genericCallablePermCheck`. A literal
that only reads has `self &` and meets a trait method of either kind. A callable
(`&<fn`) is different: its kind and the reference's permission decide
(`fnSigCallSelfFits`; `refvirtMatchesRef` keeps a `&<fn` from taking a changing
literal when it only reads). Under a trait whose method says `self &mut`, a
literal that changes state is met, and called through a reference that only reads
it is refused at the call, as a struct's is.

**A closure that fails stops its call.** A literal that failed to type check as an
argument of a generic call leaves the call nothing to infer from; `fnCallClosureArgs`
gives the call up (the failed literal's lent borrow, left behind, reached
generic substitution as a node with no name). A closure whose body already failed
reports no return mismatch of its own (`ClosureInfo.errbase`, `returnTypeCheck`).

## Where a closure may go

A closure is a struct of what it borrows and holds, so where it may go is
decided by the rules for its fields, with nothing closure-specific: a borrow
makes it lifetime-limited (the loan walk), an `Rc` makes it not Sendable and not
Shareable, a `&mut` (whose body writes through it with the `()` still `self &`)
makes it neither ([references](references.md), `Shareable`). What is closure's own
is the message, which names the variable to list so the closure holds a copy
instead of borrowing it.

- **The implicit borrow knows it is one.** The borrow the compiler makes of each
  variable the body names is a `BorrowTag` node with `RefNode.capture` set
  (`closureTypeCheck`). The loan walk's escape messages (`loanNotBoxable`,
  `loanNotGlobal`, `loanNotBound`, `loanEscape`; `loanCaptured`,
  `loanCaptureAdvice`) read it off the loan's site and add the sentence: list the
  variable, `[k]`; for `self` (a body that names a member borrows `self`), copy the
  members into the list or capture a counted handle. A borrow stored in `self`
  that borrows `self` is the same `ErrorLifetimeBound` an owner of a callable
  holding any borrow of the function's gets (`So[fn]` bounds its value by
  `'static`). A `&<fn` returned by borrow of a local closure is
  `ErrorEscape` from `returnFlowEscape`, which says the closure can leave as an
  owner and names what it borrows.
- **Threads.** `closureFirstCap` finds the first captured variable (a list entry or
  a borrowed variable) whose field the check refuses; `genericNotSendableMsg`,
  `genericNotShareableMsg`, `fnSigMarkRefusal` (a value made `So[fn() + Sendable]`
  or `&<fn() + Shareable`) and the `parallel each` reach message
  (`parCheckReach`) say it as "it borrows 'k' (&ro i32)" and add
  `closureCapAdvice`. A list entry holding an `Rc` is told to hold an `Arc`; a
  captured `&mut` to copy the value it reads, or keep what it changes in an atomic or
  behind a lock.

## In GPU code

On a GPU target (`flowGpu`) a closure is allowed in its static form only: given to
a generic bound by a signature, whose instance for the hidden struct is inlined
into the kernel with every other call (generation, section 7), so the struct, a
value of fields that are numbers or borrows, is broken into locals and nothing of
it is left to run. Three refusals keep it so:

- **What it holds** (`closureGpuCheck`, `closureGpuWhy`, `ErrorGpuClosureData`):
  the type of each state entry and of each variable it borrows may hold numbers,
  structs and arrays of them, and borrows (a slice's element, a reference's
  target) of such types; an owning reference (`So`, `Rc`, `Arc`, `Gc`), a function
  reference, a virtual reference or a raw pointer, anywhere inside, is refused,
  naming the variable. The closure is built all the same, so the call it is
  given to goes on and says nothing more.
- **A function reference** (`ErrorGpuClosureRef`): a literal given where `&fn(...)`
  is wanted, which on the CPU becomes a plain function when it holds and borrows
  nothing, is refused whatever it holds. A GPU has no pointers to code.
- **A virtual reference** (`closureGpuVirtRefused`, from the `ConvSubtype` case of
  `iexpCoerceShape`): a reference to a hidden struct converted to a virtual
  reference or an owner of a trait is refused; it would be called through a table
  of code pointers.

A function reference to a named function, a virtual reference to a hand-written
struct and an allocation are refused by `ErrorGpuUnavailable`, not by this check;
they are no closures ([generation](../phases/generation.md), section 7). So is an
owning reference type written in a signature, a field or a local, which no closure can
therefore hold: this check's owning-reference case is a second line of defence.

## Across objects

A closure written in a body an importer expands (an `inline` function, a generic's
instance) is made again by every object that expands it. `ClosureInfo.expanded`
says so, and the object defines the hidden struct's methods, internally
(`genlIsDefinedHere`, `genlImportedInstances`). Its name is the object's own
(`closure#N`), so two objects' closures never meet at the linker.

## Hazards

- **An unlowered literal.** A refused closure is replaced by an error node;
  `--checktree` reports one still in a tree that had no errors.
- **The attempts are real checks.** A clone of the body is checked to find the
  permissions, up to the number of borrows plus three times, so a closure inside a
  closure is checked that many times over; depth beyond two is slow. What an
  attempt instantiates stays instantiated.
- **A swap of a captured variable** reports its refusal in flow, after the
  permissions are chosen, so it is judged under the weakest guess that passed
  type check.
- **State initializers see the code around**, not the state before them:
  `[a = 1, b = a]` takes `a` from outside.

## Code pointer map

| File | Function | Purpose |
| --- | --- | --- |
| `parser/parseexpr.c` | `parseClosureFn`, `parseClosureArrow`, `parseClosureArrowBody` | both forms; `parseAssignFrom` reads a body that changes something |
| `ir/exp/closure.c` | `closureNameRes`, `closureNoteUse` | resolve a literal; note a variable its body names |
| | `closureTypeCheck`, `closureTry`, `closureBuild` | the lowering; a guess under which the body is tried; the hidden struct |
| | `closureUse`, `closureSelfParm` | a name that is a field; the `self` a member is reached through |
| | `closureImplicitReturn`, `closureReturnTypeCheck` | the return type read off the paths |
| | `cloneClosureNode` | a copy of a literal, for a generic's instance and for the attempts |
| | `closureGpuCheck`, `closureGpuWhy`, `closureGpuVirtRefused` | on a GPU target, what a closure may hold, and a closure made a virtual reference |
| `ir/iexp.c` | `iexpCoerceShape` (`ConvSubtype`) | calls `closureGpuVirtRefused` |
| `genllvm/genllvm.c` | `genpgm` | the GPU pipeline's two `sroa` runs, which break up what a closure borrows |
| `ir/exp/fncall.c` | `fnCallClosureArgs`, `fnCallClosureOverload`, `fnCallCallableParm` | closure arguments of a generic or an overload set |
| | `fnCallCheckClosureArg`, `fnCallLendParm` | a literal given to a `&<fn` parameter, lent as a temporary |
| | `fnCallFieldCall`, `fnCallTypeCallable` | `t.profile(3.)`: a callable field's call |
| `ir/meta/generic.c` | `genericParmBound`, `genericClosureSig` | the signature a generic's bound gives a closure (or a `&<fn` parameter writes) |
| | `genericCallablePermCheck` | a callable that changes, given to `&F`, refused at the argument |
| `ir/types/fnsig.c` | `fnSigCallTrait`, `fnSigOfCallTrait` | the trait that stands for a signature behind a virtual reference |
| | `fnSigCallSelfFits`, `fnSigCallRefusal` | which `()` a callable reference holds; why a coercion was refused (a closure's method behind a reference that only reads too) |
| `ir/exp/closure.c` | `closureTraitSig`, `closureMethodMutates` | the one required method of a trait a literal fills; whether a closure's method takes `self &mut` |
| `ir/types/struct.c` | `structInheritDefaults` | the trait's default methods cloned into the hidden struct |
| `ir/exp/fncall.c` | `fnCallLendParm`, `fnCallClosureTraitRefused` | `&<Shape` takes a lent literal; a trait without exactly one required method, or with a field, refuses it |
| `ir/meta/generic.c` | `genericParmTraitBound` | the plain trait that bounds a parameter a literal is given to |
| `ir/exp/typelit.c` | `typeLitNewCallable` | `new So[fn(sig)](c)` |
| `genllvm/genltype.c` | `genlFnStubVtable` | the vtable of a plain function's reference |
| `shared/error.c` | `errorSilent` | diagnostics counted and not printed |
