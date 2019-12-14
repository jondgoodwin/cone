/** Handling for loop nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new loop node
LoopNode *newLoopNode() {
    LoopNode *node;
    newNode(node, LoopNode, LoopTag);
    node->vtype = unknownType;  // This will be overridden with loop-as-expr
    node->blk = NULL;
    node->life = NULL;
    node->breaks = newNodes(2);
    return node;
}

// Clone loop
INode *cloneLoopNode(CloneState *cstate, LoopNode *node) {
    uint32_t dclpos = cloneDclPush();
    LoopNode *newnode;
    newnode = memAllocBlk(sizeof(LoopNode));
    memcpy(newnode, node, sizeof(LoopNode));
    newnode->breaks = cloneNodes(cstate, node->breaks);
    newnode->life = (LifetimeNode*)cloneNode(cstate, (INode*)node->life);
    newnode->blk = cloneNode(cstate, node->blk);
    cloneDclPop(dclpos);
    return (INode *)newnode;
}

// Serialize a loop node
void loopPrint(LoopNode *node) {
    inodeFprint("loop");
    inodePrintNL();
    inodePrintNode(node->blk);
}

// loop block name resolution
void loopNameRes(NameResState *pstate, LoopNode *node) {
    // If loop declares a lifetime declaration, hook into name table for name res
    LifetimeNode *lifenode = node->life;
    if (pstate->scope > 1 && lifenode) {
        if (lifenode->namesym->node) {
            errorMsgNode((INode *)lifenode, ErrorDupName, "Lifetime is already defined. Only one allowed.");
            errorMsgNode((INode*)lifenode->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
        else {
            lifenode->life = pstate->scope;
            // Add name to global name table (containing block will unhook it later)
            nametblHookNode(lifenode->namesym, (INode*)lifenode);
        }
    }
    inodeNameRes(pstate, &node->blk);
}

// Type check the loop block, and set up for type check of breaks
// Note: vtype is set to something other than unknownType if all branches have the same type
// If they are different, bidirectional type inference will resolve this later
void loopTypeCheck(TypeCheckState *pstate, LoopNode *node, INode *expectType) {
    INode *sametype = NULL;

    // Push loop node on loop stack for use by break type check
    if (pstate->loopcnt >= TypeCheckLoopMax) {
        errorMsgNode((INode*)node, ErrorBadArray, "Overflowing fixed-size loop stack.");
        errorExit(100, "Unrecoverable error!");
    }
    pstate->loopstack[pstate->loopcnt++] = node;

    // This will ensure that every break is registered to the loop
    inodeTypeCheckAny(pstate, &node->blk);
    if (node->breaks->used == 0)
        errorMsgNode((INode*)node, WarnLoop, "Loop may never stop without a break.");
    blockNoBreak((BlockNode*)node->blk);

    // Attempt to determine the vtype, assuming all breaks return the same type
    // Otherwise, we will fix this during bidirectional type inference
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->breaks, cnt, nodesp)) {
        iexpFindSameType(&sametype, ((BreakNode*)*nodesp)->exp);
    }
    node->vtype = sametype;

    --pstate->loopcnt;
}

// Bidirectional type inference
void loopBiTypeInfer(INode **totypep, LoopNode *loopnode) {

    // Type check all breaks to this loop
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(loopnode->breaks, cnt, nodesp)) {
        BreakNode *brknode = (BreakNode*)*nodesp;
        if (brknode->exp == noValue) {
            if (*totypep)
                errorMsgNode((INode*)brknode, ErrorInvType, "this break must specify a value matching loop's type");
        }
        // If this break returns an expression, ensure it matches expected/previous types
        else {
            if (*totypep == NULL && cnt != loopnode->breaks->used)
                errorMsgNode((INode*)brknode, ErrorInvType, "If this break specifies a value, earlier breaks must too");
            else if (!iexpTypeExpect(totypep, &brknode->exp))
                errorMsgNode((INode*)brknode, ErrorInvType, "break expression's type does not match other breaks");
        }
    }

    loopnode->vtype = *totypep;
}

// Perform data flow analysis on a loop expression
void loopFlow(FlowState *fstate, LoopNode **nodep) {
    LoopNode *node = *nodep;
    blockFlow(fstate, (BlockNode**)&node->blk);
}
