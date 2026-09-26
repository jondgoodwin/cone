/** Handling for swap nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new swap node
SwapNode *newSwapNode(INode *lval, INode *rval) {
    SwapNode *node;
    newNode(node, SwapNode, SwapTag);
    node->lval = lval;
    node->rval = rval;
    return node;
}

// Clone swap node
INode *cloneSwapNode(CloneState *cstate, SwapNode *node) {
    SwapNode *newnode;
    newnode = memAllocBlk(sizeof(SwapNode));
    memcpy(newnode, node, sizeof(SwapNode));
    newnode->lval = cloneNode(cstate, node->lval);
    newnode->rval = cloneNode(cstate, node->rval);
    return (INode *)newnode;
}

// Serialize swap node
void swapPrint(SwapNode *node) {
    inodeFprint("(<=>, ");
    inodePrintNode(node->lval);
    inodeFprint(", ");
    inodePrintNode(node->rval);
    inodeFprint(")");
}

// Name resolution for swap node
void swapNameRes(NameResState *pstate, SwapNode *node) {
    inodeNameRes(pstate, &node->lval);
    inodeNameRes(pstate, &node->rval);
}

// Type checking for swap node
void swapTypeCheck(TypeCheckState *pstate, SwapNode *node) {
    if (iexpTypeCheckAny(pstate, &node->lval) == 0 || iexpIsLvalError(node->lval) == 0)
        return;

    if (iexpTypeCheckAny(pstate, &node->rval) == 0 || iexpIsLvalError(node->rval) == 0)
        return;

    if (!iexpSameType(node->lval, &node->rval)) {
        errorMsgNode(node->lval, ErrorInvType, "Swap lvals do not have matching types");
        return;
    }
}

// Perform data flow analysis on swap node
// - lval and rval need to be mutable.
// - a swap is a store in both directions, so each side is held to the borrow
//   lifetime rule assignment applies: neither may outlive a borrowed reference
//   it receives from the other
void swapFlow(FlowState *fstate, SwapNode **nodep) {
    SwapNode *node = *nodep;

    uint16_t lvalscope;
    INode *lvalperm;
    iexpGetLvalInfo(node->lval, &lvalperm, &lvalscope);
    if (!(MayWrite & permGetFlags(lvalperm))) {
        errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
        return;
    }

    uint16_t rvalscope;
    iexpGetLvalInfo(node->rval, &lvalperm, &rvalscope);
    if (!(MayWrite & permGetFlags(lvalperm))) {
        errorMsgNode(node->rval, ErrorNoMut, "You do not have permission to modify rval");
        return;
    }

    assignBorrowLifetimeCheck(node->lval, lvalscope, ((IExpNode*)node->rval)->vtype);
    assignBorrowLifetimeCheck(node->rval, rvalscope, ((IExpNode*)node->lval)->vtype);

    flowGateAssigned(fstate, node->lval);
    flowGateAssigned(fstate, node->rval);
    flowLoadValue(fstate, &node->lval);
    flowLoadValue(fstate, &node->rval);
}
