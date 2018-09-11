/** Handling for references
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode() {
	RefNode *refnode;
	newNode(refnode, RefNode, RefTag);
    refnode->tuptype = NULL;
	return refnode;
}

// Serialize a pointer type
void refPrint(RefNode *node) {
	inodeFprint("&(");
	inodePrintNode(node->alloc);
	inodeFprint(" ");
	inodePrintNode((INode*)node->perm);
	inodeFprint(" ");
    if (node->flags & FlagArrSlice)
        inodeFprint("[]");
	inodePrintNode(node->pvtype);
	inodeFprint(")");
}

// Semantically analyze a reference node
void refPass(PassState *pstate, RefNode *node) {
	inodeWalk(pstate, &node->alloc);
	inodeWalk(pstate, (INode**)&node->perm);
	inodeWalk(pstate, &node->pvtype);
    if (node->flags & FlagArrSlice)
        inodeWalk(pstate, (INode**)&node->tuptype);
}

// Compare two reference signatures to see if they are equivalent
int refEqual(RefNode *node1, RefNode *node2) {
	return itypeIsSame(node1->pvtype,node2->pvtype) 
		&& permIsSame(node1->perm, node2->perm)
		&& node1->alloc == node2->alloc;
}

// Will from reference coerce to a to reference (we know they are not the same)
int refMatches(RefNode *to, RefNode *from) {
	if (0 == permMatches(to->perm, from->perm)
		|| (to->alloc != from->alloc && to->alloc != voidType))
		return 0;
	return itypeMatches(to->pvtype, from->pvtype) == 1 ? 1 : 2;
}