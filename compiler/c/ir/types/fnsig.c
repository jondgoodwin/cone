/** Handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <memory.h>

// Create a new function signature node
FnSigNode *newFnSigNode() {
    FnSigNode *sig;
    newNode(sig, FnSigNode, FnSigTag);
    sig->flags |= OpaqueType;
    sig->parms = newNodes(8);
    sig->rettype = unknownType;
    sig->lifenamed = 0;
    sig->lifeorder = NULL;
    sig->lifechecked = 0;
    sig->lifestatic = 0;
    return sig;
}

// Clone function signature
INode *cloneFnSigNode(CloneState *cstate, FnSigNode *node) {
    FnSigNode *newnode = memAllocBlk(sizeof(FnSigNode));
    memcpy(newnode, node, sizeof(FnSigNode));
    newnode->parms = cloneNodes(cstate, node->parms);
    newnode->rettype = cloneNode(cstate, node->rettype);
    INode **origp = &nodesGet(node->parms, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(newnode->parms, cnt, nodesp)) {
        cloneDclSetMap(*origp++, *nodesp);
    }
    return (INode *)newnode;
}

// Serialize a function signature node
void fnSigPrint(FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("fn(");
    for (nodesFor(sig->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint(") ");
    inodePrintNode(sig->rettype);
}

// Name resolution of the function signature.
//
// A parameter's default value is evaluated where the function is called, so
// an importer's object evaluates it from the include file's text: what it names
// is marked and recorded as an expanded body's reach is, against the function
// (NameResState.sigfn), so a library exports it and its include file carries
// it. The parameter itself is resolved first, at scope 0, so nothing is hooked
// that the value could see
void fnSigNameRes(NameResState *pstate, FnSigNode *sig) {
    uint16_t svscope = pstate->scope;
    pstate->scope = 0; // Make scope 0 to avoid parameter names being hooked.
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode*)*nodesp;
        INode *dflt = parm->tag == VarDclTag ? parm->value : NULL;
        if (dflt == NULL || pstate->sigfn == NULL || pstate->expander != NULL) {
            inodeNameRes(pstate, nodesp);
            continue;
        }
        parm->value = NULL;
        inodeNameRes(pstate, nodesp);
        parm = (VarDclNode*)*nodesp;
        parm->value = dflt;
        pstate->expander = pstate->sigfn;
        inodeNameRes(pstate, &parm->value);
        pstate->expander = NULL;
    }
    inodeNameRes(pstate, &sig->rettype);
    pstate->scope = svscope;
}

// The lifetimes a signature's types name and imply (lifeSigCheck)
static void fnSigLifeCheck(TypeCheckState *pstate, INode *node, void *extra) {
    FnSigNode *sig = (FnSigNode *)node;
    INode **nodesp;
    uint32_t cnt;
    lifeSigCheck(sig);
    // A parameter's own reference a type parameter's ''static' bound makes
    // global holds a global borrow, as one written ''static' does: the caller
    // band varDclTypeCheck gave its own copy of the type is undone
    if (sig->lifestatic) {
        for (nodesFor(sig->parms, cnt, nodesp)) {
            INode *parmtype = ((VarDclNode*)*nodesp)->vtype;
            if ((parmtype->tag == RefTag || parmtype->tag == ArrayRefTag) && ((RefNode*)parmtype)->scope == 1
                && lifeIsOwnBorrow(parmtype) && lifePartStatic(sig, parmtype, LifePartOwn))
                ((RefNode*)parmtype)->scope = 0;
        }
    }
}

// Type check the function signature
void fnSigTypeCheck(TypeCheckState *pstate, FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        // An init's 'self &new' is the one place that permission is written
        // (refTypeCheck); whether this is an init is fnDclTypeCheck's to judge
        refAllowNewPerm = cnt == sig->parms->used && ((VarDclNode*)*nodesp)->namesym == selfName
            && ((VarDclNode*)*nodesp)->vtype->tag == RefTag;
        inodeTypeCheckAny(pstate, nodesp);
        refAllowNewPerm = 0;
        // A caller's argument is checked to be global only where the
        // parameter's own reference is written ''static'
        if (sig->lifenamed && lifeParmStaticInside(((VarDclNode*)*nodesp)->vtype))
            errorMsgNode(*nodesp, ErrorLifetimePlace,
                "''static' is named on a parameter's own reference ('p &'static T') or in the result, not inside a parameter's type: what a caller passes there is not checked to be global.");
    }
    itypeTypeCheck(pstate, &sig->rettype);
    // A function reference's signature, checked as a reference's target while a
    // layout is in flight, has its lifetimes read once its types are laid out:
    // what a type's values hold is read from its layout
    if (structTargetDeferring())
        structDeferCheck(pstate, fnSigLifeCheck, (INode*)sig, NULL);
    else
        fnSigLifeCheck(pstate, (INode*)sig, NULL);
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigNode *node1, FnSigNode *node2) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // Return types and number of parameters must match
    if (!itypeIsSame(node1->rettype, node2->rettype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match. A parameter is a VarDclNode, not a
    // type, so its declared type has to be extracted before the types are compared.
    // Comparing the declarations themselves compares node identity, which no two
    // separately written signatures can ever satisfy.
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (!itypeIsSame(iexpGetTypeDcl(*nodes1p), iexpGetTypeDcl(*nodes2p)))
            return 0;
        nodes2p++;
    }
    return lifeSigsAgree(node1, node2);
}

// Is an implementation's type 'impl' the type a trait requirement's 'req' names?
// 'Self' in a requirement stands for the type meeting it, 'selftype', as it does
// in a default cloned into that type (cloneNode repoints it there). Only the
// name 'Self' is read that way, through a reference, a pointer or a slice to it:
// a requirement naming its trait outright means the trait, and so does a virtual
// reference to Self, '&<Self', which is a reference to the trait. With no
// 'selftype' the match is exact.
static int fnSigReqTypeSame(INode *req, INode *impl, INode *selftype) {
    if (itypeIsSame(req, impl))
        return 1;
    if (selftype == NULL)
        return 0;
    if (isNameUseNode(req) && ((NameUseNode*)req)->namesym == selfTypeName)
        return itypeGetTypeDcl(impl) == selftype;
    INode *reqdcl = itypeGetTypeDcl(req);
    INode *impldcl = itypeGetTypeDcl(impl);
    if (reqdcl->tag != impldcl->tag)
        return 0;
    switch (reqdcl->tag) {
    case RefTag:
    case ArrayRefTag: {
        RefNode *reqref = (RefNode*)reqdcl;
        RefNode *implref = (RefNode*)impldcl;
        return fnSigReqTypeSame(reqref->vtexp, implref->vtexp, selftype)
            && permIsSame(reqref->perm, implref->perm)
            && itypeIsSame(reqref->region, implref->region);
    }
    case PtrTag:
        return fnSigReqTypeSame(((StarNode*)reqdcl)->vtexp, ((StarNode*)impldcl)->vtexp, selftype);
    default:
        return 0;
    }
}

// For virtual reference structural matches on two methods,
// compare two function signatures to see if they are equivalent,
// ignoring the first 'self' parameter (we know their types differ).
// 'node1' is the implementation, 'node2' the requirement; see fnSigReqTypeSame
// for 'selftype'.
int fnSigVrefEqual(FnSigNode *node1, FnSigNode *node2, INode *selftype) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // Return types and number of parameters must match
    if (!fnSigReqTypeSame(node2->rettype, node1->rettype, selftype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match, exactly. A parameter is a
    // VarDclNode, not a type, so its declared type has to be extracted before the
    // types are compared; comparing the declarations themselves compares node
    // identity, which a trait's requirement and a type's implementation of it can
    // never satisfy, since they are separately written.
    //
    // The match is exact rather than by coercion. Coercion is a convenience at a
    // call site, and belongs to type comparison only where subtyping is in play,
    // which it is not between a trait requirement and an implementation of it.
    // A virtual reference dispatches through a typed vtable slot, so the machine
    // signatures have to line up. Reading 'Self' as the implementer is for a
    // match no slot is filled by, a constraint or a declared 'is'.
    //
    // A parameter's declared type is read off it as written, not through
    // iexpGetTypeDcl, so that a use of 'Self' is still one.
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (cnt < node1->parms->used
            && !fnSigReqTypeSame(((IExpNode*)*nodes2p)->vtype, ((IExpNode*)*nodes1p)->vtype, selftype))
            return 0;
        nodes2p++;
    }
    // A call through the requirement is checked against its lifetimes, so the
    // implementation must promise at least as much (lifeSigMeets)
    return lifeSigMeets(node1, node2);
}

// Do two signatures declare the same parameter types (ignoring return type)?
// Two overload candidates that compare equal would accept exactly the same
// arguments, so the overload name could never choose between them.
int fnSigParmsEqual(FnSigNode *node1, FnSigNode *node2) {
    if (node1->parms->used != node2->parms->used)
        return 0;

    INode **nodes1p, **nodes2p;
    uint32_t cnt;
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (!itypeIsSame(iexpGetTypeDcl(*nodes1p), iexpGetTypeDcl(*nodes2p)))
            return 0;
        nodes2p++;
    }
    return 1;
}

// Return TypeCompare indicating whether from type matches the function signature
TypeCompare fnSigMatches(FnSigNode *to, FnSigNode *from, SubtypeConstraint constraint) {
    TypeCompare result = EqMatch;

    // Number of parameters must match
    if (to->parms->used != from->parms->used)
        return NoMatch;

    // Every parameter's type must also match
    INode **tonodesp, **fromnodesp;
    uint32_t cnt;
    fromnodesp = &nodesGet(from->parms, 0);
    for (nodesFor(to->parms, cnt, tonodesp)) {
        // Match for parameters is contravariant, switching order of to/from
        switch (itypeMatches(iexpGetTypeDcl(*fromnodesp), iexpGetTypeDcl(*tonodesp), constraint)) {
        case NoMatch:
            return NoMatch;
        case CastSubtype:
            result = result == ConvSubtype ? ConvSubtype : CastSubtype;
            break;
        case ConvSubtype:
            result = ConvSubtype;
            break;
        default:
            break;
        }
        fromnodesp++;
    }

    // A call through 'to' is checked against its lifetimes, so 'from' must
    // promise the same
    if (!lifeSigsAgree(to, from))
        return NoMatch;

    // Return type is covariant
    switch (itypeMatches(to->rettype, from->rettype, constraint)) {
    case NoMatch:
        return NoMatch;
    case CastSubtype:
        result = result == ConvSubtype ? ConvSubtype : CastSubtype;
        break;
    case ConvSubtype:
        result = ConvSubtype;
        break;
    default:
        break;
    }
    return result;
}

// Return true if type of from-exp matches totype
int fnSigCoerce(FnSigNode *totype, INode **fromexp) {
    return itypeMatches((INode*)totype, iexpGetTypeDcl(*fromexp), Coercion) == EqMatch;
}


// Can a call passing 'self' (NULL if none) and 'args' call this signature?
// Only viability is decided: arity, required versus defaulted parameters, receiver
// compatibility, and whether every explicit argument may be passed using a permitted
// implicit coercion. Nothing is inserted into the call, and no candidate is preferred
// over another for being an exact rather than a coercible match.
int fnSigViableCall(FnSigNode *to, INode **self, Nodes *args) {
    uint32_t argcnt = args ? args->used : 0;
    if (self)
        ++argcnt;

    // Too many arguments is not a match
    if (argcnt > to->parms->used)
        return 0;

    INode **parmp = &nodesGet(to->parms, 0);

    // A receiver, when there is one, must be passable as the first parameter
    if (self) {
        INode *selftype = iexpGetTypeDcl(*self);
        if (selftype->tag != VirtRefTag) {
            if (iexpMatches(self, iexpGetTypeDcl(*parmp), Coercion) == NoMatch)
                return 0;
        }
        // A virtual reference receiver is not type checked here as a whole, since
        // '&<Trait' never coerces to the '&Trait' the method declares. The
        // candidate must expect a reference it can be dispatched through, with a
        // permission the receiver's grants, as a plain reference's must
        else {
            RefNode *parmref = (RefNode*)iexpGetTypeDcl(*parmp);
            if (parmref->tag != RefTag
                || permMatches(parmref->perm, ((RefNode*)selftype)->perm) == NoMatch)
                return 0;
        }
        ++parmp;
    }

    // Every explicit argument must be passable to its corresponding parameter
    if (args) {
        INode **argsp;
        uint32_t cnt;
        for (nodesFor(args, cnt, argsp)) {
            if (iexpMatches(argsp, ((IExpNode *)*parmp)->vtype, Coercion) == NoMatch)
                return 0;
            ++parmp;
        }
    }

    // Every parameter the call did not supply must declare a default value
    uint32_t missing = to->parms->used - argcnt;
    while (missing--) {
        if (((VarDclNode *)*parmp++)->value == NULL)
            return 0;
    }

    return 1;
}
