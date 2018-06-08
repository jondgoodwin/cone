/** AST handling for expression nodes that do not do value copy/move
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

// Create a new sizeof node
SizeofAstNode *newSizeofAstNode() {
	SizeofAstNode *node;
	newAstNode(node, SizeofAstNode, SizeofNode);
	node->vtype = (AstNode*)usizeType;
	return node;
}

// Serialize sizeof
void sizeofPrint(SizeofAstNode *node) {
	astFprint("(sizeof, ");
	astPrintNode(node->type);
	astFprint(")");
}

// Analyze sizeof node
void sizeofPass(PassState *pstate, SizeofAstNode *node) {
	astPass(pstate, node->type);
}

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
void castPass(PassState *pstate, CastAstNode *node) {
	astPass(pstate, node->exp);
	astPass(pstate, node->vtype);
	if (pstate->pass == TypeCheck && 0 == typeMatches(node->vtype, ((TypedAstNode *)node->exp)->vtype))
		errorMsgNode(node->vtype, ErrorInvType, "expression may not be type cast to this type");
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
void derefPass(PassState *pstate, DerefAstNode *node) {
	astPass(pstate, node->exp);
	if (pstate->pass == TypeCheck) {
		PtrAstNode *ptype = (PtrAstNode*)((TypedAstNode *)node->exp)->vtype;
		if (ptype->asttype == RefType || ptype->asttype == PtrType)
			node->vtype = ptype->pvtype;
		else
			errorMsgNode((AstNode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
	}
}

// Insert automatic deref, if node is a ref
void derefAuto(AstNode **node) {
	if (typeGetVtype(*node)->asttype != RefType)
		return;
	DerefAstNode *deref = newDerefAstNode();
	deref->exp = *node;
	deref->vtype = ((PtrAstNode*)((TypedAstNode *)*node)->vtype)->pvtype;
	*node = (AstNode*)deref;
}

// Create a new element node
DotOpAstNode *newDotOpAstNode() {
	DotOpAstNode *node;
	newAstNode(node, DotOpAstNode, DotOpNode);
	node->vtype = voidType;
	return node;
}

// Serialize element
void dotOpPrint(DotOpAstNode *node) {
	astPrintNode(node->instance);
	astFprint(".");
	astPrintNode(node->field);
}

// Analyze element node
void dotOpPass(PassState *pstate, DotOpAstNode *node) {
	astPass(pstate, node->instance);
	if (pstate->pass == TypeCheck) {
		if (node->field->asttype == MemberUseNode) {
			derefAuto(&node->instance);
			AstNode *ownvtype = typeGetVtype(node->instance);
			if (ownvtype->asttype == StructType) {
				NameUseAstNode *fldname = (NameUseAstNode*)node->field;
				Name *fldsym = fldname->namesym;
				SymNode *field = inodesFind(((StructAstNode *)ownvtype)->fields, fldsym);
				if (field) {
					fldname->asttype = NameUseNode;
					fldname->dclnode = (NamedAstNode*)field->node;
					node->vtype = fldname->vtype = fldname->dclnode->vtype;
				}
				else
					errorMsgNode((AstNode*)node, ErrorNoMeth, "The field `%s` is not defined by the object's type.", &fldsym->namestr);
			}
			else
				errorMsgNode(node->field, ErrorNoFlds, "Fields not supported by expression's type");
		}
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
void logicNotPass(PassState *pstate, LogicAstNode *node) {
	astPass(pstate, node->lexp);
	if (pstate->pass == TypeCheck)
		typeCoerces((AstNode*)boolType, &node->lexp);
}

// Analyze logic node
void logicPass(PassState *pstate, LogicAstNode *node) {
	astPass(pstate, node->lexp);
	astPass(pstate, node->rexp);

	if (pstate->pass == TypeCheck) {
		typeCoerces((AstNode*)boolType, &node->lexp);
		typeCoerces((AstNode*)boolType, &node->rexp);
	}
}
