/** Handling for pointers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <memory.h>

// Create a new pointer type whose info will be filled in afterwards
StarNode *newPtrNode() {
    StarNode *ptrnode;
    newNode(ptrnode, StarNode, PtrTag);
    return ptrnode;
}

// Clone pointer
INode *clonePtrNode(CloneState *cstate, StarNode *node) {
    StarNode *newnode = memAllocBlk(sizeof(StarNode));
    memcpy(newnode, node, sizeof(StarNode));
    newnode->vtexp = cloneNode(cstate, node->vtexp);
    return (INode *)newnode;
}

// Serialize a pointer type
void ptrPrint(StarNode *node) {
    inodeFprint("*");
    inodePrintNode(node->vtexp);
}

// Name resolution of a pointer type
void ptrNameRes(NameResState *pstate, StarNode *node) {
    inodeNameRes(pstate, &node->vtexp);
}

// Type check a pointer type
void ptrTypeCheck(TypeCheckState *pstate, StarNode *node) {
    if (itypeTypeCheck(pstate, &node->vtexp) == 0)
        return;
}

// Compare two pointer signatures to see if they are equivalent
int ptrEqual(StarNode *node1, StarNode *node2) {
    return itypeIsSame(node1->vtexp, node2->vtexp);
}

// Will from pointer coerce to a to pointer
TypeCompare ptrMatches(StarNode *to, StarNode *from, SubtypeConstraint constraint) {
    // Since pointers support both read and write permissions, value type invariance is expected
    return itypeIsSame(to->vtexp, from->vtexp)? EqMatch : NoMatch;
}