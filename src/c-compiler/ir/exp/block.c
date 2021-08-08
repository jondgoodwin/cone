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
    blk->vtype = unknownType;  // This will be overridden with loop-as-expr
    blk->stmts = newNodes(8);
    blk->lifesym = NULL;
    blk->breaks = NULL;
    return blk;
}

// Create a new loop block node
BlockNode *newLoopBlockNode() {
    BlockNode *blk = newBlockNode();
    blk->flags |= FlagLoop;
    blk->breaks = newNodes(2);
    return blk;
}

// Clone block
INode *cloneBlockNode(CloneState *cstate, BlockNode *node) {
    uint32_t dclpos = cloneDclPush();
    BlockNode *newnode;
    newnode = memAllocBlk(sizeof(BlockNode));
    memcpy(newnode, node, sizeof(BlockNode));
    cloneDclSetMap((INode*)node, (INode*)newnode);  // For fixing cloned break/continue/return nodes
    newnode->stmts = cloneNodes(cstate, node->stmts);
    if (node->breaks)
        newnode->breaks = cloneNodes(cstate, node->breaks);  // Does not clone any INodes. Bad if it did.
    cloneDclPop(dclpos);
    return (INode *)newnode;
}

// Serialize a block node
void blockPrint(BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;

    if (blk->flags & FlagLoop) {
        inodeFprint("loop");
        inodePrintNL();
    }
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
// - push and pop a namespace context for hooking local vars & lifetime in global name table
// - Ensure return/continue/break only appear as last statement in block
void blockNameRes(NameResState *pstate, BlockNode *blk) {
    // Set up for break and continue nodes that do not specify a labeled block
    // By default we want to resolve them to inner-most loop block
    BlockNode *svloopblock = pstate->loopblock;
    if (blk->flags & FlagLoop) {
        pstate->loopblock = blk;
    }

    ++pstate->scope; // Increment block scope counter
    nametblHookPush(); // Ensure block's local variable declarations are hooked

    // If block declares a lifetime declaration, hook into name table for name res
    Name *lifesym = blk->lifesym;
    if (lifesym) {
        // If not already declared, hook lifetime symbol to block in global name table
        if (!lifesym->node) {
            nametblHookNode(lifesym, (INode*)blk);
        }
        else {
                errorMsgNode((INode *)blk, ErrorDupName, "Lifetime is already defined. Only one allowed.");
                errorMsgNode((INode*)lifesym->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
    }

    // Name res each statement
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
    --pstate->scope;
    pstate->loopblock = svloopblock;
}

// Handle type-checking for a regular or loop block. This:
// 1. Type checks the block's statements
// 2. Verify block ends with valid block-ending statement
// 3. Performs bidirectional inference to ensure block (and any breaks) return same-typed value
//    All breaks must resolve to either the expected type or the same inferred supertype
//    Coercion is performed on breaks as needed to accomplish this, or errors result
void blockTypeCheck(TypeCheckState *pstate, BlockNode *blk, INode *expectType) {
    INode *inferredType = unknownType;
    TypeCompare match = EqMatch;
    INode **laststmtp = NULL;
    INode **lastexp = NULL;
        
    // Save and adjust pstate for block
    // This includes block stack, used for gathering all breaks that might belong to some block
    ++pstate->scope;

    // Type check all of a block's statements, treating final statement differently
    // This will populate block->breaks with pointers to all break statements for this block
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // Ensure any subblock within this block does not end with break or continue
        // as it makes no sense
        if ((*nodesp)->tag == BlockTag)
            blockNoBreak((BlockNode*)*nodesp);

        // Handle statement differently depending on whether it is last one
        if (cnt > 1) {
            // All stmt nodes except the last one
            inodeTypeCheck(pstate, nodesp, noCareType); // we don't care about the type
            if (((*nodesp)->tag == BreakTag || (*nodesp)->tag == ContinueTag))
                errorMsgNode(*nodesp, ErrorBadStmt, "break/continue may only be the last statement in the block");
        }
        else {
            laststmtp = nodesp;
            // last statement is special-handled below
        }
    }

    // Handle final statement differently for loop block vs. regular block
    if (blk->flags & FlagLoop) {
        // Warn if the loop block has no breaks, as loop may never stop
        if ((blk->flags & FlagLoop) && blk->breaks->used == 0)
            errorMsgNode((INode*)blk, WarnLoop, "Loop may never stop without a break.");

        // Last statement should not be break, continue, return
        if (laststmtp && ((*laststmtp)->tag == BreakTag || (*laststmtp)->tag == ContinueTag || (*laststmtp)->tag == ReturnTag))
            errorMsgNode((INode*)*laststmtp, ErrorBadStmt, "Don't end loop block with break, continue or return");

        inodeTypeCheck(pstate, laststmtp, noCareType); // we don't care about the type
    }
    else {
        // Last statement of a regular block needs to be a break, continue, return or blockret
        if (laststmtp && isExpNode(*laststmtp)) {
            // Wrap the final expression in a blockret statement (flow pass will need it)
            BreakRetNode *retnode = newReturnNode();
            retnode->tag = BlockRetTag;
            retnode->exp = *laststmtp;
            lastexp = &retnode->exp;
            match = iexpMultiCoerceInfer(pstate, expectType, &inferredType, lastexp, match);
        }
        else if (laststmtp == NULL ||
            !((*laststmtp)->tag == BreakTag || (*laststmtp)->tag == ContinueTag || (*laststmtp)->tag == ReturnTag)) {
            inodeTypeCheck(pstate, laststmtp, noCareType); // we don't care about the type
            // Add 'blockret nil' to end of empty block, or block ending without expression/break/cont/return
            BreakRetNode *retnode = newReturnNode();
            retnode->tag = BlockRetTag;
            retnode->exp = (INode*)newNilLitNode();
            nodesAdd(&blk->stmts, (INode*)retnode);
            match = iexpMultiCoerceInfer(pstate, expectType, &inferredType, &retnode->exp, match);
        }
        else
            inodeTypeCheck(pstate, laststmtp, noCareType); // we don't care about the type
    }

    // Do inference on all registered breaks to ensure they all return the expected type
    // Note: Iterate differently because list may grow while iterating
    if (blk->breaks && blk != (BlockNode*)pstate->fn->value) {
        nodesp = (INode**)((blk->breaks) + 1);
        cnt = 0;
        for (; cnt < blk->breaks->used; ++cnt, ++nodesp) {
            INode **breakexp = &((BreakRetNode *)*nodesp)->exp;
            match = iexpMultiCoerceInfer(pstate, expectType, &inferredType, breakexp, match);
        }
    }

    if (expectType == noCareType) {
        blk->vtype = inferredType;
        return;
    }

    // When expectType specified, all branches have been coerced (or not w/ errors)
    if (expectType != unknownType) {
        blk->vtype = expectType;
        return;
    }

    // If no specific type is expected, set the inferred type
    blk->vtype = inferredType;

    // If we have inferred a supertype, we need to re-coerce all expressions
    if (match == ConvSubtype || match == CastSubtype) {
        for (nodesFor(blk->breaks, cnt, nodesp)) {
            INode **breakexp = &((BreakRetNode *)*nodesp)->exp;
            iexpCoerce(breakexp, inferredType);
        }
        if (lastexp)
            iexpCoerce(lastexp, inferredType);
    }

    // Restore pstate to prior condition
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
        BreakRetNode *blkret = newReturnNode();
        blkret->tag = BlockRetTag;
        if (isExpNode(*lastnodep)) {
            blkret->exp = *lastnodep;
            *lastnodep = (INode*)blkret;
        }
        else {
            blkret->exp = (INode*)newNilLitNode();
            nodesAdd(&blk->stmts, (INode*)blkret);
        }
    }
    }

    // Except for last node, handle all other nodes as if they throw away returned value
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
        case SwapTag:
            swapFlow(fstate, (SwapNode **)nodesp); 
            break;
        default:
            // An expression as statement throws out its value
            if (isExpNode(*nodesp))
                flowLoadValue(fstate, nodesp);
        }
    }

    // Capture any scope-ending dealiasing in block's last node
    // That last node must now be a return, break, continue or an injected "block return"
    switch ((*nodesp)->tag) {
    case ReturnTag:
    {
        INode **retexp = &((BreakRetNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(0, &((BreakRetNode *)*nodesp)->dealias, *retexp);
        if (*retexp != unknownType && doalias) {
            flowLoadValue(fstate, retexp);
        }
        break;
    }
    case BlockRetTag:
    {
        INode **retexp = &((BreakRetNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(svpos, &((BreakRetNode *)*nodesp)->dealias, *retexp);
        if ((*retexp)->tag != NilLitTag && doalias)
            flowLoadValue(fstate, retexp);
        break;
    }
    case BreakTag: {
        INode **brkexp = &((BreakRetNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(svpos, &((BreakRetNode *)*nodesp)->dealias, *brkexp);
        if ((*brkexp)->tag != NilLitTag && doalias)
            flowLoadValue(fstate, brkexp);
        break;
    }
    case ContinueTag:
        flowScopeDealias(svpos, &((BreakRetNode *)*nodesp)->dealias, NULL);
        break;
    }

    --fstate->scope;
    flowScopePop(svpos);
}
