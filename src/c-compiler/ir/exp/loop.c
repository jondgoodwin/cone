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
    node->nbreaks = 0;
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
    pstate->flags |= PassWithinLoop;

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
            nametblHookNode((INamedNode*)lifenode);
        }
    }
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
