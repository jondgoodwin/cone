`NameUseNode` is every appearance of a name in a program. One struct, one tag,
`NameUseTag`, from the parser to generation; what the name meant is asked of the
declaration it is bound to, never stamped on the use.

**At a glance.** Built by the parser, bound to nothing. Name resolution binds
`dclnode`, and from then on `isExpNode`, `isTypeNode` and `isMetaNode` answer
for the declaration. Type check demands the declaration, reads its type, and
lowers two special cases. Flow uses it as the one place initialization and move
state are diagnosed. Generation loads.

*Provenance: read from source.*

## Shape

| Field | Meaning |
| --- | --- |
| `namesym` | the interned name — compared by pointer identity, never by string |
| `dclnode` | the declaration it names. **NULL until name resolution**, and NULL for a member name until type check selects the member |
| `vtype` | the declaration's type, taken during type check |

`FlagQualified` says the name was reached through a namespace — `math3d.Point3`
— rather than written bare. The path collapse in `fnCallNameRes` stamps it, and
the two lowerings that insert an implicit `self` read it: neither may fire on a
name the source qualified. *Which* namespace it came through is not kept,
because nothing asks.

`FlagPattern` marks a pattern's bare root — `Red` in `case is Red`, `Circle` in
`case imm c &Circle` — which is looked up in the matched value's enum before its
lexical meaning. The parser stamps it, `nameUseNameRes` leaves such a name
unbound instead of reporting it when nothing lexical answers, and
`castPatternBind` binds it at type check and clears it. See [cast](cast.md).

One tag serves three situations, told apart by `dclnode` and by where the node
sits:

| A use that is | Has | Because |
| --- | --- | --- |
| not yet resolved | `dclnode == NULL`, and is in no group | the parser built it; name resolution has not reached it, or failed to bind it, or it is a pattern's root waiting for type check |
| resolved | `dclnode` set; answers `isExpNode`, `isTypeNode` or `isMetaNode` for the declaration | `nameUseNameRes` bound it, or a constructor built it pre-resolved |
| a member name — `.field`, `.method`, an operator | `dclnode == NULL` until `fnCallLowerMethod` selects the member; sits in a call's `methfld` | selecting a member needs the receiver's type |

**A member name is a position, not a kind of node.** It is an ordinary
`NameUseNode` that name resolution never sees — `fnCallNameRes` leaves the
call's member slot alone, and nothing else hands one to `inodeNameRes` — and
every reader that meets one is holding a call's `methfld` and knows it. The one
question it can be asked before the member is selected is `isNameUseNode`, which
tells it from the `ULitTag` a tuple index puts in the same slot. Once
`fnCallLowerMethod` has selected the member the node is usually repurposed into
the call's `objfn`, and answers as a value from then on.

## Constructors

| Function | For |
| --- | --- |
| `newNameUseNode` | a parsed name |
| `newNameUseFromLex` | the same, positioned on an existing node — for anything synthesized |
| `newMemberUseNode` | a member name, for a call's `methfld` — the same node, named for what the call site is building |
| `newNameUseFromDclNode` | a **pre-resolved** use, `dclnode` already set |
| `newNameUseAndDcl` | a working variable plus a use of it, for desugaring |
| `cloneNameUseNode` | instantiation — calls `cloneDclFix` to re-point at the correspondingly cloned declaration |

## Parse

`parseNameUse` builds it from one identifier, and that is the whole production.
A path through namespaces is written with periods, parses as a chain of member
accesses, and is collapsed by `fnCallNameRes` — see [fncall](fncall.md), "Name
resolution".

**Some uses arrive already resolved.** The anonymous variables desugaring
synthesizes — `match`'s subject capture, a bound pattern's value, a lifted
closure's reference — are built with `dclnode` set. `nameUseNameRes` returns
immediately for these.

## Name resolution

`nameUseNameRes` is one pointer read and no scope walk:
`name->dclnode = name->namesym->node`. Ordering falls out of hook push order,
not from a written rule.

Every name that reaches it is a bare one. A name reached through a namespace
was bound by the path collapse, and the node it left behind arrives here with
`dclnode` already set, so the pass returns at once — the same early return the
pre-resolved desugaring synthesizes take.

That is the whole of it: `dclnode` is set and nothing else on the node changes.
A name the hooks do not answer is `ErrorUnkName`, unless it carries
`FlagPattern`: then it stays unbound and unreported, because it may be a variant
of the matched value's enum, which type check decides.

## What a use answers

`isExpNode`, `isTypeNode` and `isMetaNode` ask `nameUseGroup`, which follows
`dclnode` to the declaration at the end of the chain of names (`nameUseGetDcl`)
— through an alias too, since an alias stands for its target — and classifies
that: a variable, function, overload set, field or constant → expression; a
macro or generic parameter → meta; **everything else, including a module, →
type** by fallthrough. A use bound to nothing — unresolved, or a member name
before type check selects the member — is in no group: none of the three
answers true.

`nameUseNames(node, dcltag)` asks the sharper question a reader usually means —
does this name a `ConstDclTag`, an `FnOverloadDclTag`, a `MacroDclTag`, a
`GenVarDclTag` — and answers no for a node that is not a name use and for one
bound to nothing.

The use is asked rather than stamped because an alias has nothing to stamp: a
name that resolves to a binding pointing at a declaration is whatever the
declaration is, and only the declaration can say. The alias exists now
(`AliasDclNode`), for a method a type holds by folding, and a bare use of one
answers as the method.

Privacy is checked where the path is collapsed, not here: a declaration not
written `pub`, reached through a namespace from outside its module, is
`ErrorNotPublic`. **The declaration stays attached after that diagnostic** — it
is the one the program asked for, and leaving the use unresolved would hand the
next pass a null.

## Type check

Two entry points, because a type name and a value name want different things.

`nameUseTypeCheck` (value position), in order:

0. **A use bound to an alias is re-pointed at what the alias stands for**, so
   everything below reads a declaration's type and tag. An alias its fold
   failed to bind was reported there; the use takes `errorType`.
0a. **A use bound to an alias a *global's* fold made becomes `global.name`.**
   The alias records the global its target is reached through, so the lowering
   is `aliasDclThroughAccess` and has no receiver to find: a global is one
   instance at an address known at compile time. Unlike the bare-field lowering
   below, it fires whether or not the use was qualified — `mymod.speed` names the
   module's binding, and the binding is still reached through the global. A
   folded name being *called* never arrives here, because `fnCallTypeCheck`
   rewrites the call before it reads its callee, so what reaches this step is a
   member read.
0b. **A member of a generic type itself is refused** (`nameUseTemplateMember`):
   a function or static whose owner still carries `GenericInfo`, reached as
   `Box.stat` on a `struct Box[T]`. Only an instance's copy of it is ever
   generated, so the use has to name an instance — `ErrorArgCount`, since no type
   argument list is written where one is required. A use inside the generic's own
   body never arrives here bound to the template's member: the clone re-pointed
   it at the instance's ([generic](generic.md), "How a cloned name gets
   re-pointed"). `fnCallLowerOverloadFn` asks the same of an overload name's
   candidates before selecting one. **Nor does a bare name inside an enum
   extension's braces that names a generic base's member**: name resolution bound
   it to the base template's, since the instance the extension stands on exists
   only at type check, and `nameUseBaseInstanceMember` first points it at that
   instance's member of the same name ([struct](struct.md), "An enum extending an
   enum"). `fnCallTypeCheck` does the same for a bare overload name before its set
   is selected from. A qualified use is left as written, and refused here.
1. **An overload name is refused here.** It names a set, not a value; only a
   call may use it, and `fnCallTypeCheck` rewrites the use to the concrete
   declaration before this is reached. `ErrorOverloadUse`.
2. **A bare field name becomes `self.field`.** This is lowering — it builds a
   call node and takes its type from what that call resolves to — so it belongs
   here rather than in name resolution, which had no type to work from. It
   synthesizes a resolved `self` from parameter 0 and re-reads the name as a
   member. Outside a method there is no receiver, so it is `ErrorUnkName`: "there
   is no self here to reach it through." **Bare is the whole of the condition:
   `FlagQualified` excludes a field named through its type, `Gadget.w`, which
   asked for that type's field and not for this method's receiver.**
3. **Demand the declaration.** `inodeTypeCheckAny` on `dclnode` — this is what
   puts the work in dependency order rather than source order.
4. **Circularity.** A declaration still under check whose type is *still*
   `unknownType` is a constant or an inferred declaration defined in terms of
   itself. `ErrorCircular`.
5. Take `vtype` from the declaration.

`nameUseTypeCheckType` (type position) does step 3 and nothing else. **Naming a
type that is still being laid out is not an error** — the name resolves to the
same declaration either way. What such a type cannot answer is its *size*, and
that is asked where a value of it is held, not here.

## Flow

`nameuseFlow` is where initialization and move state are **diagnosed** — the
only place either produces a message:

- not `VarInitialized` → `ErrorMove`, "has not been initialized"
- `VarMoved` → `ErrorMove`, "value has been moved out"

It returns immediately for anything that is not a `VarDclTag`, so a function or
constant name passes through untouched. The flags themselves are read elsewhere
too — `assignlvalrtype` and `flowScopeDealias` both consult them.

Because the flags are a running summary over the whole function rather than
per-program-point state, "initialized on one branch" reads as initialized
everywhere, and a move in one arm of an `if` poisons both.

## Generation

`genlExpr` recognizes a value name (`isNameUseNode` and `isExpNode`) ahead of
its switch and loads `dclnode->llvmvar` — which is a **pointer to** the value,
since every local and parameter is an alloca. A `ConstDclTag` recurses into the
constant's value instead. `genlAddr` returns `llvmvar` itself without the load.

That one-level difference between `genlExpr` and `genlAddr` on the same node is
the most common way to be off by an indirection here.

## Hazards

- **`dclnode` is NULL for a member name** until the member is selected. Code
  that walks name uses and dereferences `dclnode` must not reach into a call's
  `methfld`; `nameUseNames` and the three group predicates are safe to ask.
- **`dclnode` may be NULL after name resolution for a pattern's root**
  (`FlagPattern`) until its `is` test is type checked. Only the pattern's own
  nodes hold it, and `castPatternPending` is the question to ask before reading
  its type.
- **A use of a module's name answers `isTypeNode` true.** `ModuleTag` is in
  the statement group, so `isTypeNode` is false for the module itself; the use's
  answer is `nameUseGroup`'s fallthrough for every declaration that is not a
  value, a macro or a generic parameter, not a claim that a module is a type.
- **Names are compared by pointer**, never by string. A name built without
  `nametblFind` will never match anything.
- **`newNameUseNode` takes the lexer's current position.** For a synthesized
  use, use `newNameUseFromLex` or copy the position, or the diagnostic points at
  end of file.
- **The overload refusal and the bare-field lowering both run before the
  declaration is demanded.** Anything inserted into `nameUseTypeCheck` has to
  decide which side of that it belongs on.
- **A cloned use needs `cloneDclFix`**, or it points at the template's
  declaration instead of the instance's. That includes a generic type's own
  bare name inside its braces, which is the instance being defined: the clone
  maps the template to its reserved instance ([generic](generic.md), "A generic
  type's own name is in the map too").

## What lives elsewhere

- Hooking, lookup order, and what the pass does retag: [Name Resolution](../phases/name-resolution.md)
- What a name *means* — visibility, imports, overloading: [Names and Namespaces](../../../../doc/design/names-and-namespaces.md)
- Demand, circularity, and the two marks: [Type Check Phase](../phases/type-check.md)
- Why an overload name has no value: [IR Nodes](_index.md), "FnOverloadDcl"
- The disjoint case, a bare *method* name being called: [fncall](fncall.md)
