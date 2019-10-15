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

// Generate a loop block
LLVMValueRef genlLoop(GenState *gen, LoopNode *loopnode) {
    LLVMBasicBlockRef loopbeg, loopend;

    loopend = genlInsertBlock(gen, "loopend");
    loopbeg = genlInsertBlock(gen, "loopbeg");

    // Push loop state info on loop stack
    if (gen->loopstackcnt >= GenLoopMax) {
        errorMsgNode((INode*)loopnode, ErrorBadArray, "Overflowing fixed-size loop stack.");
        errorExit(100, "Unrecoverable error!");
    }
    GenLoopState *loopstate = &gen->loopstack[gen->loopstackcnt];
    loopstate->loop = loopnode;
    loopstate->loopbeg = loopbeg;
    loopstate->loopend = loopend;
    ++gen->loopstackcnt;

    LLVMBuildBr(gen->builder, loopbeg);
    LLVMPositionBuilderAtEnd(gen->builder, loopbeg);
    genlBlock(gen, (BlockNode*)loopnode->blk);
    LLVMBuildBr(gen->builder, loopbeg);
    LLVMPositionBuilderAtEnd(gen->builder, loopend);

    --gen->loopstackcnt;
    return NULL;
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

// Find the loop state in loop stack whose lifetime matches
GenLoopState *genFindLoopState(GenState *gen, INode *life) {
    uint32_t cnt = gen->loopstackcnt;
    if (!life)
        return &gen->loopstack[cnt - 1]; // use most recent, when no lifetime
    LifetimeNode *lifeDclNode = (LifetimeNode *)((NameUseNode *)life)->dclnode;
    while (cnt--) {
        if (gen->loopstack[cnt].loop->life == lifeDclNode)
            return &gen->loopstack[cnt];
    }
    return NULL;  // Should never get here
}

// Generate a block's statements
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;
    LLVMValueRef lastval = NULL; // Should never be used by caller
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        switch ((*nodesp)->tag) {
        case BreakTag:
            genlDealiasNodes(gen, ((BreakNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, genFindLoopState(gen, ((BreakNode*)*nodesp)->life)->loopend); break;
        case ContinueTag:
            genlDealiasNodes(gen, ((ContinueNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, genFindLoopState(gen, ((BreakNode*)*nodesp)->life)->loopbeg); break;
        case ReturnTag:
            genlReturn(gen, (ReturnNode*)*nodesp); break;
        case BlockRetTag:
        {
            ReturnNode *node = (ReturnNode*)*nodesp;
            if (node->exp != voidType)
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
