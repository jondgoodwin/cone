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

// Check the program's AST
void pgmPass(AstPass *pstate, PgmAstNode *pgm) {
	AstNode **nodesp;
	uint32_t cnt;

	for (nodesFor(pgm->nodes, cnt, nodesp))
		astPass(pstate, *nodesp);
}

// Create a new block node
BlockAstNode *newBlockNode() {
	BlockAstNode *blk;
	newAstNode(blk, BlockAstNode, BlockNode);
	blk->nodes = newNodes(8);
	return blk;
}

// Serialize the AST for a block
void blockPrint(int indent, BlockAstNode *blk) {
	AstNode **nodesp;
	uint32_t cnt;

	astPrintLn(indent, "block statements:");
	if (blk->nodes)
		for (nodesFor(blk->nodes, cnt, nodesp))
			astPrintNode(indent + 1, *nodesp, "");
}

// Check the block's AST
void blockPass(AstPass *pstate, BlockAstNode *blk) {
	AstNode **nodesp;
	uint32_t cnt;

	if (blk->nodes)
		for (nodesFor(blk->nodes, cnt, nodesp))
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
