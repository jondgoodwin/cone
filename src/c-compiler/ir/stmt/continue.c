/** Handling for continue nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new continue node
BreakRetNode *newContinueNode() {
    BreakRetNode *node;
    newNode(node, BreakRetNode, ContinueTag);
    node->life = NULL;
    node->dealias = NULL;
    return node;
}

// Clone continue
INode *cloneContinueNode(CloneState *cstate, BreakRetNode *node) {
    BreakRetNode *newnode;
    newnode = memAllocBlk(sizeof(BreakRetNode));
    memcpy(newnode, node, sizeof(BreakRetNode));
    newnode->life = cloneNode(cstate, node->life);
    return (INode *)newnode;
}

// Name resolution for continue
// - Resolve any lifetime name
void continueNameRes(NameResState *nstate, BreakRetNode *continuenode) {
    // Resolve continue to block it applies to
    if (continuenode->life) {
        // If a lifetime was specified, resolve to lifetime-named block
        inodeNameRes(nstate, &continuenode->life);
        BlockNode *block = (BlockNode*)((NameUseNode*)(continuenode->life))->dclnode;
        if (block && block->tag == BlockTag && (block->flags & FlagLoop)) {
            continuenode->block = block;
        }
        else {
            errorMsgNode((INode*)continuenode, ErrorNoLoop, "break's lifetime not found on a lifetime-named looping block");
        }
    }
    else {
        // If no lifetime specified, resolve to inner-most loop block
        if (nstate->loopblock)
            continuenode->block = nstate->loopblock;
        else
            errorMsgNode((INode*)continuenode, ErrorNoLoop, "continue may only be used within a while/each/loop block");
    }
}

// Type check the continue node
void continueTypeCheck(TypeCheckState *pstate, BreakRetNode *node) {
}
