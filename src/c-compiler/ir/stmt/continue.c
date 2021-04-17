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
void continueNameRes(NameResState *pstate, BreakRetNode *node) {
    if (node->life)
        inodeNameRes(pstate, &node->life);
}

// Type check the continue expression, ensuring it lies within a loop
void continueTypeCheck(TypeCheckState *pstate, BreakRetNode *node) {
    if (pstate->loopcnt == 0)
        errorMsgNode((INode*)node, ErrorNoLoop, "continue may only be used within a while/each/loop block");
    LoopNode *loopnode = breakFindLoopNode(pstate, node->life);
    if (!loopnode)
        errorMsgNode((INode*)node, ErrorNoLoop, "continue's lifetime does not match a current loop");
}
