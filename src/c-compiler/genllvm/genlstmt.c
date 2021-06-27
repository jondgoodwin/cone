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

// Find the loop state in loop stack whose lifetime matches
GenBlockState *genFindBlockState(GenState *gen, BlockNode *block) {
    uint32_t cnt = gen->blockstackcnt;
    while (cnt--) {
        if (gen->blockstack[cnt].blocknode == block)
            return &gen->blockstack[cnt];
    }
    return NULL;  // Should never get here
}

// Generate a block/loop break
void genlBreak(GenState *gen, BlockNode* block, INode* exp, Nodes* dealias) {
    GenBlockState *blockstate = genFindBlockState(gen, block);
    if (exp->tag != NilLitTag) {
        blockstate->phis[blockstate->phiCnt] = genlExpr(gen, exp);
        blockstate->blocksFrom[blockstate->phiCnt++] = LLVMGetInsertBlock(gen->builder);
    }
    genlDealiasNodes(gen, dealias);
    LLVMBuildBr(gen->builder, blockstate->blockend);
}

// Generate a return statement
void genlReturn(GenState *gen, BreakRetNode *node) {
    if (node->exp->tag != NilLitTag) {
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
void genlBlockRet(GenState *gen, BreakRetNode *node) {
}

// Generate a block's statements (could be a loop block)
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk) {
    // Create separate blocks only when needed. 
    // isLoop requires blkbeg and blkend. isPhiBlk only blkend when phi values must converge for break/returns
    int isLoop = blk->flags & FlagLoop;
    int isPhiBlk = isLoop; // or multiple break/returns to phi value

    LLVMBasicBlockRef blockbeg = NULL;
    LLVMBasicBlockRef blockend = NULL;
    GenBlockState *blkstate;

    if (isPhiBlk) {
        blockend = genlInsertBlock(gen, isLoop? "loopend" : "blockend");
        if (isLoop) {
            blockbeg = genlInsertBlock(gen, "loopbeg");
            LLVMBuildBr(gen->builder, blockbeg);
            LLVMPositionBuilderAtEnd(gen->builder, blockbeg);
        }

        // Push block info on stack for break & continue to use
        if (gen->blockstackcnt >= GenBlockStackMax) {
            errorMsgNode((INode*)blk, ErrorBadArray, "Overflowing fixed-size block stack.");
            errorExit(100, "Unrecoverable error!");
        }
        blkstate = &gen->blockstack[gen->blockstackcnt];
        blkstate->blocknode = blk;
        blkstate->blockbeg = blockbeg;
        blkstate->blockend = blockend;
        if (blk->vtype->tag != VoidTag) {
            blkstate->phis = (LLVMValueRef*)memAllocBlk(sizeof(LLVMValueRef) * blk->breaks->used);
            blkstate->blocksFrom = (LLVMBasicBlockRef*)memAllocBlk(sizeof(LLVMBasicBlockRef) * blk->breaks->used);
            blkstate->phiCnt = 0;
        }
        ++gen->blockstackcnt;
    }

    INode **nodesp;
    uint32_t cnt;
    LLVMValueRef lastval = NULL; // Should never be used by caller
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        switch ((*nodesp)->tag) {
        case ContinueTag:
            genlDealiasNodes(gen, ((BreakRetNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, genFindBlockState(gen, ((BreakRetNode*)*nodesp)->block)->blockbeg); 
            break;

        case BreakTag: {
            BreakRetNode *brknode = (BreakRetNode*)*nodesp;
            genlBreak(gen, brknode->block, brknode->exp, brknode->dealias);
            break;
        }

        case ReturnTag:
            genlReturn(gen, (BreakRetNode*)*nodesp); break;
        case BlockRetTag:
        {
            BreakRetNode *node = (BreakRetNode*)*nodesp;
            if (node->exp->tag != NilLitTag)
                lastval = genlExpr(gen, node->exp);
            genlDealiasNodes(gen, node->dealias);
            break;
        }
        default:
            lastval = genlExpr(gen, *nodesp);
        }
    }

    if (isLoop)
        LLVMBuildBr(gen->builder, blockbeg);

    if (isPhiBlk) {
        LLVMPositionBuilderAtEnd(gen->builder, blockend);

        --gen->blockstackcnt;
        if (blk->vtype->tag != UnknownTag) {
            LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, blk->vtype), "phival");
            LLVMAddIncoming(phi, blkstate->phis, blkstate->blocksFrom, blkstate->phiCnt);
            return phi;
        }
        return NULL;

    }
    return lastval;
}
