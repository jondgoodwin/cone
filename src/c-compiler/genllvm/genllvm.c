/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/error.h"

#include <stdio.h>
#include <assert.h>

// Generate a term
void genlTerm(AstNode *termnode) {
	if (termnode->asttype == ULitNode) {
		printf("OMG Found an integer %lld\n", ((ULitAstNode*)termnode)->uintlit);
	}
	else if (termnode->asttype == FLitNode) {
		printf("OMG Found a float %f\n", ((FLitAstNode*)termnode)->floatlit);
	}
}

// Generate a return statement
void genlReturn(StmtExpAstNode *node) {
	if (node->exp != voidType)
		genlTerm(node->exp);
}

// Generate a function block
void genlFn(FnImplAstNode *fnnode) {
	AstNode **nodesp;
	uint32_t cnt;

	assert(fnnode->asttype == FnImplNode);
	for (nodesFor(fnnode->nodes, cnt, nodesp)) {
		switch ((*nodesp)->asttype) {
		case StmtExpNode:
			genlTerm(((StmtExpAstNode*)*nodesp)->exp); break;
		case ReturnNode:
			genlReturn((StmtExpAstNode*)*nodesp); break;
		}
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(PgmAstNode *pgm) {
	uint32_t cnt;
	AstNode **nodesp;

	assert(pgm->asttype == PgmNode);
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == FnImplNode)
			genlFn((FnImplAstNode*) nodep);
	}
}
