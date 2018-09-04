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

// Ensure a single assignment is valid
void assignTypeCheck(INode *lval, INode **rvalp) {
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;
    if (!isLval(lval))
        errorMsgNode(lval, ErrorBadLval, "Expression to left of assignment must be lval");
    else if (!iexpIsLvalMutable(lval))
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
    else if (!iexpCoerces(lval, rvalp))
        errorMsgNode(*rvalp, ErrorInvType, "Expression's type does not match lval's type");
    else
        iexpHandleCopy(rvalp);  // Move semantics, etc.
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignNode *node) {
	inodeWalk(pstate, &node->lval);
	inodeWalk(pstate, &node->rval);
    INode *lval = node->lval;
    INode *rval = node->rval;

	switch (pstate->pass) {
	case TypeCheck:
        // Handle single vs. parallel assignment based on use of vtuple nodes
        if (lval->tag != VTupleTag) {
            if (rval->tag != VTupleTag)
                assignTypeCheck(lval, &node->rval); // Simple assignment
            else {
                // Treat as simple assignment with one lval on left
                VTupleNode *rtuple = (VTupleNode *)rval;
                assignTypeCheck(lval, &nodesGet(rtuple->values,0));
            }
        }
        else if (rval->tag == VTupleTag) {
            // Parallel assignment
            VTupleNode *ltuple = (VTupleNode *)lval;
            VTupleNode *rtuple = (VTupleNode *)rval;
            if (ltuple->values->used <= rtuple->values->used) {
                INode **rnode = &nodesGet(rtuple->values, 0);
                INode **nodesp;
                uint32_t cnt;
                for (nodesFor(ltuple->values, cnt, nodesp)) {
                    assignTypeCheck(*nodesp, rnode++);
                }
            }
            else {
                errorMsgNode(lval, ErrorBadLval, "More lvals than rvals on parallel assignment");
            }
        }
        else {
            errorMsgNode(lval, ErrorBadLval, "More lvals than rvals on parallel assignment");
        }
        node->vtype = ((ITypedNode*)node->rval)->vtype;
    }
}
