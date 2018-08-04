/** AST handling for expression nodes that do not do value copy/move
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

// Create a new sizeof node
SizeofAstNode *newSizeofAstNode() {
	SizeofAstNode *node;
	newNode(node, SizeofAstNode, SizeofNode);
	node->vtype = (INode*)usizeType;
	return node;
}

// Serialize sizeof
void sizeofPrint(SizeofAstNode *node) {
	inodeFprint("(sizeof, ");
	inodePrintNode(node->type);
	inodeFprint(")");
}

// Analyze sizeof node
void sizeofPass(PassState *pstate, SizeofAstNode *node) {
	inodeWalk(pstate, &node->type);
}

// Create a new cast node
CastAstNode *newCastAstNode(INode *exp, INode *type) {
	CastAstNode *node;
	newNode(node, CastAstNode, CastNode);
	node->vtype = type;
	node->exp = exp;
	return node;
}

// Serialize cast
void castPrint(CastAstNode *node) {
	inodeFprint("(cast, ");
	inodePrintNode(node->vtype);
	inodeFprint(", ");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Analyze cast node
void castPass(PassState *pstate, CastAstNode *node) {
	inodeWalk(pstate, &node->exp);
	inodeWalk(pstate, &node->vtype);
	if (pstate->pass == TypeCheck && 0 == typeMatches(node->vtype, ((TypedAstNode *)node->exp)->vtype))
		errorMsgNode(node->vtype, ErrorInvType, "expression may not be type cast to this type");
}

// Create a new deref node
DerefAstNode *newDerefAstNode() {
	DerefAstNode *node;
	newNode(node, DerefAstNode, DerefNode);
	node->vtype = voidType;
	return node;
}

// Serialize deref
void derefPrint(DerefAstNode *node) {
	inodeFprint("*");
	inodePrintNode(node->exp);
}

// Analyze deref node
void derefPass(PassState *pstate, DerefAstNode *node) {
	inodeWalk(pstate, &node->exp);
	if (pstate->pass == TypeCheck) {
		PtrAstNode *ptype = (PtrAstNode*)((TypedAstNode *)node->exp)->vtype;
		if (ptype->asttype == RefType || ptype->asttype == PtrType)
			node->vtype = ptype->pvtype;
		else
			errorMsgNode((INode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
	}
}

// Insert automatic deref, if node is a ref
void derefAuto(INode **node) {
	if (typeGetVtype(*node)->asttype != RefType)
		return;
	DerefAstNode *deref = newDerefAstNode();
	deref->exp = *node;
	deref->vtype = ((PtrAstNode*)((TypedAstNode *)*node)->vtype)->pvtype;
	*node = (INode*)deref;
}

// Create a new logic operator node
LogicAstNode *newLogicAstNode(int16_t typ) {
	LogicAstNode *node;
	newNode(node, LogicAstNode, typ);
	node->vtype = (INode*)boolType;
	return node;
}

// Serialize logic node
void logicPrint(LogicAstNode *node) {
	if (node->asttype == NotLogicNode) {
		inodeFprint("!");
		inodePrintNode(node->lexp);
	}
	else {
		inodeFprint(node->asttype == AndLogicNode? "(&&, " : "(||, ");
		inodePrintNode(node->lexp);
		inodeFprint(", ");
		inodePrintNode(node->rexp);
		inodeFprint(")");
	}
}

// Analyze not logic node
void logicNotPass(PassState *pstate, LogicAstNode *node) {
	inodeWalk(pstate, &node->lexp);
	if (pstate->pass == TypeCheck)
		typeCoerces((INode*)boolType, &node->lexp);
}

// Analyze logic node
void logicPass(PassState *pstate, LogicAstNode *node) {
	inodeWalk(pstate, &node->lexp);
	inodeWalk(pstate, &node->rexp);

	if (pstate->pass == TypeCheck) {
		typeCoerces((INode*)boolType, &node->lexp);
		typeCoerces((INode*)boolType, &node->rexp);
	}
}
