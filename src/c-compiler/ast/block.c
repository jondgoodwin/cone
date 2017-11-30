/** AST handling for block nodes: Program, FnImpl, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Create a new Program node
PgmAstNode *newPgmNode() {
	PgmAstNode *pgm;
	newAstNode(pgm, PgmAstNode, PgmNode);
	pgm->nodes = newNodes(8);
	return pgm;
}

// Serialize the AST for a program
void pgmPrint(int indent, PgmAstNode *pgm) {
	AstNode **nodesp;
	uint32_t cnt;

	astPrintLn(indent, "AST for program %s", pgm->lexer->url);
	for (nodesFor(pgm->nodes, cnt, nodesp))
		astPrintNode(indent+1, *nodesp, "");
}

// Create a new Function Implementation node
FnImplAstNode *newFnImplNode(Symbol *name, AstNode *sig) {
	FnImplAstNode *fn;
	newAstNode(fn, FnImplAstNode, FnImplNode);
	fn->name = name;
	fn->vtype = sig;
	fn->nodes = NULL;
	return fn;
}

// Serialize the AST for a program
void fnImplPrint(int indent, FnImplAstNode *fn) {
	AstNode **nodesp;
	uint32_t cnt;

	astPrintLn(indent, "fn %s()", fn->name->name);
	astPrintNode(indent, fn->vtype, "-");
	astPrintLn(indent, "-fn statements:");
	for (nodesFor(fn->nodes, cnt, nodesp))
		astPrintNode(indent+1, *nodesp, "");
}

// Create a new Function Implementation node
StmtExpAstNode *newStmtExpNode() {
	StmtExpAstNode *node;
	newAstNode(node, StmtExpAstNode, StmtExpNode);
	return node;
}

// Serialize the AST for a program
void stmtExpPrint(int indent, StmtExpAstNode *node) {
	astPrintLn(indent, "statement expression");
	astPrintNode(indent+1, node->exp, "");
}