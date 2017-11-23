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

// Generate a function block
void genlFn(AstNode *fnnode) {
	Nodes *nodes;
	uint32_t cnt;
	AstNode **nodep;

	assert(fnnode->asttype == FnBlkNode);
	nodes = (Nodes*) ((FnBlkAstNode*)fnnode)->nodes;
	nodep = (AstNode**)(nodes+1);
	cnt = nodes->used;
	while (cnt--) {
		genlTerm(*nodep++);
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(AstNode *globalnode) {
	Nodes *nodes;
	uint32_t cnt;
	AstNode **nodep;

	assert(globalnode->asttype == GlobalNode);
	nodes = (Nodes*) ((GlobalAstNode*)globalnode)->nodes;
	nodep = (AstNode**)(nodes+1);
	cnt = nodes->used;
	while (cnt--) {
		if ((*nodep)->asttype == FnBlkNode)
			genlFn(*nodep);
		nodep++;
	}
}
