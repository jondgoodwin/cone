/** Code generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../shared/ast.h"
#include "../shared/error.h"

#include <stdio.h>
#include <assert.h>

// Generate a term
void genlTerm(AstNode *termnode) {
	if (termnode->asttype == ULitNode) {
		printf("OMG Found an integer %ld\n", ((ULitAstNode*)termnode)->uintlit);
	}
	else if (termnode->asttype == FLitNode) {
		printf("OMG Found a float %f\n", ((FLitAstNode*)termnode)->floatlit);
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(AstNode *pgmnode) {
	Nodes *nodes;
	uint32_t cnt;
	AstNode **nodep;

	assert(pgmnode->asttype == PgmNode);
	nodes = (Nodes*) ((PgmAstNode*)pgmnode)->nodes;
	nodep = (AstNode**)(nodes+1);
	cnt = nodes->used;
	while (cnt--) {
		AstNode *node;
		genlTerm(*nodep++);
	}
}
