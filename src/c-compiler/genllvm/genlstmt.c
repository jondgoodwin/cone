/** Statement generation via LLVM
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../parser/lexer.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
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
void genlWhile(GenState *gen, WhileNode *wnode) {
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
	genlBlock(gen, (BlockNode*)wnode->blk);
	LLVMBuildBr(gen->builder, whilebeg);
	LLVMPositionBuilderAtEnd(gen->builder, whileend);

	gen->whilebeg = svwhilebeg;
	gen->whileend = svwhileend;
}

// Generate a return statement
void genlReturn(GenState *gen, ReturnNode *node) {
    if (node->exp != voidType) {
        LLVMValueRef retval = genlExpr(gen, node->exp);
        genlDealiasNodes(gen, node->dealias);
        LLVMBuildRet(gen->builder, retval);
    }
    else {
        genlDealiasNodes(gen, node->dealias);
        LLVMBuildRetVoid(gen->builder);
    }
}

// Generate a block "return" node
void genlBlockRet(GenState *gen, ReturnNode *node) {
}

// Generate a block's statements
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk) {
	INode **nodesp;
	uint32_t cnt;
	LLVMValueRef lastval = NULL; // Should never be used by caller
	for (nodesFor(blk->stmts, cnt, nodesp)) {
		switch ((*nodesp)->tag) {
		case WhileTag:
			genlWhile(gen, (WhileNode *)*nodesp); break;
		case BreakTag:
            genlDealiasNodes(gen, ((BreakNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, gen->whileend); break;
		case ContinueTag:
            genlDealiasNodes(gen, ((BreakNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, gen->whilebeg); break;
		case ReturnTag:
			genlReturn(gen, (ReturnNode*)*nodesp); break;
        case BlockRetTag:
        {
            ReturnNode *node = (ReturnNode*)*nodesp;
            if (isExpNode(node->exp))
                lastval = genlExpr(gen, node->exp);
            genlDealiasNodes(gen, node->dealias);
            break;
        }
		default:
			lastval = genlExpr(gen, *nodesp);
		}
	}
	return lastval;
}
