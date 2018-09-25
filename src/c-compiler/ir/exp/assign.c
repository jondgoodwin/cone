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
    // lval must be an lval and mutable
    if (!assignIsLval(lval))
        errorMsgNode(lval, ErrorBadLval, "Expression to left of assignment must be lval");
    else if (!assignIsLvalMutable(lval))
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
}

void assignSingleCheck(INode *lval, INode **rval) {
    iexpRvalCheck(rval);
    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;
    assignLvalCheck(lval);
    if (iexpCoerces(((ITypedNode*)lval)->vtype, rval) == 0)
        errorMsgNode(*rval, ErrorInvType, "Expression's type does not match lval's type");
}

// Handle parallel assignment (multiple values on both sides)
void assignParaCheck(VTupleNode *lval, VTupleNode *rval) {
    Nodes *lnodes = lval->values;
    Nodes *rnodes = rval->values;
    if (lnodes->used > rnodes->used) {
        errorMsgNode((INode*)rval, ErrorBadTerm, "Not enough tuple values given to lvals");
        return;
    }
    int16_t lcnt;
    INode **lnodesp;
    INode **rnodesp = &nodesGet(rnodes, 0);
    int16_t rcnt = rnodes->used;
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        assignSingleCheck(*lnodesp, rnodesp++);
        rcnt--;
    }
    while (rcnt--)
        iexpRvalCheck(rnodesp++);
}

// Handle when single function/expression returns to multiple lval
void assignMultRetCheck(VTupleNode *lval, INode **rval) {
    Nodes *lnodes = lval->values;
    INode *rtype = ((ITypedNode *)*rval)->vtype;
    if (rtype->tag != TTupleTag) {
        errorMsgNode(*rval, ErrorBadTerm, "Not enough values for lvals");
        return;
    }
    Nodes *rtypes = ((TTupleNode*)((ITypedNode *)*rval)->vtype)->types;
    if (lnodes->used > rtypes->used) {
        errorMsgNode(*rval, ErrorBadTerm, "Not enough return values for lvals");
        return;
    }
    int16_t lcnt;
    INode **lnodesp;
    INode **rtypep = &nodesGet(rtypes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        if (itypeIsSame(((ITypedNode *)*lnodesp)->vtype, *rtypep++) == 0)
            errorMsgNode(*lnodesp, ErrorInvType, "Return value's type does not match lval's type");
    }
    iexpRvalCheck(rval);
}

// Handle when multiple expressions assigned to single lval
void assignToOneCheck(INode *lval, VTupleNode *rval) {
    Nodes *rnodes = rval->values;
    INode **rnodesp = &nodesGet(rnodes, 0);
    int16_t rcnt = rnodes->used;
    assignSingleCheck(lval, rnodesp++);
    while (--rcnt)
        iexpRvalCheck(rnodesp++);
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignNode *node) {
	inodeWalk(pstate, &node->lval);
	inodeWalk(pstate, &node->rval);
    INode *lval = node->lval;

	switch (pstate->pass) {
	case TypeCheck:
        // Handle tuple decomposition for parallel assignment
        if (lval->tag == VTupleTag) {
            if (node->rval->tag == VTupleTag)
                assignParaCheck((VTupleNode*)node->lval, (VTupleNode*)node->rval);
            else
                assignMultRetCheck((VTupleNode*)node->lval, &node->rval);
        }
        else {
            if (node->rval->tag == VTupleTag)
                assignToOneCheck(node->lval, (VTupleNode*)node->rval);
            else
                assignSingleCheck(node->lval, &node->rval);
        }
        node->vtype = ((ITypedNode*)node->rval)->vtype;
    }
}
