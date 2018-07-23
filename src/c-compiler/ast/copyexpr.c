/** AST handling for expression nodes that might do value copies/moves
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
#include "../shared/error.h"

#include <assert.h>

void handleCopy(PassState *pstate, AstNode *node) {

}

// Create a new assignment node
AssignAstNode *newAssignAstNode(int16_t assigntype, AstNode *lval, AstNode *rval) {
	AssignAstNode *node;
	newAstNode(node, AssignAstNode, AssignNode);
	node->assignType = assigntype;
	node->lval = lval;
	node->rval = rval;
	return node;
}

// Serialize assignment node
void assignPrint(AssignAstNode *node) {
	astFprint("(=, ");
	astPrintNode(node->lval);
	astFprint(", ");
	astPrintNode(node->rval);
	astFprint(")");
}

// expression is valid lval expression
int isLval(AstNode *node) {
	switch (node->asttype) {
	case VarNameUseTag:
	case DerefNode:
	case DotOpNode:
		return 1;
	// future:  [] indexing and .member
	default: break;
	}

	return 0;
}

// Analyze assignment node
void assignPass(PassState *pstate, AssignAstNode *node) {
	astPass(pstate, node->lval);
	astPass(pstate, node->rval);

	switch (pstate->pass) {
	case TypeCheck:
		if (!isLval(node->lval))
			errorMsgNode(node->lval, ErrorBadLval, "Expression to left of assignment must be lval");
		else if (!typeCoerces(node->lval, &node->rval))
			errorMsgNode(node->rval, ErrorInvType, "Expression's type does not match lval's type");
		else if (!permIsMutable(node->lval))
			errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
		else
			handleCopy(pstate, node->rval);
		node->vtype = ((TypedAstNode*)node->rval)->vtype;
	}
}

// Create a new addr node
AddrAstNode *newAddrAstNode() {
	AddrAstNode *node;
	newAstNode(node, AddrAstNode, AddrNode);
	return node;
}

// Serialize addr
void addrPrint(AddrAstNode *node) {
	astFprint("&(");
	astPrintNode(node->vtype);
	astFprint("->");
	astPrintNode(node->exp);
	astFprint(")");
}

// Type check borrowed reference creator
void addrTypeCheckBorrow(AddrAstNode *node, PtrAstNode *ptype) {
	AstNode *exp = node->exp;
	if (exp->asttype != VarNameUseTag
        || (((NameUseAstNode*)exp)->dclnode->asttype != VarDclTag
            && ((NameUseAstNode*)exp)->dclnode->asttype != FnDclTag)) {
		errorMsgNode((AstNode*)node, ErrorNotLval, "May only borrow from lvals (e.g., variable)");
		return;
	}
    NamedAstNode *dclnode = ((NameUseAstNode*)exp)->dclnode;
    PermAstNode *perm = (dclnode->asttype == VarDclTag) ? ((VarDclAstNode*)dclnode)->perm : immPerm;
	if (!permMatches(ptype->perm, perm))
		errorMsgNode((AstNode *)node, ErrorBadPerm, "Borrowed reference cannot obtain this permission");
}

// Analyze addr node
void addrPass(PassState *pstate, AddrAstNode *node) {
	astPass(pstate, node->exp);
	if (pstate->pass == TypeCheck) {
		if (!isValueNode(node->exp)) {
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