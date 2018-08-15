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
ULitNode *newULitNode(uint64_t nbr, INode *type) {
	ULitNode *lit;
	newNode(lit, ULitNode, ULitTag);
	lit->uintlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void ulitPrint(ULitNode *lit) {
	if (((NbrNode*)lit->vtype)->bits == 1)
		inodeFprint(lit->uintlit == 1 ? "true" : "false");
	else {
		inodeFprint("%ld", lit->uintlit);
		inodePrintNode(lit->vtype);
	}
}

// Create a new unsigned literal node
FLitNode *newFLitNode(double nbr, INode *type) {
	FLitNode *lit;
	newNode(lit, FLitNode, FLitTag);
	lit->floatlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a Unsigned literal
void flitPrint(FLitNode *lit) {
	inodeFprint("%g", lit->floatlit);
	inodePrintNode(lit->vtype);
}

// Create a new string literal node
SLitNode *newSLitNode(char *str, INode *type) {
	SLitNode *lit;
	newNode(lit, SLitNode, StrLitTag);
	lit->strlit = str;
	lit->vtype = type;
	return lit;
}

// Serialize the AST for a string
void slitPrint(SLitNode *lit) {
	inodeFprint("\"%s\"", lit->strlit);
}

int litIsLiteral(INode* node) {
	return (node->asttype == FLitTag || node->asttype == ULitTag);
}