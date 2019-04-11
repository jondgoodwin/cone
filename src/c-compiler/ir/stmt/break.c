/** Handling for break nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new break/continue node
BreakNode *newBreakNode(int16_t tag) {
    BreakNode *node;
    newNode(node, BreakNode, tag);
    node->dealias = NULL;
    return node;
}

// Name resolution for break/continue
// - Ensure it is only used within a while/each block
void breakNameRes(PassState *pstate, INode *node) {
    if (!(pstate->flags & PassWithinWhile))
        errorMsgNode(node, ErrorNoWhile, "break/continue may only be used within a while/each block");
}

// There is no type check!
void breakPass(PassState *pstate, INode *node) {
    if (pstate->pass == NameResolution)
        breakNameRes(pstate, node);
}
