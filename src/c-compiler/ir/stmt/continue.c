/** Handling for continue nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new continue node
ContinueNode *newContinueNode() {
    ContinueNode *node;
    newNode(node, ContinueNode, ContinueTag);
    node->life = NULL;
    node->dealias = NULL;
    return node;
}

// Name resolution for continue
// - Ensure it is only used within a while/each/loop block
void continueNameRes(NameResState *pstate, INode *node) {
    if (!(pstate->flags & PassWithinWhile))
        errorMsgNode(node, ErrorNoWhile, "continue may only be used within a while/each/loop block");
}
