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

void genllvm(AstNode *pgmnode) {
	Nodes *nodes;
	uint32_t cnt;
	AstNode **nodep;

	assert(pgmnode->asttype == BlockNode);
	nodes = (Nodes*) pgmnode->v.info;
	nodep = (AstNode**)(nodes+1);
	cnt = nodes->used;
	while (cnt--) {
		AstNode *node;
		node = *nodep++;
		if (node->asttype == IntNode) {
			printf("OMG Found an integer %d\n", node->v.uintlit);
		}
		else if (node->asttype == FloatNode) {
			printf("OMG Found a float %f\n", node->v.floatlit);
		}
	}
}
