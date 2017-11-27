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
	astPrintLn(indent, "Unsiged literal %ld", lit->uintlit);
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
