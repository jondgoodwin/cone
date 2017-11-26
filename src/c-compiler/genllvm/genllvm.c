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
		printf("OMG Found an integer %ld\n", ((ULitAstNode*)termnode)->uintlit);
	}
	else if (termnode->asttype == FLitNode) {
		printf("OMG Found a float %f\n", ((FLitAstNode*)termnode)->floatlit);
	}
}

// Generate a function block
void genlFn(FnBlkAstNode *fnnode) {
	AstNode **nodesp;
	uint32_t cnt;

	assert(fnnode->asttype == FnImplNode);
	for (nodesFor(fnnode->nodes, cnt, nodesp)) {
		genlTerm(*nodesp);
	}
}

// Generate AST into LLVM IR using LLVM
void genllvm(GlobalAstNode *globalnode) {
	uint32_t cnt;
	AstNode **nodesp;

	assert(globalnode->asttype == GlobalNode);
	for (nodesFor(globalnode->nodes, cnt, nodesp)) {
		AstNode *nodep = *nodesp;
		if (nodep->asttype == FnImplNode)
			genlFn((FnBlkAstNode*) nodep);
	}
}
