/** AST handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Create a new unsigned literal node
FnSigAstNode *newFnSigNode() {
	FnSigAstNode *sig;
	newAstNode(sig, FnSigAstNode, FnSig);
	sig->rettype = voidType;
	return sig;
}

// Serialize the AST for a Unsigned literal
void fnsigPrint(int indent, FnSigAstNode *sig, char* prefix) {
	astPrintLn(indent, "%s fn signature", prefix);
	astPrintNode(indent+1, sig->rettype, "-return type:");
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigAstNode *node1, FnSigAstNode *node2) {
	return node1->rettype == node2->rettype;
}