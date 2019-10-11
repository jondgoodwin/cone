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
void breakNameRes(NameResState *pstate, INode *node) {
    if (!(pstate->flags & PassWithinWhile))
        errorMsgNode(node, ErrorNoWhile, "break may only be used within a while/each/loop block");
}
