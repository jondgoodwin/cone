/** Handling for assignment nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new assignment node
AssignNode *newAssignNode(int16_t assigntype, INode *lval, INode *rval) {
	AssignNode *node;
	newNode(node, AssignNode, AssignTag);
	node->assignType = assigntype;
	node->lval = lval;
	node->rval = rval;
	return node;
}

// Serialize assignment node
void assignPrint(AssignNode *node) {
	inodeFprint("(=, ");
	inodePrintNode(node->lval);
	inodeFprint(", ");
	inodePrintNode(node->rval);
	inodeFprint(")");
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignNode *node) {
	inodeWalk(pstate, &node->lval);
	inodeWalk(pstate, &node->rval);
    INode *lval = node->lval;

	switch (pstate->pass) {
	case TypeCheck:
        iexpLvalCheck(lval); // Ensure all lvals are valid lvals and mutable
        iexpRvalCheck(&node->rval);
        if (!iexpCoerces(((ITypedNode*)lval)->vtype, &node->rval))
            errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
        node->vtype = ((ITypedNode*)node->rval)->vtype;
    }
}
