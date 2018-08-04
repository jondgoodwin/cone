/** AST handling for literals
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

// Create a new unsigned literal node
ULitAstNode *newULitNode(uint64_t nbr, INode *type) {
	ULitAstNode *lit;
	newNode(lit, ULitAstNode, ULitNode);
	lit->uintlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void ulitPrint(ULitAstNode *lit) {
	if (((NbrAstNode*)lit->vtype)->bits == 1)
		inodeFprint(lit->uintlit == 1 ? "true" : "false");
	else {
		inodeFprint("%ld", lit->uintlit);
		inodePrintNode(lit->vtype);
	}
}

// Create a new unsigned literal node
FLitAstNode *newFLitNode(double nbr, INode *type) {
	FLitAstNode *lit;
	newNode(lit, FLitAstNode, FLitNode);
	lit->floatlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void flitPrint(FLitAstNode *lit) {
	inodeFprint("%g", lit->floatlit);
	inodePrintNode(lit->vtype);
}

// Create a new string literal node
SLitAstNode *newSLitNode(char *str, INode *type) {
	SLitAstNode *lit;
	newNode(lit, SLitAstNode, SLitNode);
	lit->strlit = str;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a string
void slitPrint(SLitAstNode *lit) {
	inodeFprint("\"%s\"", lit->strlit);
}

int litIsLiteral(INode* node) {
	return (node->asttype == FLitNode || node->asttype == ULitNode);
}