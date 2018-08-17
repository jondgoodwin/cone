/** Handling for while nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new While node
WhileNode *newWhileNode() {
	WhileNode *node;
	newNode(node, WhileNode, WhileTag);
	node->blk = NULL;
	return node;
}

// Serialize a while node
void whilePrint(WhileNode *node) {
	inodeFprint("while ");
	inodePrintNode(node->condexp);
	inodePrintNL();
	inodePrintNode(node->blk);
}

// Semantic pass on the while block
void whilePass(PassState *pstate, WhileNode *node) {
	uint16_t svflags = pstate->flags;
	pstate->flags |= PassWithinWhile;

	inodeWalk(pstate, &node->condexp);
	inodeWalk(pstate, &node->blk);

	if (pstate->pass == TypeCheck)
		iexpCoerces((INode*)boolType, &node->condexp);

	pstate->flags = svflags;
}
