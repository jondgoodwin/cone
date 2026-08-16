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

// Name resolution of the function signature
void fnSigNameRes(NameResState *pstate, FnSigNode *sig) {
    uint16_t svscope = pstate->scope;
    pstate->scope = 0; // Make scope 0 to avoid parameter names being hooked.
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
    inodeNameRes(pstate, &sig->rettype);
    pstate->scope = svscope;
}

// Type check the function signature
void fnSigTypeCheck(TypeCheckState *pstate, FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        inodeTypeCheckAny(pstate, nodesp);
    itypeTypeCheck(pstate, &sig->rettype);
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
    return 1;
}

// For virtual reference structural matches on two methods,
// compare two function signatures to see if they are equivalent,
// ignoring the first 'self' parameter (we know their types differ)
int fnSigVrefEqual(FnSigNode *node1, FnSigNode *node2) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // Return types and number of parameters must match
    if (!itypeIsSame(node1->rettype, node2->rettype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (cnt < node1->parms->used && !itypeIsSame(*nodes1p, *nodes2p))
            return 0;
        nodes2p++;
    }
    return 1;
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
        // A virtual reference receiver is not type checked here, beyond requiring
        // that the candidate expects a reference it can be dispatched through
        else if (((IExpNode*)*parmp)->vtype->tag != RefTag)
            return 0;
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
