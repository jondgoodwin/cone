/** Handling for array reference (slice) type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new reference type whose info will be filled in afterwards
ArrayRefNode *newArrayRefNode() {
    ArrayRefNode *refnode;
    newNode(refnode, ArrayRefNode, ArrayRefTag);
    return refnode;
}

// Is type a nullable reference?
int arrayRefIsNullable(INode *typenode) {
    ArrayRefNode *ref = (ArrayRefNode*)typenode;
    return ref->tag == ArrayRefTag && (ref->flags & FlagRefNull);
}

// Serialize a pointer type
void arrayRefPrint(ArrayRefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->alloc);
    inodeFprint(" ");
    inodePrintNode((INode*)node->perm);
    inodeFprint(node->flags & FlagRefNull? " &?[]" : " &[]");
    inodePrintNode(node->pvtype);
    inodeFprint(")");
}

// Semantically analyze a reference node
void arrayRefPass(PassState *pstate, ArrayRefNode *node) {
    inodeWalk(pstate, &node->alloc);
    inodeWalk(pstate, (INode**)&node->perm);
    inodeWalk(pstate, &node->pvtype);
}

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(ArrayRefNode *node1, ArrayRefNode *node2) {
    return itypeIsSame(node1->pvtype,node2->pvtype) 
        && permIsSame(node1->perm, node2->perm)
        && node1->alloc == node2->alloc
        && (node1->flags & FlagRefNull) == (node2->flags & FlagRefNull);
}

// Will from reference coerce to a to reference (we know they are not the same)
int arrayRefMatches(ArrayRefNode *to, ArrayRefNode *from) {
    if (0 == permMatches(to->perm, from->perm)
        || (to->alloc != from->alloc && to->alloc != voidType)
        || ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull)))
        return 0;
    return itypeMatches(to->pvtype, from->pvtype) == 1 ? 1 : 2;
}
