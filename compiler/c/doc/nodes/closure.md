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
  parameter bound by one), so a number's overload never takes it; among those,
  the ones taking as many parameters as it writes; and among several, the ones
  whose callable has exactly the types the literal writes. One left is chosen;
  more than one is `ErrorClosureOverload` naming them and saying to write the
  parameter's types; none is `ErrorClosureOverload`. The body is checked against
  none of them.

## A callable used dynamically

`&<fn(sig)`, `&<mut fn(sig)`, `So[fn(sig)]` and `Rc[fn(sig)]` take anything with a
pub `()` of that signature. The signature is **a trait the compiler writes**
(`fnSigCallTrait`, `ir/types/fnsig.c`): a `StructNode` flagged `TraitType` whose
`callsig` is the signature and whose one method is a `()` of it, with a `self` of
`&` or `&mut`. It exists once per signature and kind (`callmut`), found by
`fnSigEqual`, and it is in no module: it is named for the symbol spelling of its
signature (`fnR<nameType>` and `fnM<nameType>`, a long one by its hash), which is
what lets two objects' vtables for one signature be one symbol. Everything after
it is the virtual reference of a trait that already exists.

- **Where it is made.** `refvirtTypeCheck` replaces a signature target by the
  trait, choosing the kind from the reference's permission: one that may write
  (`&<mut`, `So`, `Rc[mut, ...]`) is the `self &mut` trait, any other
  (`&<`, `So[imm, ...]`) the `self &` one. `fnCallLowerManagedRef` makes
  `So[fn(sig)]` a virtual reference as it does for a trait. The signature is
  written bare in a type argument or an index (`parseIndexArg`, `parseFnBound`), as
  a term where a type is expected (`parseTerm` with `intype`), and as an alias's
  target (`parseAlias`). A bare signature held by value (`Applied[fn(i32) i32]`
  with a field `f F`) is `ErrorNoSize`, a function signature being no value;
  `So[F]` and `&<F` with it as the argument are the callable references.
- **What meets it.** `structMapVtableImpl` maps a struct's `()` as it does any slot
  and, for a callable trait, asks `fnSigCallSelfFits`: a read-only trait takes a
  `self &` (or `imm`, `opaq`) and a state-changing one also a `self &mut`; a `self`
  by value or `&uni` fits neither (call once is later). The kinds are two types:
  a `&<mut fn` is not lent as a `&<fn`, because the callable behind it may change.
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
  type and converts the owner to the callable's. A callable that holds nothing
  (a zero-field struct) has no allocation to own and is refused
  (`ErrorCallableUse`). Whether the closure may be kept is the loan walk's, as for
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
they are no closures ([generation](../phases/generation.md), section 7).

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
| | `fnSigCallSelfFits`, `fnSigCallRefusal` | which `()` a kind of callable reference holds; why a coercion was refused |
| `ir/exp/typelit.c` | `typeLitNewCallable` | `new So[fn(sig)](c)` |
| `genllvm/genltype.c` | `genlFnStubVtable` | the vtable of a plain function's reference |
| `shared/error.c` | `errorSilent` | diagnostics counted and not printed |
