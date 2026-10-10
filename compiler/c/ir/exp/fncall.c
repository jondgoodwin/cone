/** Handling for function/method calls
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

// Create a function call node
FnCallNode *newFnCallNode(INode *fn, int nnodes) {
    FnCallNode *node;
    newNode(node, FnCallNode, FnCallTag);
    node->vtype = unknownType;  // Will be overridden by return type
    node->objfn = fn;
    node->methfld = NULL;
    node->args = nnodes == 0? NULL : newNodes(nnodes);
    return node;
}

// Create new fncall node, prefilling method, self, and creating room for nnodes args
// These three are the only way an operator application is built, so each marks the
// node FlagOperator: a member access by name builds the same shape and must stay
// distinguishable from it.
FnCallNode *newFnCallOpname(INode *obj, Name *opname, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    node->methfld = (INode*)newMemberUseNode(opname);
    return node;
}

FnCallNode *newFnCallOp(INode *obj, char *op, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    node->methfld = (INode*)newMemberUseNode(nametblFind(op, strlen(op)));
    return node;
}

FnCallNode *newFnCallOpnameLower(INode *oldnode, INode *obj, Name *opname, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    node->flags |= FlagOperator;
    inodeLexCopy((INode*)node, oldnode);
    node->methfld = (INode*)newMemberUseNode(opname);
    inodeLexCopy((INode*)node->methfld, oldnode);
    return node;
}

FnCallNode *newFnCallLower(INode *oldnode, INode *obj, int nnodes) {
    FnCallNode *node = newFnCallNode(obj, nnodes);
    inodeLexCopy((INode*)node, oldnode);
    return node;
}

// Clone fncall
INode *cloneFnCallNode(CloneState *cstate, FnCallNode *node) {
    FnCallNode *newnode;
    newnode = memAllocBlk(sizeof(FnCallNode));
    memcpy(newnode, node, sizeof(FnCallNode));
    // Expanding a macro, 'srcFile()' and 'srcLine()' written in its body answer
    // where the macro is used, as 'file!()' and 'line!()' do in a Rust macro:
    // that is how 'assertDebug' reports its caller's line. The place is the
    // outermost use (macroSrcSite); its arguments keep their own (cloneNode)
    if (cstate->srcsite && intrinsicSrcKind((INode*)node))
        copyNodeLex(newnode, cstate->srcsite);
    // Read before the receiver is cloned, since cloning is what substitutes the
    // use site's expression for 'self'
    if (cstate->selfparm && nameUseNames(node->objfn, GenVarDclTag)
        && ((NameUseNode*)node->objfn)->dclnode == cstate->selfparm)
        newnode->flags |= FlagSelfRecv;
    newnode->objfn = cloneNode(cstate, node->objfn);
    if (node->args)
        newnode->args = cloneNodes(cstate, node->args);
    newnode->methfld = cloneNode(cstate, node->methfld);

    // Inside a generic type's braces its bare name is the instance being cloned,
    // and the clone maps the generic to it (genericReserve). Given type arguments
    // it is the generic again: 'Box[i32]' inside 'Box[T]' is another instance,
    // and 'Box[T]' is this one, both reached through the memo. A bare use with a
    // value list -- 'Mb.No[]', 'Box[v]' -- stays mapped: it builds this instance.
    if (isNameUseNode(node->objfn) && isNameUseNode(newnode->objfn)) {
        INode *generic = ((NameUseNode*)node->objfn)->dclnode;
        if (generic && generic != ((NameUseNode*)newnode->objfn)->dclnode
            && ((generic->tag == StructTag && ((StructNode*)generic)->genericinfo)
                || (generic->tag == ModuleTag && ((ModuleNode*)generic)->genericinfo))
            && fnCallHasTypeArgs(newnode))
            ((NameUseNode*)newnode->objfn)->dclnode = generic;
    }
    return (INode *)newnode;
}

// Does this call give type arguments -- 'Box[i32]', 'Mb.No[T]' -- rather than
// values? A generic's type argument list is recognized as genericSubstitute
// recognizes it, by any argument that is a type, or a type parameter not yet
// substituted.
int fnCallHasTypeArgs(FnCallNode *node) {
    if (node->args == NULL)
        return 0;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        if (*argsp && (isTypeNode(*argsp) || nameUseNames(*argsp, GenVarDclTag)))
            return 1;
    }
    return 0;
}

// Serialize function call node
void fnCallPrint(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    // A construction, 'new Point(1, 2)', names its type until type check
    // lowers it to its declared 'init''s call, whose type it then has
    // 'trynew' holds the bound 'Option' in its methfld until type check
    if (node->flags & FlagNew) {
        inodeFprint((node->flags & FlagTryNew) ? "trynew " : "new ");
        inodePrintNode(nameUseNames(node->objfn, FnDclTag) ? node->vtype : node->objfn);
        if (node->args == NULL)
            inodeFprint("()");
    }
    else
        inodePrintNode(node->objfn);
    if (node->methfld && !(node->flags & FlagTryNew)) {
        inodeFprint(".");
        inodePrintNode((INode*)node->methfld);
    }
    if (node->args) {
        inodeFprint(node->tag==ArrIndexTag? "[" : "(");
        for (nodesFor(node->args, cnt, nodesp)) {
            inodePrintNode(*nodesp);
            if (cnt > 1)
                inodeFprint(", ");
        }
        inodeFprint(node->tag == ArrIndexTag ? "]" : ")");
    }
}

// Whether a declaration is a method, or an overload name one of whose
// candidates is
static int fnCallNamesMethod(INode *dcl) {
    if (dcl->tag == FnDclTag)
        return (dcl->flags & FlagMethFld) != 0;
    if (dcl->tag != FnOverloadDclTag)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((FnOverloadDclNode*)dcl)->overloads, cnt, nodesp)) {
        if ((*nodesp)->flags & FlagMethFld)
            return 1;
    }
    return 0;
}

// A '.' whose left side names a namespace is a path through it, not an access
// to a value: 'math3d.Point3', 'Tally.make(1)', 'Tally.made'. The parser cannot
// tell the two apart, so the decision is made here, as soon as the base name is
// bound, and what it binds to is the whole of the test -- a module or a type is
// a namespace, anything else is a receiver.
//
// The member is looked up in that namespace and the hop disappears: with no
// arguments the node becomes the bound name, and with arguments it becomes a
// plain call of it. Either way the shape handed on is the one an unqualified
// name of the same declaration would have produced, so nothing downstream
// learns that a path was written -- except FlagQualified, which the two
// implicit-'self' lowerings ask.
//
// It has to happen here rather than in type check, because name resolution
// itself asks isTypeNode of an operand: '&mut mymod.Gadget' and
// '(mymod.A, mymod.B)' are settled by refNameRes and ttupleNameRes, which run
// after this and need a resolved type name to look at.
//
// Returns 1 when the node was replaced outright, so the caller stops.
static int fnCallNameResPath(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // An operator, a tuple index, or a call with no member name is never a path
    if (node->methfld == NULL || !isNameUseNode(node->methfld) || (node->flags & FlagOperator))
        return 0;
    if (!isNameUseNode(node->objfn))
        return 0;
    INode *basedcl = nameUseGetDcl((NameUseNode*)node->objfn);
    if (basedcl == NULL)
        return 0;
    Namespace *namespace;
    // A generic module has no members of its own to reach: each instance has
    // its copy, the only one compiled, so a path through it names the instance,
    // 'stack[i64].push' -- which is collapsed at type check, once it exists
    // (fnCallModuleInstancePath). Inside its own body its bare name means the
    // instance being defined, as a generic type's does, and the clone points
    // the member at the instance's
    if (basedcl->tag == ModuleTag && ((ModuleNode*)basedcl)->genericinfo
        && (ModuleNode*)basedcl != pstate->mod) {
        errorMsgNode(node->objfn, ErrorGenModBare,
            "%s is a generic module: its members belong to each instance, named with type arguments as %s[...].%s.",
            &inodeGetName(basedcl)->namestr, &inodeGetName(basedcl)->namestr,
            &((NameUseNode*)node->methfld)->namesym->namestr);
        return 0;
    }
    if (basedcl->tag == ModuleTag)
        namespace = &((ModuleNode*)basedcl)->namespace;
    else if (basedcl->tag == StructTag)
        namespace = &((StructNode*)basedcl)->namespace;
    else if (basedcl->tag == ModTraitTag)
        namespace = &((ModTraitNode*)basedcl)->namespace;
    else
        return 0;  // a value: this '.' is a member access, and type check binds it

    NameUseNode *member = (NameUseNode*)node->methfld;
    member->dclnode = namespaceFind(namespace, member->namesym);
    // An enum that extends another holds its copies of the base's variants only
    // once it is resolved, which may not have happened yet. Asked only for a name
    // not found: a variant the enum declares is bound at parse, and demanding
    // the enum for it would close a cycle through a base variant that names it.
    int complete = 1;
    if (member->dclnode == NULL && basedcl->tag == StructTag) {
        complete = structEnumDemandSet(pstate, (StructNode*)basedcl);
        member->dclnode = namespaceFind(namespace, member->namesym);
    }
    if (member->dclnode == NULL && !complete) {
        errorMsgNode((INode*)member, ErrorCircular,
            "%s is not complete until this is resolved, so its variants cannot be named here: each depends on the other.",
            &inodeGetName(basedcl)->namestr);
        return 0;
    }
    if (member->dclnode == NULL) {
        errorMsgNode((INode*)member, ErrorUnkName,
            "The name %s does not refer to a declared name", &member->namesym->namestr);
        return 0;
    }
    member->flags |= FlagQualified;
    // What the path reaches is marked as a bare name's would be, since
    // nameUseNameRes returns at once for a use that arrives bound
    nameUseMarkExpandReached(pstate, member);

    // A private name belongs to the module that declares it, and naming a path
    // through that module reaches past it. Refusing it here is what
    // refmodule.html says, and is also the only answer generation can honour:
    // it emits no symbol for a private declaration of a module whose bodies
    // this compile does not generate, so the call site would otherwise be left
    // with nothing to call. A private candidate selected through a *public*
    // overload name is untouched by this, because the program never names it.
    //
    // A type's own namespace is measured by the module that owns the type, so
    // 'modulesyms.Gadget.make' is judged against modulesyms, one hop back.
    //
    // The declaration stays attached after the diagnostic: it is the one the
    // program asked for, and leaving the use unresolved would only hand the
    // next pass a null to trip over.
    ModuleNode *qualmod = dclInfoGetModule(basedcl);
    if (qualmod && qualmod != pstate->mod && inodeIsPrivate(member->dclnode))
        errorMsgNode((INode*)member, ErrorNotPublic,
            "%s is private to its module and may not be named from outside it.",
            &member->namesym->namestr);

    // A method of a trait or an enum is a template: each implementer or variant
    // owns a clone of it, and the abstraction's own copy is never generated
    // (genlGlobalSyms), so a path naming it called or borrowed a null. A static
    // function is the abstraction's own and is reached exactly this way. An
    // overload name is refused when a call through it could select a method.
    if (basedcl->tag == StructTag && (basedcl->flags & TraitType)
        && fnCallNamesMethod(member->dclnode))
        errorMsgNode((INode*)member, ErrorAbstractMeth,
            "%s names a method of %s, which has no code of its own for it: each implementer or variant has its own copy. Call it on a value, or name it through a type that has it.",
            &member->namesym->namestr, &inodeGetName(basedcl)->namestr);
    // A module trait's members are all templates in the same way: a requirement
    // has no body, and each module conforming to the trait owns and generates
    // its own copy of a default, function or global
    if (basedcl->tag == ModTraitTag)
        errorMsgNode((INode*)member, ErrorAbstractMeth,
            "%s names a member of module trait %s, which has no code or storage of its own for it: each module conforming to it has its own. Name it through such a module.",
            &member->namesym->namestr, &inodeGetName(basedcl)->namestr);

    if (node->args == NULL) {
        *((INode**)nodep) = (INode*)member;
        return 1;
    }
    node->objfn = (INode*)member;
    node->methfld = NULL;
    return 0;
}

// Name resolution on 'fncall'
// - If node is indexing on a type, retag node as a typelit
// Note: this never name resolves .methfld, which is handled in type checking --
// except for a member that turns out to be a namespace hop, which is this
// pass's to bind (fnCallNameResPath)
void fnCallNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;

    // 'xs.parallel().sum()': a reduction called on a parallel view is a call of the
    // actors package's function, made once its parts are resolved (pareach.c)
    int reduction = parallelReduceIs(node);

    // Name resolve objfn so we know what it is to vary subsequent processing
    inodeNameRes(pstate, &node->objfn);

    // A '.' through a module or a type is a path, and collapses here
    if (fnCallNameResPath(pstate, nodep))
        return;
    node = *nodep;

    // Name resolve arguments/statements
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            inodeNameRes(pstate, argsp);
    }

    if (reduction) {
        parallelReduceNameRes(pstate, nodep);
        return;
    }
    // '(lo < hi).parallel()': a number range has no view yet, and is refused with the reason
    if (parallelRangeIs(node)) {
        parallelRangeNameRes(pstate, nodep);
        return;
    }

    // 'Array[f32, 3]' is the array type, lowered here rather than at type check
    // because name resolution's own type-or-value votes ask isTypeNode of it.
    // 'Array[T]' with the element alone is the body of a run-time length, the
    // generic struct core declares: an ordinary instance, not lowered here.
    if ((node->flags & FlagIndex) && node->methfld == NULL
        && isNameUseNode(node->objfn) && nameUseGetDcl((NameUseNode*)node->objfn) == (INode*)arrayTypeDcl
        && !(arrayTypeDcl->genericinfo != NULL && !(node->flags & FlagRange)
            && node->args != NULL && node->args->used == 1))
        arrayTypeLower(pstate, (INode**)nodep);
}

// Is an lval operator's receiver already a reference? Then it is passed as it
// is, the way a named method call takes a reference receiver, rather than
// borrowed again into a reference to a reference.
static int fnCallIsRefReceiver(INode *objtype) {
    return objtype->tag == RefTag || objtype->tag == VirtRefTag || objtype->tag == ArrayRefTag;
}

// The type an operator-assign looks its operator up on: the receiver's own
// type, or, when the receiver is a reference, the type it refers to. NULL when
// neither declares methods, and the operator-assign lowering does not own the call.
static INode *fnCallOpAssgnMethodType(INode *objtype) {
    if (objtype->tag == RefTag)
        objtype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);
    return isMethodType(objtype) ? objtype : NULL;
}

// Can a range index this receiver? A one-dimensional array, a slice, or a
// reference to either -- which is what a borrow of one makes the receiver.
static int fnCallRangeReceiver(INode *objtype) {
    if (objtype->tag == RefTag)
        objtype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);
    if (objtype->tag == ArrayTag)
        return ((ArrayNode *)objtype)->dimens->used == 1;
    return objtype->tag == ArrayRefTag || objtype->tag == ArrayDerefTag;
}

// We have an object that is an array, arrayref, ptr, or reference to an array
// We won't arrive here if a method or field was specified
// We can indexing into array or borrow reference to the indexed element
void fnCallArrIndex(FnCallNode *node) {
    if (!(node->flags & FlagIndex)) {
        errorMsgNode((INode *)node, ErrorBadIndex, "Indexing not supported on a value of this type.");
        return;
    }

    // A range, 'x[a..<b]', is a slice of part of the array only when borrowed.
    // Unborrowed it would copy or fill a segment (refarrayref.html, "Copy or
    // Fill Elements"), which is not implemented.
    if ((node->flags & FlagRange) && !(node->flags & FlagBorrow)) {
        errorMsgNode((INode *)node, ErrorBadIndex,
            "A range makes a slice when borrowed, as &x[a..<b]; copying or filling a segment of an array is not implemented");
        node->vtype = errorType;
        return;
    }

    // Correct number of indices? A range has its start and, unless it runs to
    // the end, its end
    INode *objtype = iexpGetTypeDcl(node->objfn);
    uint32_t nexpected = objtype->tag == ArrayTag ? ((ArrayNode*)objtype)->dimens->used : 1;
    uint32_t nargs = node->args? node->args->used : 0;
    if (node->flags & FlagRange)
        nexpected = nargs;
    if (nargs != nexpected) {
        errorMsgNode((INode *)node, ErrorBadIndex, "Incorrect number of indexing arguments");
        return;
    }
    // Ensure all indices are integers
    INode **indexp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, indexp)) {
        INode *indextype = iexpGetTypeDcl(*indexp);
        if (objtype->tag == PtrTag) {
            // Pointer supports signed or unsigned integer index
            int match = NoMatch;
            if (indextype->tag == UintNbrTag)
                match = iexpCoerce(indexp, (INode*)usizeType);
            else if (indextype->tag == IntNbrTag)
                match = iexpCoerce(indexp, (INode*)isizeType);
            if (!match)
                errorMsgNode((INode *)node, ErrorBadIndex, "Pointer index must be an integer");
        }
        else {
            // All other array types only support unsigned (positive) integer indexing
            int match = NoMatch;
            if (indextype->tag == UintNbrTag || (*indexp)->tag == ULitTag)
                match = iexpCoerce(indexp, (INode*)usizeType);
            if (!match)
                errorMsgNode((INode *)node, ErrorBadIndex, "Array index must be an unsigned integer");
        }
    }

    // Capture the element type returned
    switch (objtype->tag) {
    case ArrayTag:
        node->vtype = arrayElemType(objtype);
        break;
    case RefTag: {
        // Resolve the pointee, exactly as fnCallTypeCheck did when it decided
        // this call was an index at all. Reading the tag off the unresolved
        // node instead made '&Alias' -- a reference to an alias of an array --
        // match neither arm, so a valid index was left with no element type and
        // reported as a return-type mismatch two lines later.
        INode *vtype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);
        if (vtype->tag == ArrayTag)
            node->vtype = arrayElemType(vtype);
        else if (vtype->tag == ArrayDerefTag)
            node->vtype = ((RefNode*)vtype)->vtexp;
        else {
            // fnCallTypeCheck resolved the same pointee to decide this call was
            // an index, and reaches here only for an array or a slice
            errorUnreachable((INode*)node, "an index through a reference to something that is not an array or slice");
            return;
        }
        break;
    }
    case ArrayRefTag:
        node->vtype = ((RefNode*)objtype)->vtexp;
        break;
    case PtrTag:
        node->vtype = ((StarNode*)objtype)->vtexp;
        break;
    default:
        // fnCallTypeCheck calls this from its array, slice, reference and
        // pointer arms only, switching on this same receiver type
        errorUnreachable((INode*)node, "an index on a receiver type fnCallTypeCheck does not index");
        return;
    }

    // If we are borrowing a reference to indexed element, fix up type. A range's
    // borrow is a slice of the elements it spans, with the same permission and
    // lifetime an element's borrow would have.
    if (node->flags & FlagBorrow) {
        assert(objtype->tag == RefTag || objtype->tag == ArrayRefTag);
        uint16_t reftag = (node->flags & FlagRange) ? ArrayRefTag : RefTag;
        RefNode *refnode = newRefNodeFull(reftag, (INode*)node, borrowRef, ((RefNode*)objtype)->perm, node->vtype);
        // An element of what a borrow points at lives exactly as long as the borrow
        // does, so it inherits its lifetime. borrowTypeCheck sets the scope on the
        // receiver it built; newRefNode defaults to 0, which means global, so
        // leaving it would let '&mut a[1]' on a local be returned from a function
        // while '&mut (p.x)' on the same local is refused.
        refnode->scope = ((RefNode*)objtype)->scope;
        node->vtype = (INode*)refnode;
    }
    node->tag = ArrIndexTag;
}

// Is this the type of a borrowed reference, whose scope is a lifetime?
static int fnCallIsBorrowType(INode *type) {
    return iexpIsBorrowType(type);
}

FnCallNode *fnCallSetIndex = NULL;

FnCallNode *fnCallSetIndexRoot(INode *lval) {
    while (lval->tag == FnCallTag) {
        FnCallNode *call = (FnCallNode*)lval;
        if ((call->flags & FlagIndex) && call->methfld == NULL)
            return (call->flags & (FlagBorrow | FlagRange)) ? NULL : call;
        // Only a field read, 'x.f', leads on to the place's root
        if (call->methfld == NULL || !isNameUseNode(call->methfld) || call->args != NULL
            || (call->flags & (FlagOperator | FlagIndex)))
            return NULL;
        lval = call->objfn;
    }
    return NULL;
}

// The narrowest lifetime among the borrowed-reference arguments whose borrows
// a value of the type 'wanted', the result, may hold, as the highest scope
// number: 0 when there is none. What a call may store through a writable
// argument is the loan walk's (pwCallStores).
// Every borrowed reference in a signature written without a lifetime shares
// one, and every one written with a name shares it with the others written
// with that name, and with those its 'where' clause orders shorter, and with
// nothing else (doc/reference/reflifefn.html): so 'wanted' may hold the borrows
// of an argument whose parameter's own lifetime flows to it (lifeCarry), or,
// where only what that points at holds one that does, the borrows held there,
// and the only lifetime those have in common is the shortest. What a borrowed
// struct holds has no scope here: the loan walk follows it.
static uint16_t fnCallNarrowestBorrowScope(FnCallNode *node, FnSigNode *fnsig, INode *wanted) {
    if (fnsig && !fnsig->lifenamed)
        fnsig = NULL;
    uint16_t narrowest = 0;
    INode **argsp;
    uint32_t cnt;
    uint32_t i = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        INode *argtype = iexpGetTypeDcl(*argsp);
        uint32_t at = i++;
        if (!fnCallIsBorrowType(argtype))
            continue;
        uint32_t slots;
        int carry = fnsig == NULL || at >= fnsig->parms->used ? LifeCarryWhole
            : lifeCarry(fnsig, ((IExpNode*)nodesGet(fnsig->parms, at))->vtype, wanted, &slots);
        uint16_t scope = 0;
        if (carry == LifeCarryWhole)
            scope = ((RefNode*)argtype)->scope;
        else if (carry == LifeCarryHeld) {
            INode *held = itypeGetTypeDcl(((RefNode*)argtype)->vtexp);
            if (fnCallIsBorrowType(held))
                scope = ((RefNode*)held)->scope;
        }
        if (scope > narrowest)
            narrowest = scope;
    }
    return narrowest;
}

// A parameter whose reference is written ''static' takes only a global borrow.
// A variable holding a borrow its type does not record is the loan walk's to
// check (pwCall).
static void fnCallStaticArgs(FnCallNode *node, FnSigNode *fnsig) {
    INode **argsp;
    uint32_t cnt;
    uint32_t i = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        if (i >= fnsig->parms->used)
            break;
        INode *argtype = iexpGetTypeDcl(*argsp);
        INode *parmtype = ((IExpNode*)nodesGet(fnsig->parms, i))->vtype;
        ++i;
        if (!fnCallIsBorrowType(argtype) || ((RefNode*)argtype)->scope == 0)
            continue;
        char *lives = ((RefNode*)argtype)->scope == 1 ? "a borrow this function's caller lent" : "a value of this function";
        if (lifeIsStatic(parmtype))
            errorMsgNode(*argsp, ErrorCallEscape,
                "This parameter's lifetime is ''static', so the borrow handed to it must be global: this one lives only as long as %s.",
                lives);
        // A type parameter's ''static' bound makes the parameter's own
        // reference global in the instance (lifetime.h, "Lifetime bounds")
        else if (fnsig->lifestatic && lifeIsOwnBorrow(parmtype) && lifePartStatic(fnsig, parmtype, LifePartOwn)) {
            Name *tparm = lifeStaticBoundOf(fnsig, parmtype);
            errorMsgNode(*argsp, ErrorLifetimeBound,
                "The type parameter %s is bounded by ''static' ('%s + 'static'), so the borrow handed in for it must be global: this one lives only as long as %s.",
                tparm ? &tparm->namestr : "here", tparm ? &tparm->namestr : "T", lives);
        }
    }
}

// A copy of a returned borrowed reference's type belonging to this call site,
// carrying the lifetime this call gives it. The declared return type is one
// node shared by every call, so the scope cannot be written there.
static INode *fnCallScopedBorrow(FnCallNode *node, INode *rettype, uint16_t scope) {
    RefNode *retref = (RefNode*)rettype;
    RefNode *callref = newRefNodeFull(rettype->tag, (INode*)node, borrowRef, retref->perm, retref->vtexp);
    callref->scope = scope;
    return (INode*)callref;
}

// Several returned values need the same per-call-site treatment element by
// element, because a multi-value assignment checks each returned borrow against
// the lifetime of its own lval. Elements that are not borrows are shared with
// the declaration's tuple, which nothing writes a scope onto.
static void fnCallScopeRetTuple(FnCallNode *node, TupleNode *rettuple, uint16_t scope) {
    INode **elemp;
    uint32_t cnt;
    int anyborrow = 0;
    for (nodesFor(rettuple->elems, cnt, elemp))
        anyborrow |= fnCallIsBorrowType(itypeGetTypeDcl(*elemp));
    if (!anyborrow)
        return;
    TupleNode *calltuple = newTupleNode(rettuple->elems->used);
    calltuple->tag = TTupleTag;
    copyNodeLex((INode*)calltuple, (INode*)node);
    for (nodesFor(rettuple->elems, cnt, elemp)) {
        INode *elemtype = itypeGetTypeDcl(*elemp);
        nodesAdd(&calltuple->elems, fnCallIsBorrowType(elemtype)
            ? fnCallScopedBorrow(node, elemtype, scope) : *elemp);
    }
    node->vtype = (INode*)calltuple;
}

// Does this type-checked expression call a function declared to return 'Never'?
// Such a call does not return, so it may end a block as a 'return' does.
int fnCallIsNever(INode *node) {
    return node->tag == FnCallTag && itypeIsNever(((FnCallNode *)node)->vtype);
}

// A virtual dispatch's receiver that is an owning virtual reference ('So[Trait]',
// Trait an open trait) is lent to the method as a borrowed one, with the permission the
// method declares for 'self', as a plain owner is lent to a 'self &' or 'self
// &mut' method: a recast that flow analysis reads as a borrow of what the owner
// points at (pwOwnedLent), so the owner stays, frozen while the call uses it.
// Passed as it is, the owner was moved into the call. A 'self &uni' method
// is lent nothing: the recast to a move type would still move the owner.
static void fnCallLendVirtOwner(INode **selfp, INode *parmtype) {
    RefNode *owner = (RefNode*)iexpGetTypeDcl(*selfp);
    RefNode *parm = (RefNode*)itypeGetTypeDcl(parmtype);
    if (owner->tag != VirtRefTag || itypeGetTypeDcl(owner->region) == borrowRef || parm->tag != RefTag)
        return;
    RefNode *lent = newRefNodeFull(VirtRefTag, *selfp, borrowRef, parm->perm, owner->vtexp);
    if (itypeIsMove((INode*)lent))
        return;
    iexpCoerce(selfp, (INode*)lent);
}

// At this point, we have a properly-lowered function call. objfn could be:
// - nameuse to a function dcl
// - an indirect ref/ptr to a function
// - a de-reffed ref/ptr to a function
// From the function's signature, we want to pick up the return type
// and ensure that all arguments are specified and coerced to the right types
static INode *fnCallBrandPathTake(FnCallNode *call);

void fnCallFinalizeArgs(TypeCheckState *pstate, FnCallNode *node) {
    FnSigNode *fnsig = (FnSigNode*)iexpGetDerefTypeDcl(node->objfn);
    assert(fnsig->tag == FnSigTag);

    // An init fills memory that holds no value yet, which only a construction
    // has: called on a value, it would write over one without finalizing it
    INode *initdcl = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->dclnode : NULL;
    if (initdcl && initdcl->tag == FnDclTag && fnDclIsInit((FnDclNode*)initdcl)) {
        INode *owner = inodeGetOwner(initdcl);
        errorMsgNode((INode*)node, ErrorInitCall,
            "An init runs only in a construction, which gives it memory to fill: 'new %s(...)'.",
            owner && owner->tag == StructTag ? &((StructNode*)owner)->namesym->namestr : "T");
    }

    // A GPU has no operating system and no C library: a function defined
    // elsewhere is a call to code that is not there. The exceptions are the C
    // library's math, which a GPU lowers to its own instructions, and core's
    // own, whose 'panic' a GPU records (genlGpuPanic). A C binding's own
    // inline functions call what they wrap, and are refused where a kernel
    // calls them, at generation (genlFnCallInternal)
    if (flowGpuRefuses((INode*)node) && initdcl && initdcl->tag == FnDclTag
        && flowGpuExternRefused((FnDclNode*)initdcl, fnsig->parms->used)) {
        ModuleNode *callermod = pstate->fn ? dclInfoGetModule((INode*)pstate->fn) : NULL;
        if (callermod == NULL || !(callermod->dclinfo.facts & DclCName))
            errorMsgNode((INode*)node, ErrorGpuUnavailable, flowGpuExternMsg,
                &((FnDclNode*)initdcl)->namesym->namestr);
    }

    // Establish the return type of the function call (or error if not what was expected)
    if (node->vtype != unknownType && !itypeIsSame(fnsig->rettype, node->vtype)) {
        errorMsgNode((INode*)node, ErrorNoMeth, "Type of call's returned value does not match what is expected");
    }
    node->vtype = fnsig->rettype;

    // Ensure we have enough arguments, based on how many expected
    int argsunder = fnsig->parms->used - node->args->used;
    if (argsunder < 0) {
        errorMsgNode((INode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
        return;
    }

    // Coerce provided arguments to expected types. Where the signature names
    // an invariant lifetime, the arguments bind each to the brand they carry,
    // one brand per name: an arena and a key of another arena's are refused
    // here (lifeBrandsCoerce).
    LifeBind *brands = lifeSigHasBrands(fnsig) ? lifeBindBegin(fnsig) : NULL;
    INode *brandpath = lifeInvariantSeen ? fnCallBrandPathTake(node) : NULL;
    if (brands && brandpath)
        lifeBindUse(brands, itypeGetTypeDcl(brandpath), brandpath, (INode*)node);
    // A generic function's instance names its arguments' brands by place: the
    // type arguments it was called with give them, 'mem.readRaw[T](p)'
    if (brands && isNameUseNode(node->objfn) && ((NameUseNode*)node->objfn)->lifeuse
        && ((NameUseNode*)node->objfn)->lifeuse->typeargs && ((NameUseNode*)node->objfn)->dclnode)
        lifeBindArgs(brands, itypeInstanceTypeArgs(((NameUseNode*)node->objfn)->dclnode),
            ((NameUseNode*)node->objfn)->lifeuse->typeargs, (INode*)node);
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(fnsig->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        if (cnt == node->args->used && (node->flags & FlagVDisp))
            fnCallLendVirtOwner(argsp, ((IExpNode*)*parmp)->vtype);
        // Make sure the type matches (and coerce as needed)
        // (but not for vref as self)
        // (A string literal wanted as a read-only borrow of a type declaring
        // 'fromLiteral' is lent as a temporary of it: iexpCoerceIn.)
        if (!iexpCoerceIn(pstate, argsp, ((IExpNode*)*parmp)->vtype)
            && !(cnt == node->args->used && (node->flags & FlagVDisp))) {
            // A lock-managed reference lends nothing but through a borrow
            INode *argtype = iexpGetTypeDcl(*argsp);
            INode *parmtype = iexpGetTypeDcl(*parmp);
            if (argtype->tag == RefTag && permIsLock(((RefNode*)argtype)->perm)
                && parmtype->tag == RefTag && ((RefNode*)parmtype)->region == borrowRef)
                permLockRefused(*argsp, ((RefNode*)argtype)->perm, "lend the value");
            else {
                int whycode;
                char *why = fnSigCallRefusal(argtype, parmtype, &whycode);
                if (why)
                    errorMsgNode(*argsp, whycode, "%s", why);
                else
                    errorMsgNode(*argsp, ErrorInvType, "Expression's type does not match declared parameter");
            }
        }
        parmp++;
    }
    if (brands) {
        lifeBindEnd(brands);
        lifeBindClose(brands, (INode*)node);
    }

    // If we have too few arguments, use default values, if provided
    if (argsunder > 0) {
        if (((VarDclNode*)*parmp)->value == NULL)
            errorMsgNode((INode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
        else {
            while (argsunder--) {
                // One default value node serves every call that takes it,
                // except 'srcFile()' and 'srcLine()', which answer where the
                // call taking them is: each such call gets a copy of its own,
                // placed there
                INode *dflt = ((VarDclNode*)*parmp)->value;
                if (intrinsicIsSrcDefault(dflt))
                    dflt = intrinsicSrcCallAt(dflt, (INode*)node);
                // A string literal for a type declaring 'fromLiteral' is made
                // here, a copy of it coerced as a literal written as this
                // call's argument is: the temporary of its own, dropped with
                // this call's statement (or the value, by value)
                else if (slitDefaultDeferred(dflt, ((IExpNode*)*parmp)->vtype)) {
                    dflt = cloneSLitNode((SLitNode*)dflt);
                    if (!iexpCoerceIn(pstate, &dflt, ((IExpNode*)*parmp)->vtype))
                        errorMsgNode((INode*)node, ErrorInvType,
                            "This parameter's default is a string literal, made into the wanted type where the call is, which needs a function's body: a global's initializer or a constant has no statement for the temporary.");
                }
                nodesAdd(&node->args, dflt);
                parmp++;
            }
        }
    }

    // An atomic intrinsic's orderings are read where it is called, so are
    // checked there, lowered or not
    INode *callee = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->dclnode : node->objfn;
    if (callee && callee->tag == FnDclTag && (((FnDclNode*)callee)->dclinfo.facts & DclIntrinsic))
        intrinsicCallCheck(node, (FnDclNode*)callee);

    // A returned borrowed reference lives as long as the narrowest borrow the
    // call was handed. The declared return type is one node shared by every
    // call site, so the scope goes on a type node of the call's own, exactly as
    // fnCallArrIndex builds one for an element borrow; the lifetime checks in
    // assignlvalrtype and returnFlowEscape then read it from there. A call
    // returning several values gets a tuple of its own on the same terms.
    // With no borrowed argument the declaration's own global scope stands, and
    // so it does for a result whose lifetime no parameter shares: ''static',
    // or a name no parameter is given.
    if (fnsig->lifenamed)
        fnCallStaticArgs(node, fnsig);
    uint16_t narrowest = fnCallNarrowestBorrowScope(node, fnsig, fnsig->rettype);
    INode *rettype = itypeGetTypeDcl(fnsig->rettype);
    if (narrowest != 0) {
        if (fnCallIsBorrowType(rettype))
            node->vtype = fnCallScopedBorrow(node, rettype, narrowest);
        else if (rettype->tag == TTupleTag)
            fnCallScopeRetTuple(node, (TupleNode*)rettype, narrowest);
    }

    // The result's invariant lifetimes are the brands the arguments bound, and
    // one no parameter names is minted here, fresh for this call site
    if (brands) {
        node->vtype = lifeBrandSubst(node->vtype, brands, (INode*)node);
        lifeKeyBorrow(node->vtype, (INode*)node);
    }
}

// objfn is a function or a pointer to one. Make sure it is called correctly.
void fnCallFnSigTypeCheck(TypeCheckState *pstate, FnCallNode *node) {
    if ((node->flags & FlagIndex) || node->methfld != NULL) {
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "A function may not be called using indexing or a method.");
        return;
    }
    fnCallFinalizeArgs(pstate, node);
}

Name *fnCallOpEqMethod(Name *opeqname) {
    if (opeqname == plusEqName) return plusName;
    if (opeqname == minusEqName) return minusName;
    if (opeqname == multEqName) return multName;
    if (opeqname == divEqName) return divName;
    if (opeqname == remEqName) return remName;
    if (opeqname == orEqName) return orName;
    if (opeqname == andEqName) return andName;
    if (opeqname == xorEqName) return xorName;
    if (opeqname == shlEqName) return shlName;
    if (opeqname == shrEqName) return shrName;
    return NULL;
}

// Lower integer field index for tuple
int fnCallLowerIntField(FnCallNode *callnode) {
    if (callnode->methfld == NULL || callnode->methfld->tag != ULitTag || callnode->args != NULL)
        return 0;
    // Resolved, since the object's type may be a name standing for the tuple
    TupleNode* ttuple = (TupleNode*)iexpGetTypeDcl(callnode->objfn);
    uint64_t index = ((ULitNode*)callnode->methfld)->uintlit;
    if (index >= (uint64_t)ttuple->elems->used)
        return 0;
    callnode->vtype = nodesGet(ttuple->elems, index);
    callnode->tag = FldAccessTag;
    return 1;
}

// A number after '.' is not a name, so it must not reach the method and field
// lookups, which take one. A reference or pointer to a tuple reaches the element
// through what it points at, as a field is reached through a reference to a
// struct; a receiver whose dispatch would look the number up as a name is told
// it has no numbered elements. Answer 0 to leave the node to the dispatch: a
// member that is a name, a tuple itself, or a receiver the dispatch refuses
// on its own.
static int fnCallLowerRefIntField(FnCallNode *callnode, INode *objtype) {
    if (callnode->methfld == NULL || callnode->methfld->tag != ULitTag)
        return 0;
    switch (objtype->tag) {
    case RefTag:
    case PtrTag: {
        INode *held = itypeGetTypeDcl(objtype->tag == RefTag
            ? ((RefNode*)objtype)->vtexp : ((StarNode*)objtype)->vtexp);
        if (held->tag != TTupleTag)
            break;
        derefInject(&callnode->objfn);
        if (fnCallLowerIntField(callnode) == 0)
            errorMsgNode((INode*)callnode, ErrorNoMeth, "Invalid expression on a tuple");
        return 1;
    }
    case StructTag:
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
    case ArrayRefTag:
    case VirtRefTag:
        break;
    default:
        return 0;
    }
    errorMsgNode((INode*)callnode, ErrorNoMbr,
        "A number after '.' names a tuple's element, and this is not a tuple or a reference to one.");
    callnode->vtype = errorType;
    return 1;
}

// Report why the name the caller used selected no single candidate.
// 'kind' names what the name declares, for a call ("function") or a method call ("method").
static void fnCallNoCandidate(INode *callnode, enum OverloadMatch status, Name *namesym, char *kind) {
    if (status == OverloadAmbiguous && iNsTypeAmbiguous[0] && iNsTypeAmbiguous[1])
        errorMsgNode(callnode, ErrorAmbigCandidate,
            "More than one %s declared by `%s` accepts these arguments (`%s` and `%s`). Call a concrete name or convert the arguments.",
            kind, &namesym->namestr, &iNsTypeAmbiguous[0]->namesym->namestr, &iNsTypeAmbiguous[1]->namesym->namestr);
    else if (status == OverloadAmbiguous)
        errorMsgNode(callnode, ErrorAmbigCandidate,
            "More than one %s declared by `%s` accepts these arguments. Call a concrete name or convert the arguments.",
            kind, &namesym->namestr);
    else
        errorMsgNode(callnode, ErrorNoCandidate,
            "No %s declared by `%s` accepts the call's arguments.", kind, &namesym->namestr);
}

// '&x[i]' hands the type's '&[]' a read-only borrow of 'x' as its receiver,
// which a '&[]' declaring 'self &mut' refuses. That refusal is the rule; what
// this adds is the reason and the spellings that work, reported in place of the
// bare no-candidate message where a '&mut' receiver would have been accepted.
// The probe alters nothing. Answer whether it reported.
static int fnCallRefIndexWantsMut(FnCallNode *callnode, INode *foundnode, Name *methsym, enum OverloadMatch status) {
    if (status != OverloadNone || methsym != refIndexName || !(callnode->flags & FlagIndex))
        return 0;
    INode *recvtype = iexpGetTypeDcl(callnode->objfn);
    if (recvtype->tag != RefTag || (permGetFlags(((RefNode*)recvtype)->perm) & MayWrite))
        return 0;
    INode *mutrecvr = newBorrowMutRef(callnode->objfn, ((RefNode*)recvtype)->vtexp, newPermUseNode(mutPerm));
    enum OverloadMatch mutstatus;
    if (iNsTypeFindMethod(foundnode, &mutrecvr, callnode->args, &mutstatus) == NULL)
        return 0;
    errorMsgNode((INode*)callnode, ErrorNoCandidate,
        "`&x[i]` hands `&[]` a read-only receiver, and this type's `&[]` takes `self &mut`. "
        "Write `x[i]` to read the element, or `&mut x[i]` to borrow it through a mutable receiver.");
    return 1;
}

// An operator or an index wanting a number, given a bool, which never coerces to
// one: 'n + b', '1u8 == b' and 'list[b]' select nothing. That refusal is the
// rule; this reports it as the bool coercion it is, naming the conversion, in
// place of the bare no-candidate message, where a candidate declares a number
// in that argument's place. Answer whether it reported.
static int fnCallBoolOperandWantsNumber(FnCallNode *callnode, INode *foundnode, enum OverloadMatch status) {
    if (status != OverloadNone || !(callnode->flags & (FlagOperator | FlagIndex)) || callnode->args == NULL)
        return 0;
    INode **argsp;
    uint32_t cnt;
    uint32_t argi = 0;
    for (nodesFor(callnode->args, cnt, argsp)) {
        INode *wanted;
        if (isExpNode(*argsp) && iexpGetTypeDcl(*argsp) == (INode*)boolType
            && (wanted = iNsTypeNumberParm(foundnode, argi))) {
            errorMsgNode(*argsp, ErrorBoolNotNbr,
                "A bool is not a number, and %s is wanted here. Convert it explicitly, '%s.from(b)', which gives 0 or 1.",
                itypeName(wanted), itypeName(wanted));
            return 1;
        }
        // Nor does a char reach a number, a number a char, or a non-ASCII
        // character literal a u8; the same report names the conversion
        if (isExpNode(*argsp) && (wanted = iNsTypeNumberParm(foundnode, argi))
            && iexpCharNumberMismatch(*argsp, wanted))
            return 1;
        ++argi;
    }
    return 0;
}

// Find the one field or method that accepts the call's receiver and arguments,
// then lower the node to a function call (objfn+args) or field access (objfn+methfld).
// A receiver held through a reference or pointer is dereferenced where the selected
// method declared 'self' by value; a receiver held as a value is borrowed where it
// declared 'self &' or 'self &mut'; and an operator written on a pointer does not
// reach through at all.
// The access reaching field 'fld' on 'obj', positioned on 'lexnode'. For a
// declared field that is one field access; for a folded copy it is an access
// per hop, root first, and then one for the copy itself -- the nesting the
// hand-written path 'obj.hop.field' produces, so that borrowing, permissions
// and generation see the true target. The copy carries the index, type and
// permission of the field it stands for, so the access naming it is generated
// as an access to that field.
INode *fnCallFieldAccess(INode *obj, FieldDclNode *fld, INode *lexnode) {
    if (fld->hop)
        obj = fnCallFieldAccess(obj, fld->hop, lexnode);
    derefInject(&obj);  // reach through a reference or pointer, as any field access does
    FnCallNode *access = newFnCallLower(lexnode, obj, 0);
    access->methfld = newNameUseFromDclNode((INode*)fld, lexnode);
    access->vtype = lifeBrandField(((IExpNode*)obj)->vtype, fld->vtype, lexnode);
    access->tag = FldAccessTag;
    return (INode*)access;
}

// Rule 1: reaching a name analyzes its declaration, and a member name reaches
// every candidate it declares. Selection compares each candidate's signature
// with the receiver and arguments, so the signature has to be type checked
// first. A method of the type whose own method is making the call may not be
// yet: the type checks its methods in order, so one declared later -- or spliced
// in after the type's own, as an enum's methods are into each variant -- is
// still waiting, and its unchecked signature accepted nothing. A bare call has
// always had this through its name use; 'self.name()' now has it too.
//
// An overload name's candidates are free functions of a module as often as
// members of a type, and one declared later in its module is as unchecked: its
// parameters may still be compared, but a return type such as 'List[Vec2]' is
// still the generic call it was written as.
//
// The walk state is the candidate's own type's (Rule 8): the caller may be a
// method of some other type, and fnDclTypeCheck compares a method's self with
// the type it is checked under. A module's function is checked with no type
// around it, as the module's own walk checks it. A candidate already analyzed,
// or under way and so with its signature checked, is left alone, and so is a
// method of a number type: corenumber builds those typed, with intrinsic
// bodies, and nothing ever type checks them.
void fnCallDemandCandidates(INode *binding) {
    INode **candp;
    uint32_t cnt;
    if (binding->tag == FnDclTag) {
        candp = &binding;
        cnt = 1;
    }
    else if (binding->tag == FnOverloadDclTag) {
        Nodes *overloads = ((FnOverloadDclNode*)binding)->overloads;
        candp = &nodesGet(overloads, 0);
        cnt = overloads->used;
    }
    else
        return;
    while (cnt--) {
        INode *cand = *candp++;
        INode *owner = inodeGetOwner(cand);
        if ((cand->flags & (TypeChecked | TypeChecking)) || owner == NULL
            || (owner->tag != StructTag && owner->tag != ModuleTag))
            continue;
        TypeCheckState tstate;
        tstate.typenode = owner->tag == StructTag ? owner : NULL;
        tstate.fn = NULL;
        tstate.scope = 0;
        tstate.extend = NULL;
        inodeTypeCheckAny(&tstate, &cand);
    }
}

// A receiver held as a value -- 'v.push(x)' for a 'mut v List[i64]' -- reaches
// a method that declared 'self &' or 'self &mut' by being borrowed, as though
// '(&v).len()' or '(&mut v).push(x)' had been written: "Cone will automatically
// transform 'self' to a mutable borrowed reference" (doc/reference/
// reftypesafe.html, "Implicit Coercion of 'self'"), as lval operators already
// do. Asked only when no candidate accepts the receiver as it is, so a method
// taking 'self' by value is always preferred, and never of a receiver that is
// already a reference or a pointer: a pointer is not borrowed from.
//
// The weakest borrow a candidate accepts is taken: 'ro' first, so a 'self &'
// method borrows for reading even from a mutable variable, then 'mut'. The
// borrow is made by borrowMutRef, so its permission and lifetime are the ones a
// hand-written '&mut v' gets: a 'self &mut' method on an immutable variable is
// ErrorBadPerm. A temporary is borrowed where it is, as '&' of it is, and
// lives to the end of its statement (borrowTempRef).
// Returns the selected method, with the borrow now the receiver, or NULL.
static FnDclNode *fnCallBorrowReceiver(TypeCheckState *pstate, FnCallNode *callnode, INode *foundnode,
        enum OverloadMatch *status) {
    INode *obj = callnode->objfn;
    INode *objtype = iexpGetTypeDcl(obj);
    // A reference is passed as it is, but for an owner (not a borrow) of the type: a
    // method declaring 'self &So[str]' (fnDclTypeCheck) takes the borrow of the owner
    INode *ownerof = objtype->tag == RefTag && itypeGetTypeDcl(((RefNode*)objtype)->region) != borrowRef
        ? itypeGetTypeDcl(((RefNode*)objtype)->vtexp) : NULL;
    if (ownerof ? !isMethodType(ownerof)
        : (fnCallIsRefReceiver(objtype) || objtype->tag == PtrTag || !(isMethodType(objtype) || objtype->tag == ArrayTag)))
        return NULL;
    // '(*p).push(x)' written on a pointer is the pointer's own business: the
    // borrow would be '&mut *p', a reference made from a pointer
    if (obj->tag == DerefTag && iexpGetTypeDcl(((StarNode*)obj)->vtexp)->tag == PtrTag)
        return NULL;
    // A type declaring Immutable has 'self &' as 'self &imm', which a 'ro'
    // borrow is not accepted as, so 'imm' is tried between the two
    PermNode *perms[3];
    int nperms = 0;
    perms[nperms++] = roPerm;
    if (itypeIsImmutable(objtype))
        perms[nperms++] = immPerm;
    perms[nperms++] = mutPerm;
    for (int i = 0; i < nperms; ++i) {
        INode *perm = newPermUseNode(perms[i]);
        INode *probe = newBorrowMutRef(obj, objtype, perm);
        enum OverloadMatch probestatus;
        FnDclNode *selected = iNsTypeFindMethod(foundnode, &probe, callnode->args, &probestatus);
        if (selected == NULL) {
            if (probestatus == OverloadAmbiguous) {
                *status = probestatus;
                return NULL;
            }
            continue;
        }
        if (iexpIsLval(obj))
            borrowMutRef(&callnode->objfn, objtype, perm);
        else
            borrowTempRef(&callnode->objfn, objtype, perm, borrowTempScope(pstate));
        return selected;
    }
    return NULL;
}

// An operand of an enum's '==' is lent to the comparison, read-only, where it
// lies: a value is borrowed (a temporary, to the end of its statement), and a
// reference is passed as it is. Nothing is copied or moved into the comparison,
// so an enum that moves is compared as one that copies is.
static void fnCallLendEnumOperand(TypeCheckState *pstate, INode **operandp) {
    INode *type = iexpGetTypeDcl(*operandp);
    if (fnCallIsRefReceiver(type) || type->tag == PtrTag)
        return;
    INode *perm = newPermUseNode(roPerm);
    if (iexpIsLval(*operandp))
        borrowMutRef(operandp, type, perm);
    else
        borrowTempRef(operandp, type, perm, borrowTempScope(pstate));
}

// '==' on an enum where a variant carries fields: tags first, then the variant's
// own '==' on the two payloads (doc/reference/refenum.html, "Comparing"). The
// comparison is a function the compiler gave the enum (structSetEnumEqFn), made
// when every variant that carries fields declares a '==', and the call becomes a
// call of it on the two operands lent. Where one does not, the call is refused
// here, naming the first variant that has none and what it carries.
static void fnCallLowerEnumEq(TypeCheckState *pstate, FnCallNode *callnode, StructNode *enumnode) {
    FnDclNode *eqfn = structEnumEqFn(enumnode);
    if (eqfn == NULL) {
        StructNode *lacking = structEnumVariantWithoutEq(enumnode);
        if (lacking == NULL) {
            errorUnreachable((INode*)callnode, "an enum's '==' with no variant lacking one and no comparison made");
            callnode->vtype = errorType;
            return;
        }
        char carries[200] = "";
        size_t used = 0;
        INode **fldp;
        uint32_t cnt;
        for (nodelistFor(&lacking->fields, cnt, fldp)) {
            FieldDclNode *field = (FieldDclNode*)*fldp;
            if (field->flags & (IsTagField | IsMixin) || used + 40 >= sizeof(carries))
                continue;
            used += snprintf(carries + used, sizeof(carries) - used, "%s%s %s", used ? ", " : "",
                &field->namesym->namestr, itypeName(field->vtype));
        }
        errorMsgNode((INode*)callnode, ErrorEnumEquality,
            "`==` and `!=` on %s compare the variants' own `==`, and %s carries a payload (%s) and declares none. Give %s a `==` taking another %s, or use 'match' to recover the variant.",
            &enumnode->namesym->namestr, &lacking->namesym->namestr, carries,
            &lacking->namesym->namestr, &lacking->namesym->namestr);
        callnode->vtype = errorType;
        return;
    }
    fnCallDemandCandidates((INode*)eqfn);
    fnCallLendEnumOperand(pstate, &callnode->objfn);
    fnCallLendEnumOperand(pstate, &nodesGet(callnode->args, 0));
    nodesInsert(&callnode->args, callnode->objfn, 0);
    callnode->objfn = newNameUseFromDclNode((INode*)eqfn, (INode*)callnode);
    callnode->methfld = NULL;
    callnode->vtype = unknownType;
    fnCallFinalizeArgs(pstate, callnode);
}

// The receiver of 'x[i].m()' is the element 'x[i]' lent by the type's '[]',
// a read-only borrow. Where no 'm' takes that, and the type declares '&[]'
// and 'x' may be borrowed mutably, the index is lowered again as '&mut x[i]'
// is, from its receiver and arguments as they were checked, and becomes the
// receiver: the element's mutable borrow, as an assignment's index is in set
// position (assignTypeCheck). Answers whether it did.
static int fnCallIndexAsMut(TypeCheckState *pstate, FnCallNode *callnode) {
    FnCallNode *index = (FnCallNode*)callnode->objfn;
    if (index->tag != FnCallTag || !(index->flags & FlagIndex) || (index->flags & (FlagBorrow | FlagRange))
        || index->args == NULL || index->args->used == 0 || !isNameUseNode(index->objfn))
        return 0;
    INode *recv = nodesGet(index->args, 0);
    if (recv->tag == BorrowTag)
        recv = ((RefNode*)recv)->vtexp;
    INode *recvtype = iexpGetTypeDcl(recv);
    INode *held = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)recvtype)->vtexp) : recvtype;
    if (held->tag != StructTag)
        return 0;
    INode *refindex = aliasDclResolve(iNsTypeFindFnField((INsTypeNode*)held, refIndexName));
    if (refindex == NULL || (refindex->tag != FnDclTag && refindex->tag != FnOverloadDclTag))
        return 0;
    // Only where a mutable borrow of the receiver would be allowed
    if (recvtype->tag == RefTag) {
        if (!(permGetFlags(((RefNode*)recvtype)->perm) & MayWrite))
            return 0;
    }
    else {
        INode *lvalperm = (INode*)immPerm;
        uint16_t scope;
        if (!iexpIsLval(recv) || iexpGetLvalInfo(recv, &lvalperm, &scope) == NULL
            || !(permGetFlags(lvalperm) & MayWrite))
            return 0;
    }
    FnCallNode *mutindex = newFnCallLower((INode*)index, recv, index->args->used);
    mutindex->flags |= FlagIndex | FlagBorrow;
    mutindex->methfld = (INode*)newMemberUseNode(refIndexName);
    inodeLexCopy(mutindex->methfld, (INode*)index);
    INode **argsp;
    uint32_t cnt;
    uint32_t at = 0;
    for (nodesFor(index->args, cnt, argsp)) {
        if (at++ > 0)
            nodesAdd(&mutindex->args, *argsp);
    }
    if (fnCallLowerMethod(pstate, mutindex) != 1)
        return 0;
    callnode->objfn = (INode*)mutindex;
    return 1;
}

// A virtual dispatch reaches a member through its slot in the trait's vtable,
// and the vtable holds public members only (structMakeVtable): a private member
// is not a requirement an implementer meets, so there is nothing to load for
// it. Module-wide privacy lets the trait's own module name it through any
// value, a virtual reference included, so the call is refused here, the member
// and the trait named. Not where the virtual reference is made: one that never
// reaches the private member is sound. A member already refused as not visible
// is not reported twice. The call is still lowered as it would have been, so
// its type is the member's and nothing around it reports a consequence; the
// error keeps generation, which would read the slot index, from running.
static void fnCallPrivateVtable(FnCallNode *callnode, INode *member, INode *trait, int notpublic) {
    if (!(callnode->flags & FlagVDisp) || notpublic || !inodeIsPrivate(member))
        return;
    Name *name = member->tag == FieldDclTag ? ((FieldDclNode*)member)->namesym : ((FnDclNode*)member)->namesym;
    Name *traitname = ((StructNode*)trait)->namesym;
    // An enum's method is dispatched on the variant through a plain reference
    // too (fnCallLowerTraitMethod), by the same vtable
    if (trait->flags & EnumType)
        errorMsgNode((INode*)callnode, ErrorPrivateVtable,
            "`%s` is private, so %s's vtable has no slot for it, and a call dispatched on the variant goes through that vtable. Declare it 'pub' in %s.",
            &name->namestr, &traitname->namestr, &traitname->namestr);
    else
        errorMsgNode((INode*)callnode, ErrorPrivateVtable,
            "`%s` is private, so %s's vtable has no slot for it and it cannot be reached through a virtual reference. Declare it 'pub' in %s.",
            &name->namestr, &traitname->namestr, &traitname->namestr);
}

// An integer's or bool's 'hash(self, h &mut Hasher)', which core's Hash requires
// of every type that is Hash (corenumber.c, nbrAddHashMethods). It is no function:
// the call becomes core's 'h.writeU64(bits)', the value converted to a u64, the
// sign extended for a signed integer. The receiver is the value, already
// selected, and the one argument is the hasher. Answers the method the call is
// now to, or NULL after reporting that core's Hasher lacks it.
static FnDclNode *fnCallHashNumber(FnCallNode *callnode, FnDclNode *selected) {
    FnSigNode *sig = (FnSigNode*)selected->vtype;
    VarDclNode *hparm = (VarDclNode*)nodesGet(sig->parms, 1);
    INode *hasher = itypeGetTypeDcl(((RefNode*)itypeGetTypeDcl(hparm->vtype))->vtexp);
    INode *write = hasher->tag == StructTag ? iNsTypeFindFnField((INsTypeNode*)hasher, writeU64Name) : NULL;
    if (write == NULL || write->tag != FnDclTag) {
        errorMsgNode((INode*)callnode, ErrorNoMbr,
            "core's Hasher has no `writeU64`, which an integer's `hash` feeds its bits through.");
        callnode->vtype = errorType;
        return NULL;
    }
    fnCallDemandCandidates(write);
    INode *bits = (INode*)newConvCastNode(callnode->objfn, (INode*)u64Type);
    inodeLexCopy(bits, callnode->objfn);
    callnode->objfn = nodesGet(callnode->args, 0);
    callnode->args = newNodes(1);
    nodesAdd(&callnode->args, bits);
    return (FnDclNode*)write;
}

// The body 'Array[T]' of an element type T: an instance of the generic struct
// core declares, whose methods (core.cone) an array, a reference to one and a
// slice call. NULL if core has not declared it or it could not be made.
INode *fnCallArrayBody(TypeCheckState *pstate, INode *errnode, INode *elem) {
    if (arrayTypeDcl->genericinfo == NULL)
        return NULL;
    FnCallNode *body = newFnCallNode(newNameUseFromDclNode((INode*)arrayTypeDcl, errnode), 1);
    inodeLexCopy((INode*)body, errnode);
    body->flags |= FlagIndex;
    nodesAdd(&body->args, elem);
    INode *bodytype = (INode*)body;
    if (!itypeTypeCheck(pstate, &bodytype))
        return NULL;
    INode *dcl = itypeGetTypeDcl(bodytype);
    return isMethodType(dcl) ? dcl : NULL;
}

// The body of an array's or a slice's element type, if it has a method of this
// name. NULL where the receiver is none of those or the body has none.
static INode *fnCallSliceBodyOf(TypeCheckState *pstate, FnCallNode *callnode) {
    if (callnode->methfld == NULL || !isNameUseNode(callnode->methfld) || arrayTypeDcl->genericinfo == NULL)
        return NULL;
    INode *type = iexpGetDerefTypeDcl(callnode->objfn);
    INode *elem;
    if (type->tag == ArrayRefTag)
        elem = ((RefNode*)type)->vtexp;
    else if (type->tag == ArrayTag) {
        // An array's length is its type's, not a member of the value: 'len' stays a slice's
        if (((NameUseNode*)callnode->methfld)->namesym == lenName)
            return NULL;
        elem = arrayElemType(type);
    }
    else
        return NULL;
    INode *dcl = fnCallArrayBody(pstate, (INode*)callnode, elem);
    if (dcl == NULL)
        return NULL;
    INode *found = iNsTypeFindFnField((INsTypeNode*)dcl, ((NameUseNode*)callnode->methfld)->namesym);
    return found != NULL && found->tag != StructTag && (found->flags & FlagMethFld) ? dcl : NULL;
}

static int fnCallLowerMethodOn(TypeCheckState *pstate, FnCallNode *callnode, INode *bodytype);
static void fnCallReadThroughRefs(FnCallNode *node);

// A method called on an array, a reference to one or a slice, lowered against
// core's 'Array[T]' for its element type. Answers 0, changing nothing, where
// the body has no such method.
static int fnCallLowerSliceMethod(TypeCheckState *pstate, FnCallNode *callnode) {
    INode *bodytype = fnCallSliceBodyOf(pstate, callnode);
    if (bodytype == NULL)
        return 0;
    return fnCallLowerMethodOn(pstate, callnode, bodytype);
}

// Does a value of this type copy freely: a number, bool, char, or a plain struct
// or enum with no finalizer and no owner? Reading one through a borrow is a copy
// and moves nothing, which is what lets an operator take the value of a borrow
// without a '*'. A type that moves (a String, a list, anything with a 'final'),
// a trait other than an enum, and text are never read through that way.
static int fnCallCopiesFreely(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (!isMethodType(dcl) || itypeIsMove(dcl))
        return 0;
    return !((dcl->flags & TraitType) && !(dcl->flags & EnumType));
}

// Is this type a borrow of a value that copies freely, so that an operator
// wanting the value may read through an operand of it? Only a borrow: an owner
// (So, Rc), a key and a lock-managed reference reach their value in ways of their own.
static int fnCallBorrowTypeReadsThrough(INode *type) {
    if (type->tag != RefTag)
        return 0;
    RefNode *ref = (RefNode*)type;
    if (itypeGetTypeDcl(ref->region) != borrowRef || permIsLock(ref->perm) || lifeIsKey(type) || refIsFat(ref))
        return 0;
    return fnCallCopiesFreely(ref->vtexp);
}

static int fnCallBorrowReadsThrough(INode *operand) {
    return fnCallBorrowTypeReadsThrough(iexpGetTypeDcl(operand));
}

// Returns 1 when lowered, 0 when the receiver's type supports no methods at all
// (so the caller may try another way), and -1 when a diagnostic was reported.
int fnCallLowerMethod(TypeCheckState *pstate, FnCallNode *callnode) {
    return fnCallLowerMethodOn(pstate, callnode, NULL);
}

// The same, finding the method in 'bodytype' where that is not NULL, the type
// whose methods a receiver that has none of its own (an array) reaches
static int fnCallLowerMethodOn(TypeCheckState *pstate, FnCallNode *callnode, INode *bodytype) {
    INode *obj = callnode->objfn;
    // An ASCII character literal beside a byte is that byte, whichever side it is on
    if (callnode->flags & FlagOperator)
        litAdoptCharBesideByte(&callnode->objfn, callnode->args);
    assert(isNameUseNode(callnode->methfld));
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    INode *objdereftype = bodytype ? bodytype : iexpGetDerefTypeDcl(obj);
    if (!isMethodType(objdereftype)) {
        return 0;
    }

    // 'self.m()', m one of an actor's behaviours, is a send: its receiver
    // becomes the actor's handle, whose method sends m (selfActorSend)
    int send = selfActorSend(pstate, callnode, objdereftype);
    if (send < 0)
        return -1;
    if (send > 0) {
        obj = callnode->objfn;
        if (inodeIsError(obj)) {
            callnode->vtype = errorType;
            return -1;
        }
        objdereftype = iexpGetDerefTypeDcl(obj);
    }

    // Visibility is that of the binding the caller's name reaches: a method's
    // DclPrivate bit, or the 'pub' flag of a field or an overload name. A public
    // overload name may therefore select a private concrete candidate. A name
    // that binds nothing has no visibility to refuse, and is reported missing.
    // A private member is reached through 'self': the method's own, or a macro
    // method's, which its expansion has already replaced with the use site's
    // receiver (FlagSelfRecv). Anywhere in the module that declares the type it
    // is reached through any value of the type, because the module is the
    // privacy boundary for members as for names (structSeesPrivate).
    INode *foundnode = iNsTypeFindFnField((INsTypeNode*)objdereftype, methsym);
    // A type in the namespace -- an enum's variant, or 'Self' -- is a name of the
    // type and never a member of its values, so it is reported missing below and
    // has no visibility to refuse here
    if (foundnode && foundnode->tag == StructTag)
        foundnode = NULL;
    int notpublic = foundnode && inodeIsPrivate(foundnode) && !(callnode->flags & FlagSelfRecv)
        && !(isNameUseNode(obj) && isExpNode(obj)
             && ((VarDclNode*)((NameUseNode*)obj)->dclnode)->namesym == selfName)
        && !structSeesPrivate(pstate, objdereftype);
    if (notpublic)
        errorMsgNode((INode*)callnode, ErrorNotPublic, "May not access the private method/field `%s`.", &methsym->namestr);
    // A method the type holds by folding is bound to an alias; the visibility
    // just checked was the alias's own, and everything from here on is the
    // method's. A folded field is a copy in the namespace directly.
    int folded = foundnode && foundnode->tag == AliasDclTag;
    if (folded)
        foundnode = aliasDclResolve(foundnode);
    if (!foundnode
        || !(foundnode->tag == FnDclTag || foundnode->tag == FnOverloadDclTag || foundnode->tag == FieldDclTag)
        || !(foundnode->flags & FlagMethFld)) {
        // A generic type's method this instance lacks, its 'where' clause unmet
        if (foundnode == NULL && genericReportAbsent((INode*)callnode, objdereftype, methsym))
            return -1;
        // An actor's handle carries its behaviours and nothing else: its state,
        // and its synchronous methods, run only inside it, are its own
        StructNode *state;
        INode *statemember = foundnode == NULL ? actorStateMember(objdereftype, methsym, &state) : NULL;
        if (statemember) {
            Name *actorname = ((StructNode*)objdereftype)->namesym;
            if (statemember->tag == FieldDclTag)
                errorMsgNode((INode*)callnode, ErrorNotPublic,
                    "`%s` is part of actor %s's private state, which only its own methods reach, one message at a time. Send it a behaviour that uses it.",
                    &methsym->namestr, &actorname->namestr);
            else
                errorMsgNode((INode*)callnode, ErrorNotPublic,
                    "`%s` is a synchronous method of actor %s's, run only inside the actor: its handle sends the actor's behaviours, declared 'async do', and nothing else.",
                    &methsym->namestr, &actorname->namestr);
            return -1;
        }
        // A method every Iterator shares, asked of a type that has a 'next' and does
        // not declare it is one: fitting the trait gives a bound its cursor, and the
        // shared methods come only with the declaration
        if (foundnode == NULL && objdereftype->tag == StructTag && !(objdereftype->flags & TraitType)
            && iNsTypeFindFnField((INsTypeNode*)objdereftype, nametblFind("next", 4)) != NULL && pstate->fn) {
            INode *shared = coreIteratorTrait ? iNsTypeFindFnField((INsTypeNode*)coreIteratorTrait, methsym) : NULL;
            if (shared && shared->tag == FnDclTag && ((FnDclNode*)shared)->value != NULL) {
                Name *tname = ((StructNode*)objdereftype)->namesym;
                int declared = 0;
                StructNode *tnode = (StructNode*)objdereftype;
                INode **tp;
                uint32_t tcnt;
                if (tnode->traits)
                    for (nodesFor(tnode->traits, tcnt, tp))
                        if ((*tp)->tag == StructTag && ((StructNode*)*tp)->namesym == iteratorTraitName)
                            declared = 1;
                if (declared)
                    errorMsgNode((INode*)callnode, ErrorNoMbr,
                        "`%s` is a method of Iterator that %s's items do not qualify for: it is there only where its `where` clause holds of the items (`sum`, `min` and `max` want numbers, integers, f32 or f64), so a cursor that lends is mapped to values first, `map(x => *x)`.",
                        &methsym->namestr, &tname->namestr);
                else
                    errorMsgNode((INode*)callnode, ErrorNoMbr,
                        "`%s` is a method every Iterator shares, and %s has a `next` but does not declare itself one. Declare it to get the shared methods: `struct %s is Iterator[T]`, T being what `next` gives.",
                        &methsym->namestr, &tname->namestr, &tname->namestr);
                return -1;
            }
        }
        if (foundnode == NULL && parallelViewNotFound(callnode, objdereftype, methsym))
            return -1;
        errorMsgNode((INode*)callnode, ErrorNoMbr, "Method or field `%s` not found.", &methsym->namestr);
        return -1;
    }

    // Handle when methfld refers to a field
    if (foundnode->tag == FieldDclTag) {
        if (callnode->args != NULL)
            errorMsgNode((INode*)callnode, ErrorFldArgs, "May not provide arguments for a field access");
        fnCallPrivateVtable(callnode, foundnode, objdereftype, notpublic);

        // A folded copy is reached through the field it was folded through:
        // the receiver becomes the access to that field, and this node the
        // access to the copy on it, as if the path had been written out
        FieldDclNode *fld = (FieldDclNode*)foundnode;
        if (fld->hop)
            callnode->objfn = fnCallFieldAccess(callnode->objfn, fld->hop, (INode*)callnode);
        derefInject(&callnode->objfn);  // automatically deref any reference/ptr, if needed
        methfld->dclnode = foundnode;
        // A field's invariant lifetimes are the struct's own: read from a
        // value, they are the brands its type's use gives them
        callnode->vtype = methfld->vtype = lifeBrandField(((IExpNode*)callnode->objfn)->vtype,
            ((IExpNode*)foundnode)->vtype, (INode*)callnode);
        callnode->tag = FldAccessTag;
        return 1;
    }

    // A folded method runs with the field it was folded through as its self:
    // the receiver becomes the access to that field before any candidate is
    // tried, and selection, borrowing and the permission checks then see the
    // receiver the method was declared for
    if (folded) {
        structFoldReceiver(pstate, (StructNode*)objdereftype, methsym, &callnode->objfn, (INode*)callnode);
        obj = callnode->objfn;
    }

    // A generic method is called through its instance: the one its written type
    // arguments name, already made and bound to the member (fnCallMethodTypeArgs,
    // or a bare name's substitution), or else the one its arguments infer. The
    // instance is then the one candidate, selected as any method is, so the
    // receiver is dereferenced or borrowed to fit its 'self'. A generic method
    // takes no overload name, so the name binds it alone.
    if (foundnode->tag == FnDclTag && ((FnDclNode*)foundnode)->genericinfo) {
        if (genericIsInstanceOf(methfld->dclnode, (FnDclNode*)foundnode))
            foundnode = methfld->dclnode;
        else
            foundnode = (INode*)genericMethodInstance(pstate, callnode, (FnDclNode*)foundnode, NULL);
        if (foundnode == NULL) {
            callnode->vtype = errorType;
            return -1;
        }
    }

    // Test every candidate the name declares, without altering the call
    fnCallDemandCandidates(foundnode);
    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);

    // A receiver held through a reference or a pointer still satisfies a method that
    // declared 'self' by value, so dereference it and select again. That is a load and
    // a copy -- the same adjustment a field access already makes -- and nothing more:
    // no reference is manufactured here, and none ever is from a pointer (the borrow
    // retry below serves a value receiver). The retry runs only when no candidate
    // matched at all, so an ambiguity among the candidates the receiver already fits is
    // reported as such, and a set holding both a value and a reference candidate still
    // selects the reference one for a reference receiver.
    //
    // An operator written on a pointer is the one thing the retry does not reach.
    // An operation on a pointer is an operation on the pointer, and the dereference
    // has to be written, because the alternative is two lines that look alike doing
    // different things: a pointer declares its own '+', so 'p + 2' offsets it, while
    // it declares no '*', so 'p * 2' would quietly become '(*p) * 2'. It is an error
    // again, as it was in C. Only a pointer narrows. A reference's arithmetic
    // reaching the value's is by design, and so is its comparison, which
    // fnCallLowerRefCompare reads through on both sides before arriving here.
    int opOnPointer = (callnode->flags & FlagOperator) && iexpGetTypeDcl(obj)->tag == PtrTag;
    //
    // An operator reads through its other operand just the same: 'total + x' and
    // 'total += x' with 'x' a borrow of a number, 'x > 1', 'a == b' for plain
    // structs, take the value the borrow lends, as the receiver does. The
    // argument is read through only where it is a borrow of a value that copies
    // freely (fnCallBorrowReadsThrough), so nothing is ever moved out of a borrow,
    // and only after the operands as written found no candidate, so an operator
    // declared for references still takes them as they are. The receiver is
    // tried read through first, then both, then the argument alone, which is what
    // an operator-assign on a number needs: its receiver is the '&mut' it takes.
    if (selected == NULL && status == OverloadNone && !opOnPointer) {
        INode **argp = (callnode->flags & FlagOperator) && callnode->args && callnode->args->used == 1
            && fnCallBorrowReadsThrough(nodesGet(callnode->args, 0))
            ? &nodesGet(callnode->args, 0) : NULL;
        INode *arg = argp ? *argp : NULL;
        int objread = derefInject(&callnode->objfn);
        if (objread)
            selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);
        if (selected == NULL && status == OverloadNone && argp) {
            derefInject(argp);
            selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);
            if (selected == NULL && status == OverloadNone && objread) {
                callnode->objfn = obj;
                selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);
            }
            if (selected == NULL)
                *argp = arg;
        }
        if (selected == NULL)
            callnode->objfn = obj;  // a failed retry leaves the call as it was found
    }

    // A receiver held as a value reaches a method declaring 'self &' or
    // 'self &mut' by being borrowed (fnCallBorrowReceiver)
    if (selected == NULL && status == OverloadNone)
        selected = fnCallBorrowReceiver(pstate, callnode, foundnode, &status);

    // 'x[i].m()' where no 'm' takes the element's read-only borrow: the index
    // is in set position, so a type declaring '&[]' lends its mutable borrow
    if (selected == NULL && status == OverloadNone && fnCallIndexAsMut(pstate, callnode)) {
        selected = iNsTypeFindMethod(foundnode, &callnode->objfn, callnode->args, &status);
        if (selected == NULL && status == OverloadNone)
            selected = fnCallBorrowReceiver(pstate, callnode, foundnode, &status);
    }

    if (selected == NULL) {
        // A lock-managed reference reaches its value through a borrow only,
        // which a method's receiver is not made of
        INode *objtype = iexpGetTypeDcl(obj);
        if (objtype->tag == RefTag && permIsLock(((RefNode*)objtype)->perm)) {
            permLockRefused(obj, ((RefNode*)objtype)->perm, "call a method on the value");
            callnode->vtype = errorType;
        }
        else if (!fnCallRefIndexWantsMut(callnode, foundnode, methsym, status)
            && !fnCallBoolOperandWantsNumber(callnode, foundnode, status))
            fnCallNoCandidate((INode*)callnode, status, methsym, "method");
        return -1;
    }

    // A generic that joined the set, bound only by function signatures, was
    // selected by its arguments' types: its instance for them is what is called,
    // as a generic method named alone is
    if (selected->genericinfo) {
        selected = genericMethodInstance(pstate, callnode, selected, NULL);
        if (selected == NULL) {
            callnode->vtype = errorType;
            return -1;
        }
    }

    fnCallPrivateVtable(callnode, (INode*)selected, objdereftype, notpublic);

    if (selected->value && selected->value->tag == IntrinsicTag
        && ((IntrinsicNode*)selected->value)->intrinsicFn == HashNbrIntrinsic) {
        selected = fnCallHashNumber(callnode, selected);
        if (selected == NULL)
            return -1;
    }

    // An enum's equality reads its discriminant, which is the whole of the value
    // only where every variant is empty. Where a variant carries fields, the call
    // is the comparison the compiler gave the enum, or a refusal naming the
    // variant that has no '==' of its own for it to call.
    if (selected->value && selected->value->tag == IntrinsicTag
        && ((IntrinsicNode*)selected->value)->intrinsicFn == NoEqIntrinsic) {
        fnCallLowerEnumEq(pstate, callnode, (StructNode*)objdereftype);
        return 1;
    }

    // An enum's method belongs to its variants: each has a clone of it, and the
    // enum's own is never generated. Called through a reference, the tag selects
    // the variant's (fnCallLowerTraitMethod). Called on a value of the enum --
    // which only a method taking 'self' by value can be -- the tag would have to
    // select it too, and dispatch on a value is not built, so it is refused here
    // rather than left as a call to a function that does not exist. An intrinsic,
    // such as a payload-free enum's '==', is no function and is not refused. On a
    // variant's value the receiver's type is the variant, whose clone is selected.
    if (!(callnode->flags & FlagVDisp) && (selected->flags & FlagMethFld)
        && (selected->value == NULL || selected->value->tag != IntrinsicTag)) {
        INode *owner = inodeGetOwner((INode*)selected);
        if (owner != NULL && owner->tag == StructTag && (owner->flags & TraitType)) {
            errorMsgNode((INode*)callnode, ErrorEnumValueDispatch,
                "`%s` runs the variant's own, and which variant a value of %s is would have to be read from its tag at run time, which is not built for a value receiver. Match to reach the variant, or declare the method with a reference receiver, which dispatches on the tag.",
                &methsym->namestr, &((StructNode*)owner)->namesym->namestr);
            callnode->vtype = errorType;
            return 1;
        }
    }

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    // Re-purpose method's name use node into objfn, so name refers to selected method
    NameUseNode *methodrefnode = (NameUseNode*)callnode->methfld;
    methodrefnode->namesym = selected->namesym;
    methodrefnode->dclnode = (INode*)selected;
    methodrefnode->vtype = selected->vtype;

    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;
    callnode->vtype = ((FnSigNode*)selected->vtype)->rettype;

    // Handle copying of value arguments and default arguments
    fnCallFinalizeArgs(pstate, callnode);
    return 1;
}

// We have a reference or pointer, and a method to find (comparison or arithmetic)
// If found, lower the node to a function call (objfn+args)
// Otherwise try again against the type it points to
int fnCallLowerPtrMethod(FnCallNode *callnode, INsTypeNode *methtype) {
    INode *obj = callnode->objfn;
    INode *objtype = iexpGetTypeDcl(obj);
    assert(isNameUseNode(callnode->methfld));
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    INode *foundnode = iNsTypeFindFnField(methtype, methsym);
    if (!foundnode)
        return 0;

    // For a method call, make sure object is specified as first argument
    if (callnode->args == NULL) {
        callnode->args = newNodes(1);
    }
    nodesInsert(&callnode->args, callnode->objfn, 0);

    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindPtrMethod(foundnode, callnode->args, &status);
    if (selected == NULL) {
        fnCallNoCandidate((INode*)callnode, status, methsym, "method");
        callnode->vtype = ((IExpNode*)obj)->vtype; // make up a vtype
        return 1;
    }

    // Re-purpose method's name use node into objfn, so name refers to selected method
    INode **selfp = &nodesGet(callnode->args, 0);
    INode *selftype = iexpGetTypeDcl(*selfp);
    NameUseNode *methodrefnode = (NameUseNode*)callnode->methfld;
    methodrefnode->namesym = selected->namesym;
    methodrefnode->dclnode = (INode*)selected;
    methodrefnode->vtype = selected->vtype;
    callnode->objfn = (INode*)methodrefnode;
    callnode->methfld = NULL;

    // Now that exactly one candidate is selected, coerce the argument once.
    // These compiler-declared signatures are generic over the pointer/reference's
    // value type, so only a concretely typed parameter takes part in coercion.
    Nodes *parms = ((FnSigNode *)selected->vtype)->parms;
    // A '&uni' slice held in a place and taken as the slice a method borrows
    // ('s.len()') is borrowed from, as it is when a user's method takes it
    // (iexpCoerce): handed over as it is, it would be moved, and the second
    // call refused
    if (objtype->tag == ArrayRefTag && parms->used > 0 && iexpGetTypeDcl(nodesGet(parms, 0))->tag == ArrayRefTag) {
        INode *wanted = (INode*)newRefNodeFull(ArrayRefTag, (INode*)callnode, borrowRef,
            newPermUseNode(roPerm), ((RefNode*)objtype)->vtexp);
        if (borrowUniReborrows(*selfp, wanted))
            borrowUniReborrow(selfp, wanted);
    }
    if (parms->used > 1) {
        INode *parm1type = iexpGetTypeDcl(nodesGet(parms, 1));
        if (parm1type->tag != PtrTag && parm1type->tag != RefTag && parm1type->tag != ArrayRefTag
            && !iexpCoerce(&nodesGet(callnode->args, 1), parm1type))
            errorMsgNode(nodesGet(callnode->args, 1), ErrorInvType,
                "Expression's type does not match declared parameter");
    }

    callnode->vtype = ((FnSigNode*)selected->vtype)->rettype;
    if (callnode->vtype->tag == PtrTag) {
        INode *t_type = selftype->tag == RefTag? ((RefNode *)selftype)->vtexp : selftype;
        callnode->vtype = t_type;  // Generic substitution for T
    }
    return 1;
}

// The operator an operator application names, or NULL for any other call
static Name *fnCallOperatorName(FnCallNode *node) {
    if (!(node->flags & FlagOperator) || node->methfld == NULL || !isNameUseNode(node->methfld))
        return NULL;
    return ((NameUseNode*)node->methfld)->namesym;
}

// Is this one of the comparisons a reference reads through to its referent for:
// '==', '!=' and the four orderings?
static int fnCallIsValueCompare(Name *op) {
    return op == eqName || op == neName || op == ltName || op == leName || op == gtName || op == geName;
}

// Does a value of this type have a place that '===' can ask about?
static int fnCallHasPlace(INode *type) {
    return type->tag == RefTag || type->tag == VirtRefTag || type->tag == ArrayRefTag || type->tag == PtrTag;
}

// Type a 'null' operand of a comparison by the other operand: 'p == null' and
// 'null != p' compare with p's raw pointer type, which a pointer's comparison
// requires of both sides (iNsTypeFindPtrMethod). Compared with anything other
// than a raw pointer it is refused. A 'null' receiving any other call has no
// type to take, and is refused too. Answers 0 when the call is now an error.
static int fnCallTypeNullOperands(FnCallNode *node) {
    Name *op = fnCallOperatorName(node);
    int compare = op && (fnCallIsValueCompare(op) || op == sameName || op == notSameName)
        && node->args && node->args->used == 1;
    INode **nullp = NULL;
    if (compare) {
        INode **argp = &nodesGet(node->args, 0);
        if (litIsUntypedNull(*argp) && !litIsUntypedNull(node->objfn))
            litAdoptNullType(nullp = argp, ((IExpNode*)node->objfn)->vtype);
        else if (litIsUntypedNull(node->objfn) && !litIsUntypedNull(*argp))
            litAdoptNullType(nullp = &node->objfn, ((IExpNode*)*argp)->vtype);
    }
    if (litIsUntypedNull(node->objfn))
        litAdoptNullType(nullp = &node->objfn, unknownType);
    if (nullp && inodeIsError(*nullp)) {
        node->vtype = errorType;
        return 0;
    }
    return 1;
}

// Refuse a comparison through a reference whose referent offers none
static void fnCallRefNoCompare(FnCallNode *node, Name *op, char *why) {
    errorMsgNode((INode*)node, ErrorRefNoCompare,
        (op == eqName || op == neName)
            ? "`%s` on references compares the values they refer to, and %s. Use `===` to ask whether two references point to the same place."
            : "`%s` on references compares the values they refer to, and %s. References have no order of their own.",
        &op->namestr, why);
    node->vtype = errorType;
}

static StructNode *fnCallTextOf(INode *objtype);

// Read through one operand of a comparison, positioned on the comparison
static void fnCallDerefOperand(INode **operandp, FnCallNode *node) {
    derefInject(operandp);
    inodeLexCopy(*operandp, (INode*)node);
}

// Why two elements of this type cannot be compared with '==', or NULL when
// they can. It asks what fnCallLowerRefCompare would of the '&a[i] == &b[i]'
// that core's mem.sliceEq compares each pair with, so a refusal is reported
// where the slices are compared rather than inside core: a number, bool or
// pointer compares by its own '==', a reference and a slice by what they
// refer to, and a struct or a payload-free enum by the '==' it declares.
static char *fnCallSliceElemNoEq(INode *elemtype, char *buf, size_t size) {
    INode *type = itypeGetTypeDcl(elemtype);
    switch (type->tag) {
    case PtrTag:
        return NULL;
    case RefTag:
    case ArrayRefTag:
        return fnCallSliceElemNoEq(((RefNode*)type)->vtexp, buf, size);
    case VirtRefTag:
        return "comparing what two virtual references refer to is not built";
    case ArrayTag:
        return "comparing two arrays is not built";
    default:
        break;
    }
    if (!isMethodType(type))
        return "their element type has no `==`";
    if ((type->flags & TraitType) && !(type->flags & EnumType))
        return "comparing what a reference to a trait refers to is not built";
    INode *found = iNsTypeFindFnField((INsTypeNode*)type, eqName);
    if (found && found->tag == AliasDclTag)
        found = aliasDclResolve(found);
    Name *typename = isNamedNode(type) ? inodeGetName(type) : NULL;
    if (!found || !(found->tag == FnDclTag || found->tag == FnOverloadDclTag) || !(found->flags & FlagMethFld)) {
        if (typename == NULL)
            return "their element type declares no `==`";
        snprintf(buf, size, "%s declares no `==`", &typename->namestr);
        return buf;
    }
    FnDclNode *eqdcl = (FnDclNode*)found;
    if (found->tag == FnDclTag && eqdcl->value && eqdcl->value->tag == IntrinsicTag
        && ((IntrinsicNode*)eqdcl->value)->intrinsicFn == NoEqIntrinsic) {
        // Compared by the comparison the compiler gave it, where it has one
        if (structEnumEqFn((StructNode*)type) != NULL)
            return NULL;
        StructNode *lacking = structEnumVariantWithoutEq((StructNode*)type);
        snprintf(buf, size, "%s is an enum whose variant %s carries fields and declares no `==`",
            typename ? &typename->namestr : "their element type",
            lacking ? &lacking->namesym->namestr : "of them");
        return buf;
    }
    return NULL;
}

// '==' on two slices compares their elements (doc/reference/refarrayref.html,
// "Comparison"): equal when the counts are and each element is '==' to its
// partner. '!=' arrives here as '==' under a 'not' (fnCallNeFromEq) unless the
// elements have no '==', which is reported under the '!=' that was written. A
// slice has no order, so an ordering is refused.
//
// The comparison is core's 'mem.sliceEq' (sliceEqFn), a generic function whose
// body is the loop, instantiated at the receiver's element type and called
// with the two slices; the call is then checked as any call is, so the other
// side is converted to that slice as any slice argument is: a slice, a
// reference to an array, an array, or a string literal. A body in Cone rather
// than a lowering of its own gets every element type's '==' -- a number's, a
// pointer's, one a struct declares by value or on references, a nested
// slice's -- from the comparison of references that already selects it.
static void fnCallLowerSliceCompare(TypeCheckState *pstate, FnCallNode *node) {
    Name *op = ((NameUseNode*)node->methfld)->namesym;
    if (op != eqName && op != neName) {
        errorMsgNode((INode*)node, ErrorRefNoCompare,
            "`%s` on two slices is refused: a slice has no order. Compare their elements one by one.",
            &op->namestr);
        node->vtype = errorType;
        return;
    }
    RefNode *slicetype = (RefNode*)iexpGetTypeDcl(node->objfn);
    char buf[256];
    char *why = fnCallSliceElemNoEq(slicetype->vtexp, buf, sizeof(buf));
    if (why) {
        errorMsgNode((INode*)node, ErrorRefNoCompare,
            "`%s` on two slices compares their elements, and %s. Use `===` to ask whether two slices view the same elements.",
            &op->namestr, why);
        node->vtype = errorType;
        return;
    }
    if (op == neName) {
        errorUnreachable((INode*)node, "a slice's '!=' not derived from its '=='");
        node->vtype = errorType;
        return;
    }
    // Found as the core package itself is: a compile without it cannot go on
    FnDclNode *sliceeq = sliceEqFn();
    if (sliceeq == NULL)
        errorExit(ExitNF, "'==' on two slices calls the core package's 'mem.sliceEq', and the core package found declares none.");

    // mem.sliceEq[T], T the receiver's element type
    Nodes *typeargs = newNodes(1);
    nodesAdd(&typeargs, slicetype->vtexp);
    FnDclNode *instance = genericMethodInstance(pstate, node, sliceeq, typeargs);
    if (instance == NULL) {
        node->vtype = errorType;
        return;
    }

    // The receiver becomes the first argument, as for any method
    if (node->args == NULL)
        node->args = newNodes(1);
    nodesInsert(&node->args, node->objfn, 0);
    node->objfn = (INode*)newNameUseFromDclNode((INode*)instance, (INode*)node);
    node->methfld = NULL;
    node->vtype = ((FnSigNode*)instance->vtype)->rettype;
    fnCallFinalizeArgs(pstate, node);
}

// An array, a reference to one, or a '&str' (a string literal is one), compared
// with a slice is taken as the slice it converts to, as the other side of a slice's comparison already is:
// '"box" == s' means what 's == "box"' does. Answers 0, changing nothing,
// where the other side is not a slice.
static int fnCallArrayAsSlice(TypeCheckState *pstate, FnCallNode *node) {
    if (node->args == NULL || node->args->used != 1)
        return 0;
    INode *argtype = iexpGetTypeDcl(nodesGet(node->args, 0));
    if (argtype->tag != ArrayRefTag)
        return 0;
    INode *slicetype = (INode*)newRefNodeFull(ArrayRefTag, (INode*)node, borrowRef,
        newPermUseNode(roPerm), ((RefNode*)argtype)->vtexp);
    if (!iexpCoerce(&node->objfn, slicetype)) {
        errorMsgNode(node->objfn, ErrorInvType,
            "An array compared with a slice is compared as a slice of the same element type, and this one's elements differ.");
        node->vtype = errorType;
        return 1;
    }
    fnCallLowerSliceCompare(pstate, node);
    return 1;
}

// '==', '!=' or an ordering written on a reference compares what it refers to.
// A reference reads as its value everywhere else -- 'r.x', 'r.method()' -- so a
// comparison does too; '===' is what asks whether two references are the same
// place, and refType declares only that. A raw pointer is the exception, whose
// operators are on the pointer (the retry in fnCallLowerMethod says why).
//
// A reference compared with a value is read through only where it is a borrow of
// a value that copies freely (fnCallBorrowReadsThrough), as an operator reads any
// such borrow through. Any other is refused rather than read through on one
// side only, so 'r == v' never says something '*r == v' does not.
//
// A referent type that declares the operator for references ('self &', 'other &T')
// takes the operands as they are. Otherwise both are dereferenced and the value's
// operator selected, exactly as for '*a == *b'. A reference to a pointer, to
// another reference or to a slice reads through to that, which then compares as
// it would by value.
static void fnCallLowerRefCompare(TypeCheckState *pstate, FnCallNode *node) {
    Name *op = ((NameUseNode*)node->methfld)->namesym;
    RefNode *reftype = (RefNode*)iexpGetTypeDcl(node->objfn);
    INode **argp = &nodesGet(node->args, 0);
    if (iexpGetTypeDcl(*argp)->tag != RefTag) {
        // A reference to an array, or to 'str' (a literal is one), is taken as a slice
        if ((itypeGetTypeDcl(reftype->vtexp)->tag == ArrayTag || refIsFat(reftype))
            && fnCallArrayAsSlice(pstate, node))
            return;
        // A borrow of a value that copies freely is read through against a value
        // too: 'x > 1', 'x == y' with 'y' a plain value. Any other referent is
        // refused rather than read through on one side only.
        if (!fnCallBorrowReadsThrough(node->objfn)) {
            errorMsgNode((INode*)node, ErrorRefCompareMixed,
                "`%s` on a reference compares the value it refers to, so the other side must be a reference too. Dereference the reference (`*r`) to compare it with a value.",
                &op->namestr);
            node->vtype = errorType;
            return;
        }
    }

    INode *referent = itypeGetTypeDcl(reftype->vtexp);

    // Text through an owner, '&So[str]' or '&Rc[str]', against text one level
    // shallower, '&str': only the owner's side is read through, as '&str ==
    // &So[str]' already reads through the owner when it takes its other side, so
    // the comparison does not depend on operand order
    if (referent->tag == RefTag) {
        INode *otherreferent = itypeGetTypeDcl(((RefNode*)iexpGetTypeDcl(*argp))->vtexp);
        if (otherreferent->tag != RefTag && otherreferent->tag != PtrTag && otherreferent->tag != ArrayRefTag
            && fnCallTextOf(iexpGetTypeDcl(node->objfn)) != NULL && fnCallTextOf(iexpGetTypeDcl(*argp)) != NULL) {
            fnCallDerefOperand(&node->objfn, node);
            fnCallLowerRefCompare(pstate, node);
            return;
        }
    }
    if (referent->tag == PtrTag || referent->tag == RefTag || referent->tag == ArrayRefTag) {
        fnCallDerefOperand(&node->objfn, node);
        fnCallDerefOperand(argp, node);
        if (referent->tag == RefTag)
            fnCallLowerRefCompare(pstate, node);
        else if (referent->tag == ArrayRefTag)
            fnCallLowerSliceCompare(pstate, node);
        else
            fnCallLowerPtrMethod(node, ptrType);
        return;
    }
    if (!isMethodType(referent)) {
        fnCallRefNoCompare(node, op, "the type they refer to has no comparison");
        return;
    }
    // A trait's method is dispatched on the variant, and neither dispatch nor a
    // load of a trait's value is what a comparison can build here. An enum is
    // flagged a trait too, but is a value of one size with its own '=='.
    if ((referent->flags & TraitType) && !(referent->flags & EnumType)) {
        fnCallRefNoCompare(node, op, "comparing what a reference to a trait refers to is not built");
        return;
    }
    INode *found = iNsTypeFindFnField((INsTypeNode*)referent, op);
    if (found && found->tag == AliasDclTag)
        found = aliasDclResolve(found);
    if (!found || !(found->tag == FnDclTag || found->tag == FnOverloadDclTag) || !(found->flags & FlagMethFld)) {
        fnCallRefNoCompare(node, op, "the type they refer to declares no such operator");
        return;
    }

    // A candidate declared for references matches the operands as written; only
    // when none does are both read through. Selection itself, and any ambiguity,
    // is fnCallLowerMethod's.
    fnCallDemandCandidates(found);
    enum OverloadMatch status;
    if (iNsTypeFindMethod(found, &node->objfn, node->args, &status) == NULL && status == OverloadNone) {
        fnCallDerefOperand(&node->objfn, node);
        if (iexpGetTypeDcl(*argp)->tag == RefTag)
            fnCallDerefOperand(argp, node);
    }
    fnCallLowerMethod(pstate, node);
}

// An owner of 'Array[T]' indexed, 'o[i]', '&o[a..<b]' or '&o[i]', is indexed as
// the slice it lends: the receiver is coerced to '&Array[T]' (the slice, read
// only unless the borrow written is writable, or an element is assigned), and
// the index goes on as a slice's. A range borrowed has the borrow the parser
// put round the receiver dropped, as for text (fnCallLowerStrRange). Answers 1
// when the receiver was replaced, 0 when the index is not of an owner of the
// body, and -1 when the owner cannot be lent as the index needs (reported).
static int fnCallBodyAsSlice(TypeCheckState *pstate, FnCallNode *node) {
    if (!(node->flags & FlagIndex) || node->methfld != NULL || (node->flags & FlagLvalOp))
        return 0;
    INode *recv = node->objfn;
    // Read only, unless an element is assigned through the owner, or borrowed
    // writable as written
    INode *perm = (INode*)(node == fnCallSetIndex ? mutPerm : roPerm);
    if ((node->flags & FlagBorrow) && recv->tag == BorrowTag && (recv->flags & FlagSuffix)) {
        perm = ((RefNode*)recv)->perm;
        INode *inner = ((RefNode*)recv)->vtexp;
        if (inner->tag == DerefTag)
            inner = ((StarNode*)inner)->vtexp;
        if (!isExpNode(inner))
            return 0;
        INode *held = iexpGetTypeDcl(inner);
        while (held->tag == RefTag && itypeGetTypeDcl(((RefNode*)held)->vtexp)->tag == RefTag) {
            derefInject(&inner);
            held = iexpGetTypeDcl(inner);
        }
        if (held->tag != RefTag || !itypeIsArrayBody(((RefNode*)held)->vtexp))
            return 0;
        recv = varDclTempValue(pstate, inner);
    }
    else {
        INode *held = iexpGetTypeDcl(recv);
        while (held->tag == RefTag && itypeGetTypeDcl(((RefNode*)held)->vtexp)->tag == RefTag) {
            derefInject(&recv);
            held = iexpGetTypeDcl(recv);
        }
        if (held->tag != RefTag || !itypeIsArrayBody(((RefNode*)held)->vtexp))
            return 0;
    }
    INode *held = iexpGetTypeDcl(recv);
    INode *slicetype = (INode*)newRefNodeFull(ArrayRefTag, (INode*)node, borrowRef, perm,
        itypeLenBodyElem(((RefNode*)held)->vtexp));
    if (!itypeTypeCheck(pstate, &slicetype))
        return -1;
    if (!iexpCoerce(&recv, slicetype)) {
        if (perm == (INode*)roPerm)
            errorMsgNode((INode*)node, ErrorBadIndex, "This owner of an Array cannot be lent as a slice to read it through.");
        else
            errorMsgNode((INode*)node, ErrorNoMut,
                "The elements of this owner of an Array cannot be changed through it: its permission does not allow a writable borrow.");
        return -1;
    }
    node->objfn = recv;
    return 1;
}

// The text a receiver reaches, through any references: 'str' itself, or a type
// that lends it (structLentBody). NULL for anything else.
static StructNode *fnCallTextOf(INode *objtype) {
    for (;;) {
        if (objtype->tag == RefTag)
            objtype = itypeGetTypeDcl(((RefNode*)objtype)->vtexp);
        else
            break;
    }
    if (objtype == (INode*)strTypeDcl)
        return strTypeDcl;
    if (objtype->tag == StructTag && structLentBody((StructNode*)objtype) == strTypeDcl)
        return (StructNode*)objtype;
    return NULL;
}

// A range borrowed from text, '&s[a..<b]', '&s[a..]' or '&s[a...b]', is a call
// of the text's 'slice', 'sliceFrom' or 'sliceThrough' on what is being
// indexed: the method checks the bounds fall between characters and gives a
// '&str' that keeps the text borrowed, as any method's borrow does. Answers 0,
// changing nothing, where the receiver is not text.
//
// The receiver is the borrow the parser put around it, already checked
// (borrowReassocIndex). That borrow is dropped: the method borrows its receiver
// itself, as 'x.slice(a, b)' written out would. What it borrowed is the place
// itself, or, where the place is reached through a reference, the reference
// ('s' for a '&str' s: the borrow dereferenced it); a temporary the borrow
// hid in a local of the statement (the 's.view()' of '&s.view()[1..]') is taken
// as the expression it was, the local never declared.
static int fnCallLowerStrRange(TypeCheckState *pstate, FnCallNode *node) {
    if ((node->flags & (FlagIndex | FlagRange | FlagBorrow)) != (FlagIndex | FlagRange | FlagBorrow)
        || node->methfld != NULL || node->objfn == NULL || node->objfn->tag != BorrowTag)
        return 0;
    RefNode *borrow = (RefNode*)node->objfn;
    if (!(borrow->flags & FlagSuffix) || fnCallTextOf(iexpGetTypeDcl(borrow->vtexp)) == NULL)
        return 0;
    INode *recv = borrow->vtexp;
    if (recv->tag == DerefTag)
        recv = ((StarNode*)recv)->vtexp;
    node->objfn = varDclTempValue(pstate, recv);
    // A reference to a reference to text is read through to the reference to it
    for (;;) {
        INode *rtype = iexpGetTypeDcl(node->objfn);
        if (rtype->tag != RefTag || itypeGetTypeDcl(((RefNode*)rtype)->vtexp)->tag != RefTag)
            break;
        derefInject(&node->objfn);
    }
    Name *meth = node->args->used == 1 ? nametblFind("sliceFrom", 9)
        : (node->flags & FlagRangeIncl) ? nametblFind("sliceThrough", 12) : nametblFind("slice", 5);
    node->methfld = (INode*)newMemberUseNode(meth);
    node->flags &= ~(FlagIndex | FlagBorrow | FlagRange | FlagRangeIncl);
    if (fnCallLowerMethod(pstate, node) == 0) {
        errorMsgNode((INode*)node, ErrorNoMeth, "No method named %s found that matches the range.", &meth->namestr);
        node->vtype = errorType;
    }
    return 1;
}

// '==', '!=' and the orderings between two kinds of text -- 'str' through any
// reference or owner, and a type that lends one -- compare the bytes, as Rust's
// cross-type PartialEq does: a lending operand (a String) is replaced by the
// borrow it lends, 'a.view()', so the comparison is between two texts, whether
// the operands are values or references.
static void fnCallLentOperands(TypeCheckState *pstate, FnCallNode *node) {
    Name *op = fnCallOperatorName(node);
    if (op == NULL || !fnCallIsValueCompare(op) || node->args == NULL || node->args->used != 1)
        return;
    INode **argp = &nodesGet(node->args, 0);
    if (!isExpNode(node->objfn) || !isExpNode(*argp))
        return;
    StructNode *left = fnCallTextOf(iexpGetTypeDcl(node->objfn));
    StructNode *right = fnCallTextOf(iexpGetTypeDcl(*argp));
    if (left == NULL || right == NULL || (left == strTypeDcl && right == strTypeDcl))
        return;
    if (left != strTypeDcl)
        structLendView(pstate, left, &node->objfn, (INode*)node);
    if (right != strTypeDcl)
        structLendView(pstate, right, argp, (INode*)node);
}

// The receiver is a plain reference to a trait (or union) and the name it calls
// is a method rather than a field. Return 0 when this is not that case.
//
// Such a call dispatches on which variant the reference points at, which the
// trait's own declaration cannot answer: an abstract method has no body, and a
// method with a body is a default that was cloned into each variant, so neither
// the requirement nor the original is a function anything can call. Reaching one
// is what left the call naming a declaration with no symbol, and generation
// dereferenced that null.
//
// The route is the one doc/reference/reftraitvar.html describes -- the tag says which
// variant, that selects the vtable, and the vtable holds the method -- and it is
// already built as the coercion from '&Trait' to '&<Trait'. So this coerces and
// then dispatches virtually, which is what a caller otherwise has to write by
// hand. An open trait has no tag and refvirtMatches refuses the coercion, which
// is the same page's rule; that refusal is reported here rather than left to a
// crash.
int fnCallLowerTraitMethod(TypeCheckState *pstate, FnCallNode *callnode, INode *objdereftype) {
    if (objdereftype->tag != StructTag || !(objdereftype->flags & TraitType))
        return 0;
    if (callnode->methfld == NULL || !isNameUseNode(callnode->methfld))
        return 0;

    // A field is reached through the trait's own layout and needs no dispatch,
    // which is what fnCallLowerMethod already does with it
    Name *methsym = ((NameUseNode*)callnode->methfld)->namesym;
    INode *foundnode = iNsTypeFindFnField((INsTypeNode*)objdereftype, methsym);
    if (foundnode == NULL || foundnode->tag == FieldDclTag)
        return 0;

    RefNode *objtype = (RefNode*)iexpGetTypeDcl(callnode->objfn);
    if (objtype->tag != RefTag)
        return 0;

    // Build '&<perm Trait' and let coercion decide whether it is reachable
    RefNode *vreftype = newRefNodeFull(VirtRefTag, (INode*)callnode, (INode*)borrowRef,
                                       objtype->perm, objtype->vtexp);
    INode *vreftypep = (INode*)vreftype;
    if (itypeTypeCheck(pstate, &vreftypep) == 0)
        return 1;   // already reported

    if (!iexpCoerce(&callnode->objfn, vreftypep)) {
        errorMsgNode((INode*)callnode, ErrorNoMeth,
            "`%s` is dispatched on the variant, which a reference to an open trait cannot determine. Use a virtual reference (&<).",
            &methsym->namestr);
        callnode->vtype = errorType;
        return 1;
    }

    callnode->flags |= FlagVDisp;
    fnCallLowerMethod(pstate, callnode);
    return 1;
}

// objfn names an overload set. Select the one candidate that accepts the call's
// arguments, rewrite the call to that concrete function, then finalize its arguments.
int fnCallLowerOverloadFn(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    NameUseNode *fnuse = (NameUseNode*)node->objfn;
    // Through the alias where a fold is what bound the name here. The visibility
    // already checked was the alias's own, and the overload set is its target's
    FnOverloadDclNode *overloadnode = (FnOverloadDclNode*)nameUseGetDcl(fnuse);

    if ((node->flags & FlagIndex) || node->methfld != NULL) {
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "A function may not be called using indexing or a method.");
        return 1;
    }

    // A generic type's own overload name, reached from outside it, names
    // candidates that only its instances have
    if (nameUseTemplateMember(fnuse, nodesGet(overloadnode->overloads, 0))) {
        node->vtype = errorType;
        return 1;
    }

    // Test every candidate the overload name declares, without altering the call,
    // each analyzed first (Rule 1), as a function named alone is by its name use
    fnCallDemandCandidates((INode*)overloadnode);
    enum OverloadMatch status;
    FnDclNode *selected = iNsTypeFindMethod((INode*)overloadnode, NULL, node->args, &status);
    if (selected == NULL) {
        fnCallNoCandidate((INode*)node, status, overloadnode->namesym, "function");
        return 1;
    }

    // Rewrite the callee to the selected concrete declaration, so nothing downstream
    // ever sees the overload node, then insert coercions and defaults exactly once
    fnuse->namesym = selected->namesym;
    fnuse->dclnode = (INode*)selected;
    fnuse->vtype = selected->vtype;
    // A generic is called as one named directly is: its type arguments are
    // inferred from the arguments, which are all checked, and the instance
    // becomes the callee (genericSubstitute)
    if (selected->genericinfo)
        return genericSubstitute(pstate, nodep);
    fnCallFinalizeArgs(pstate, node);
    return 1;
}

// Lower opassign method for method-based types
void fnCallOpAssgn(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *callnode = *nodep;
    INode *objtype = iexpGetTypeDcl(callnode->objfn);
    assert(isNameUseNode(callnode->methfld));
    NameUseNode *methfld = (NameUseNode*)callnode->methfld;
    Name *methsym = methfld->namesym;

    // Change first argument to &mut obj, unless the receiver already is a
    // reference, which is taken as it is. Either way the rest of this works on
    // a reference to the method-declaring type: the operator is looked up on
    // that type, and the rewrite below dereferences the reference to reach it.
    if (objtype->tag == RefTag)
        objtype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);
    else
        borrowMutRef(&callnode->objfn, objtype, newPermUseNode(mutPerm));

    // Lower to op-assign, if method supported by type
    if (iNsTypeFindFnField((INsTypeNode*)objtype, methsym)) {
        fnCallLowerMethod(pstate, callnode);
        return;
    }

    // '<-' has no base operator to rewrite to, so a type that does not declare
    // it is missing it, and is reported as any other missing operator method is
    Name *basesym = fnCallOpEqMethod(methsym);
    if (basesym == NULL) {
        errorMsgNode((INode*)callnode, ErrorNoMbr, "Method or field `%s` not found.", &methsym->namestr);
        callnode->vtype = errorType;
        return;
    }

    // An operand holding an 'await', 'x += await f()', is made first, into a
    // local of the rewrite: the seam comes before the place is borrowed, so the
    // rest of the statement -- borrowing the place, reading it, storing the
    // sum -- runs after the seam, where the place is reached again (await.h,
    // awaitIsPath). Made after the borrow, the operand's seam would end it
    VarDclNode *operand = NULL;
    if (callnode->args && callnode->args->used == 1 && awaitWithin(nodesGet(callnode->args, 0))) {
        INode *exp = nodesGet(callnode->args, 0);
        operand = newVarDclFull(tempLocalName, VarDclTag, ((IExpNode*)exp)->vtype, (INode*)immPerm, exp);
        inodeLexCopy((INode*)operand, exp);
        NameUseNode *opname = newNameUseNode(tempLocalName);
        opname->vtype = operand->vtype;
        opname->dclnode = (INode *)operand;
        inodeLexCopy((INode*)opname, exp);
        nodesGet(callnode->args, 0) = (INode *)opname;
    }

    // Let's try rewriting to: {imm tmp = lval; *tmp = *tmp + expr}
    VarDclNode *tmpvar = newVarDclFull(tempName, VarDclTag, ((IExpNode*)callnode->objfn)->vtype, (INode*)immPerm, callnode->objfn);
    inodeLexCopy((INode*)tmpvar, (INode*)callnode);
    NameUseNode *tmpname = newNameUseNode(tempName);
    tmpname->vtype = tmpvar->vtype;
    tmpname->dclnode = (INode *)tmpvar;
    inodeLexCopy((INode*)tmpname, (INode*)callnode);
    INode *derefvar = (INode *)tmpname;
    derefInject(&derefvar);
    inodeLexCopy(derefvar, (INode*)callnode);
    callnode->objfn = derefvar;
    methfld->namesym = basesym;
    if (fnCallLowerMethod(pstate, callnode) == 0) {
        errorMsgNode((INode*)callnode, ErrorNoMeth,
            "No method/field named %s found that matches the call's arguments.",
            &methsym->namestr);
        return;
    }
    INode *dereflval = (INode *)tmpname;
    derefInject(&dereflval);
    inodeLexCopy(dereflval, (INode*)callnode);
    AssignNode *tmpassgn = newAssignNode(NormalAssign, dereflval, (INode*)callnode);
    inodeLexCopy((INode*)tmpassgn, (INode*)callnode);
    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, (INode*)callnode);
    blk->vtype = callnode->vtype;
    blk->flags |= FlagKeepTemps;
    if (operand)
        nodesAdd(&blk->stmts, (INode*)operand);
    nodesAdd(&blk->stmts, (INode*)tmpvar);
    nodesAdd(&blk->stmts, (INode*)tmpassgn);
    *((INode**)nodep) = (INode*)blk;
}

// A type that declares '==' and no '!=' has its '!=' derived, as 'not (a == b)'
// (doc/reference/refmethop.html, "Comparison Operator Methods"). Asked of a struct
// receiver's own type, or of the struct a reference refers to, through any
// number of references, since '!=' on references compares the values
// (fnCallLowerRefCompare). A type that declares its own '!=' keeps it, an
// enum's intrinsic pair is declared together, a number declares both, and a
// type declaring neither is left to be reported missing its '!='. A slice's
// '!=', directly or through references, is derived the same way, since its
// '==' is the only comparison of elements (fnCallLowerSliceCompare), where
// the elements have a '==' to compare with; so is that of an array, or a
// reference to one, compared with a slice (fnCallArrayAsSlice). A pointer
// declares its own '!=', which is on the pointer and never asks the referent;
// a virtual reference refuses '!=' on what it refers to, and '!==', identity,
// is never derived.
static int fnCallNeFromEq(FnCallNode *node, INode *objtype) {
    if (!(node->flags & FlagOperator) || node->methfld == NULL || !isNameUseNode(node->methfld)
        || ((NameUseNode*)node->methfld)->namesym != neName)
        return 0;
    INode *argtype = node->args && node->args->used > 0 ? iexpGetTypeDcl(nodesGet(node->args, 0)) : NULL;
    INode *held = objtype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)objtype)->vtexp) : objtype;
    if (held->tag == ArrayTag && argtype && argtype->tag == ArrayRefTag)
        objtype = argtype;
    // Text against a slice of bytes is compared as the slice it converts to
    // (fnCallArrayAsSlice), which '!=' derives from '==' as for two slices
    if (objtype->tag == RefTag && argtype && argtype->tag == ArrayRefTag
        && itypeGetTypeDcl(((RefNode*)objtype)->vtexp) == (INode*)strTypeDcl)
        return 1;
    // A reference against a value is refused under the '!=' that was written,
    // not under a derived '==', unless it is a borrow that is read through
    // against a value (fnCallBorrowTypeReadsThrough)
    if (objtype->tag == RefTag && argtype && argtype->tag != RefTag && !fnCallBorrowTypeReadsThrough(objtype))
        return 0;
    while (objtype->tag == RefTag)
        objtype = itypeGetTypeDcl(((RefNode*)objtype)->vtexp);
    if (objtype->tag == ArrayRefTag) {
        char buf[256];
        return fnCallSliceElemNoEq(((RefNode*)objtype)->vtexp, buf, sizeof(buf)) == NULL;
    }
    return objtype->tag == StructTag
        && iNsTypeFindFnField((INsTypeNode*)objtype, neName) == NULL
        && iNsTypeFindFnField((INsTypeNode*)objtype, eqName) != NULL;
}

// A path through an instance of a generic module -- 'stack[i64].push(x)',
// 'stack[i64].count' -- collapsed as fnCallNameResPath collapses a path through
// any module, but here: the instance does not exist until type check makes it
// (genericMemoize), so name resolution left the member unbound. The member is
// looked up in the instance's namespace, a private one refused from outside the
// module, and the hop disappears: with no arguments the node becomes the bound
// name, and with arguments the callee. Returns 1 when the node was replaced
// outright (and checked), so the caller stops.
static int fnCallModuleInstancePath(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    ModuleNode *mod = (ModuleNode*)nameUseGetDcl((NameUseNode*)node->objfn);
    NameUseNode *member = (NameUseNode*)node->methfld;
    member->dclnode = namespaceFind(&mod->namespace, member->namesym);
    if (member->dclnode == NULL) {
        errorMsgNode((INode*)member, ErrorUnkName,
            "The name %s does not refer to a declared name of module %s",
            &member->namesym->namestr, &mod->namesym->namestr);
        node->vtype = errorType;
        *((INode**)nodep) = (INode*)newErrorNode((INode*)node);
        return 1;
    }
    member->flags |= FlagQualified;
    ModuleNode *asker = pstate->fn ? dclInfoGetModule((INode*)pstate->fn) : NULL;
    if (asker != mod && inodeIsPrivate(member->dclnode))
        errorMsgNode((INode*)member, ErrorNotPublic,
            "%s is private to its module and may not be named from outside it.",
            &member->namesym->namestr);
    if (node->args == NULL) {
        *((INode**)nodep) = (INode*)member;
        inodeTypeCheckAny(pstate, (INode**)nodep);
        return 1;
    }
    node->objfn = (INode*)member;
    node->methfld = NULL;
    return 0;
}

// A path through an instance of a generic type to one of its functions --
// 'List[i64].empty()', 'Box[f32].make(1.)' -- collapsed as fnCallNameResPath
// collapses a path through any type, but here, for the reason a generic
// module's instance is (fnCallModuleInstancePath): the instance does not exist
// until type check makes it. The member is looked up in the instance's
// namespace, and the same rules apply as to a path through a type that was
// named: a private member is refused from outside the type's module, and a
// method of an enum or trait, which has no code of its own, is refused. Only a
// function is reached this way; any other member is left as it was, and
// diagnosed below as a path this does not collapse. Returns 1 when the node was
// replaced outright (and checked), so the caller stops.
// A call of a generic instance's function through a use of it naming brands,
// 'List[&'=a T].empty()': the instance's brands, named by place, are bound to them at the call
// (fnCallFinalizeArgs), not minted. Pairs of call and use, few at a time.
static INode **fnCallBrandPaths = NULL;
static uint32_t fnCallBrandPathCnt = 0;
static uint32_t fnCallBrandPathCap = 0;

static void fnCallBrandPathAdd(FnCallNode *call, INode *use) {
    if (!lifeTypeHasBrands(use))
        return;
    if (fnCallBrandPathCnt == fnCallBrandPathCap) {
        uint32_t cap = fnCallBrandPathCap ? fnCallBrandPathCap << 1 : 16;
        INode **grown = memAllocBlk(2 * cap * sizeof(INode*));
        if (fnCallBrandPathCnt)
            memcpy(grown, fnCallBrandPaths, 2 * fnCallBrandPathCnt * sizeof(INode*));
        fnCallBrandPaths = grown;
        fnCallBrandPathCap = cap;
    }
    fnCallBrandPaths[2 * fnCallBrandPathCnt] = (INode*)call;
    fnCallBrandPaths[2 * fnCallBrandPathCnt++ + 1] = use;
}

// The use a call was made through, taken off the list, or NULL
static INode *fnCallBrandPathTake(FnCallNode *call) {
    for (uint32_t i = fnCallBrandPathCnt; i-- > 0;) {
        if (fnCallBrandPaths[2 * i] == (INode*)call) {
            INode *use = fnCallBrandPaths[2 * i + 1];
            fnCallBrandPaths[2 * i] = fnCallBrandPaths[2 * (fnCallBrandPathCnt - 1)];
            fnCallBrandPaths[2 * i + 1] = fnCallBrandPaths[2 * (fnCallBrandPathCnt - 1) + 1];
            --fnCallBrandPathCnt;
            return use;
        }
    }
    return NULL;
}

static int fnCallTypeInstancePath(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    StructNode *inst = (StructNode*)nameUseGetDcl((NameUseNode*)node->objfn);
    if (lifeInvariantSeen)
        fnCallBrandPathAdd(node, node->objfn);
    NameUseNode *member = (NameUseNode*)node->methfld;
    INode *found = namespaceFind(&inst->namespace, member->namesym);
    if (found == NULL || (found->tag != FnDclTag && found->tag != FnOverloadDclTag))
        return 0;
    member->dclnode = found;
    member->flags |= FlagQualified;
    ModuleNode *qualmod = dclInfoGetModule((INode*)inst);
    ModuleNode *asker = pstate->fn ? dclInfoGetModule((INode*)pstate->fn) : NULL;
    if (qualmod && qualmod != asker && inodeIsPrivate(found))
        errorMsgNode((INode*)member, ErrorNotPublic,
            "%s is private to its module and may not be named from outside it.",
            &member->namesym->namestr);
    if ((inst->flags & TraitType) && fnCallNamesMethod(found))
        errorMsgNode((INode*)member, ErrorAbstractMeth,
            "%s names a method of %s, which has no code of its own for it: each implementer or variant has its own copy. Call it on a value, or name it through a type that has it.",
            &member->namesym->namestr, &inst->namesym->namestr);
    if (node->args == NULL) {
        *((INode**)nodep) = (INode*)member;
        inodeTypeCheckAny(pstate, (INode**)nodep);
        return 1;
    }
    node->objfn = (INode*)member;
    node->methfld = NULL;
    return 0;
}

// 'u64.from(count)': a number type's conversion, the method of the type that
// takes the value to convert. Every number type has one, taking any number
// (bool's also a reference or a pointer). Lowered here,
// once the receiver is known to be a number type -- named directly, through an
// alias, or as the argument a type parameter has in an instance, since name
// resolution sees only the parameter -- into the conversion node: a type
// literal whose type is the number, which generation expands with
// genlConvert. The value is checked with no expected type, so an untyped
// literal keeps its i32 default and is converted from that. Returns 1 when the
// node was handled, lowered or refused, so the caller stops.
static int fnCallNumberFrom(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    if (!isTypeNode(node->objfn) || ((NameUseNode*)node->methfld)->namesym != fromName)
        return 0;
    INode *nbrtype = itypeGetTypeDcl(node->objfn);
    if (nbrtype->tag != IntNbrTag && nbrtype->tag != UintNbrTag && nbrtype->tag != FloatNbrTag)
        return 0;
    Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : ((NbrNode*)nbrtype)->namesym;
    // Whatever goes wrong below, the expression is a value of the number type
    node->vtype = node->objfn;

    if (node->args == NULL || node->args->used != 1) {
        errorMsgNode(node->args == NULL ? node->methfld : (INode*)node, ErrorNbrFrom,
            "%s.from converts one value: %s.from(value)", &written->namestr, &written->namestr);
        return 1;
    }
    if (namedValRefuseArgs(node->args, "a call"))
        return 1;
    INode **argp = &nodesGet(node->args, 0);
    inodeTypeCheckAny(pstate, argp);
    if (!isExpNode(*argp)) {
        errorMsgNode(*argp, ErrorNotTyped, "Expected a typed expression.");
        return 1;
    }
    if (inodeIsError(*argp))
        return 1;

    node->methfld = NULL;
    node->tag = TypeLitTag;
    typeLitNbrFromCheck(node, nbrtype);
    return 1;
}

// Is this, name resolved but not yet type checked, a type or module, or an
// instance of a generic one -- a path's base rather than a receiver?
static int fnCallIsPathBase(INode *node) {
    if (node->tag == FnCallTag && (node->flags & FlagIndex))
        node = ((FnCallNode*)node)->objfn;
    return isNameUseNode(node) && (isTypeNode(node) || nameUseNames(node, ModuleTag));
}

// Type arguments given to a method called on a receiver: 'h.pick[i32](6)', or
// 'h.pick[i32]' taking no arguments. The parser reads the member access
// 'h.pick' with no arguments, indexes it by the type arguments, and applies the
// call to that. A type is never an index, so a member access indexed by one is
// a generic method given its type arguments, and this lowers the whole of it --
// the call when there is one, else the index -- to the one method call
// 'h.pick(6)' calling the instance they name. The receiver is checked here, to
// learn what the name is, so the lowering is finished here too rather than
// handed back to check the receiver again. A member that is not a generic
// method is refused, which it was before: a type is not a value to index with.
// A path through a type or a module, 'Box[i64].make[i32]', is not a receiver
// and is left alone. Returns 1 when the node was handled.
static int fnCallMethodTypeArgs(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    FnCallNode *index = node;
    if (node->methfld == NULL && !(node->flags & FlagIndex) && node->objfn->tag == FnCallTag)
        index = (FnCallNode*)node->objfn;
    if (!(index->flags & FlagIndex) || (index->flags & (FlagRange | FlagBorrow))
        || index->methfld != NULL || index->objfn->tag != FnCallTag)
        return 0;
    // Type arguments, or numbers, 'h.pick[3]': told from an index only by
    // the name being a generic method taking a number, and then by the
    // receiver's type
    int numargs = 0;
    if (!fnCallHasTypeArgs(index)) {
        if (index->args == NULL || index->args->used == 0)
            return 0;
        INode **litp;
        uint32_t litcnt;
        for (nodesFor(index->args, litcnt, litp))
            if ((*litp)->tag != ULitTag)
                return 0;
        numargs = 1;
    }
    FnCallNode *member = (FnCallNode*)index->objfn;
    if (member->methfld == NULL || !isNameUseNode(member->methfld) || member->args != NULL
        || (member->flags & (FlagOperator | FlagIndex)) || fnCallIsPathBase(member->objfn))
        return 0;
    if (numargs && !genericValueFnNamed(((NameUseNode*)member->methfld)->namesym))
        return 0;

    inodeTypeCheckAny(pstate, &member->objfn);
    if (inodeIsError(member->objfn)) {
        node->vtype = errorType;
        return 1;
    }
    fnCallReadThroughRefs(member);
    NameUseNode *methfld = (NameUseNode*)member->methfld;
    INode *rcvtype = isExpNode(member->objfn) ? iexpGetDerefTypeDcl(member->objfn) : NULL;
    // An array or a slice has the methods of core's body 'Array[T]' (numbers
    // only: type arguments written to a method of an array's body are refused
    // as they were)
    INode *bodytype = NULL;
    if (numargs && rcvtype && !isMethodType(rcvtype)) {
        bodytype = fnCallSliceBodyOf(pstate, member);
        rcvtype = bodytype;
    }
    INode *found = rcvtype && isMethodType(rcvtype)
        ? aliasDclResolve(iNsTypeFindFnField((INsTypeNode*)rcvtype, methfld->namesym)) : NULL;
    // Numbers after a name that is not a generic method taking a number, on
    // this receiver, are an index, as they were. The receiver has been checked,
    // once, and the member's own check will not walk it again.
    if (numargs && (found == NULL || found->tag != FnDclTag || !(found->flags & FlagMethFld)
        || !genericHasValueParm(((FnDclNode*)found)->genericinfo))) {
        member->flags |= FlagRcvChecked;
        return 0;
    }
    if (found == NULL || found->tag != FnDclTag || !(found->flags & FlagMethFld)
        || ((FnDclNode*)found)->genericinfo == NULL) {
        errorMsgNode(nodesGet(index->args, 0), ErrorNotTyped,
            "Expected a typed expression: `%s` is not a generic method, so it takes no type arguments.",
            &methfld->namesym->namestr);
        node->vtype = errorType;
        return 1;
    }

    // One method call: the receiver, the member, and the call's own arguments,
    // positioned where 'h.pick(6)' would be
    Nodes *typeargs = index->args;
    inodeLexCopy((INode*)node, (INode*)member);
    node->objfn = member->objfn;
    node->methfld = (INode*)methfld;
    if (node == index)
        node->args = NULL;
    node->flags &= ~FlagIndex;

    INode **argsp;
    uint32_t cnt;
    int badarg = 0;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp)) {
            inodeTypeCheckAny(pstate, argsp);
            if (!isExpNode(*argsp)) {
                errorMsgNode(*argsp, ErrorNotTyped, "Expected a typed expression.");
                badarg = 1;
            }
        }
    }
    if (badarg || namedValRefuseArgs(node->args, "a call")) {
        node->vtype = errorType;
        return 1;
    }

    // The type arguments are checked as a generic function's are before it is
    // instantiated, so a written instance, 'Box[i32]', is the type it names
    for (nodesFor(typeargs, cnt, argsp))
        inodeTypeCheckAny(pstate, argsp);

    methfld->dclnode = (INode*)genericMethodInstance(pstate, node, (FnDclNode*)found, typeargs);
    if (methfld->dclnode == NULL) {
        node->vtype = errorType;
        return 1;
    }
    if (bodytype)
        fnCallLowerSliceMethod(pstate, node);
    else
        fnCallLowerMethod(pstate, node);
    return 1;
}

// The field a struct literal's argument gives a value to, as
// typeLitStructReorder will match it: a named value by its name, a value by
// position among the fields a literal writes (not a discriminant) when no named
// value comes before it. NULL where no field is certain yet.
FieldDclNode *fnCallTypeLitField(StructNode *strnode, Nodes *args, uint32_t argi) {
    INode *arg = nodesGet(args, argi);
    Name *name = arg->tag == NamedValTag ? ((NameUseNode*)((NamedValNode*)arg)->name)->namesym : NULL;
    if (name == NULL) {
        for (uint32_t i = 0; i < argi; i++)
            if (nodesGet(args, i)->tag == NamedValTag)
                return NULL;
    }
    uint32_t pos = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (field->flags & IsTagField)
            continue;
        if (name ? field->namesym == name : pos == argi)
            return field;
        ++pos;
    }
    return NULL;
}

// Does this argument name a permission, 'mut' in 'Rc[mut, Node]'?
static int fnCallArgIsPerm(INode *arg) {
    return arg != NULL && isNameUseNode(arg) && nameUseNames(arg, PermTag);
}

// A permission given in the brackets of anything but a region: a permission is
// a type, so a generic would otherwise take it as a type argument, and only a
// managed reference type has a slot for one. Reported, and the node becomes
// the error it is.
int fnCallRefusePermArg(FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    if (!(node->flags & FlagIndex) || node->args == NULL)
        return 0;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        if (fnCallArgIsPerm(*argsp)) {
            Name *head = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : NULL;
            // A region's head whose other argument is a value, not a type
            if (itypeManagedRefRegion((INode*)node) != NULL)
                errorMsgNode((INode*)node, ErrorRefTypeArgs,
                    "A managed reference type takes a permission, which may be left out, and a value type: '%s[%s, T]'.",
                    head ? &head->namestr : "R", &((NameUseNode*)*argsp)->namesym->namestr);
            else
                errorMsgNode(*argsp, ErrorPermNotRegion,
                    "%s%sA permission is given only to a region, a struct declaring 'is RegionRef', as a managed reference type's first argument: 'Rc[%s, T]'.",
                    head ? &head->namestr : "", head ? " is not a region. " : "",
                    &((NameUseNode*)*argsp)->namesym->namestr);
            *((INode**)nodep) = newErrorNode((INode*)node);
            return 1;
        }
    }
    return 0;
}

// The markers written after a type in a reference type's brackets,
// 'So[Shape + Sendable + Shareable]' (the parser joins them with '+', as
// any sum): the type they follow is returned and the markers, RefMark bits, are
// put in *marks. A '+' joining anything but a marker is reported, and the
// operands that are not markers are left in place.
static INode *fnCallMarkSplit(INode *arg, uint8_t *marks) {
    while (arg->tag == FnCallTag && (arg->flags & FlagOperator)
        && ((FnCallNode*)arg)->methfld != NULL && isNameUseNode(((FnCallNode*)arg)->methfld)
        && ((NameUseNode*)((FnCallNode*)arg)->methfld)->namesym == plusName
        && ((FnCallNode*)arg)->args != NULL && ((FnCallNode*)arg)->args->used == 1
        && ((FnCallNode*)arg)->objfn != NULL) {
        FnCallNode *sum = (FnCallNode*)arg;
        INode *right = nodesGet(sum->args, 0);
        INode *rightdcl = isNameUseNode(right) ? nameUseGetDcl((NameUseNode*)right) : NULL;
        uint8_t mark = rightdcl == (INode*)sendableTrait ? RefMarkSendable
            : rightdcl == (INode*)shareableTrait ? RefMarkShareable : 0;
        if (mark == 0) {
            // A sum of values: not ours. A type that is no marker is refused, and left off
            if (!(isNameUseNode(right) && rightdcl != NULL && isTypeNode(right)))
                return arg;
            errorMsgNode(right, ErrorMarkUse,
                "After a '+' in a reference type go the markers of the value it points at, '+ Sendable' and '+ Shareable'; other traits are not intersected yet.");
        }
        *marks |= mark;
        arg = sum->objfn;
    }
    return arg;
}

// Lower a managed reference type, 'Rc[Node]' or 'Rc[mut, Node]', into the
// reference node it names, and type check that. The region is the head; the
// brackets hold an optional permission, 'uni' when it is left out, then the
// value type. The reference is virtual exactly when the value type is an open
// trait: an enum is a trait to the compiler too, and a reference to one is thin.
// A virtual one may say what the value behind it is, 'So[fn() + Sendable]'.
// From here on nothing downstream sees how the type was written.
static void fnCallLowerManagedRef(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    Nodes *args = node->args;
    INode *perm = NULL;
    uint8_t marks = 0;
    INode *vtype = fnCallMarkSplit(nodesGet(args, args->used - 1), &marks);
    Name *regname = ((StructNode*)itypeManagedRefRegion((INode*)node))->namesym;
    if (args->used > 2)
        errorMsgNode(nodesGet(args, 2), ErrorRefTypeArgs,
            "A managed reference type takes a permission, which may be left out, and a value type: '%s[mut, T]' or '%s[T]'.",
            &regname->namestr, &regname->namestr);
    // The first of two is the permission: a static one, or a struct standing in
    // the slot as the unbuilt dynamic permissions do (refThreadBinds), which the
    // '+R-Lock T' spelling took and which a traced region's rules judge
    if (args->used >= 2) {
        INode *first = nodesGet(args, 0);
        INode *firstdcl = isNameUseNode(first) ? nameUseGetDcl((NameUseNode*)first) : NULL;
        if (fnCallArgIsPerm(first)
            || (firstdcl && firstdcl->tag == StructTag && !(firstdcl->flags & TraitType)))
            perm = first;
        else
            errorMsgNode(first, ErrorRefTypePerm,
                "A managed reference type's first of two arguments is its permission: '%s[mut, T]'.",
                &regname->namestr);
    }
    // Left out, the permission is the reference's to settle once it knows what
    // it refers to: 'uni', or 'imm' where the type declares Immutable
    // (refTypeCheck)
    if (perm == NULL)
        perm = unknownType;
    if (fnCallArgIsPerm(vtype)) {
        errorMsgNode(vtype, ErrorRefTypePerm,
            "A managed reference type's last argument is the type it refers to, not a permission: '%s[%s, T]'.",
            &regname->namestr, &((NameUseNode*)vtype)->namesym->namestr);
        *((INode**)nodep) = newErrorNode((INode*)node);
        return;
    }

    // The value type is checked first, since whether it is an open trait decides
    // the reference's shape, and a generic trait's instance exists only once
    // checked. A type under check that refers to itself, 'next Rc[Node]', finds
    // Node in progress, as a reference always has. It is the reference's
    // target, so it is not laid out while a layout is in flight
    // (refTargetTypeCheck).
    uint16_t tag = RefTag;
    if (refTargetTypeCheck(pstate, &vtype, NULL)) {
        INode *vdcl = itypeGetTypeDcl(vtype);
        if (vdcl->tag == StructTag && (vdcl->flags & TraitType) && !(vdcl->flags & HasTagField))
            tag = VirtRefTag;
        // 'So[fn(i32) i32]': an owner of anything callable with that signature
        else if (vdcl->tag == FnSigTag)
            tag = VirtRefTag;
    }
    else {
        // Reported where the value type was written; the type stands as the error
        *((INode**)nodep) = newErrorNode((INode*)node);
        return;
    }

    if (marks && tag != VirtRefTag) {
        errorMsgNode((INode*)node, ErrorMarkUse,
            "A marker after a '+' says what the value behind a virtual reference is, since its type is hidden: %s[...] here refers to %s, a type that is known, so ask it of the type, 'T is %s', where it is written.",
            &regname->namestr, isNameUseNode(vtype) ? &((NameUseNode*)vtype)->namesym->namestr : "a type",
            (marks & RefMarkSendable) ? "Sendable" : "Shareable");
        *((INode**)nodep) = newErrorNode((INode*)node);
        return;
    }
    RefNode *ref = newRefNode(tag);
    inodeLexCopy((INode*)ref, (INode*)node);
    ref->region = node->objfn;
    ref->perm = perm;
    ref->vtexp = vtype;
    ref->marks = marks;
    *((INode**)nodep) = (INode*)ref;
    inodeTypeCheckAny(pstate, (INode**)nodep);
}

// The signature of the callable a candidate takes at parameter 'pos' (counting
// a method's self): the function reference's, a callable reference's, or the
// signature bound of the type parameter it is (or is a reference to). NULL
// where the parameter is not callable.
static FnSigNode *fnCallCallableParm(FnDclNode *cand, uint32_t pos) {
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(cand->vtype);
    if (pos >= sig->parms->used)
        return NULL;
    if (cand->genericinfo) {
        INode *refperm;
        FnSigNode *bound = genericParmBound(cand, pos, &refperm);
        if (bound == NULL) {
            Name *method;
            uint32_t count;
            StructNode *trait = genericParmTraitBound(cand, pos, &refperm);
            bound = trait ? closureTraitSig(trait, &method, &count) : NULL;
        }
        return bound;
    }
    INode *ptype = iexpGetTypeDcl(nodesGet(sig->parms, pos));
    if (ptype->tag == RefTag) {
        INode *target = itypeGetTypeDcl(((RefNode*)ptype)->vtexp);
        if (target->tag == FnSigTag)
            return (FnSigNode*)target;
    }
    if (ptype->tag == VirtRefTag) {
        INode *target = itypeGetTypeDcl(((RefNode*)ptype)->vtexp);
        Name *method;
        uint32_t count;
        return target->tag == StructTag ? closureTraitSig((StructNode*)target, &method, &count) : NULL;
    }
    return NULL;
}

// Can a value of this type be called: a function reference, a reference or owner
// of a callable ('&<fn(...)', 'So[fn(...)]'), or a struct with a '()'?
static int fnCallTypeCallable(INode *type) {
    if (type == NULL || type == unknownType || !isTypeNode(type))
        return 1;
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag == VirtRefTag)
        return 1;
    if (dcl->tag == RefTag)
        dcl = itypeGetTypeDcl(((RefNode*)dcl)->vtexp);
    if (dcl->tag == FnSigTag)
        return 1;
    if (dcl->tag != StructTag)
        return 0;
    INode *call = iNsTypeFindFnField((INsTypeNode*)dcl, parensName);
    return call != NULL && (call->tag == FnDclTag || call->tag == FnOverloadDclTag || call->tag == AliasDclTag);
}

// 't.profile(3.)', where 'profile' is a field of the receiver: the call is of what
// the field holds, as '(t.profile)(3.)' is. The call's receiver becomes the field's
// access. Answers 0, having reported it, when the field holds nothing callable.
static int fnCallFieldCall(TypeCheckState *pstate, FnCallNode *node, FieldDclNode *fld, INode *rcvtype) {
    if (!fnCallTypeCallable(fld->vtype)) {
        char held[200] = "";
        itypeSpellCat(held, sizeof(held), fld->vtype, 0);
        errorMsgNode((INode*)node, ErrorFldArgs,
            "`%s` is a field of %s holding %s, which is not callable, so it takes no arguments. A field is called when it holds a function reference, a `&<fn(...)`, a `So[fn(...)]` or `Rc[fn(...)]`, or a struct with a `()` method.",
            &fld->namesym->namestr, itypeName(rcvtype), held);
        node->vtype = errorType;
        return 0;
    }
    FnCallNode *access = newFnCallNode(node->objfn, 0);
    inodeLexCopy((INode*)access, (INode*)node);
    access->methfld = node->methfld;
    node->objfn = (INode*)access;
    node->methfld = NULL;
    return 1;
}

// A parameter that takes a borrowed callable, '&<fn(x i32) i32' or '&<mut fn(...)':
// the signature a closure literal given to it is to fit, and the permission it
// is lent with. A parameter that takes a borrowed trait with one method,
// '&<Shape', is the same: the literal fills the method, whose name is in
// '*method'. NULL for any other parameter; '*bad' is the trait of a borrowed
// virtual reference a literal cannot fill (it has not exactly one method).
static FnSigNode *fnCallLendParm(INode *ptype, INode **lendperm, Name **method, StructNode **filled, StructNode **bad) {
    *method = NULL;
    *filled = NULL;
    *bad = NULL;
    if (ptype == NULL || ptype == unknownType || !isTypeNode(ptype))
        return NULL;
    INode *dcl = itypeGetTypeDcl(ptype);
    if (dcl->tag != VirtRefTag || itypeGetTypeDcl(((RefNode*)dcl)->region) != (INode*)borrowRef)
        return NULL;
    FnSigNode *sig = fnSigOfCallTrait(((RefNode*)dcl)->vtexp);
    if (sig == NULL) {
        StructNode *trait = (StructNode*)itypeGetTypeDcl(((RefNode*)dcl)->vtexp);
        uint32_t count;
        if (trait->tag != StructTag || !(trait->flags & TraitType))
            return NULL;
        sig = closureTraitSig(trait, method, &count);
        if (sig == NULL) {
            *bad = trait;
            return NULL;
        }
        *filled = trait;
    }
    *lendperm = (INode*)newPermUseNode((PermNode*)itypeGetTypeDcl(((RefNode*)dcl)->perm));
    return sig;
}

// A closure literal wanted where 'trait' is, which has not exactly one method for
// it to fill: refused, naming what the trait has
void fnCallClosureTraitRefused(INode *lit, StructNode *trait) {
    Name *method;
    uint32_t count;
    closureTraitSig(trait, &method, &count);
    if (count == 0)
        errorMsgNode(lit, ErrorClosureTrait,
            "A closure fills a trait that has one required method, and %s has none.", &trait->namesym->namestr);
    else
        errorMsgNode(lit, ErrorClosureTrait,
            "A closure fills a trait that has exactly one required method and no field (any default methods come with it), and %s has %u required methods and fields (the first required method is `%s`): write a struct that implements it.",
            &trait->namesym->namestr, count, method ? &method->namestr : "...");
}

// Check a closure literal given as an argument to a parameter of type 'ptype'.
// Where that parameter takes a borrowed callable, the literal is lent to it as
// a temporary of the statement, as a borrow of it would be.
static void fnCallCheckClosureArg(TypeCheckState *pstate, INode **argp, INode *ptype) {
    INode *lendperm;
    Name *method;
    StructNode *bad;
    StructNode *filled;
    FnSigNode *sig = fnCallLendParm(ptype, &lendperm, &method, &filled, &bad);
    if (bad) {
        fnCallClosureTraitRefused(*argp, bad);
        *argp = newErrorNode(*argp);
        return;
    }
    if (sig == NULL) {
        inodeTypeCheck(pstate, argp, ptype);
        return;
    }
    INode *lit = *argp;
    *argp = (INode*)newRefNodeFull(BorrowTag, lit, borrowRef, lendperm, lit);
    closureHint = sig;
    closureMethod = method;
    closureTrait = filled;
    inodeTypeCheck(pstate, argp, unknownType);
    closureHint = NULL;
    closureMethod = NULL;
    closureTrait = NULL;
    // A literal that failed is reported once; the lent borrow of it is no argument to coerce
    if ((*argp)->tag == BorrowTag && inodeIsError(((RefNode*)*argp)->vtexp))
        *argp = newErrorNode(*argp);
}

// The overload a closure literal written as argument 'argi' calls for. It is
// considered against the overloads whose parameter there is callable, so a
// number's overload never takes it; among those, the one taking as many
// parameters as the literal writes; and where several remain, the literal
// written with its parameters' types picks the one with exactly them. Its body
// is checked against none of them. NULL once the refusal is reported.
static FnDclNode *fnCallClosureOverload(TypeCheckState *pstate, FnCallNode *node, INode *binding,
        uint32_t firstparm, uint32_t argi) {
    ClosureNode *clo = (ClosureNode*)nodesGet(node->args, argi);
    fnCallDemandCandidates(binding);
    INode **candp;
    uint32_t ncand;
    if (binding->tag == FnDclTag) {
        candp = &binding;
        ncand = 1;
    }
    else {
        candp = &nodesGet(((FnOverloadDclNode*)binding)->overloads, 0);
        ncand = ((FnOverloadDclNode*)binding)->overloads->used;
    }
    Name *written = binding->tag == FnDclTag ? ((FnDclNode*)binding)->namesym : ((FnOverloadDclNode*)binding)->namesym;
    uint32_t nclo = closureParmCount(clo);
    FnDclNode *viable[32];
    uint32_t nviable = 0;
    uint32_t j;
    for (; ncand-- ; ++candp) {
        FnDclNode *cand = (FnDclNode*)*candp;
        FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(cand->vtype);
        if (node->args->used + firstparm > sig->parms->used)
            continue;
        FnSigNode *callable = fnCallCallableParm(cand, firstparm + argi);
        if (callable == NULL || callable->parms->used != nclo)
            continue;
        // A generic is judged by what its other arguments give its type parameters
        int fits = !cand->genericinfo || genericOverloadViable(cand, NULL, node->args, firstparm);
        for (j = 0; j < node->args->used && !cand->genericinfo; ++j) {
            INode **argp = &nodesGet(node->args, j);
            if (j == argi || (*argp)->tag == ClosureTag || !isExpNode(*argp))
                continue;
            if (iexpMatches(argp, ((VarDclNode*)nodesGet(sig->parms, firstparm + j))->vtype, Coercion) == NoMatch)
                fits = 0;
        }
        if (fits && nviable < 32)
            viable[nviable++] = cand;
    }
    // Written in the full form, its parameters' types pick exactly
    if (nviable > 1 && !closureNeedsSig(clo)) {
        INode **parmp;
        uint32_t cnt;
        for (nodesFor(clo->sig->parms, cnt, parmp))
            itypeTypeCheck(pstate, &((VarDclNode*)*parmp)->vtype);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < nviable; ++i) {
            FnSigNode *callable = fnCallCallableParm(viable[i], firstparm + argi);
            int same = 1;
            for (j = 0; j < nclo && same; ++j)
                if (!itypeIsSame(iexpGetTypeDcl(nodesGet(clo->sig->parms, j)), iexpGetTypeDcl(nodesGet(callable->parms, j))))
                    same = 0;
            if (same)
                viable[kept++] = viable[i];
        }
        nviable = kept;
    }
    if (nviable == 1)
        return viable[0];
    if (nviable == 0) {
        errorMsgNode((INode*)clo, ErrorClosureOverload,
            "No overload of %s takes a closure of %u parameter%s here: one of them needs a callable parameter, taking that many parameters, at this position.",
            &written->namestr, nclo, nclo == 1 ? "" : "s");
        return NULL;
    }
    char names[300] = "";
    for (uint32_t i = 0; i < nviable; ++i) {
        size_t used = strlen(names);
        snprintf(names + used, sizeof(names) - used, "%s%s", i ? ", " : "", &viable[i]->namesym->namestr);
    }
    errorMsgNode((INode*)clo, ErrorClosureOverload,
        "This closure could be given to more than one overload of %s (%s). Write the parameter's types, as in 'fn (p Vec3) { ... }', to say which.",
        &written->namestr, names);
    return NULL;
}

// Check the closure literals among a call's arguments, once the others are
// checked: each takes its signature from the parameter it fills in the
// generic's bound, or from the overload its parameter count and types pick.
// A closure given to a parameter taking a reference to the callable ('f &F')
// is lent as a temporary, as a borrow of it would be. Answers 0 once refused.
static int fnCallClosureArgs(TypeCheckState *pstate, FnCallNode *node, FnDclNode *gengeneric, INode *overloadset,
        uint32_t firstparm) {
    uint32_t argi = 0;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        if ((*argsp)->tag != ClosureTag) {
            ++argi;
            continue;
        }
        FnDclNode *pick = NULL;
        if (overloadset) {
            pick = fnCallClosureOverload(pstate, node, overloadset, firstparm, argi);
            if (pick == NULL) {
                *argsp = newErrorNode(*argsp);
                return 0;
            }
        }
        FnDclNode *generic = gengeneric ? gengeneric : (pick && pick->genericinfo ? pick : NULL);
        if (generic) {
            INode *refperm;
            Name *method = NULL;
            StructNode *filled = NULL;
            FnSigNode *sig = genericClosureSig(pstate, generic, node->args, firstparm, argi, &refperm);
            // A parameter bound by a trait with one method: the literal fills that method
            if (sig == NULL) {
                INode *traitperm;
                StructNode *trait = genericParmTraitBound(generic, firstparm + argi, &traitperm);
                if (trait) {
                    uint32_t count;
                    sig = closureTraitSig(trait, &method, &count);
                    if (sig == NULL) {
                        fnCallClosureTraitRefused(*argsp, trait);
                        *argsp = newErrorNode(*argsp);
                        return 0;
                    }
                    refperm = traitperm;
                    filled = trait;
                }
            }
            if (refperm) {
                INode *lit = *argsp;
                *argsp = (INode*)newRefNodeFull(BorrowTag, lit, borrowRef, refperm, lit);
            }
            closureHint = sig;
            closureMethod = method;
            closureTrait = filled;
            inodeTypeCheck(pstate, argsp, unknownType);
            closureHint = NULL;
            closureMethod = NULL;
            closureTrait = NULL;
        }
        else if (pick) {
            FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(pick->vtype);
            fnCallCheckClosureArg(pstate, argsp, ((VarDclNode*)nodesGet(sig->parms, firstparm + argi))->vtype);
        }
        else
            inodeTypeCheck(pstate, argsp, unknownType);
        // A closure that failed leaves the call nothing to infer from: the cause is
        // reported, and the call is given up without a second report
        if (inodeIsError(*argsp) || ((*argsp)->tag == BorrowTag && inodeIsError(((RefNode*)*argsp)->vtexp))) {
            *argsp = newErrorNode(*argsp);
            return 0;
        }
        ++argi;
    }
    return 1;
}

// A field, a method or an index reached through a reference to a reference, to
// an owner of one, to a slice or to a virtual reference ('&&Pt', '&&&Pt',
// '&mut &Pt', '&So[&Pt]') reads through every reference but the last: the
// receiver becomes '*r', '**r', ... as though the dereferences were written, so
// everything after sees a receiver of one reference level. Only a '.name' or an
// index reads through, never an operator: the outer reference's own operators
// ('===') and the comparisons, which read through on their own terms, are the
// operator's to select first. A key and a lock-managed reference are left to
// the refusal each has, and a raw pointer is not read through.
//
// What the path permits is what the explicit dereferences would: 'r.x' is
// '(**r).x', so a borrow read out of another keeps its own permission. An owner
// reached through a step that cannot write lends only what that step lets
// (borrowOwnerLendRefused: a '&' lends only '&' of an owner), so the dereference
// that reaches it is typed with the permission of that step, and every use
// after it -- a field written, a method wanting 'self &mut' -- is held to it.
static void fnCallReadThroughRefs(FnCallNode *node) {
    if (!isExpNode(node->objfn) || (node->flags & FlagOperator))
        return;
    // A borrowed or ranged index has the borrow the parser put round its receiver
    // (borrowReassocIndex), which reads through on its own
    if (node->flags & FlagIndex ? (node->flags & (FlagBorrow | FlagRange)) != 0
                                : !(node->methfld && isNameUseNode(node->methfld)))
        return;
    INode *clamp = NULL;  // the permission of the first step that cannot write
    for (;;) {
        INode *type = iexpGetTypeDcl(node->objfn);
        if (type->tag != RefTag || lifeIsKey(type) || permIsLock(((RefNode*)type)->perm))
            return;
        RefNode *step = (RefNode*)type;
        RefNode *held = (RefNode*)itypeGetTypeDcl(step->vtexp);
        if (held->tag != RefTag && held->tag != ArrayRefTag && held->tag != VirtRefTag)
            return;
        if (clamp == NULL && !(permGetFlags(step->perm) & MayWrite))
            clamp = step->perm;
        derefInject(&node->objfn);
        if (clamp && held->tag == RefTag && itypeGetTypeDcl(held->region) != borrowRef
            && !permIsLock(held->perm) && (permGetFlags(held->perm) & MayWrite)) {
            RefNode *seen = newRefNode(held->tag);
            *seen = *held;
            seen->typeinfo = NULL;
            seen->perm = clamp;
            ((IExpNode*)node->objfn)->vtype = (INode*)seen;
        }
    }
}

// Perform type check on function/method call node
// This should only be run once on a node, as it mutably lowers the node to another form:
// - If a generic/macro, it instantiates, then type checks instantiated nodes
// - If a type literal, it dispatches it to typelit for handlings
// - If a field access, it turns it into a FldAccess node
// - If an array index, it turns it into an ArrIndex node
// - A method call is resolved by lookup and lowered to a function call
// - A function call coerces and injects arguments as needed
void fnCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // 'new Point(1, 2)': a construction, which selects one of the type's 'init's
    if (node->flags & FlagNew) {
        typeLitNewCheck(pstate, nodep);
        return;
    }

    // A callee a global's 'use' clause folded into this module is reached through
    // that global, so the call is rewritten to 'global.name(...)' before anything
    // below reads the callee. Ahead of every other test here deliberately: from
    // this point the node is an ordinary member call, so the macro-method probe,
    // overload selection, the receiver adjustments and generation all see the
    // path the author could have written, and none of them learns about folding.
    if (isNameUseNode(node->objfn) && node->methfld == NULL) {
        AliasDclNode *alias = (AliasDclNode*)((NameUseNode*)node->objfn)->dclnode;
        if (alias && alias->tag == AliasDclTag && alias->through) {
            FnCallNode *access = aliasDclThroughAccess(alias, node->objfn);
            node->objfn = access->objfn;
            node->methfld = access->methfld;
        }
    }

    // 'x[i] += 1', 'x[i].f++': an operator changing its operand in place
    // writes to it, so an index at the root of it is in set position, as an
    // assignment's is (assignTypeCheck)
    if ((node->flags & FlagLvalOp) && node->objfn) {
        FnCallNode *setindex = fnCallSetIndexRoot(node->objfn);
        if (setindex)
            fnCallSetIndex = setindex;
    }

    // 'h.pick[i32](6)': a generic method given its type arguments
    if (fnCallMethodTypeArgs(pstate, nodep))
        return;

    // If we have a true macro, go handle it elsewhere
    // Note: Macros don't want us to type check arguments until after substitution
    //
    // Only when the macro name is what is being called. An operator or method
    // application builds the same node shape as a call -- 'TWO + 1' is
    // objfn 'TWO', methfld '+', one argument -- so methfld is what tells the two
    // apart. With it set, the name is the receiver a value is expected of, and
    // it expands below like the name in any other value position; the operator
    // is then applied to what it expanded to.
    if (nameUseNames(node->objfn, MacroDclTag) && node->methfld == NULL) {
        macroCallTypeCheck(pstate, nodep);
        return;
    }

    // '<-' given a list of entries, entries other than plain values, or the
    // contents of a construction becomes the applications they stand for
    // (contents.c). Ahead of the arguments below, because the list is taken
    // apart rather than checked as an argument in its own right.
    if (contentsIsAppend(node)) {
        contentsLower(pstate, nodep);
        return;
    }

    // 'Rc[mut, Node]' is a managed reference type, lowered here before the
    // struct-literal pass below could take its head for a literal's struct.
    // Told from the region's value in brackets, 'Rc[1usize]' -- a struct's
    // literal, refused below -- by its arguments being types
    // (itypeIsManagedRefType).
    if (itypeIsManagedRefType((INode*)node)) {
        fnCallLowerManagedRef(pstate, nodep);
        return;
    }
    if (fnCallRefusePermArg(nodep))
        return;

    // An overload name has no value of its own, so it is only legal here, naming what
    // is called. Skipping the ordinary name-use check leaves that check free to reject
    // the overload name everywhere else.
    int calleeIsOverload = nameUseNames(node->objfn, FnOverloadDclTag);
    // Not checked as a name, so a generic base's overload name, bare inside an
    // extension's braces, is pointed at its instance's set here
    if (calleeIsOverload)
        nameUseBaseInstanceMember(pstate, (NameUseNode*)node->objfn);

    // A member named on a receiver may be a macro method, and a macro's
    // arguments stay unchecked until they have been substituted -- so the
    // receiver alone is checked first, its type asked what the name binds, and
    // only then are the arguments checked. An operator is never a macro, and
    // keeps the order the arguments always had. A member slot holding a tuple
    // index rather than a name is not a member access by name.
    int objfnChecked = 0;
    // The signature whose parameter types the arguments are checked against,
    // when one declaration is all the callee can be, and the parameter the
    // first written argument fills
    FnSigNode *argsig = NULL;
    uint32_t firstparm = 0;
    // Where the callee is a generic, or an overload set, a closure literal
    // among the arguments takes its signature from the parameter it fills once
    // the other arguments are checked (closure.c)
    FnDclNode *gengeneric = NULL;
    INode *overloadset = NULL;
    if (calleeIsOverload && node->methfld == NULL && !(node->flags & FlagIndex))
        overloadset = nameUseGetDcl((NameUseNode*)node->objfn);
    if (node->methfld && isNameUseNode(node->methfld)
        && !(node->flags & FlagOperator) && !calleeIsOverload) {
        if (node->flags & FlagRcvChecked)
            node->flags &= ~FlagRcvChecked;
        else
            inodeTypeCheckAny(pstate, &node->objfn);
        objfnChecked = 1;
        if (inodeIsError(node->objfn)) {
            node->vtype = errorType;
            return;
        }
        fnCallReadThroughRefs(node);
        // 'stack[i64].push(x)': a path through an instance of a generic module,
        // which exists only now that the instantiation above made it
        if (nameUseNames(node->objfn, ModuleTag)) {
            if (fnCallModuleInstancePath(pstate, nodep))
                return;
            objfnChecked = 0;
            calleeIsOverload = nameUseNames(node->objfn, FnOverloadDclTag);
        }
        // 'List[i64].empty()': a path through an instance of a generic type,
        // which likewise exists only now
        else if (isTypeNode(node->objfn) && nameUseNames(node->objfn, StructTag)) {
            if (fnCallTypeInstancePath(pstate, nodep))
                return;
            if (node->methfld == NULL) {
                objfnChecked = 0;
                calleeIsOverload = nameUseNames(node->objfn, FnOverloadDclTag);
            }
        }
        // 'u64.from(count)': a number type's conversion
        else if (fnCallNumberFrom(pstate, nodep))
            return;
        else if (isExpNode(node->objfn)) {
            INode *rcvtype = iexpGetDerefTypeDcl(node->objfn);
            if (isMethodType(rcvtype)) {
                Name *membersym = ((NameUseNode*)node->methfld)->namesym;
                INode *found = iNsTypeFindFnField((INsTypeNode*)rcvtype, membersym);
                int folded = found && found->tag == AliasDclTag;
                if (folded)
                    found = aliasDclResolve(found);
                // 't.profile(3.)' with a field of that name calls what the field
                // holds, as '(t.profile)(3.)' does. A field and a method never share
                // a name, so it cannot be a method's call.
                if (found && found->tag == FieldDclTag && node->args != NULL && (found->flags & FlagMethFld)
                    && !(node->flags & (FlagIndex | FlagOperator))) {
                    if (!fnCallFieldCall(pstate, node, (FieldDclNode*)found, rcvtype)) {
                        *((INode**)nodep) = newErrorNode((INode*)node);
                        return;
                    }
                    objfnChecked = 0;
                    found = NULL;
                }
                if (found && found->tag == MacroDclTag && (found->flags & FlagMethFld)) {
                    // A folded macro method expands with the field it was
                    // folded through as its self, as a folded method runs with it
                    if (folded)
                        structFoldReceiver(pstate, (StructNode*)rcvtype, membersym, &node->objfn, (INode*)node);
                    macroMethodTypeCheck(pstate, nodep, (MacroDclNode*)found);
                    return;
                }
                // One method, not generic: its parameters after 'self' are
                // what the arguments are wanted as
                if (found && found->tag == FnDclTag && (found->flags & FlagMethFld)
                    && ((FnDclNode*)found)->genericinfo == NULL) {
                    fnCallDemandCandidates(found);
                    argsig = (FnSigNode*)((FnDclNode*)found)->vtype;
                    firstparm = 1;
                }
                else if (found && found->tag == FnDclTag && (found->flags & FlagMethFld)) {
                    gengeneric = (FnDclNode*)found;
                    firstparm = 1;
                }
                else if (found && found->tag == FnOverloadDclTag && (found->flags & FlagMethFld)) {
                    overloadset = found;
                    firstparm = 1;
                }
            }
        }
    }
    // A function named directly, one declaration and not generic, is checked
    // ahead of its arguments, so its parameters are what they are wanted as. A
    // method's name written bare is 'self.method', whose 'self' is not written.
    else if (node->methfld == NULL && !(node->flags & FlagIndex) && !calleeIsOverload
        && nameUseNames(node->objfn, FnDclTag)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)node->objfn);
        if (genericGetInfo(dcl) == NULL) {
            inodeTypeCheckAny(pstate, &node->objfn);
            objfnChecked = 1;
            INode *sig = inodeIsError(node->objfn) ? NULL : iexpGetDerefTypeDcl(node->objfn);
            if (sig && sig->tag == FnSigTag) {
                argsig = (FnSigNode*)sig;
                firstparm = (dcl->flags & FlagMethFld) && !(node->objfn->flags & FlagQualified) ? 1 : 0;
            }
        }
        else if (dcl->tag == FnDclTag) {
            gengeneric = (FnDclNode*)dcl;
            firstparm = (dcl->flags & FlagMethFld) && !(node->objfn->flags & FlagQualified) ? 1 : 0;
        }
    }
    // A struct's literal, the struct named directly and not generic: its
    // fields are what its values are wanted as
    StructNode *litstruct = NULL;
    if (node->methfld == NULL && (node->flags & FlagIndex) && isTypeNode(node->objfn)
        && nameUseNames(node->objfn, StructTag)
        && genericGetInfo(nameUseGetDcl((NameUseNode*)node->objfn)) == NULL) {
        inodeTypeCheckAny(pstate, &node->objfn);
        objfnChecked = 1;
        INode *littype = itypeGetTypeDcl(node->objfn);
        if (littype->tag == StructTag)
            litstruct = (StructNode*)littype;
    }

    // Type check arguments (methfld is handled later), each against the type of
    // the parameter or field it fills when the callee is already known. That is
    // what lets an 'if', a block or an array literal passed as an argument be
    // coerced branch by branch, as it is when a variable of that type is
    // initialized with it. Overload selection needs the arguments' own types
    // to choose, so an overload set's arguments are checked with no expectation.
    INode **argsp;
    uint32_t cnt;
    if (node->args) {
        uint32_t argi = 0;
        for (nodesFor(node->args, cnt, argsp)) {
            INode *expect = unknownType;
            if (argsig && firstparm + argi < argsig->parms->used)
                expect = ((IExpNode*)nodesGet(argsig->parms, firstparm + argi))->vtype;
            else if (litstruct) {
                FieldDclNode *field = fnCallTypeLitField(litstruct, node->args, argi);
                if (field)
                    expect = field->vtype;
            }
            // A closure literal given to a generic or an overload set waits for
            // the other arguments, which say what its signature is
            if ((*argsp)->tag == ClosureTag && (gengeneric || overloadset)) {
                ++argi;
                continue;
            }
            if ((*argsp)->tag == ClosureTag)
                fnCallCheckClosureArg(pstate, argsp, expect);
            else
                inodeTypeCheck(pstate, argsp, expect);
            ++argi;
        }
        if (gengeneric || overloadset) {
            if (!fnCallClosureArgs(pstate, node, gengeneric, overloadset, firstparm)) {
                node->vtype = errorType;
                return;
            }
        }
    }

    // Perform generic substitution (if requested) and quit if that finishes processing
    if (genericSubstitute(pstate, nodep))
        return;

    if (!calleeIsOverload && !objfnChecked)
        inodeTypeCheckAny(pstate, &node->objfn);

    // A callee already reported as bad -- a generic that could not be
    // instantiated, say -- leaves nothing to call. The call inherits the mark
    // rather than earning a second diagnostic saying its callee is not callable.
    if (inodeIsError(node->objfn)) {
        node->vtype = errorType;
        return;
    }
    fnCallReadThroughRefs(node);

    // All arguments must now be expressions
    int badarg = 0;
    if (node->args) {
        for (nodesFor(node->args, cnt, argsp))
            if (!isExpNode(*argsp)) {
                errorMsgNode(*argsp, ErrorNotTyped, "Expected a typed expression.");
                badarg = 1;
            }
    }
    if (badarg) {
        node->vtype = errorType;
        return;
    }

    // 'name: value' is a type literal's alone; a function, method, closure or
    // initializer call and an index have no parameter a name is matched to
    int isTypeLit = isTypeNode(node->objfn) && (node->flags & FlagIndex);
    if (!isTypeLit && namedValRefuseArgs(node->args, node->flags & FlagIndex ? "an index" : "a call")) {
        node->vtype = errorType;
        return;
    }

    // If objfn is a type: a literal in brackets, or the type called
    if (isTypeNode(node->objfn)) {
        // A literal in brackets: a variant's, 'Some[x]', or a struct's, refused
        // but where an allocation takes it as its value (typeLitTypeCheck)
        if (node->flags & FlagIndex) {
            node->tag = TypeLitTag;
            node->vtype = node->objfn;
            typeLitTypeCheck(pstate, *nodep);
            return;
        }
        // A member named on a type is a path through that type's namespace, and
        // name resolution collapses every path whose base it can see: a module,
        // a struct or a trait. One that arrives here is one it could not -- an
        // alias, a number type's member other than 'from' (fnCallNumberFrom
        // took that), a generic parameter, or a generic instance's member other
        // than a function (fnCallTypeInstancePath took those). Diagnosed rather
        // than left to read as a bad call.
        if (node->methfld != NULL) {
            errorMsgNode(node->objfn, ErrorUnkName,
                "A path may pass through a module, a struct or a trait, and a number type has 'from'; reaching a member through anything else is not built.");
            node->vtype = errorType;
            return;
        }
        // A type is not called: a value of it is constructed with 'new', which
        // runs one of its 'init's (typeLitNewCheck)
        INode *typedcl = itypeGetTypeDcl(node->objfn);
        Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym
            : typedcl->tag == StructTag ? ((StructNode*)typedcl)->namesym : NULL;
        errorMsgNode((INode*)node, ErrorInitCall,
            "A type is not called: its value is constructed with 'new', 'new %s(...)'.",
            written ? &written->namestr : "T");
        node->vtype = errorType;
        return;
    }
    
    if (!isExpNode(node->objfn)) {
        errorMsgNode(node->objfn, ErrorNotTyped, "Expected a typed expression.");
        node->vtype = errorType;
        return;
    }

    // If objfn is the name of a method/field, rewrite to: self.method
    if (isNameUseNode(node->objfn) && isExpNode(node->objfn)
        && ((NameUseNode*)node->objfn)->dclnode->flags & FlagMethFld
        && !(node->objfn->flags & FlagQualified)) {
        // Only a method has a receiver to reach a method through: not a static
        // function's body, and not a signature, which is checked with no
        // function of its own around it -- as for a bare field name, in
        // nameUseTypeCheck. A member's name hides a module of the same name
        // inside its type, so 'mesh.Mesh' in the signature of a method named
        // 'mesh' arrives here.
        // (A closure's '()' reaches it through the self of the method the
        // closure is written in.)
        if (pstate->fn == NULL || !(pstate->fn->flags & FlagMethFld)
            || (pstate->fn->closure && pstate->fn->closure->outerself == NULL)) {
            nameUseNoSelf(pstate, (NameUseNode*)node->objfn);
            node->vtype = errorType;
            return;
        }
        // Build a resolved 'self' node
        NameUseNode *selfnode = newNameUseNode(selfName);
        selfnode->dclnode = closureSelfParm(pstate->fn);
        selfnode->vtype = ((VarDclNode*)selfnode->dclnode)->vtype;
        // Reuse existing fncallnode if we can
        if (node->methfld == NULL) {
            node->methfld = node->objfn;
            node->objfn = (INode*)selfnode;
            // The self a closure reaches a member through is a variable it borrows
            if (pstate->fn->closure)
                inodeTypeCheckAny(pstate, &node->objfn);
        }
        else {
            // Re-purpose objfn as self.method
            FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
            fncall->methfld = node->objfn;
            copyNodeLex(fncall, node->objfn); // Copy lexer info into injected node in case it has errors
            node->objfn = (INode*)fncall;
            inodeTypeCheckAny(pstate, &node->objfn);
        }
    }

    // A call whose callee names an overload set selects its one viable candidate
    if (nameUseNames(node->objfn, FnOverloadDclTag)) {
        if (fnCallLowerOverloadFn(pstate, nodep))
            return;
        // The candidate was a generic, and its instance is now the callee
        inodeTypeCheckAny(pstate, &node->objfn);
        if (inodeIsError(node->objfn)) {
            node->vtype = errorType;
            return;
        }
        if (!isExpNode(node->objfn)) {
            errorMsgNode(node->objfn, ErrorNotTyped, "Expected a typed expression.");
            node->vtype = errorType;
            return;
        }
    }

    // A 'null' compared with a pointer, on either side, is that pointer's type
    if (!fnCallTypeNullOperands(node))
        return;

    // Text of different kinds compared: a lending operand is its borrow
    fnCallLentOperands(pstate, node);

    // Handle when method operator requires an lval
    // This is true for ++, --, <- and operator-equals (+=)
    INode *objtype = iexpGetTypeDcl(node->objfn);
    if (node->flags & FlagLvalOp) {
        // Lower opassign for method-based types with extra logic. A reference
        // to such a type takes the same path, so a type that declares no '+='
        // reaches the rewrite to '+' through a reference as it does by value.
        if ((node->flags & FlagOpAssgn) && fnCallOpAssgnMethodType(objtype)) {
            fnCallOpAssgn(pstate, nodep);
            return;
        }

        // Turn objfn into &mut objfn, unless it already is a reference
        if (!fnCallIsRefReceiver(objtype)) {
            borrowMutRef(&node->objfn, objtype, newPermUseNode(mutPerm));
            objtype = iexpGetTypeDcl(node->objfn);
        }
    }

    // '===' and '!==' ask whether two references are the same place, which a value
    // that is neither a reference nor a pointer does not have. Refused here, ahead
    // of the dispatch, because a type's own namespace is not asked: identity is not
    // an operator a type declares.
    Name *opname = fnCallOperatorName(node);
    if ((opname == sameName || opname == notSameName) && !fnCallHasPlace(objtype)) {
        errorMsgNode((INode*)node, ErrorSameNotRef,
            "`%s` asks whether two references point to the same place, and this is not a reference or a pointer. Use `%s` to compare values.",
            &opname->namestr, opname == sameName ? "==" : "!=");
        node->vtype = errorType;
        return;
    }

    // A derived '!=': this node becomes the '==' application, lowered below like
    // any other, and a 'not' takes its place in the tree
    LogicNode *derivedne = NULL;
    if (fnCallNeFromEq(node, objtype)) {
        ((NameUseNode*)node->methfld)->namesym = eqName;
        opname = eqName;
        derivedne = newLogicNode(NotLogicTag);
        inodeLexCopy((INode*)derivedne, (INode*)node);
        derivedne->lexp = (INode*)node;
        *((INode**)nodep) = (INode*)derivedne;
    }

    // An owner of 'Array[T]' indexed is indexed as the slice it lends
    int lent = fnCallBodyAsSlice(pstate, node);
    if (lent < 0) {
        node->vtype = errorType;
        return;
    }
    if (lent)
        objtype = iexpGetTypeDcl(node->objfn);

    // A range borrowed from text, '&s[a..<b]', is a call of the text's 'slice'
    if ((node->flags & FlagRange) && fnCallLowerStrRange(pstate, node))
        return;

    // A range index slices an array or a slice, which fnCallArrIndex does. Any
    // other receiver is refused here, before its '[]' method could be handed the
    // range's two ends as though they were two indices.
    if ((node->flags & FlagRange) && !fnCallRangeReceiver(objtype)) {
        // Borrowed, the receiver is the borrow of what the pointer points at
        INode *held = node->objfn;
        if (held->tag == BorrowTag && ((RefNode *)held)->vtexp->tag == DerefTag)
            held = ((StarNode *)((RefNode *)held)->vtexp)->vtexp;
        errorMsgNode((INode*)node, ErrorBadIndex,
            iexpGetTypeDcl(held)->tag == PtrTag
                ? "A slice of part of what a pointer points at is not implemented; mem.sliceFromParts makes one"
                : fnCallTextOf(iexpGetTypeDcl(held)) != NULL
                ? "A range of text makes a part of it only when borrowed, as &s[a..<b]: the text itself has no size to hold by value"
                : "A range may only index an array or a slice");
        node->vtype = errorType;
        return;
    }

    // A tuple's element numbered through a reference or pointer
    if (fnCallLowerRefIntField(node, objtype))
        return;

    // 'x[i] = v', the index in set position (assignTypeCheck): a type that
    // declares '&[]' sets through the element's mutable borrow, as '&mut x[i]'
    // would reach it, and the assignment stores through it
    if (node == fnCallSetIndex && (node->flags & FlagIndex) && !(node->flags & (FlagBorrow | FlagRange))
        && node->methfld == NULL) {
        INode *held = objtype->tag == RefTag ? itypeGetTypeDcl(((RefNode *)objtype)->vtexp) : objtype;
        INode *refindex = held->tag == StructTag
            ? aliasDclResolve(iNsTypeFindFnField((INsTypeNode*)held, refIndexName)) : NULL;
        if (refindex && (refindex->tag == FnDclTag || refindex->tag == FnOverloadDclTag))
            node->flags |= FlagBorrow;
    }

    // Dispatch for correct handling based on the type of the object
    int ownerread = 0;
dispatch:
    switch (objtype->tag) {
    // Pure function call
    case FnSigTag:
        fnCallFnSigTypeCheck(pstate, node); break;

    // Types expecting method call or field access
    case StructTag:
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
        // Fill in empty methfld with '()', '[]' or '&[]' based on parser flags
        if (node->methfld == NULL)
            node->methfld = (INode*)newMemberUseNode(
                node->flags & FlagIndex ? (node->flags & FlagBorrow ? refIndexName : indexName) : parensName);
        // Lower to a field access or function call
        if (fnCallLowerMethod(pstate, node) == 0) {
            errorMsgNode((INode*)node, ErrorNoMeth,
                "No method/field named %s found that matches the call's arguments.",
                &((NameUseNode*)node->methfld)->namesym->namestr);
        }
        break;

    // Tuple type
    case TTupleTag:
        if (fnCallLowerIntField(node) == 0)
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid expression on a tuple");
        break;

    // Array type
    case ArrayTag:
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);  // indexing or borrowed ref to index
        else if (fnCallIsValueCompare(opname) && fnCallArrayAsSlice(pstate, node))
            ;
        else if (fnCallLowerSliceMethod(pstate, node))
            ;
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on an array.");
        break;

    // Array reference
    case ArrayRefTag:
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);
        else if (fnCallIsValueCompare(opname))
            fnCallLowerSliceCompare(pstate, node);
        else if (node->methfld && fnCallLowerPtrMethod(node, arrayRefType))
            ;
        else if (fnCallLowerSliceMethod(pstate, node))
            ;
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on an array ref.");
        break;

    // Regular reference
    case RefTag: {
        INode *objdereftype = itypeGetTypeDcl(((RefNode *)objtype)->vtexp);

        // A key reaches nothing on its own: no method, field, index or value
        // comparison through it. Only '===' asks of it, which place it names.
        if (lifeIsKey(objtype) && opname != sameName && opname != notSameName) {
            lifeKeyAccessError((INode*)node, objtype);
            node->vtype = errorType;
            break;
        }

        // Handle calling a function-by-ref (only callable using parens)
        if (objdereftype->tag == FnSigTag && !node->methfld) {
            if ((node->flags & FlagIndex))
                errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a function reference.");
            else
                fnCallFnSigTypeCheck(pstate, node);
        }

        // Handle indexing an array
        else if ((node->flags & FlagIndex) && (objdereftype->tag == ArrayTag || objdereftype->tag == ArrayDerefTag))
            fnCallArrIndex(node);

        // Handle method call to some other type
        else {
            // Fill in empty methfld with '()', '[]' or '&[]' based on parser flags
            if (node->methfld == NULL) {
                Name *methname = node->flags & FlagIndex ? (node->flags & FlagBorrow ? refIndexName : indexName) : parensName;
                node->methfld = (INode*)newMemberUseNode(methname);
            }
            if (fnCallIsValueCompare(opname))
                fnCallLowerRefCompare(pstate, node);
            else if (fnCallLowerPtrMethod(node, refType) == 0) {
                // Lower to a field access or function call, dereferencing the receiver
                // where that is what the selected method wants. fnCallLowerMethod cannot
                // answer 0 here, having already been told the deref type supports methods.
                if (isMethodType(objdereftype)) {
                    if (fnCallLowerTraitMethod(pstate, node, objdereftype) == 0)
                        fnCallLowerMethod(pstate, node);
                }
                else if (objdereftype->tag == PtrTag)
                    fnCallLowerPtrMethod(node, ptrType);
                else if (objdereftype->tag == ArrayTag && fnCallLowerSliceMethod(pstate, node))
                    ;
                // A borrow of an owner ('self &So[Node]', a method's receiver form) is
                // read through to the owner, whose value's fields and methods these are
                else if (!ownerread && objdereftype->tag == RefTag
                    && itypeGetTypeDcl(((RefNode*)objdereftype)->region) != borrowRef
                    && isMethodType(itypeGetTypeDcl(((RefNode*)objdereftype)->vtexp))) {
                    derefInject(&node->objfn);
                    objtype = iexpGetTypeDcl(node->objfn);
                    ownerread = 1;
                    goto dispatch;
                }
                else
                    errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a reference.");
            }
        }
        break;
    }

    // Virtual reference
    case VirtRefTag: {
        // Calling the reference itself calls the trait's '()' method, as it does
        // through a regular reference
        if (node->methfld == NULL && !(node->flags & FlagIndex))
            node->methfld = (INode*)newMemberUseNode(parensName);
        if (fnCallIsValueCompare(opname))
            fnCallRefNoCompare(node, opname, "comparing what two virtual references refer to is not built");
        else if (node->methfld) {
            if (fnCallLowerPtrMethod(node, refType) == 0) {
                node->flags |= FlagVDisp;
                fnCallLowerMethod(pstate, node);
            }
        }
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a virtual reference.");
        break;
    }

    // Pointer type
    case PtrTag: {
        INode *objdereftype = ((StarNode *)objtype)->vtexp;
        if (node->flags & FlagIndex)
            fnCallArrIndex(node);
        else if (node->methfld) {
            // A pointer's own operators first, then the value type's fields and named
            // methods, reaching a value receiver by dereferencing the pointer. An
            // operator the pointer does not declare stops here rather than reaching
            // the value's; fnCallLowerMethod is what refuses it.
            if (fnCallLowerPtrMethod(node, ptrType) == 0 && fnCallLowerMethod(pstate, node) == 0)
                errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a pointer.");
        }
        else if (objdereftype->tag == FnSigTag)
            fnCallFnSigTypeCheck(pstate, node);
        else
            errorMsgNode((INode*)node, ErrorNoMeth, "Invalid operation on a pointer.");
        break;
    }

    default:
        errorMsgNode((INode*)node->objfn, ErrorNoMeth, "This type does not support calls or field access.");
        node->vtype = errorType;
    }

    // 'not' takes a bool, which a '==' returning anything else reaches through
    // isTrue. A '==' that selected nothing has been reported, and the 'not' carries
    // that on rather than earning a second diagnostic.
    if (derivedne) {
        if (node->vtype == unknownType || node->vtype == errorType)
            derivedne->vtype = errorType;
        else if (!iexpCoerce(&derivedne->lexp, (INode*)boolType))
            errorMsgNode((INode*)node, ErrorInvType, "Conditional expression must be coercible to boolean value.");
    }
}

// Do data flow analysis for fncall node (only real function calls)
void fnCallFlow(FlowState *fstate, FnCallNode **nodep) {
    // Handle function call aliasing
    FnCallNode *node = *nodep;
    INode **argsp;
    uint32_t cnt;
    uint16_t inflight = fstate->inflightcnt;
    // A call type check gave up on without a diagnostic of its own, because what
    // it reads failed elsewhere -- a field of a value returned by a function
    // whose signature failed -- passes this function's flow gate but was never
    // lowered: a field access still has no arguments. The failure was reported
    // where it happened, and generation will not run.
    if (node->vtype == errorType)
        return;
    // A method called on an init's 'self' reaches through it (flowNewSelf),
    // and so does an actor's 'selfactor' made from it (Counter.self', which
    // reads only where the state is: an actor's init sends through it)
    INode *callee = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->dclnode : NULL;
    int method = callee && callee->tag == FnDclTag
        && ((callee->flags & FlagMethFld) || actorOfSelfFn((FnDclNode*)callee));
    // A call through a variable holding a function reference reads that variable
    // first, so one never given a value is refused as any other use of it is
    if (callee && callee->tag == VarDclTag)
        nameuseFlow(fstate, (NameUseNode**)&node->objfn);
    for (nodesFor(node->args, cnt, argsp)) {
        if (method && cnt == node->args->used && flowNewSelf(*argsp))
            flowNewSelfThrough(fstate, argsp);
        else
            flowLoadValue(fstate, argsp);
        flowHandleMoveOrCopy(argsp);  // Argument values are moved or copied
        flowGateOperand(fstate, *argsp);
    }
    flowGateOperandsEnd(fstate, inflight);
    flowGateCall(fstate, node->args);
    flowGateShape(fstate, node);
    // A type parameter's ''static' bound may make an argument passed by value
    // global, which no type of it shows: what it carries is the loan walk's
    // to check (pwStaticArgs)
    if (lifeStaticBoundSeen && flowGateOpen(fstate, FlowGateStore) && node->args && node->args->used) {
        FnSigNode *sig = (FnSigNode*)iexpGetDerefTypeDcl(node->objfn);
        if (sig->tag == FnSigTag && sig->lifestatic)
            fstate->gate |= FlowGateStore;
    }
}

// Perform data flow analysis on array index node
// A reference to a fixed-size array and a slice are indexed without a
// dereference being injected, so the element is read through the reference here
void fnCallArrIndexFlow(FlowState *fstate, FnCallNode **node) {
    flowLoadThroughRef(fstate, &(*node)->objfn);
    // Every index is read: each dimension's, or a range's start and end
    INode **argsp;
    uint32_t cnt;
    for (nodesFor((*node)->args, cnt, argsp))
        flowLoadValue(fstate, argsp);
}

// Perform data flow analysis on field access node
// A plain reference had a dereference injected, which derefFlow reads through;
// a virtual reference did not, so the field is read through it here
void fnCallFldAccessFlow(FlowState *fstate, FnCallNode **node) {
    flowLoadThroughRef(fstate, &(*node)->objfn);
}

