/** Handling for 'yield' nodes: a generator's seam
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

// ---- The generators made so far ----------------------------------------------

static GenInfo **gens = NULL;
static uint32_t gencnt = 0;
static uint32_t genmax = 0;

GenInfo *yieldGenNew(FnDclNode *step, StructNode *gen, FnDclNode *none) {
    GenInfo *info = (GenInfo *)memAllocBlk(sizeof(GenInfo));
    memset(info, 0, sizeof(GenInfo));
    info->step = step;
    info->gen = gen;
    info->none = none;
    if (gencnt == genmax) {
        uint32_t newmax = genmax ? genmax * 2 : 8;
        GenInfo **grown = (GenInfo **)memAllocBlk(newmax * sizeof(GenInfo *));
        if (gencnt)
            memcpy(grown, gens, gencnt * sizeof(GenInfo *));
        gens = grown;
        genmax = newmax;
    }
    gens[gencnt++] = info;
    return info;
}

GenInfo *yieldGenOf(FnDclNode *step) {
    for (uint32_t i = 0; i < gencnt; ++i) {
        if (gens[i]->step == step)
            return gens[i];
    }
    return NULL;
}

GenInfo *yieldGenOfStruct(INode *type) {
    for (uint32_t i = 0; i < gencnt; ++i) {
        if ((INode *)gens[i]->gen == type)
            return gens[i];
    }
    return NULL;
}

void yieldSplitRegister(GenInfo *info, Nodes *yields) {
    info->yields = yields;
    uint32_t no = 0;
    INode **nodesp;
    uint32_t cnt;
    if (yields == NULL)
        return;
    for (nodesFor(yields, cnt, nodesp))
        ((YieldNode *)*nodesp)->yieldno = ++no;
}

int yieldAny() {
    return gencnt != 0;
}

// ---- The node ------------------------------------------------------------------

YieldNode *newYieldNode() {
    YieldNode *node;
    newNode(node, YieldNode, YieldTag);
    node->vtype = unknownType;
    node->exp = NULL;
    node->seamvars = NULL;
    node->nseamvars = 0;
    node->seamcap = 0;
    node->yieldno = 0;
    node->walked = 0;
    return node;
}

// Clone yield: what a walk found at the seam is the original's own
INode *cloneYieldNode(CloneState *cstate, YieldNode *node) {
    YieldNode *newnode = memAllocBlk(sizeof(YieldNode));
    memcpy(newnode, node, sizeof(YieldNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->seamvars = NULL;
    newnode->nseamvars = 0;
    newnode->seamcap = 0;
    newnode->yieldno = 0;
    newnode->walked = 0;
    return (INode *)newnode;
}

void yieldPrint(YieldNode *node) {
    inodeFprint("(yield, ");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

void yieldNameRes(NameResState *pstate, YieldNode *node) {
    inodeNameRes(pstate, &node->exp);
}

// A yield hands the caller a borrow only of what outlives the call, as a
// 'return' does (returnFlowEscape): not of the generator's own locals, which
// are in its frame but not the caller's to hold
void yieldTypeCheck(TypeCheckState *pstate, YieldNode *node) {
    FnDclNode *fn = pstate->fn;
    GenInfo *info = fn ? yieldGenOf(fn) : NULL;
    if (info == NULL) {
        errorMsgNode((INode *)node, ErrorYieldPlace,
            "'yield' stands only in the body of a function declared with 'yields', a generator, which hands its caller a value and waits to be resumed there.");
        node->vtype = errorType;
        return;
    }
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    if (!iexpTypeCheckCoerce(pstate, sig->rettype, &node->exp)) {
        errorMsgNode((INode *)node, ErrorInvType, "The value yielded does not match the type the generator yields");
        node->vtype = errorType;
        return;
    }
    returnFlowEscape(node->exp);
    node->vtype = (INode *)newVoidNode();
}
