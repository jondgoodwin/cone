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

// expression is valid lval expression
int isLval(INode *node) {
	switch (node->tag) {
	case VarNameUseTag:
	case DerefTag:
	case FnCallTag:
		return 1;
	// future:  [] indexing and .member
	default: break;
	}

	return 0;
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignNode *node) {
	inodeWalk(pstate, &node->lval);
	inodeWalk(pstate, &node->rval);

	switch (pstate->pass) {
	case TypeCheck:
		if (!isLval(node->lval))
			errorMsgNode(node->lval, ErrorBadLval, "Expression to left of assignment must be lval");
		else if (!iexpCoerces(node->lval, &node->rval))
			errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
		else if (!permIsMutable(node->lval))
			errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
		else
			iexpHandleCopy(&node->rval);
		node->vtype = ((ITypedNode*)node->rval)->vtype;
	}
}
