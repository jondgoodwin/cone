/** AST handling for expression nodes that do not do value copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

#include <assert.h>

// Create a new cast node
CastAstNode *newCastAstNode(AstNode *exp, AstNode *type) {
	CastAstNode *node;
	newAstNode(node, CastAstNode, CastNode);
	node->vtype = type;
	node->exp = exp;
	return node;
}

// Serialize cast
void castPrint(CastAstNode *node) {
	astFprint("(cast, ");
	astPrintNode(node->vtype);
	astFprint(", ");
	astPrintNode(node->exp);
	astFprint(")");
}

// Analyze cast node
void castPass(AstPass *pstate, CastAstNode *node) {
	astPass(pstate, node->exp);
}

// Create a new deref node
DerefAstNode *newDerefAstNode() {
	DerefAstNode *node;
	newAstNode(node, DerefAstNode, DerefNode);
	node->vtype = voidType;
	return node;
}

// Serialize deref
void derefPrint(DerefAstNode *node) {
	astFprint("*");
	astPrintNode(node->exp);
}

// Analyze deref node
void derefPass(AstPass *pstate, DerefAstNode *node) {
	astPass(pstate, node->exp);
	if (pstate->pass == TypeCheck) {
		PtrTypeAstNode *ptype = (PtrTypeAstNode*)((TypedAstNode *)node->exp)->vtype;
		if (ptype->asttype == PtrType)
			node->vtype = ptype->pvtype;
		else
			errorMsgNode((AstNode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
	}
}

// Create a new logic operator node
LogicAstNode *newLogicAstNode(int16_t typ) {
	LogicAstNode *node;
	newAstNode(node, LogicAstNode, typ);
	node->vtype = (AstNode*)boolType;
	return node;
}

// Serialize logic node
void logicPrint(LogicAstNode *node) {
	if (node->asttype == NotLogicNode) {
		astFprint("!");
		astPrintNode(node->lexp);
	}
	else {
		astFprint(node->asttype == AndLogicNode? "(&&, " : "(||, ");
		astPrintNode(node->lexp);
		astFprint(", ");
		astPrintNode(node->rexp);
		astFprint(")");
	}
}

// Analyze not logic node
void logicNotPass(AstPass *pstate, LogicAstNode *node) {
	astPass(pstate, node->lexp);
	if (pstate->pass == TypeCheck)
		typeCoerces((AstNode*)boolType, &node->lexp);
}

// Analyze logic node
void logicPass(AstPass *pstate, LogicAstNode *node) {
	astPass(pstate, node->lexp);
	astPass(pstate, node->rexp);

	if (pstate->pass == TypeCheck) {
		typeCoerces((AstNode*)boolType, &node->lexp);
		typeCoerces((AstNode*)boolType, &node->rexp);
	}
}
