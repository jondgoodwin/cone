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

// expression is valid lval expression: we can get its address and assign it a value
int isLval(INode *node) {
	switch (node->tag) {

    // Variable names and dereferences (*) are always lvals
    case VarNameUseTag:
	case DerefTag:
        return 1;

    // A FnCall node is only an lval if it is *not* a function call
    // (i.e., it is a property access or array index/slice)
	case FnCallTag:
        return ((ITypedNode*)((FnCallNode*)node)->objfn)->vtype->tag != FnSigTag;

    default: break;
	}

	return 0;
}

// Verify that all lval expressions are valid lvals and are mutable
void iexpLvalCheck(INode *lval) {
    // Check each lval separately in a value tuple of lvals
    if (lval->tag == VTupleTag) {
        VTupleNode *ltuple = (VTupleNode *)lval;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ltuple->values, cnt, nodesp))
            iexpLvalCheck(*nodesp);
        return;
    }

    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;

    // lval must be an lval and mutable
    if (!isLval(lval))
        errorMsgNode(lval, ErrorBadLval, "Expression to left of assignment must be lval");
    else if (!iexpIsLvalMutable(lval))
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
}

// Ensure we can read and copy/move all rvals
void iexpRvalCheck(INode **rvalp) {
    // Check each lval separately in a value tuple of lvals
    if ((*rvalp)->tag == VTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((VTupleNode *)*rvalp)->values, cnt, nodesp))
            iexpRvalCheck(nodesp);
        return;
    }

    iexpHandleCopy(rvalp);  // Move semantics, etc.
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
