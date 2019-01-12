/** Handling for block nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new block node
BlockNode *newBlockNode() {
    BlockNode *blk;
    newNode(blk, BlockNode, BlockTag);
    blk->vtype = voidType;
    blk->stmts = newNodes(8);
    return blk;
}

// Serialize a block node
void blockPrint(BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;

    if (blk->stmts) {
        inodePrintIncr();
        for (nodesFor(blk->stmts, cnt, nodesp)) {
            inodePrintIndent();
            inodePrintNode(*nodesp);
            inodePrintNL();
        }
        inodePrintDecr();
    }
}

// Handle name resolution and control structure compliance for a block
// - push and pop a namespace context for hooking local vars in global name table
// - Ensure return/continue/break only appear as last statement in block
void blockResolvePass(PassState *pstate, BlockNode *blk) {
    int16_t oldscope = pstate->scope;
    blk->scope = ++pstate->scope; // Increment scope counter

    nametblHookPush(); // Ensure block's local variable declarations are hooked

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // Ensure 'return', 'break', 'continue' only appear as last statement in a block
        if (cnt > 1) {
            switch ((*nodesp)->tag) {
            case ReturnTag:
                errorMsgNode(*nodesp, ErrorRetNotLast, "return may only appear as the last statement in a block"); break;
            case BreakTag:
                errorMsgNode(*nodesp, ErrorRetNotLast, "break may only appear as the last statement in a block"); break;
            case ContinueTag:
                errorMsgNode(*nodesp, ErrorRetNotLast, "continue may only appear as the last statement in a block"); break;
            }
        }
        inodeWalk(pstate, nodesp);
    }

    nametblHookPop();  // Unhook local variables from global name table
    pstate->scope = oldscope;
}

// Handle type-checking for a block. 
// Mostly this is a pass-through to type-check the block's statements.
// Note: When coerced by iexpCoerces, vtype of the block will be specified
void blockTypePass(PassState *pstate, BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        inodeWalk(pstate, nodesp);
    }
}

// Check the block node
void blockPass(PassState *pstate, BlockNode *blk) {
    switch (pstate->pass) {
    case NameResolution:
        blockResolvePass(pstate, blk); break;
    case TypeCheck:
        blockTypePass(pstate, blk); break;
    }
}

// Perform data flow analysis on a block
void blockFlow(FlowState *fstate, BlockNode **blknode) {
    BlockNode *blk = *blknode;
    size_t svpos = flowScopePush();

    // If this is function's main block, include parameters in flow analysis
    if (++fstate->scope == 2) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(fstate->fnsig->parms, cnt, nodesp))
            flowAddVar((VarDclNode*)*nodesp);
    }

    // Ensure last node is return, blockret, break or continue
    // Inject blockret, if not present
    INode **lastnodep = &nodesLast(blk->stmts);
    switch ((*lastnodep)->tag) {
    case ReturnTag:
    case BreakTag:
    case ContinueTag:
        break;
    default:
    {
        // Inject blockret node
        ReturnNode *blkret = newReturnNode();
        blkret->tag = BlockRetTag;
        if (isExpNode(*lastnodep)) {
            blkret->exp = *lastnodep;
            *lastnodep = (INode*)blkret;
        }
        else {
            blkret->exp = voidType;
            nodesAdd(&blk->stmts, (INode*)blkret);
        }
    }
    }

    // Except for last node, handle all other nodes as if they throw away returned value
    size_t svAliasPos = flowAliasPushNew(0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // Handle last node differently, below
        if (cnt <= 1)
            break;
        switch ((*nodesp)->tag) {
        case VarDclTag:
            varDclFlow(fstate, (VarDclNode**)nodesp);
            break;
        case WhileTag:
            whileFlow(fstate, (WhileNode **)nodesp);
            break;
        default:
            // An expression as statement throws out its value
            if (isExpNode(*nodesp))
                flowLoadValue(fstate, nodesp);
        }
        flowAliasReset();
    }
    flowAliasPop(svAliasPos);

    // Capture any scope-ending dealiasing in block's last node
    // That last node must now be a return, break, continue or an injected "block return"
    switch ((*nodesp)->tag) {
    case ReturnTag:
    {
        INode **retexp = &((ReturnNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(0, &((ReturnNode *)*nodesp)->dealias, *retexp);
        if (*retexp != voidType && doalias) {
            size_t svAliasPos = flowAliasPushNew(1);
            flowLoadValue(fstate, retexp);
            flowAliasPop(svAliasPos);
        }
        break;
    }
    case BlockRetTag:
    {
        INode **retexp = &((ReturnNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(svpos, &((ReturnNode *)*nodesp)->dealias, *retexp);
        if (*retexp != voidType && doalias)
            flowLoadValue(fstate, retexp);
        break;
    }
    case BreakTag:
    case ContinueTag:
        flowScopeDealias(svpos, &((BreakNode *)*nodesp)->dealias, voidType);
        break;
    }

    --fstate->scope;
    flowScopePop(svpos);
}