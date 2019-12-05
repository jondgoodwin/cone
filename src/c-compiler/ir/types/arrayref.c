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
    inodePrintNode(node->region);
    inodeFprint(" ");
    inodePrintNode((INode*)node->perm);
    inodeFprint(node->flags & FlagRefNull? " &?[]" : " &[]");
    inodePrintNode(node->pvtype);
    inodeFprint(")");
}

// Name resolution of an array reference node
void arrayRefNameRes(NameResState *pstate, RefNode *node) {
    inodeNameRes(pstate, &node->region);
    inodeNameRes(pstate, (INode**)&node->perm);
    if (node->pvtype)
        inodeNameRes(pstate, &node->pvtype);
}

// Type check an array reference node
void arrayRefTypeCheck(TypeCheckState *pstate, RefNode *node) {
    inodeTypeCheck(pstate, &node->region);
    inodeTypeCheck(pstate, (INode**)&node->perm);
    if (node->pvtype)
        inodeTypeCheck(pstate, &node->pvtype);
}

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->pvtype,node2->pvtype) 
        && permIsSame(node1->perm, node2->perm)
        && node1->region == node2->region
        && (node1->flags & FlagRefNull) == (node2->flags & FlagRefNull);
}

// Will from reference coerce to a to reference (we know they are not the same)
int arrayRefMatches(RefNode *to, RefNode *from) {
    if (0 == permMatches(to->perm, from->perm)
        || (to->region != from->region && to->region != borrowRef)
        || ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull)))
        return 0;
    if (to->pvtype && from->pvtype)
        return itypeMatches(to->pvtype, from->pvtype) == 1 ? 1 : 2;
    return 0;
}
