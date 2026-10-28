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
    inodeFprint("(<->, ");
    inodePrintNode(node->lval);
    inodeFprint(", ");
    inodePrintNode(node->rval);
    inodeFprint(")");
}

// Name resolution for swap node
void swapNameRes(NameResState *pstate, SwapNode *node) {
    inodeNameRes(pstate, &node->lval);
    inodeNameRes(pstate, &node->rval);
    // In a macro's body, a swap of a plain parameter is refused here
    macroChangeNameRes(node->lval, node->lval, MacroChangePlace);
    macroChangeNameRes(node->rval, node->rval, MacroChangePlace);
}

// Type check one side of a swap as a place stored through. Answer 0 if it is not one.
// 'x[i] <-> y[j]', 'u.f[i].n <-> u.f[j].n': a swap stores through both of its
// sides, so every index in each place is in set position, as an assignment's
// are (assignTypeCheck): on a type declaring '&[]', each is the element's
// mutable borrow, and the borrow stands for the place, as on the left of '='
static int swapTypeCheckSide(TypeCheckState *pstate, INode **sidep) {
    FnCallNode *setindex = fnCallSetIndexRoot(*sidep);
    uint32_t setmark = fnCallSetPlace(*sidep);
    int errsbefore = errors;
    int typed = iexpTypeCheckAny(pstate, sidep);
    fnCallSetPlaceEnd(setmark);
    // An index refused for its permission has said so: no second error about its type
    if (typed == 0 || errors != errsbefore)
        return 0;
    if (setindex && *sidep == (INode*)setindex && (setindex->flags & FlagBorrow))
        derefInject(sidep);
    return iexpIsLvalError(*sidep);
}

// Type checking for swap node
void swapTypeCheck(TypeCheckState *pstate, SwapNode *node) {
    if (swapTypeCheckSide(pstate, &node->lval) == 0)
        return;

    if (swapTypeCheckSide(pstate, &node->rval) == 0)
        return;

    if (!iexpSameType(node->lval, &node->rval)) {
        errorMsgNode(node->lval, ErrorInvType, "Swap lvals do not have matching types");
        return;
    }
    // Each side now holds what the other did
    narrowAssigned(node->lval, NULL);
    narrowAssigned(node->rval, NULL);
    narrowCallMade((INode*)node);
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
    INode *lvalvar = iexpGetStoreLvalInfo(node->lval, &lvalperm, &lvalscope);
    if (!(MayWrite & permGetFlags(lvalperm))) {
        if (permIsLock(lvalperm))
            permLockRefused(node->lval, lvalperm, "write");
        else if (lvalvar && lvalvar->tag == VarDclTag && (lvalvar->flags & FlagMacroBind))
            macroParmAssignRefused(node->lval, (VarDclNode*)lvalvar);
        else
            errorMsgNode(node->lval, ErrorNoMut, "You do not have permission to modify lval");
        return;
    }

    uint16_t rvalscope;
    INode *rvalvar = iexpGetStoreLvalInfo(node->rval, &lvalperm, &rvalscope);
    if (!(MayWrite & permGetFlags(lvalperm))) {
        if (permIsLock(lvalperm))
            permLockRefused(node->rval, lvalperm, "write");
        else if (rvalvar && rvalvar->tag == VarDclTag && (rvalvar->flags & FlagMacroBind))
            macroParmAssignRefused(node->rval, (VarDclNode*)rvalvar);
        else
            errorMsgNode(node->rval, ErrorNoMut, "You do not have permission to modify rval");
        return;
    }

    assignBorrowLifetimeCheck(node->lval, lvalscope, ((IExpNode*)node->rval)->vtype);
    assignBorrowLifetimeCheck(node->rval, rvalscope, ((IExpNode*)node->lval)->vtype);

    flowGateAssigned(fstate, node->lval);
    flowGateAssigned(fstate, node->rval);
    flowGateShapeStore(fstate, node->lval);
    flowGateShapeStore(fstate, node->rval);
    flowLoadValue(fstate, &node->lval);
    flowLoadValue(fstate, &node->rval);
}
