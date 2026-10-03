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
    blk->flowmark = 0;
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

// Give a 'continue' the loop step it would otherwise jump over.
//
// 'each' lowers to a 'while' whose body ends with the step that advances the loop
// variable, so a 'continue' anywhere in the body would reach the loop's guard with
// the variable unchanged and the loop would never end. The repair is to copy the
// step in immediately ahead of the 'continue', into the block that already holds
// it: that cannot break the "break/continue must be last" rule, because the rule
// guarantees the 'continue' is already this block's last statement, and the copy
// becomes an ordinary non-final expression statement in front of it.
//
// A labelled 'continue' needs nothing more. It abandons every inner loop, whose
// variables the outer body re-initializes on the next iteration, so the only step
// that has to run is the step of the loop it names -- which is the block
// continueNameRes has already resolved it to.
//
// Called after the statement loop has resolved this block's statements, so every
// 'continue' in it knows its target, and before the local variables are unhooked,
// so the copied step can still resolve the loop variable.
static void blockContinueStep(NameResState *pstate, BlockNode *blk) {
    uint32_t trailing = (blk->flags & FlagLoopStep) ? 1 : 0;
    if (blk->stmts->used <= trailing)
        return;
    // The statement the reader wrote last, which a synthesized step sits behind
    uint32_t pos = blk->stmts->used - 1 - trailing;
    INode *stmt = nodesGet(blk->stmts, pos);
    if (stmt->tag != ContinueTag)
        return;
    BlockNode *target = ((BreakRetNode *)stmt)->block;
    if (target == NULL || !(target->flags & FlagLoopStep) || target->stmts->used == 0)
        return;

    // Clone rather than re-use. One node reachable twice in the tree is type
    // checked twice, and lowering is not idempotent. Cloning is sound on
    // evaluation count because only one of the two copies runs per iteration.
    INode *origstep = nodesLast(target->stmts);
    CloneState cstate = {0};     // Every field the clone reads, the ones not set below NULL
    cstate.instnode = origstep->instnode;
    cstate.selftype = NULL;
    cstate.scope = (uint16_t)pstate->scope;
    nodesInsert(&blk->stmts, cloneNode(&cstate, origstep), pos);
    inodeNameRes(pstate, &nodesGet(blk->stmts, pos));
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

    // An 'each' loop block ends with the synthesized step that advances the loop
    // variable, so a jump the reader wrote last sits one place further back and the
    // rule below, which counts from the end, has to count the step out. It applies
    // to 'break' and 'continue' only: those jump to a block boundary that the loop
    // owns, whereas a 'return' would leave the step behind as unreachable code that
    // generation would emit after a terminator.
    uint32_t lastpos = (blk->flags & FlagLoopStep) ? 2 : 1;

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
                if (cnt > lastpos)
                    errorMsgNode(*nodesp, ErrorRetNotLast, "break may only appear as the last statement in a block");
                break;
            case ContinueTag:
                if (cnt > lastpos)
                    errorMsgNode(*nodesp, ErrorRetNotLast, "continue may only appear as the last statement in a block");
                break;
            }
        }
        inodeNameRes(pstate, nodesp);
    }

    blockContinueStep(pstate, blk);

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
// Type check a block's statement, its value unwanted. One declaring a local
// may extend a temporary its initializer borrows into a hidden local
// (varDclExtendBegin); 'hoists' gathers, for each such statement, its hidden
// locals in order and then the statement, for blockHoist.
static void blockStmtTypeCheck(TypeCheckState *pstate, INode **stmtp, Nodes **hoists) {
    if ((*stmtp)->tag != VarDclTag) {
        inodeTypeCheck(pstate, stmtp, noCareType);
        return;
    }
    VarDclExtend ext;
    INode *stmt = *stmtp;
    varDclExtendBegin(pstate, &ext, (VarDclNode *)stmt);
    inodeTypeCheck(pstate, stmtp, noCareType);
    Nodes *hoisted = varDclExtendEnd(pstate, &ext);
    if (hoisted == NULL)
        return;
    if (*hoists == NULL)
        *hoists = newNodes(hoisted->used + 1);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(hoisted, cnt, nodesp))
        nodesAdd(hoists, *nodesp);
    nodesAdd(hoists, stmt);
}

// Declare each hidden local just before the statement whose initializer
// extends it. Done once nothing holds a pointer into the statement list.
static void blockHoist(BlockNode *blk, Nodes *hoists) {
    if (hoists == NULL)
        return;
    uint32_t from = 0;
    for (uint32_t at = 0; at < blk->stmts->used && from < hoists->used; ++at) {
        uint32_t to = from;
        while (nodesGet(hoists, to)->tag == VarDclTag
            && ((VarDclNode *)nodesGet(hoists, to))->namesym == tempLocalName)
            ++to;
        if (nodesGet(blk->stmts, at) != nodesGet(hoists, to))
            continue;
        for (uint32_t h = from; h < to; ++h)
            nodesInsert(&blk->stmts, nodesGet(hoists, h), at++);
        from = to + 1;
    }
}

void blockTypeCheck(TypeCheckState *pstate, BlockNode *blk, INode *expectType) {
    INode *inferredType = unknownType;
    TypeCompare match = EqMatch;
    INode **laststmtp = NULL;
    INode **lastexp = NULL;
    Nodes *hoists = NULL;

    // Save and adjust pstate for block
    // This includes block stack, used for gathering all breaks that might belong to some block
    ++pstate->scope;

    // A brand minted in a loop's body is its pass's (lifetime.h)
    if (blk->flags & FlagLoop)
        lifeBrandLoopEnter((INode*)blk);

    // An 'each' loop block ends with the synthesized step that advances the loop
    // variable, so a 'continue' the reader wrote last sits one place further back.
    uint32_t lastpos = (blk->flags & FlagLoopStep) ? 2 : 1;

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
            blockStmtTypeCheck(pstate, nodesp, &hoists);
            if (cnt > lastpos && ((*nodesp)->tag == BreakTag || (*nodesp)->tag == ContinueTag))
                errorMsgNode(*nodesp, ErrorBadStmt, "break/continue may only be the last statement in the block");
        }
        else {
            laststmtp = nodesp;
            // last statement is special-handled below
        }
    }

    // Handle final statement differently for loop block vs. regular block
    if (blk->flags & FlagLoop) {
        // Last statement should not be break, continue, return
        if (laststmtp && ((*laststmtp)->tag == BreakTag || (*laststmtp)->tag == ContinueTag || (*laststmtp)->tag == ReturnTag))
            errorMsgNode((INode*)*laststmtp, ErrorBadStmt, "Don't end loop block with break, continue or return");

        if (laststmtp)
            blockStmtTypeCheck(pstate, laststmtp, &hoists);

        // Warn if the loop block has no breaks, as loop may never stop
        if (blk->breaks->used == 0)
            errorMsgNode((INode*)blk, WarnLoop, "Loop may never stop without a break.");
    }
    else {
        // Last statement of a regular block needs to be a break, continue, return or blockret
        if (laststmtp && isExpOrMacroNode(*laststmtp)) {
            inodeTypeCheck(pstate, laststmtp, expectType);
            // A call that does not return ends the block as a 'return' would,
            // giving it no value to coerce, so it becomes one: returnTypeCheck
            // accepts it whatever the function returns, and every later pass
            // already knows a path ends at a 'return'. The call is checked
            // already, so the return is joined to its function, not checked.
            if (fnCallIsNever(*laststmtp)) {
                BreakRetNode *retnode = newReturnNodeExp(*laststmtp);
                *laststmtp = (INode*)retnode;
                returnJoinFn(pstate, retnode);
            }
            else {
                lastexp = laststmtp;
                match = iexpMultiCheckedCoerceInfer(expectType, &inferredType, lastexp, match);
            }
        }
        else if (laststmtp == NULL ||
            !((*laststmtp)->tag == BreakTag || (*laststmtp)->tag == ContinueTag || (*laststmtp)->tag == ReturnTag)) {
            // An empty block has no last statement to check, only the 'blockret
            // nil' added just below. '{}' is legal, and so is the outer block an
            // 'each' builds before it knows what it is iterating over.
            if (laststmtp)
                blockStmtTypeCheck(pstate, laststmtp, &hoists);
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

    if (blk->flags & FlagLoop)
        lifeBrandLoopExit();

    // Do inference on all registered breaks to ensure they all return the expected type
    // Note: Iterate differently because list may grow while iterating
    if (blk->breaks && blk != (BlockNode*)pstate->fn->value) {
        nodesp = (INode**)((blk->breaks) + 1);
        cnt = 0;
        for (; cnt < blk->breaks->used; ++cnt, ++nodesp) {
            INode **breakexp = &((BreakRetNode *)*nodesp)->exp;
            match = iexpMultiCoerceInfer(pstate, expectType, &inferredType, breakexp, match);
        }
        // What a break carries out of a loop's pass carries no brand that pass minted
        lifeBrandBreaks((INode*)blk, blk->breaks);
    }

    // The scope this block opened is closed on every path out of here, not only
    // the last one. It used to leak on the two returns below, so the counter
    // climbed for the rest of the compile. Nothing read it closely enough to
    // notice until a declaration could be analyzed from the middle of a body.
    if (expectType == noCareType) {
        blk->vtype = inferredType;
        blockHoist(blk, hoists);
        --pstate->scope;
        return;
    }

    // When expectType specified, all branches have been coerced (or not w/ errors)
    if (expectType != unknownType)
        blk->vtype = expectType;
    else {
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
    }

    // The block gives its last value or a break's, so a borrowed reference lives
    // only as long as the shortest-lived of them, not as the type expected of it
    uint16_t narrowest = lastexp ? iexpNarrowerScope(0, *lastexp) : 0;
    if (blk->breaks && blk != (BlockNode*)pstate->fn->value) {
        for (nodesFor(blk->breaks, cnt, nodesp))
            narrowest = iexpNarrowerScope(narrowest, ((BreakRetNode *)*nodesp)->exp);
    }
    blk->vtype = iexpNarrowestType(blk->vtype, (INode*)blk, narrowest);
    blockHoist(blk, hoists);

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
// Where the scope being left by a break or continue starts on the flow stack.
// The jump leaves every scope between here and the block it names, so the mark
// is that block's, not the current one's. 'svpos' is the fallback for a jump
// whose target failed to resolve.
static size_t blockJumpMark(BreakRetNode *brknode, size_t svpos) {
    return brknode->block ? brknode->block->flowmark : svpos;
}

// A value a function hands back is moved to the caller: one it reached through
// a borrow is refused (flowResultMove). A block or an 'if' hands back what its
// values do, and moves each as it would into a variable (flowHandleMove), so
// that a local it hands back on only some paths is released on the others by
// its drop flag, where exempting it on all of them would leak it. A local
// handed back directly is exempt from the release instead (flowScopeDealias).
static void blockResultMove(INode *result) {
    INode *exp = result;
    while (exp->tag == CastTag && flowCastCarries(exp))
        exp = ((CastNode *)exp)->exp;
    if ((exp->tag == IfTag || (exp->tag == BlockTag && !(exp->flags & FlagLoop))) && iexpIsMove(result))
        flowHandleMove(result);
    else
        flowResultMove(result);
}

// A copy of a counted value handed out of a scope -- a return's, a block's
// value, a break's -- is one more holder of what it counts, unless it is a
// variable of that scope handed over whole, which the scope's release exempts
// instead (flowScopeHandsBack). A part of a local the scope releases (a field,
// an element, what a local owner points at), a variable of an enclosing scope,
// a global, or a value reached through a borrow is read and still held where it
// was, so the copy is counted as a copy into a variable is (flowHandleMoveOrCopy).
// 'result' is the value as it stood before the walk, which is what the release
// exemption names.
static void blockResultCount(INode **retexp, INode *result, size_t startpos) {
    if (iexpIsMove(*retexp) || !flowIsLvalRead(*retexp) || flowScopeHandsBack(startpos, result))
        return;
    flowInjectRefCount(retexp);
}

// Does this block throw its final expression's value away? A loop's loops
// back, and a block with no value -- a statement's, a branch of an 'if' that
// is one -- hands nothing back: the value is a temporary there, and a local
// it names is no result, but dies with the scope. A function's own block,
// typed with no value whatever it returns, hands its value to the caller
// unless the function returns nothing.
static int blockDiscards(FlowState *fstate, BlockNode *blk) {
    if (blk->flags & FlagLoop)
        return 1;
    if (fstate->scope == 2)
        return itypeGetTypeDcl(fstate->fnsig->rettype)->tag == VoidTag;
    INode *vtype = blk->vtype;
    return vtype == NULL || vtype == unknownType || itypeGetTypeDcl(vtype)->tag == VoidTag;
}

// Once a statement is walked: if it made temporaries, keep each that what
// goes out of it ('out': its value is stored or handed back) may point into
static void blockTempEscape(uint32_t tempmark, INode *node, int out) {
    if (flowTempCount != tempmark)
        flowTempEscape(node, out);
}

void blockFlow(FlowState *fstate, BlockNode **blknode) {
    BlockNode *blk = *blknode;
    size_t svpos = flowScopePush();
    // Record where this block's scope starts, so a break or continue naming it
    // releases every scope between the jump and this block, not just its own.
    blk->flowmark = svpos;

    // If this is function's main block, include parameters in flow analysis
    if (++fstate->scope == 2) {
        flowFnBegin(fstate);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(fstate->fnsig->parms, cnt, nodesp))
            flowAddVar((VarDclNode*)*nodesp);
    }
    // A loop's body runs on some paths only, and again after itself
    int loop = (blk->flags & FlagLoop) != 0;
    if (loop)
        ++flowDepth;

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
        uint32_t tempmark = flowTempCount;
        switch ((*nodesp)->tag) {
        case VarDclTag:
            varDclFlow(fstate, (VarDclNode**)nodesp);
            if (((VarDclNode *)*nodesp)->value)
                blockTempEscape(tempmark, ((VarDclNode *)*nodesp)->value, 1);
            break;
        case SwapTag:
            swapFlow(fstate, (SwapNode **)nodesp);
            break;
        // A break or continue reaches a non-final position only in an 'each' loop
        // block, whose synthesized step follows the jump the reader wrote last.
        // Nothing after the jump runs, so the scope's de-aliasing has to be captured
        // here too, exactly as it is for a block's final node below.
        case BreakTag: {
            BreakRetNode *brknode = (BreakRetNode *)*nodesp;
            INode **brkexp = &brknode->exp;
            INode *result = *brkexp;
            brknode->flowresult = result;
            flowGateResult(fstate, result);
            if (result->tag != NilLitTag)
                flowLoadValue(fstate, brkexp);
            blockTempEscape(tempmark, *brkexp, 1);
            flowScopeDealias(blockJumpMark(brknode, svpos), &brknode->dealias, result, *nodesp);
            break;
        }
        case ContinueTag: {
            // A continue node carries no expression, so it de-aliases against none
            BreakRetNode *brknode = (BreakRetNode *)*nodesp;
            brknode->flowresult = NULL;
            flowScopeDealias(blockJumpMark(brknode, svpos), &brknode->dealias, NULL, *nodesp);
            break;
        }
        default:
            // An expression as statement throws out its value: a temporary
            if (isExpNode(*nodesp)) {
                flowLoadValue(fstate, nodesp);
                flowTempRead(nodesp);
                blockTempEscape(tempmark, *nodesp, 0);
            }
        }
    }

    // Capture any scope-ending dealiasing in block's last node
    // That last node must now be a return, break, continue or an injected "block return"
    //
    // The result expression is walked first and the dealias list built after it.
    // The walk is what marks a variable moved, so a value handed to a call in the
    // result -- 'hold(a)' as the last expression -- is known to have left 'a'
    // before the list that would have released 'a' at scope exit is built.
    // The list is built against the result node as it stood before the walk,
    // which is the node whose name the release exemption is about.
    // Whether every path through the block leaves the function: it ends in a
    // return, or hands back an 'if' or a block every path through which does
    // (ifFlow). A 'break' or 'continue' goes on inside it, to a place this walk
    // reaches with one state (it walks a loop once), so a path ending in one is
    // still joined, which counts what it moved as moved from there on.
    int jumped = 0;
    uint32_t tempmark = flowTempCount;
    switch ((*nodesp)->tag) {
    case ReturnTag:
    {
        INode **retexp = &((BreakRetNode *)*nodesp)->exp;
        // Check the borrow's lifetime before flowLoadValue can replace the node
        returnFlow((BreakRetNode *)*nodesp);
        INode *result = *retexp;
        ((BreakRetNode *)*nodesp)->flowresult = result;
        if (result != unknownType) {
            flowGateResult(fstate, result);
            flowLoadValue(fstate, retexp);
            // A returned value is moved to the caller, so it must be one this
            // function may move: not a value it reached through a borrow
            blockResultMove(*retexp);
            blockResultCount(retexp, result, 0);
            blockTempEscape(tempmark, *retexp, 1);
        }
        // An init returns only once it has filled self
        flowNewSelfReturn(fstate, *nodesp);
        flowScopeDealias(0, &((BreakRetNode *)*nodesp)->dealias, result, *nodesp);
        jumped = 1;
        break;
    }
    case BlockRetTag:
    {
        INode **retexp = &((BreakRetNode *)*nodesp)->exp;
        INode *result = *retexp;
        // A block that throws its value away hands back nothing: its final
        // expression is a statement's, and exempts no local from the release
        int discards = blockDiscards(fstate, blk);
        if (discards)
            result = NULL;
        ((BreakRetNode *)*nodesp)->flowresult = result;
        flowGateResult(fstate, *retexp);
        if ((*retexp)->tag != NilLitTag) {
            INode *exp = *retexp;
            fstate->jumped = 0;
            flowLoadValue(fstate, retexp);
            jumped = (exp->tag == IfTag || (exp->tag == BlockTag && !(exp->flags & FlagLoop)))
                && fstate->jumped;
            // The function's own block hands its value to the caller, as a return does
            if (fstate->scope == 2 && !loop && (exp->tag == IfTag || exp->tag == BlockTag)
                && itypeGetTypeDcl(fstate->fnsig->rettype)->tag != VoidTag && iexpIsMove(*retexp))
                blockResultMove(*retexp);
            if (discards)
                flowTempRead(retexp);
            else
                blockResultCount(retexp, result, svpos);
            blockTempEscape(tempmark, *retexp, !discards);
        }
        flowScopeDealias(svpos, &((BreakRetNode *)*nodesp)->dealias, result, *nodesp);
        break;
    }
    case BreakTag: {
        BreakRetNode *brknode = (BreakRetNode *)*nodesp;
        INode **brkexp = &brknode->exp;
        INode *result = *brkexp;
        brknode->flowresult = result;
        flowGateResult(fstate, result);
        if (result->tag != NilLitTag) {
            flowLoadValue(fstate, brkexp);
            blockResultCount(brkexp, result, blockJumpMark(brknode, svpos));
        }
        blockTempEscape(tempmark, *brkexp, 1);
        flowScopeDealias(blockJumpMark(brknode, svpos), &brknode->dealias, result, *nodesp);
        break;
    }
    case ContinueTag:
        ((BreakRetNode *)*nodesp)->flowresult = NULL;
        flowScopeDealias(blockJumpMark((BreakRetNode *)*nodesp, svpos), &((BreakRetNode *)*nodesp)->dealias, NULL, *nodesp);
        break;
    }

    if (loop) {
        --flowDepth;
        jumped = 0;
    }
    fstate->jumped = jumped;
    --fstate->scope;
    flowScopePop(svpos);
}
