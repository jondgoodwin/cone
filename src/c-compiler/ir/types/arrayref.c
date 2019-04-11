/** Handling for array reference (slice) type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Serialize an array reference type
void arrayRefPrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->alloc);
    inodeFprint(" ");
    inodePrintNode((INode*)node->perm);
    inodeFprint(node->flags & FlagRefNull? " &?[]" : " &[]");
    inodePrintNode(node->pvtype);
    inodeFprint(")");
}

// Name resolution of an array reference node
void arrayRefNameRes(PassState *pstate, RefNode *node) {
    inodeWalk(pstate, &node->alloc);
    inodeWalk(pstate, (INode**)&node->perm);
    inodeWalk(pstate, &node->pvtype);
}

// Type check an array reference node
void arrayRefPass(PassState *pstate, RefNode *node) {
    inodeWalk(pstate, &node->alloc);
    inodeWalk(pstate, (INode**)&node->perm);
    inodeWalk(pstate, &node->pvtype);
}

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->pvtype,node2->pvtype) 
        && permIsSame(node1->perm, node2->perm)
        && node1->alloc == node2->alloc
        && (node1->flags & FlagRefNull) == (node2->flags & FlagRefNull);
}

// Will from reference coerce to a to reference (we know they are not the same)
int arrayRefMatches(RefNode *to, RefNode *from) {
    if (0 == permMatches(to->perm, from->perm)
        || (to->alloc != from->alloc && to->alloc != voidType)
        || ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull)))
        return 0;
    return itypeMatches(to->pvtype, from->pvtype) == 1 ? 1 : 2;
}
