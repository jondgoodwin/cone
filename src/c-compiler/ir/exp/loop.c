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
    node->blk = NULL;
    node->life = NULL;
    return node;
}

// Serialize a loop node
void loopPrint(LoopNode *node) {
    inodeFprint("loop");
    inodePrintNL();
    inodePrintNode(node->blk);
}

// loop block name resolution
void loopNameRes(NameResState *pstate, LoopNode *node) {
    uint16_t svflags = pstate->flags;
    pstate->flags |= PassWithinWhile;

    inodeNameRes(pstate, &node->blk);

    pstate->flags = svflags;
}

// Type check the loop block (conditional expression must be coercible to bool)
void loopTypeCheck(TypeCheckState *pstate, LoopNode *node) {
    inodeTypeCheck(pstate, &node->blk);
}

// Perform data flow analysis on a loop expression
void loopFlow(FlowState *fstate, LoopNode **nodep) {
    LoopNode *node = *nodep;
    blockFlow(fstate, (BlockNode**)&node->blk);
}
