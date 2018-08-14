/** AST handling for expression nodes that might do value copies/moves
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"
#include "../../shared/error.h"

#include <assert.h>

// Create a new assignment node
AssignAstNode *newAssignAstNode(int16_t assigntype, INode *lval, INode *rval) {
	AssignAstNode *node;
	newNode(node, AssignAstNode, AssignTag);
	node->assignType = assigntype;
	node->lval = lval;
	node->rval = rval;
	return node;
}

// Serialize assignment node
void assignPrint(AssignAstNode *node) {
	inodeFprint("(=, ");
	inodePrintNode(node->lval);
	inodeFprint(", ");
	inodePrintNode(node->rval);
	inodeFprint(")");
}

// expression is valid lval expression
int isLval(INode *node) {
	switch (node->asttype) {
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
void assignPass(PassState *pstate, AssignAstNode *node) {
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
		node->vtype = ((TypedAstNode*)node->rval)->vtype;
	}
}

// Create a new addr node
AddrAstNode *newAddrAstNode() {
	AddrAstNode *node;
	newNode(node, AddrAstNode, AddrTag);
	return node;
}

// Serialize addr
void addrPrint(AddrAstNode *node) {
	inodeFprint("&(");
	inodePrintNode(node->vtype);
	inodeFprint("->");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrAstNode *node, PtrAstNode *ptype) {
	INode *exp = node->exp;
	if (exp->asttype != VarNameUseTag
        || (((NameUseAstNode*)exp)->dclnode->asttype != VarDclTag
            && ((NameUseAstNode*)exp)->dclnode->asttype != FnDclTag)) {
		errorMsgNode((INode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    NamedAstNode *dclnode = ((NameUseAstNode*)exp)->dclnode;
    PermAstNode *perm = (dclnode->asttype == VarDclTag) ? ((VarDclAstNode*)dclnode)->perm : immPerm;
	if (!permMatches(ptype->perm, perm))
		errorMsgNode((INode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(PassState *pstate, AddrAstNode *node) {
	inodeWalk(pstate, &node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isExpNode(node->exp)) {
			errorMsgNode(node->exp, ErrorBadTerm, "Needs to be an expression");
			return;
		}
		PtrAstNode *ptype = (PtrAstNode *)node->vtype;
		if (ptype->pvtype == NULL)
			ptype->pvtype = ((TypedAstNode *)node->exp)->vtype; // inferred
		if (ptype->alloc == voidType)
			addrTypeCheckBorrow(node, ptype);
		else
			allocAllocate(node, ptype);
	}
}