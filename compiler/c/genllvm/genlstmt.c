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

#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

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

// A 'return' or break whose value is a call that does not return
// (blockTypeCheck makes one of a block ending in such a call): the call, and
// the path ends there. Nothing is released and nothing is handed back, since
// control never comes back from the call; 'unreachable' tells LLVM so. So too
// an 'if' every path through which jumps away, which a function returning
// 'Never' may end in: its end is reached by no path, and has no value.
static int genlNeverJump(GenState *gen, INode *exp) {
    if (!fnCallIsNever(exp) && !(exp->tag == IfTag && ifAllPathsJump((IfNode *)exp)))
        return 0;
    genlExpr(gen, exp);
    LLVMBuildUnreachable(gen->builder);
    return 1;
}

// Generate a block/loop break
void genlBreak(GenState *gen, BlockNode* block, INode* exp, Nodes* dealias) {
    if (genlNeverJump(gen, exp))
        return;
    GenBlockState *blockstate = genFindBlockState(gen, block);
    LLVMValueRef brkval = NULL;
    // Generate the value for its effects either way
    if (exp->tag != NilLitTag)
        brkval = genlExpr(gen, exp);
    // The temporaries made since the block began, then its scopes' locals
    genlTempsJump(gen, blockstate->tempmark);
    genlDealiasNodes(gen, dealias);
    // Record the value only where the block converges on a value and so has
    // phi arrays to record it in, from the block the jump leaves, which the
    // releases may have moved on from
    if (brkval && blockstate->phis) {
        blockstate->phis[blockstate->phiCnt] = brkval;
        blockstate->blocksFrom[blockstate->phiCnt++] = LLVMGetInsertBlock(gen->builder);
    }
    LLVMBuildBr(gen->builder, blockstate->blockend);
}

// Generate a return statement
void genlReturn(GenState *gen, BreakRetNode *retnode) {
    // Handle inlined returns as breaks
    if ((INode*)retnode->block != gen->fnblock) {
        if (retnode->block->breaks->used > 1)
            genlBreak(gen, retnode->block, retnode->exp, retnode->dealias);
        return;
    }

    if (genlNeverJump(gen, retnode->exp))
        return;
    LLVMValueRef retval = genlExpr(gen, retnode->exp);
    genlTempsJump(gen, gen->tempbase);
    genlDealiasNodes(gen, retnode->dealias);
    if (gen->exitzero)
        retval = LLVMConstInt(LLVMInt32TypeInContext(gen->context), 0, 0);
    genlFnDclReturn(gen, gen->fndcl, retval);
}

// Generate a block "return" retnode
void genlBlockRet(GenState *gen, BreakRetNode *node) {
}

// Generate a block's statements (could be a loop block)
LLVMValueRef genlBlock(GenState *gen, BlockNode *blk) {
    // Create separate blocks only when needed. 
    // isLoop requires blkbeg and blkend. isPhiBlk only blkend when phi values must converge for break/returns
    int isLoop = blk->flags & FlagLoop;
    int isPhiBlk = isLoop || (blk->breaks && blk->breaks->used > 1); 

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
            errorExit(ExitGen, "Unrecoverable error!");
        }
        blkstate = &gen->blockstack[gen->blockstackcnt];
        blkstate->blocknode = blk;
        blkstate->blockbeg = blockbeg;
        blkstate->blockend = blockend;
        // A phi is wanted only where the block converges on a value. Allocating the
        // arrays and building the phi must agree on that: they used to test
        // different conditions, so a void block built a phi over an uninitialized
        // count and emitted one with no incoming entries.
        if (blk->vtype->tag != VoidTag && blk->vtype->tag != UnknownTag) {
            blkstate->phis = (LLVMValueRef*)memAllocBlk(sizeof(LLVMValueRef) * blk->breaks->used);
            blkstate->blocksFrom = (LLVMBasicBlockRef*)memAllocBlk(sizeof(LLVMBasicBlockRef) * blk->breaks->used);
        }
        else {
            blkstate->phis = NULL;
            blkstate->blocksFrom = NULL;
        }
        blkstate->phiCnt = 0;
        blkstate->tempmark = gen->tempcnt;
        ++gen->blockstackcnt;
    }

    INode **nodesp;
    uint32_t cnt;
    LLVMValueRef lastval = NULL; // Should never be used by caller
    // A break or continue terminates the LLVM basic block, so whatever follows it in
    // the statement list is unreachable and must not be emitted -- an instruction
    // after a terminator is invalid IR. It is reachable in an 'each' loop block,
    // whose synthesized step sits behind the jump the reader wrote last.
    int terminated = 0;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // The temporaries a statement makes die at its end, newest first, after
        // its value and before the locals a scope's end releases. A jump
        // finalizes them before it leaves (genlBreak, genlReturn), and nothing
        // follows it to finalize them again.
        uint32_t tempmark = gen->tempcnt;
        int jumped = 0;
        switch ((*nodesp)->tag) {
        case ContinueTag: {
            GenBlockState *target = genFindBlockState(gen, ((BreakRetNode*)*nodesp)->block);
            genlTempsJump(gen, target->tempmark);
            genlDealiasNodes(gen, ((BreakRetNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, target->blockbeg);
            terminated = 1;
            break;
        }

        case BreakTag: {
            BreakRetNode *brknode = (BreakRetNode*)*nodesp;
            genlBreak(gen, brknode->block, brknode->exp, brknode->dealias);
            terminated = 1;
            break;
        }

        case ReturnTag: {
            BreakRetNode *node = (BreakRetNode*)*nodesp;
            // Handle inlined returns as breaks
            if ((INode*)node->block != gen->fnblock) {
                if (node->block->breaks->used > 1) {
                    // Add to phi
                    genlBreak(gen, node->block, node->exp, node->dealias);
                    jumped = 1;
                }
                else {
                    // Just one?  Handle return like BlockRet
                    if (node->exp->tag != NilLitTag)
                        lastval = genlExpr(gen, node->exp);
                    genlTempsEnd(gen, tempmark);
                    genlDealiasNodes(gen, node->dealias);
                }
                break;
            }

            genlReturn(gen, (BreakRetNode*)*nodesp);
            jumped = 1;
            break;
        }
        case BlockRetTag:
        {
            BreakRetNode *node = (BreakRetNode*)*nodesp;
            if (node->exp->tag != NilLitTag)
                lastval = genlExpr(gen, node->exp);
            genlTempsEnd(gen, tempmark);
            genlDealiasNodes(gen, node->dealias);
            break;
        }
        default:
            lastval = genlExpr(gen, *nodesp);
        }
        if (terminated || jumped)
            gen->tempcnt = tempmark;
        else
            genlTempsEnd(gen, tempmark);
        if (terminated)
            break;
    }

    if (isLoop && !terminated)
        LLVMBuildBr(gen->builder, blockbeg);

    if (isPhiBlk) {
        LLVMPositionBuilderAtEnd(gen->builder, blockend);

        --gen->blockstackcnt;
        if (blkstate->phis) {
            LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, blk->vtype), "phival");
            LLVMAddIncoming(phi, blkstate->phis, blkstate->blocksFrom, blkstate->phiCnt);
            return phi;
        }
        return LLVMGetUndef(gen->emptyStructType);

    }
    return lastval;
}
