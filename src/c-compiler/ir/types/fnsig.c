/** Handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new function signature node
FnSigNode *newFnSigNode() {
    FnSigNode *sig;
    newNode(sig, FnSigNode, FnSigTag);
    sig->parms = newNodes(8);
    sig->rettype = voidType;
    return sig;
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
void fnSigNameRes(PassState *pstate, FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        inodeWalk(pstate, nodesp);
    inodeWalk(pstate, &sig->rettype);
}

// Type check the function signature
void fnSigPass(PassState *pstate, FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp))
        inodeWalk(pstate, nodesp);
    inodeWalk(pstate, &sig->rettype);
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigNode *node1, FnSigNode *node2) {
    INode **nodes1p, **nodes2p;
    int16_t cnt;

    // Return types and number of parameters must match
    if (!itypeIsSame(node1->rettype, node2->rettype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (!itypeIsSame(*nodes1p, *nodes2p))
            return 0;
        nodes2p++;
    }
    return 1;
}

// Will the function call (caller) be able to call the 'to' function
// Return 0 if not. 1 if perfect match. 2+ for imperfect matches
int fnSigMatchesCall(FnSigNode *to, Nodes *args) {
    int matchsum = 1;

    // Too many arguments is not a match
    if (args->used > to->parms->used)
        return 0;

    // Every parameter's type must also match
    INode **tonodesp;
    INode **callnodesp;
    int16_t cnt;
    tonodesp = &nodesGet(to->parms, 0);
    for (nodesFor(args, cnt, callnodesp)) {
        int match;
        switch (match = itypeMatches(((ITypedNode *)*tonodesp)->vtype, ((ITypedNode*)*callnodesp)->vtype)) {
        case 0: return 0;
        case 1: break;
        case 2: matchsum += match; break;
        default:
            if ((*callnodesp)->tag != ULitTag)
                return 0;
        }
        tonodesp++;
    }
    // Match fails if not enough arguments & method has no default values on parms
    if (args->used != to->parms->used 
        && ((VarDclNode *)tonodesp)->value==NULL)
        return 0;

    // It is a match; return how perfect a match it is
    return matchsum;
}


// Will the method call (caller) be able to call the 'to' function
// Return 0 if not. 1 if perfect match. 2+ for imperfect matches
int fnSigMatchMethCall(FnSigNode *to, Nodes *args) {

    // Too many arguments is not a match
    if (args->used > to->parms->used)
        return 0;

    // Every parameter's type must also match
    int first = 1;
    int matchsum = 1;
    INode **tonodesp;
    INode **callnodesp;
    int16_t cnt;
    tonodesp = &nodesGet(to->parms, 0);
    for (nodesFor(args, cnt, callnodesp)) {
        int match;
        switch (match = itypeMatches(((ITypedNode *)*tonodesp)->vtype, ((ITypedNode*)*callnodesp)->vtype)) {
        case 0:
            if (!first)
                return 0;
            // For self parm, we might autoref/autoderef, but only if necessary
            matchsum = 100;
            break;
        case 1: break;
        case 2: matchsum += match; break;
        default:
            if ((*callnodesp)->tag != ULitTag)
                return 0;
        }
        first = 0;
        tonodesp++;
    }
    // Match fails if not enough arguments & method has no default values on parms
    if (args->used != to->parms->used
        && ((VarDclNode *)tonodesp)->value == NULL)
        return 0;

    // It is a match; return how perfect a match it is
    return matchsum;
}