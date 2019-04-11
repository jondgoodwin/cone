/** Handling for while nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new While node
WhileNode *newWhileNode() {
    WhileNode *node;
    newNode(node, WhileNode, WhileTag);
    node->blk = NULL;
    return node;
}

// Serialize a while node
void whilePrint(WhileNode *node) {
    inodeFprint("while ");
    inodePrintNode(node->condexp);
    inodePrintNL();
    inodePrintNode(node->blk);
}

// while block name resolution
void whileNameRes(PassState *pstate, WhileNode *node) {
    uint16_t svflags = pstate->flags;
    pstate->flags |= PassWithinWhile;

    inodeWalk(pstate, &node->condexp);
    inodeWalk(pstate, &node->blk);

    pstate->flags = svflags;
}

// Type check the while block (conditional expression must be coercible to bool)
void whilePass(PassState *pstate, WhileNode *node) {
    if (pstate->pass == NameResolution) {
        whileNameRes(pstate, node);
        return;
    }

    inodeWalk(pstate, &node->condexp);
    inodeWalk(pstate, &node->blk);
    if (0 == iexpCoerces((INode*)boolType, &node->condexp))
        errorMsgNode(node->condexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
}

// Perform data flow analysis on an while statement
void whileFlow(FlowState *fstate, WhileNode **nodep) {
    WhileNode *node = *nodep;
    flowLoadValue(fstate, &node->condexp);
    blockFlow(fstate, (BlockNode**)&node->blk);
}
