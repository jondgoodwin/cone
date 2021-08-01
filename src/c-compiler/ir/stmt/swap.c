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
void swapFlow(FlowState *fstate, SwapNode **nodep) {
    SwapNode *node = *nodep;

    uint16_t lvalscope;
    INode *lvalperm;
    INode *lvalvar = iexpGetLvalInfo(node->lval, &lvalperm, &lvalscope);
    if (node->lval->tag == NameUseTag)
        ((VarDclNode*)lvalvar)->flowtempflags |= VarInitialized;
    if (!(MayWrite & permGetFlags(lvalperm))) {
        errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
        return;
    }

    lvalvar = iexpGetLvalInfo(node->rval, &lvalperm, &lvalscope);
    if (node->rval->tag == NameUseTag)
        ((VarDclNode*)lvalvar)->flowtempflags |= VarInitialized;
    if (!(MayWrite & permGetFlags(lvalperm))) {
        errorMsgNode(node->rval, ErrorNoMut, "You do not have permission to modify rval");
        return;
    }

    flowLoadValue(fstate, &node->lval);
    flowLoadValue(fstate, &node->rval);
}
