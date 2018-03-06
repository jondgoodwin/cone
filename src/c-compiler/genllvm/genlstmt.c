/** Statement generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../shared/nametbl.h"
#include "../shared/fileio.h"
#include "genllvm.h"

#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/Scalar.h>

#include <stdio.h>
#include <assert.h>

// Create a new basic block after the current one
LLVMBasicBlockRef genlInsertBlock(GenState *gen, char *name) {
	LLVMBasicBlockRef nextblock = LLVMGetNextBasicBlock(LLVMGetInsertBlock(gen->builder));
	if (nextblock)
		return LLVMInsertBasicBlockInContext(gen->context, nextblock, name);
	else
		return LLVMAppendBasicBlockInContext(gen->context, gen->fn, name);

}

// Generate a while block
void genlWhile(GenState *gen, WhileAstNode *wnode) {
	LLVMBasicBlockRef whilebeg, whileblk, whileend;
	LLVMBasicBlockRef svwhilebeg, svwhileend;

	// Push and pop for break and continue statements
	svwhilebeg = gen->whilebeg;
	svwhileend = gen->whileend;

	gen->whileend = whileend = genlInsertBlock(gen, "whileend");
	whileblk = genlInsertBlock(gen, "whileblk");
	gen->whilebeg = whilebeg = genlInsertBlock(gen, "whilebeg");

	LLVMBuildBr(gen->builder, whilebeg);
	LLVMPositionBuilderAtEnd(gen->builder, whilebeg);
	LLVMBuildCondBr(gen->builder, genlExpr(gen, wnode->condexp), whileblk, whileend);
	LLVMPositionBuilderAtEnd(gen->builder, whileblk);
	genlBlock(gen, (BlockAstNode*)wnode->blk);
	LLVMBuildBr(gen->builder, whilebeg);
	LLVMPositionBuilderAtEnd(gen->builder, whileend);

	gen->whilebeg = svwhilebeg;
	gen->whileend = svwhileend;
}

// Generate a return statement
void genlReturn(GenState *gen, ReturnAstNode *node) {
	if (node->exp != voidType)
		LLVMBuildRet(gen->builder, genlExpr(gen, node->exp));
	else
		LLVMBuildRetVoid(gen->builder);
}

// Generate a block's statements
LLVMValueRef genlBlock(GenState *gen, BlockAstNode *blk) {
	AstNode **nodesp;
	uint32_t cnt;
	LLVMValueRef lastval = NULL; // Should never be used by caller
	for (nodesFor(blk->stmts, cnt, nodesp)) {
		switch ((*nodesp)->asttype) {
		case WhileNode:
			genlWhile(gen, (WhileAstNode *)*nodesp); break;
		case BreakNode:
			LLVMBuildBr(gen->builder, gen->whileend); break;
		case ContinueNode:
			LLVMBuildBr(gen->builder, gen->whilebeg); break;
		case ReturnNode:
			genlReturn(gen, (ReturnAstNode*)*nodesp); break;
		default:
			lastval = genlExpr(gen, *nodesp);
		}
	}
	return lastval;
}
