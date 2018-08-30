/** Handling for pointers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new pointer type whose info will be filled in afterwards
PtrNode *newPtrNode() {
	PtrNode *ptrnode;
	newNode(ptrnode, PtrNode, PtrTag);
	return ptrnode;
}

// Serialize a pointer type
void ptrPrint(PtrNode *node) {
	inodeFprint("*");
	inodePrintNode(node->pvtype);
}

// Semantically analyze a pointer type
void ptrPass(PassState *pstate, PtrNode *node) {
	inodeWalk(pstate, &node->pvtype);
}

// Compare two pointer signatures to see if they are equivalent
int ptrEqual(PtrNode *node1, PtrNode *node2) {
    return itypeIsSame(node1->pvtype, node2->pvtype);
}

// Will from pointer coerce to a to pointer (we know they are not the same)
int ptrMatches(PtrNode *to, PtrNode *from) {
	return itypeMatches(to->pvtype, from->pvtype) == 1 ? 1 : 2;
}