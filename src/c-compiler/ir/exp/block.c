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
    blk->vtype = unknownType;
    blk->stmts = newNodes(8);
    return blk;
}

// Clone block
INode *cloneBlockNode(CloneState *cstate, BlockNode *node) {
    uint32_t dclpos = cloneDclPush();
    BlockNode *newnode;
    newnode = memAllocBlk(sizeof(BlockNode));
    memcpy(newnode, node, sizeof(BlockNode));
    newnode->stmts = cloneNodes(cstate, node->stmts);
    newnode->scope = ++cstate->scope;
    cloneDclPop(dclpos);
    return (INode *)newnode;
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
void blockNameRes(NameResState *pstate, BlockNode *blk) {
    uint16_t oldscope = pstate->scope;
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
        inodeNameRes(pstate, nodesp);
    }

    nametblHookPop();  // Unhook local variables from global name table
    pstate->scope = oldscope;
}

// Handle type-checking for a block. 
// Mostly this is a pass-through to type-check the block's statements.
// Note: By default, we set the block's type to that of the last statement
// Bidirectional type inference may later change this
void blockTypeCheck(TypeCheckState *pstate, BlockNode *blk, INode *expectType) {
    pstate->scope = blk->scope;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, nodesp);
        if (cnt > 1 && ((*nodesp)->tag == BreakTag || (*nodesp)->tag == ContinueTag))
            errorMsgNode(*nodesp, ErrorBadStmt, "break/continue may only be the last statement in the block");
        if ((*nodesp)->tag == BlockTag)
            blockNoBreak((BlockNode*)*nodesp);
    }
    if (blk->stmts->used > 0) {
        IExpNode *laststmt = (IExpNode *)nodesLast(blk->stmts);
        if (isExpNode(laststmt))
            blk->vtype = laststmt->vtype;
    }
    --pstate->scope;
}

// Ensure this particular block does not end with break/continue
// Used by regular and loop blocks, but not by 'if' based blocks
void blockNoBreak(BlockNode *blk) {
    if (blk->stmts->used > 0) {
        INode *laststmt = nodesLast(blk->stmts);
        if (laststmt->tag == BreakTag || laststmt->tag == ContinueTag)
            errorMsgNode(laststmt, ErrorBadStmt, "break/continue may only finish a conditional block");
    }
}

// Bidirectional type inference
void blockBiTypeInfer(INode **totypep, BlockNode *blk) {
    // Value of block is value of last statement
    // Ensure its type matches expected type for block
    INode **nodesp = &nodesLast(blk->stmts);
    if (!iexpTypeExpect(totypep, nodesp))
        errorMsgNode(*nodesp, ErrorInvType, "expression type does not match expected type");
    blk->vtype = *totypep;
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
            blkret->exp = noValue;
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
        case LoopTag:
            loopFlow(fstate, (LoopNode **)nodesp);
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
        if (*retexp != unknownType && doalias) {
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
        if (*retexp != noValue && doalias)
            flowLoadValue(fstate, retexp);
        break;
    }
    case BreakTag: {
        INode **brkexp = &((BreakNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(svpos, &((ReturnNode *)*nodesp)->dealias, *brkexp);
        if (*brkexp != noValue && doalias)
            flowLoadValue(fstate, brkexp);
        break;
    }
    case ContinueTag:
        flowScopeDealias(svpos, &((ContinueNode *)*nodesp)->dealias, noValue);
        break;
    }

    --fstate->scope;
    flowScopePop(svpos);
}