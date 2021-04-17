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
    blk->life = NULL;
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
    newnode->stmts = cloneNodes(cstate, node->stmts);
    newnode->life = (LifetimeNode*)cloneNode(cstate, (INode*)node->life);
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
    ++pstate->scope; // Increment block scope counter
    nametblHookPush(); // Ensure block's local variable declarations are hooked

    // If block declares a lifetime declaration, hook into name table for name res
    LifetimeNode *lifenode = blk->life;
    if (lifenode) {
        if (!lifenode->namesym->node) {
            lifenode->life = pstate->scope;
            // Add name to global name table for life of block
            nametblHookNode(lifenode->namesym, (INode*)lifenode);
        }
        else {
                errorMsgNode((INode *)lifenode, ErrorDupName, "Lifetime is already defined. Only one allowed.");
                errorMsgNode((INode*)lifenode->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
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
}

// Handle type-checking for a block. This:
// 1. Type checks the block's statements
// 2. Verify block ends with valid block-ending statement
// 3. Performs bidirectional inference to ensure block (and any breaks) return same-typed value
//    All breaks must resolve to either the expected type or the same inferred supertype
//    Coercion is performed on breaks as needed to accomplish this, or errors result
void blockTypeCheck(TypeCheckState *pstate, BlockNode *blk, INode *expectType) {
    if (blk->stmts->used == 0)
        return;
    ++pstate->scope;
    BlockNode *svRecentLoop = pstate->recentLoop;
    if (blk->flags & FlagLoop)
        pstate->recentLoop = blk;

    // Push block node on block stack so we can later gather all breaks that might belong to it
    if (pstate->blockcnt >= TypeCheckBlockMax) {
        errorMsgNode((INode*)blk, ErrorBadArray, "Overflowing fixed-size block stack.");
        errorExit(100, "Unrecoverable error!");
    }
    pstate->blockstack[pstate->blockcnt++] = blk;

    // Type check the block's statements.
    // Do special checks of block-ending statements
    // Note: blk->breaks will get populated here
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
            // Last statement might need to deliver a value of some expected type
            iexpTypeCheckCoerce(pstate, expectType, nodesp);

            // Capture last statement's type
            if (isExpNode(*nodesp))
                blk->vtype = ((IExpNode *)*nodesp)->vtype;
        }
    }
    --pstate->scope;
    --pstate->blockcnt;
    pstate->recentLoop = svRecentLoop;

    if ((blk->flags & FlagLoop) && blk->breaks->used == 0)
        errorMsgNode((INode*)blk, WarnLoop, "Loop may never stop without a break.");

    /*
    // Do inference on all registered breaks to ensure they all return the expected type
    INode *inferredType = unknownType;
    TypeCompare match = EqMatch;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->breaks, cnt, nodesp)) {
        INode **breakexp = &((BreakRetNode *)*nodesp)->exp;
        if ((*breakexp)->tag == NilLitTag && expectType != noCareType) {
            errorMsgNode(*nodesp, ErrorInvType, "Loop/block expects a typed expression on break");
            match = NoMatch;
        }
        else if (!iexpTypeCheckCoerce(pstate, expectType, breakexp)) {
            errorMsgNode(*breakexp, ErrorInvType, "Expression does not match expected type.");
            match = NoMatch;
        }
        else if (!isExpNode(*breakexp)) {
            match = NoMatch;
        }
        else {
            switch (iexpMultiInfer(expectType, &inferredType, breakexp)) {
            case NoMatch:
                match = NoMatch;
                break;
            case CastSubtype:
                if (match != NoMatch)
                    match = CastSubtype;
                break;
            case ConvSubtype:
                if (match != NoMatch)
                    match = ConvSubtype;
                break;
            default:
                break;
            }
        }
    }

    if (expectType == noCareType)
        return;

    // When expectType specified, all branches have been coerced (or not w/ errors)
    if (expectType != unknownType) {
        blk->vtype = expectType;
        return;
    }

    // If no specific type is expected, set the inferred type
    blk->vtype = inferredType;

    //  If coercion is needed for some blocks, perform them as needed
    if (match == ConvSubtype || match == CastSubtype) {
        for (nodesFor(blk->breaks, cnt, nodesp)) {
            INode **breakexp = &((BreakRetNode *)*nodesp)->exp;
            iexpCoerce(breakexp, inferredType);
        }
    }
    */
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
        INode **retexp = &((BreakRetNode *)*nodesp)->exp;
        int doalias = flowScopeDealias(0, &((BreakRetNode *)*nodesp)->dealias, *retexp);
        if (*retexp != unknownType && doalias) {
            size_t svAliasPos = flowAliasPushNew(1);
            flowLoadValue(fstate, retexp);
            flowAliasPop(svAliasPos);
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
