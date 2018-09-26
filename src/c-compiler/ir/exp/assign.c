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

// Extract lval variable, scope and overall permission from lval
INamedNode *assignLvalInfo(INode *lval, INode **lvalperm, int16_t *scope) {
    switch (lval->tag) {

    // A variable or named function node
    case VarNameUseTag:
    {
        INamedNode *lvalvar = ((NameUseNode *)lval)->dclnode;
        if (lvalvar->tag == VarDclTag) {
            *lvalperm = ((VarDclNode *)lvalvar)->perm;
            *scope = ((VarDclNode *)lvalvar)->scope;
            return lvalvar;
        }
        else
            return NULL; // A function name cannot be an lval
    }

    case DerefTag:
    {
        INamedNode *lvalvar = assignLvalInfo(((DerefNode *)lval)->exp, lvalperm, scope);
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)lval)->exp);
        if (vtype->tag == RefTag)
            *lvalperm = vtype->perm;
        else if (vtype->tag == PtrTag)
            *lvalperm = (INode*)mutPerm;
        return lvalvar;
    }

    // An array element (obj[2]) or property access (obj.prop)
    case FnCallTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        if (((ITypedNode*)element->objfn)->vtype->tag == FnSigTag)
            return NULL; // function calls are not lvals
        INamedNode *lvalvar = assignLvalInfo(element->objfn, lvalperm, scope);
        if (lvalvar == NULL)
            return NULL;
        if (element->methprop) {
            PermNode *methperm = (PermNode *)((VarDclNode*)((NameUseNode *)element->methprop)->dclnode)->perm;
            // Downgrade overall static permission if property is immutable
            if (methperm == immPerm)
                *lvalperm = (INode*)constPerm;
        }
        return lvalvar;
    }

    // No other node is an lval
    default:
        return NULL;
    }
}

void assignSingleCheck(INode *lval, INode **rval) {
    iexpRvalCheck(rval);
    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (lval->tag == VarNameUseTag && ((NameUseNode*)lval)->namesym == anonName)
        return;

    int16_t lvalscope;
    INode *lvalperm;
    INamedNode *lvalvar = assignLvalInfo(lval, &lvalperm, &lvalscope);
    if (lvalvar == NULL) {
        errorMsgNode(lval, ErrorBadLval, "Expression must be lval");
        return;
    }
    if (!(MayWrite & permGetFlags(lvalperm))) {
        errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
        return;
    }

    if (iexpCoerces(((ITypedNode*)lval)->vtype, rval) == 0) {
        errorMsgNode(*rval, ErrorInvType, "Expression's type does not match lval's type");
        return;
    }
    RefNode* rvaltype = (RefNode *)((ITypedNode*)*rval)->vtype;
    RefNode* lvaltype = (RefNode *)((ITypedNode*)lval)->vtype;
    if (rvaltype->tag == RefTag && lvaltype->tag == RefTag && lvaltype->alloc == voidType) {
        if (lvalscope < rvaltype->scope) {
            errorMsgNode(*rval, ErrorInvType, "This reference's value does not outlive lval");
            return;
        }
    }
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
