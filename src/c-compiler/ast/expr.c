/** AST handling for expression nodes: Literals, Variables, etc.
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

// Create a new variable node
VarAstNode *newVarNode(Symbol *name, AstNode *type, AstNode *perm) {
	VarAstNode *var;
	newAstNode(var, VarAstNode, VarNode);
	var->name = name;
	var->vtype = type;
	var->value = NULL;
	return var;
}

// Serialize the AST for a variable/function
void varPrint(int indent, VarAstNode *var) {
	astPrintLn(indent, var->vtype->asttype==FnSig? "fn %s()" : "var %s", var->name->name);
	astPrintNode(indent, var->vtype, "-");
	if (var->value)
		astPrintNode(indent + 1, var->value, "");
}

// Add variable to global namespace if it does not conflict or dupe implementation with prior definition
void varGlobalPass(VarAstNode *varnode) {
	Symbol *name = varnode->name;

	// Remember function in symbol table, but error out if prior name has a different type
	// or both this and saved node define an implementation
	if (!name->node)
		name->node = (AstNode*)varnode;
	else if (!typeEqual((AstNode*)varnode, name->node)) {
		errorMsgNode((AstNode *)varnode, ErrorTypNotSame, "Name is already defined with a different type/signature.");
		errorMsgNode(name->node, ErrorTypNotSame, "This is the conflicting definition for that name.");
	}
	else if (varnode->value) {
		if (((VarAstNode*)name->node)->value) {
			errorMsgNode((AstNode *)varnode, ErrorDupImpl, "Name has a duplicate implementation/value. Only one allowed.");
			errorMsgNode(name->node, ErrorDupImpl, "This is the other implementation/value.");
		}
		else
			name->node = (AstNode*)varnode;
	}
}

// Check the variable's AST
void varPass(AstPass *pstate, VarAstNode *var) {
	switch (pstate->pass) {
	case GlobalPass:
		varGlobalPass(var);
		return;
	}

	if (var->value)
		astPass(pstate, var->value);
}

// Create a new unsigned literal node
ULitAstNode *newULitNode(uint64_t nbr, AstNode *type) {
	ULitAstNode *lit;
	newAstNode(lit, ULitAstNode, ULitNode);
	lit->uintlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void ulitPrint(int indent, ULitAstNode *lit) {
	astPrintLn(indent, "Unsigned literal %ld", lit->uintlit);
}

// Create a new unsigned literal node
FLitAstNode *newFLitNode(double nbr, AstNode *type) {
	FLitAstNode *lit;
	newAstNode(lit, FLitAstNode, FLitNode);
	lit->floatlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void flitPrint(int indent, FLitAstNode *lit) {
	astPrintLn(indent, "Float literal %g", lit->floatlit);
}
