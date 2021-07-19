/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new break breaknode
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
void breakNameRes(NameResState *nstate, BreakRetNode *breaknode) {
    inodeNameRes(nstate, &breaknode->exp);

    // Resolve break to block it applies to
    if (breaknode->life) {
        // If a lifetime was specified, resolve to lifetime-named block
        inodeNameRes(nstate, &breaknode->life);
        BlockNode *block = (BlockNode*)((NameUseNode*)(breaknode->life))->dclnode;
        if (block && block->tag == BlockTag) {
            breaknode->block = block;
        }
        else {
            errorMsgNode((INode*)breaknode, ErrorNoLoop, "break's lifetime not found on a lifetime-named block");
        }
    }
    else {
        // If no lifetime specified, resolve to inner-most loop block
        if (nstate->loopblock)
            breaknode->block = nstate->loopblock;
        else
            errorMsgNode((INode*)breaknode, ErrorNoLoop, "break may only be used within a while/each/loop block");
    }
}

// Type check the break expression, ensure it matches loop's type
void breakTypeCheck(TypeCheckState *pstate, BreakRetNode *breaknode) {

    // Note:  we don't type check the break expression until later (as part of block type check),
    // when we can ensure all of them match to each other and whatever is expected
    nodesAdd(&breaknode->block->breaks, (INode*)breaknode);
}
