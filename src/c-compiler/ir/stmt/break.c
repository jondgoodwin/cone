/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new break node
BreakRetNode *newBreakNode() {
    BreakRetNode *node;
    newNode(node, BreakRetNode, BreakTag);
    node->life = NULL;
    node->block = NULL;
    node->exp = NULL;
    node->dealias = NULL;
    return node;
}

// Clone break
INode *cloneBreakNode(CloneState *cstate, BreakRetNode *node) {
    BreakRetNode *newnode;
    newnode = memAllocBlk(sizeof(BreakRetNode));
    memcpy(newnode, node, sizeof(BreakRetNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->life = cloneNode(cstate, node->life);
    return (INode *)newnode;
}

// Name resolution for break
// - Resolve any lifetime or expression names
void breakNameRes(NameResState *pstate, BreakRetNode *node) {
    if (node->life)
        inodeNameRes(pstate, &node->life);
    inodeNameRes(pstate, &node->exp);
}

// Find the loop node in stack that matches the lifetime
BlockNode *breakFindBlockNode(TypeCheckState *pstate, INode *life) {
    uint32_t cnt = pstate->blockcnt;
    if (!life) {
        return pstate->recentLoop; // use most recent, when no lifetime
    }
    LifetimeNode *lifeDclNode = (LifetimeNode *)((NameUseNode *)life)->dclnode;
    while (cnt--) {
        if (pstate->blockstack[cnt]->life == lifeDclNode)
            return pstate->blockstack[cnt];
    }
    return NULL;
}

// Type check the break expression, ensure it matches loop's type
void breakTypeCheck(TypeCheckState *pstate, BreakRetNode *node) {
    if (pstate->recentLoop == NULL) {
        errorMsgNode((INode*)node, ErrorNoLoop, "break may only be used within a while/each/loop block");
        return;
    }

    // Register the break with the block node it applies to
    // Note:  we don't type check the break expression until later (as part of block type check),
    // when we can ensure all of them match to each other and whatever is expected
    BlockNode *blocknode = breakFindBlockNode(pstate, node->life);
    if (blocknode) {
        nodesAdd(&blocknode->breaks, (INode*)node);
        node->block = blocknode;
    }
    else
        errorMsgNode((INode*)node, ErrorNoLoop, "break's lifetime does not match a current loop");
}
