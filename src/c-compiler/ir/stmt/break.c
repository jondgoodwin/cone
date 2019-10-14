/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new break node
BreakNode *newBreakNode() {
    BreakNode *node;
    newNode(node, BreakNode, BreakTag);
    node->life = NULL;
    node->exp = NULL;
    node->dealias = NULL;
    return node;
}

// Name resolution for break
// - Ensure it is only used within a while/each/loop block
// - Resolve any lifetime or exp names
void breakNameRes(NameResState *pstate, BreakNode *node) {
    if (!(pstate->flags & PassWithinLoop))
        errorMsgNode((INode*)node, ErrorNoWhile, "break may only be used within a while/each/loop block");
    if (node->life)
        inodeNameRes(pstate, &node->life);
    if (node->exp)
        inodeNameRes(pstate, &node->exp);
}
