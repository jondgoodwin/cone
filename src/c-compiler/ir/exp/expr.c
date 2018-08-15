/** Handling for expression nodes that do not do value copy/move
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
SizeofNode *newSizeofNode() {
	SizeofNode *node;
	newNode(node, SizeofNode, SizeofTag);
	node->vtype = (INode*)usizeType;
	return node;
}

// Serialize sizeof
void sizeofPrint(SizeofNode *node) {
	inodeFprint("(sizeof, ");
	inodePrintNode(node->type);
	inodeFprint(")");
}

// Analyze sizeof node
void sizeofPass(PassState *pstate, SizeofNode *node) {
	inodeWalk(pstate, &node->type);
}

// Create a new cast node
CastNode *newCastNode(INode *exp, INode *type) {
	CastNode *node;
	newNode(node, CastNode, CastTag);
	node->vtype = type;
	node->exp = exp;
	return node;
}

// Serialize cast
void castPrint(CastNode *node) {
	inodeFprint("(cast, ");
	inodePrintNode(node->vtype);
	inodeFprint(", ");
	inodePrintNode(node->exp);
	inodeFprint(")");
}

// Analyze cast node
void castPass(PassState *pstate, CastNode *node) {
	inodeWalk(pstate, &node->exp);
	inodeWalk(pstate, &node->vtype);
	if (pstate->pass == TypeCheck && 0 == typeMatches(node->vtype, ((ITypedNode *)node->exp)->vtype))
		errorMsgNode(node->vtype, ErrorInvType, "expression may not be type cast to this type");
}

// Create a new deref node
DerefNode *newDerefNode() {
	DerefNode *node;
	newNode(node, DerefNode, DerefTag);
	node->vtype = voidType;
	return node;
}

// Serialize deref
void derefPrint(DerefNode *node) {
	inodeFprint("*");
	inodePrintNode(node->exp);
}

// Analyze deref node
void derefPass(PassState *pstate, DerefNode *node) {
	inodeWalk(pstate, &node->exp);
	if (pstate->pass == TypeCheck) {
		PtrNode *ptype = (PtrNode*)((ITypedNode *)node->exp)->vtype;
		if (ptype->tag == RefTag || ptype->tag == PtrTag)
			node->vtype = ptype->pvtype;
		else
			errorMsgNode((INode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
	}
}

// Insert automatic deref, if node is a ref
void derefAuto(INode **node) {
	if (typeGetVtype(*node)->tag != RefTag)
		return;
	DerefNode *deref = newDerefNode();
	deref->exp = *node;
	deref->vtype = ((PtrNode*)((ITypedNode *)*node)->vtype)->pvtype;
	*node = (INode*)deref;
}

// Create a new logic operator node
LogicNode *newLogicNode(int16_t typ) {
	LogicNode *node;
	newNode(node, LogicNode, typ);
	node->vtype = (INode*)boolType;
	return node;
}

// Serialize logic node
void logicPrint(LogicNode *node) {
	if (node->tag == NotLogicTag) {
		inodeFprint("!");
		inodePrintNode(node->lexp);
	}
	else {
		inodeFprint(node->tag == AndLogicTag? "(&&, " : "(||, ");
		inodePrintNode(node->lexp);
		inodeFprint(", ");
		inodePrintNode(node->rexp);
		inodeFprint(")");
	}
}

// Analyze not logic node
void logicNotPass(PassState *pstate, LogicNode *node) {
	inodeWalk(pstate, &node->lexp);
	if (pstate->pass == TypeCheck)
		typeCoerces((INode*)boolType, &node->lexp);
}

// Analyze logic node
void logicPass(PassState *pstate, LogicNode *node) {
	inodeWalk(pstate, &node->lexp);
	inodeWalk(pstate, &node->rexp);

	if (pstate->pass == TypeCheck) {
		typeCoerces((INode*)boolType, &node->lexp);
		typeCoerces((INode*)boolType, &node->rexp);
	}
}
