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

// Generate AST into LLVM IR using LLVM
void genllvm(AstNode *pgmnode) {
	Nodes *nodes;
	uint32_t cnt;
	AstNode **nodep;

	assert(pgmnode->asttype == BlockNode);
	nodes = (Nodes*) ((BlockAstNode*)pgmnode)->nodes;
	nodep = (AstNode**)(nodes+1);
	cnt = nodes->used;
	while (cnt--) {
		AstNode *node;
		node = *nodep++;
		if (node->asttype == ULitNode) {
			printf("OMG Found an integer %ld\n", ((ULitAstNode*)node)->uintlit);
		}
		else if (node->asttype == FLitNode) {
			printf("OMG Found a float %f\n", ((FLitAstNode*)node)->floatlit);
		}
	}
}
