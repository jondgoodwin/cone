/** Handling for expression nodes that might do value copies/moves
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
		else if (!typeCoerces(node->lval, &node->rval))
			errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
		else if (!permIsMutable(node->lval))
			errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
		else
			typeHandleCopy(&node->rval);
		node->vtype = ((ITypedNode*)node->rval)->vtype;
	}
}

// Create a new addr node
AddrNode *newAddrNode() {
	AddrNode *node;
	newNode(node, AddrNode, AddrTag);
	return node;
}

// Serialize addr
void addrPrint(AddrNode *node) {
	inodeFprint("&(");
	inodePrintNode(node->vtype);
	inodeFprint("->");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrNode *node, PtrNode *ptype) {
	INode *exp = node->exp;
	if (exp->tag != VarNameUseTag
        || (((NameUseNode*)exp)->dclnode->tag != VarDclTag
            && ((NameUseNode*)exp)->dclnode->tag != FnDclTag)) {
		errorMsgNode((INode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    INamedNode *dclnode = ((NameUseNode*)exp)->dclnode;
    PermNode *perm = (dclnode->tag == VarDclTag) ? ((VarDclNode*)dclnode)->perm : immPerm;
	if (!permMatches(ptype->perm, perm))
		errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(PassState *pstate, AddrNode *node) {
	inodeWalk(pstate, &node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isExpNode(node->exp)) {
			errorMsgNode(node->exp, ErrorBadTerm, "Needs to be an expression");
			return;
		}
		PtrNode *ptype = (PtrNode *)node->vtype;
		if (ptype->pvtype == NULL)
			ptype->pvtype = ((ITypedNode *)node->exp)->vtype; // inferred
		if (ptype->alloc == voidType)
			addrTypeCheckBorrow(node, ptype);
		else
			allocAllocate(node, ptype);
	}
}