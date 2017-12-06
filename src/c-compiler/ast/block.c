/** AST handling for block nodes: Program, FnImpl, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"
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

// Check the program's AST
void pgmPass(AstPass *pstate, PgmAstNode *pgm) {
	AstNode **nodesp;
	uint32_t cnt;

	for (nodesFor(pgm->nodes, cnt, nodesp))
		astPass(pstate, *nodesp);
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

// Serialize the AST for a function implementation
void fnImplPrint(int indent, FnImplAstNode *fn) {
	AstNode **nodesp;
	uint32_t cnt;

	astPrintLn(indent, "fn %s()", fn->name->name);
	astPrintNode(indent, fn->vtype, "-");
	astPrintLn(indent, "-fn statements:");
	if (fn->nodes)
		for (nodesFor(fn->nodes, cnt, nodesp))
			astPrintNode(indent+1, *nodesp, "");
}

// Add function to global namespace if it does not conflict or dupe implementation with prior definition
void fnImplGlobalPass(FnImplAstNode *fnnode) {
	Symbol *name = fnnode->name;

	// Remember function in symbol table, but error out if prior name has a different type
	// or both this and saved node define an implementation
	if (!name->node)
		name->node = (AstNode*)fnnode;
	else if (!typeEqual((AstNode*)fnnode, name->node)) {
		errorMsgNode((AstNode *)fnnode, ErrorTypNotSame, "Name is already defined with a different type/signature.");
		errorMsgNode(name->node, ErrorTypNotSame, "This is the conflicting definition for that name.");
	} else if (fnnode->nodes) {
		if (((FnImplAstNode*)name->node)->nodes) {
			errorMsgNode((AstNode *)fnnode, ErrorFnDupImpl, "Function has a duplicate implementations. Only one allowed.");
			errorMsgNode(name->node, ErrorFnDupImpl, "This is the other function implementation.");
		} else
			name->node = (AstNode*)fnnode;
	}
}

// Check the function's AST
void fnImplPass(AstPass *pstate, FnImplAstNode *fn) {
	AstNode **nodesp;
	uint32_t cnt;

	switch (pstate->pass) {
	case GlobalPass:
		fnImplGlobalPass(fn);
		return;
	}

	if (fn->nodes)
		for (nodesFor(fn->nodes, cnt, nodesp))
			astPass(pstate, *nodesp);
}

// Create a new expression statement node
StmtExpAstNode *newStmtExpNode() {
	StmtExpAstNode *node;
	newAstNode(node, StmtExpAstNode, StmtExpNode);
	return node;
}

// Serialize the AST for a statement expression
void stmtExpPrint(int indent, StmtExpAstNode *node) {
	astPrintLn(indent, "statement expression");
	astPrintNode(indent+1, node->exp, "");
}

// Check the AST for a statement expression
void stmtExpPass(AstPass *pstate, StmtExpAstNode *node) {
	astPass(pstate, node->exp);
}

// Create a new return statement node
StmtExpAstNode *newReturnNode() {
	StmtExpAstNode *node;
	newAstNode(node, StmtExpAstNode, ReturnNode);
	node->exp = voidType;
	return node;
}

// Serialize the AST for a return statement
void returnPrint(int indent, StmtExpAstNode *node) {
	astPrintLn(indent, "return");
	astPrintNode(indent+1, node->exp, "");
}

// Check the AST for a return statement
void returnPass(AstPass *pstate, StmtExpAstNode *node) {
	astPass(pstate, node->exp);
}
