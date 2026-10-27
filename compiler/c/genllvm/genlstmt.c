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

// A jump the program wrote has just emitted its terminator. Whatever is
// generated after it -- the rest of an expression the jump was inside, a
// branch's jump to its 'if''s end, a loop's jump back to its start, a block's
// releases -- would land after that terminator in the same basic block, where
// LLVM takes the last one as the block's terminator and the jump is lost. So
// generation goes on in a fresh block no path reaches, where it is harmless,
// and the constructs that would only add dead code there ask genlPathEnded and
// stop. genlSettleUnreached settles each such block once the module is made.
void genlEndPath(GenState *gen) {
    // Unnamed: a name would take a number from the function's own count of
    // the names it has made unique, renaming every value named after it
    // although the block is deleted
    LLVMBasicBlockRef blk = genlInsertBlock(gen, "");
    LLVMPositionBuilderAtEnd(gen->builder, blk);
    gen->unreached = blk;
    if (gen->unreachedcnt == gen->unreachedmax) {
        uint32_t newmax = gen->unreachedmax ? gen->unreachedmax * 2 : 64;
        LLVMBasicBlockRef *blks = (LLVMBasicBlockRef *)memAllocBlk(newmax * sizeof(LLVMBasicBlockRef));
        if (gen->unreachedcnt)
            memcpy(blks, gen->unreachedblks, gen->unreachedcnt * sizeof(LLVMBasicBlockRef));
        gen->unreachedblks = blks;
        gen->unreachedmax = newmax;
    }
    gen->unreachedblks[gen->unreachedcnt++] = blk;
}

// The builder is at the block genlEndPath made last, so no path reaches what is
// generated now. Generation that moves on to a block a path reaches -- an
// 'if''s end, a loop's end -- is on a live path again.
int genlPathEnded(GenState *gen) {
    return gen->unreached != NULL && LLVMGetInsertBlock(gen->builder) == gen->unreached;
}

// No path reaches a block genlEndPath made, nor anything branches to it, so
// one nothing was emitted into is deleted, and one that dead code was emitted
// into but that no terminator ends is ended by 'unreachable'
void genlSettleUnreached(GenState *gen) {
    for (uint32_t i = 0; i < gen->unreachedcnt; ++i) {
        LLVMBasicBlockRef blk = gen->unreachedblks[i];
        LLVMValueRef last = LLVMGetLastInstruction(blk);
        if (last == NULL && LLVMGetFirstUse(LLVMBasicBlockAsValue(blk)) == NULL)
            LLVMDeleteBasicBlock(blk);
        else if (last == NULL || !LLVMIsATerminatorInst(last)) {
            LLVMPositionBuilderAtEnd(gen->builder, blk);
            LLVMBuildUnreachable(gen->builder);
        }
    }
    gen->unreachedcnt = 0;
    gen->unreached = NULL;
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
    if (!genlPathEnded(gen)) {
        LLVMBuildUnreachable(gen->builder);
        genlEndPath(gen);
    }
    return 1;
}

// Generate a block/loop break
void genlBreak(GenState *gen, BlockNode* block, INode* exp, Nodes* dealias) {
    if (genlNeverJump(gen, exp))
        return;
    GenBlockState *blockstate = genFindBlockState(gen, block);
    LLVMValueRef brkval = NULL;
    // Generate the value for its effects either way. A value that itself
    // jumped away ('break {return 7;}') leaves the jump nothing to do.
    if (exp->tag != NilLitTag)
        brkval = genlExpr(gen, exp);
    if (genlPathEnded(gen))
        return;
    // The temporaries made since the block began, then its scopes' locals
    genlTempsJump(gen, blockstate->tempmark);
    genlDealiasNodes(gen, dealias);
    // Record the value only where the block converges on a value and so has
    // phi arrays to record it in, from the block the jump leaves, which the
    // releases may have moved on from
    if (brkval && blockstate->phis) {
        blockstate->phis[blockstate->phiCnt] = brkval;
        blockstate->blocksFrom[blockstate->phiCnt++] = LLVMGetInsertBlock(gen->builder);
        if (blockstate->slot)
            LLVMBuildStore(gen->builder, brkval, blockstate->slot);
    }
    LLVMBuildBr(gen->builder, blockstate->blockend);
    genlEndPath(gen);
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
    if (genlPathEnded(gen))
        return;
    genlTempsJump(gen, gen->tempbase);
    genlDealiasNodes(gen, retnode->dealias);
    if (gen->exitzero)
        retval = LLVMConstInt(LLVMInt32TypeInContext(gen->context), 0, 0);
    // A generator's body ends here: it hands back None from now on
    genlGenReturn(gen);
    genlFnDclReturn(gen, gen->fndcl, retval);
    genlEndPath(gen);
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
            blkstate->slot = genlMergeSlot(gen, blk->vtype);
        }
        else {
            blkstate->phis = NULL;
            blkstate->blocksFrom = NULL;
            blkstate->slot = NULL;
        }
        blkstate->phiCnt = 0;
        blkstate->tempmark = gen->tempcnt;
        ++gen->blockstackcnt;
    }

    INode **nodesp;
    uint32_t cnt;
    LLVMValueRef lastval = NULL; // Should never be used by caller
    // An operator's rewrite keeps every statement's temporaries to its end
    // (FlagKeepTemps)
    int keeptemps = blk->flags & FlagKeepTemps;
    uint32_t blocktempmark = gen->tempcnt;
    // A parallel each is not generated statement by statement: from the index
    // its pieces count with to its loop they are a function of their own, which
    // the actors' workers run (genlParallelRun); the loop is the last of them
    INode *outlined = NULL;     // That loop, once it has been generated
    int skipping = 0;           // The statements from the index to the loop are generated in the function
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        if (skipping) {
            // (the loop, the block's last statement, may be its value: the rest
            // of that statement, the block's end, is generated here)
            if (*nodesp == outlined) {
                skipping = 0;
                continue;
            }
            if ((*nodesp)->tag != BlockRetTag || ((BreakRetNode*)*nodesp)->exp != outlined)
                continue;
            skipping = 0;
        }
        else if ((blk->flags & FlagParallel) && (*nodesp)->tag == VarDclTag && ((VarDclNode*)*nodesp)->namesym == parKName) {
            outlined = genlParallelRun(gen, blk, *nodesp);
            skipping = 1;
            continue;
        }
        // The temporaries a statement makes die at its end, newest first, after
        // its value and before the locals a scope's end releases. A jump
        // finalizes them before it leaves (genlBreak, genlReturn), and nothing
        // follows it to finalize them again.
        uint32_t tempmark = keeptemps ? blocktempmark : gen->tempcnt;
        switch ((*nodesp)->tag) {
        case ContinueTag: {
            GenBlockState *target = genFindBlockState(gen, ((BreakRetNode*)*nodesp)->block);
            genlTempsJump(gen, target->tempmark);
            genlDealiasNodes(gen, ((BreakRetNode*)*nodesp)->dealias);
            LLVMBuildBr(gen->builder, target->blockbeg);
            genlEndPath(gen);
            break;
        }

        case BreakTag: {
            BreakRetNode *brknode = (BreakRetNode*)*nodesp;
            genlBreak(gen, brknode->block, brknode->exp, brknode->dealias);
            break;
        }

        case ReturnTag: {
            BreakRetNode *node = (BreakRetNode*)*nodesp;
            // Handle inlined returns as breaks
            if ((INode*)node->block != gen->fnblock) {
                if (node->block->breaks->used > 1) {
                    // Add to phi
                    genlBreak(gen, node->block, node->exp, node->dealias);
                }
                else {
                    // Just one?  Handle return like BlockRet
                    if (node->exp->tag != NilLitTag)
                        lastval = genlExpr(gen, node->exp);
                    if (!genlPathEnded(gen)) {
                        genlTempsEnd(gen, tempmark);
                        genlDealiasNodes(gen, node->dealias);
                    }
                }
                break;
            }

            genlReturn(gen, (BreakRetNode*)*nodesp);
            break;
        }
        case BlockRetTag:
        {
            // A value that jumped away -- a block ending in a 'return' -- has
            // released, as it left, what this block's end would
            BreakRetNode *node = (BreakRetNode*)*nodesp;
            if (node->exp->tag != NilLitTag && node->exp != outlined)
                lastval = genlExpr(gen, node->exp);
            if (!genlPathEnded(gen)) {
                genlTempsEnd(gen, tempmark);
                genlDealiasNodes(gen, node->dealias);
            }
            break;
        }
        default:
            lastval = genlExpr(gen, *nodesp);
        }
        // A jump -- the statement itself, or one inside it however deeply,
        // such as the 'return' ending a plain block in an 'if''s branch -- has
        // ended the path. It finalized the temporaries as it left, and nothing
        // after it in this list is reached: generated, it would only be dead
        // code (genlEndPath). Something does follow a jump in an 'each' loop
        // block, whose synthesized step sits behind the jump the reader wrote last.
        if (genlPathEnded(gen)) {
            gen->tempcnt = tempmark;
            break;
        }
        if (!keeptemps || cnt == 1)
            genlTempsEnd(gen, tempmark);
    }

    // Whether the block's path ended is what the LLVM block the builder is at
    // says, not which statement came last: a jump inside a statement ends it
    // as surely as one that is the statement
    int ended = genlPathEnded(gen);
    if (isLoop && !ended)
        LLVMBuildBr(gen->builder, blockbeg);

    if (isPhiBlk) {
        LLVMPositionBuilderAtEnd(gen->builder, blockend);

        --gen->blockstackcnt;
        if (blkstate->phis && blkstate->slot)
            return LLVMBuildLoad2(gen->builder, genlType(gen, blk->vtype), blkstate->slot, "mergeval");
        if (blkstate->phis) {
            LLVMValueRef phi = LLVMBuildPhi(gen->builder, genlType(gen, blk->vtype), "phival");
            LLVMAddIncoming(phi, blkstate->phis, blkstate->blocksFrom, blkstate->phiCnt);
            return phi;
        }
        return LLVMGetUndef(gen->emptyStructType);

    }
    // A block whose path ended has no value, but what it is part of may go on
    // being generated, where no path reaches, and be handed one
    if (ended && lastval == NULL)
        return blk->vtype->tag != VoidTag && blk->vtype->tag != UnknownTag
            ? LLVMGetUndef(genlType(gen, blk->vtype)) : LLVMGetUndef(gen->emptyStructType);
    return lastval;
}
