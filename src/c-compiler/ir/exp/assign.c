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
int assignIsLval(INode *node) {
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

// Is Lval mutable
int assignIsLvalMutable(INode *lval) {
    return MayWrite & iexpGetPermFlags(lval);
}

// Verify that all lval expressions are valid lvals and are mutable
void assignLvalCheck(INode *lval) {
    // Check each lval separately in a value tuple of lvals
    if (lval->tag == VTupleTag) {
        VTupleNode *ltuple = (VTupleNode *)lval;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ltuple->values, cnt, nodesp))
            assignLvalCheck(*nodesp);
        return;
    }

    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;

    // lval must be an lval and mutable
    if (!assignIsLval(lval))
        errorMsgNode(lval, ErrorBadLval, "Expression to left of assignment must be lval");
    else if (!assignIsLvalMutable(lval))
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignNode *node) {
	inodeWalk(pstate, &node->lval);
	inodeWalk(pstate, &node->rval);
    INode *lval = node->lval;

	switch (pstate->pass) {
	case TypeCheck:
        assignLvalCheck(lval); // Ensure all lvals are valid lvals and mutable
        iexpRvalCheck(&node->rval);
        if (!iexpCoerces(((ITypedNode*)lval)->vtype, &node->rval))
            errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
        node->vtype = ((ITypedNode*)node->rval)->vtype;
    }
}
