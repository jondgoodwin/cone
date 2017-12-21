/** AST handling for expression nodes
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

// lval expression is valid lval
int isLval(AstNode *node) {
	return node->asttype == VarNameUseNode;
}

// Analyze assignment node
void assignPass(AstPass *pstate, AssignAstNode *node) {
	astPass(pstate, node->lval);
	astPass(pstate, node->rval);

	switch (pstate->pass) {
	case TypeCheck:
		if (!isLval(node->lval))
			errorMsgNode(node->lval, ErrorBadLval, "Expression to left of assignment must be lval");
		else if (!typeCoerces(node->lval, &node->rval))
			errorMsgNode(node->lval, ErrorInvType, "Lval's type does not match the expression's type");
		else if (!(MayWrite & permGetFlags(node->lval)))
			errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
		node->vtype = ((TypedAstNode*)node->rval)->vtype;
	}
}

// Create a function call node
FnCallAstNode *newFnCallAstNode(AstNode *fn) {
	FnCallAstNode *node;
	newAstNode(node, FnCallAstNode, FnCallNode);
	node->fn = fn;
	node->parms = NULL;
	return node;
}

// Serialize function call node
void fnCallPrint(FnCallAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	astPrintNode(node->fn);
	astFprint("(");
	for (nodesFor(node->parms, cnt, nodesp)) {
		astPrintNode(*nodesp);
		if (cnt > 1)
			astFprint(", ");
	}
	astFprint(")");
}

// Analyze function call node
void fnCallPass(AstPass *pstate, FnCallAstNode *node) {
	AstNode **argsp;
	uint32_t cnt;
	for (nodesFor(node->parms, cnt, argsp))
		astPass(pstate, *argsp);
	astPass(pstate, node->fn);

	switch (pstate->pass) {
	case TypeCheck:
	{
		// Capture return vtype and ensure we are calling a function
		AstNode *fnsig = typeGetVtype(node->fn);
		if (fnsig->asttype == FnSig)
			node->vtype = ((FnSigAstNode*)fnsig)->rettype;
		else
			errorMsgNode(node->fn, ErrorNotFn, "Cannot call a value that is not a function");

		// Error out if we have too many arguments
		int argsunder = ((FnSigAstNode*)fnsig)->parms->used - node->parms->used;
		if (argsunder < 0) {
			errorMsgNode((AstNode*)node, ErrorManyArgs, "Too many arguments specified vs. function declaration");
			return;
		}

		// Type check that passed arguments match declared parameters
		AstNode **argsp;
		uint32_t cnt;
		SymNode *parmp = (SymNode*)((((FnSigAstNode*)fnsig)->parms)+1);
		for (nodesFor(node->parms, cnt, argsp)) {
			if (!typeCoerces(parmp->node, argsp))
				errorMsgNode(*argsp, ErrorInvType, "Argument type does not match declared parameter");
			parmp++;
		}

		// If we have too few arguments, use default values, if provided
		if (argsunder > 0) {
			if (((NameDclAstNode*)parmp->node)->value == NULL)
				errorMsgNode((AstNode*)node, ErrorFewArgs, "Function call requires more arguments than specified");
			else {
				while (argsunder--) {
					nodesAdd(&node->parms, ((NameDclAstNode*)parmp->node)->value);
					parmp++;
				}
			}
		}
		break;
	}
	}
}

// Create a new name use node
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
