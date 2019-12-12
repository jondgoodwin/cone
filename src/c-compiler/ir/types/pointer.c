/** Handling for pointers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <memory.h>

// Create a new pointer type whose info will be filled in afterwards
PtrNode *newPtrNode() {
    PtrNode *ptrnode;
    newNode(ptrnode, PtrNode, PtrTag);
    return ptrnode;
}

// Clone pointer
INode *clonePtrNode(CloneState *cstate, PtrNode *node) {
    PtrNode *newnode = memAllocBlk(sizeof(PtrNode));
    memcpy(newnode, node, sizeof(PtrNode));
    newnode->pvtype = cloneNode(cstate, node->pvtype);
    return (INode *)newnode;
}

// Serialize a pointer type
void ptrPrint(PtrNode *node) {
    inodeFprint("*");
    inodePrintNode(node->pvtype);
}

// Name resolution of a pointer type
void ptrNameRes(NameResState *pstate, PtrNode *node) {
    inodeNameRes(pstate, &node->pvtype);
}

// Type check a pointer type
void ptrTypeCheck(TypeCheckState *pstate, PtrNode *node) {
    if (itypeTypeCheck(pstate, &node->pvtype) == 0)
        return;
}

// Compare two pointer signatures to see if they are equivalent
int ptrEqual(PtrNode *node1, PtrNode *node2) {
    return itypeIsSame(node1->pvtype, node2->pvtype);
}

// Will from pointer coerce to a to pointer (we know they are not the same)
int ptrMatches(PtrNode *to, PtrNode *from) {
    return itypeMatches(to->pvtype, from->pvtype) == EqMatch ? EqMatch : CoerceMatch;
}