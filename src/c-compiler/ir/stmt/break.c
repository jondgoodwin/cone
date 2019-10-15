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
// - Resolve any lifetime or expression names
void breakNameRes(NameResState *pstate, BreakNode *node) {
    if (node->life)
        inodeNameRes(pstate, &node->life);
    if (node->exp)
        inodeNameRes(pstate, &node->exp);
}

// Find the loop node in stack that matches the lifetime
LoopNode *breakFindLoopNode(TypeCheckState *pstate, INode *life) {
    uint32_t cnt = pstate->loopcnt;
    if (!life)
        return pstate->loopstack[cnt - 1]; // use most recent, when no lifetime
    LifetimeNode *lifeDclNode = (LifetimeNode *)((NameUseNode *)life)->dclnode;
    while (cnt--) {
        if (pstate->loopstack[cnt]->life == lifeDclNode)
            return pstate->loopstack[cnt];
    }
    return NULL;
}

// Type check the break expression, ensure it matches loop's type
void breakTypeCheck(TypeCheckState *pstate, BreakNode *node) {
    if (pstate->loopcnt == 0)
        errorMsgNode((INode*)node, ErrorNoLoop, "break may only be used within a while/each/loop block");

    // Determine the vtype of the break's expression
    INode *vtype = voidType;
    if (node->exp) {
        inodeTypeCheck(pstate, &node->exp);
        vtype = ((ITypedNode *)node->exp)->vtype;
    }

    // Coordinate break's vtype with the vtype of the corresponding loop
    LoopNode *loopnode = breakFindLoopNode(pstate, node->life);
    if (loopnode) {
        // Ensure all breaks return an expression of the same type
        if (loopnode->nbreaks == 0)
            loopnode->vtype = vtype;
        else if (!iexpSameType(loopnode->vtype, &vtype))
            errorMsgNode((INode*)node, ErrorInvType, "break expression's type does not match other breaks");
        ++loopnode->nbreaks;
    }
    else
        errorMsgNode((INode*)node, ErrorNoLoop, "break's lifetime does not match a current loop");
}
