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
		PtrAstNode *ptype = (PtrAstNode*)((TypedAstNode *)node->exp)->vtype;
		if (ptype->asttype == RefType)
			node->vtype = ptype->pvtype;
		else
			errorMsgNode((AstNode*)node, ErrorNotPtr, "Cannot de-reference a non-pointer value.");
	}
}

// Create a new element node
ElementAstNode *newElementAstNode() {
	ElementAstNode *node;
	newAstNode(node, ElementAstNode, ElementNode);
	node->vtype = voidType;
	return node;
}

// Serialize element
void elementPrint(ElementAstNode *node) {
	astPrintNode(node->owner);
	astFprint(".");
	astPrintNode(node->element);
}

// Analyze element node
void elementPass(AstPass *pstate, ElementAstNode *node) {
	astPass(pstate, node->owner);
	if (pstate->pass == TypeCheck) {
		if (node->element->asttype == FieldNameUseNode) {
			AstNode *ownvtype = typeGetVtype(node->owner);
			if (ownvtype->asttype == StructType) {
				NameUseAstNode *fldname = (NameUseAstNode*)node->element;
				Symbol *fldsym = fldname->namesym;
				SymNode *field = inodesFind(((StructAstNode *)ownvtype)->fields, fldsym);
				if (field) {
					fldname->asttype = VarNameUseNode;
					fldname->dclnode = (NameDclAstNode*)field->node;
					node->vtype = fldname->vtype = fldname->dclnode->vtype;
				}
				else
					errorMsgNode((AstNode*)node, ErrorNoMeth, "The field `%s` is not defined by the object's type.", &fldsym->namestr);
			}
			else
				errorMsgNode(node->element, ErrorNoFlds, "Fields not supported by expression's type");
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
