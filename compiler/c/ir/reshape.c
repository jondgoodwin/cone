/** Could a call change the shape of a collection a borrow points into?
 *
 * reshape.h says what is asked. This reads the types for what a value can reach,
 * and a call's arguments for which it is handed; what a function's body does is
 * read by shapeinfer.c (shapeParamReshapes, shapeTypeReshapes).
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>

// ---- What a type can reach -----------------------------------------------------

#define RcSeenCap 4096
static INode *rcSeen[RcSeenCap];
static uint8_t rcSeenInd[RcSeenCap];
static uint8_t rcOnStack[RcSeenCap];
static int rcN;
static unsigned rcBits;
static INode *rcWant;
static int rcWritable;
static int rcLow;           // the lowest stack position a struct below reached back to

// What one struct reaches, kept: asked of nearly every call made while a borrow is
// held, and the same types come round. Kept only where the answer was settled
// without leaning on a struct still being walked above it
typedef struct {
    INode *st;
    INode *want;
    uint8_t writable;
    uint8_t ind;
    uint8_t bits;
    uint8_t used;
} RcMemo;

static RcMemo *rcMemo = NULL;
static uint32_t rcMemoCap = 0;
static uint32_t rcMemoUsed = 0;

static uint32_t rcMemoHash(INode *st, INode *want, int writable, int ind) {
    uint64_t h = ((uint64_t)(uintptr_t)st * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)want * 0xC2B2AE3D27D4EB4Full)
        ^ ((uint64_t)writable << 3) ^ ((uint64_t)ind << 5);
    return (uint32_t)(h >> 32);
}

static RcMemo *rcMemoFind(INode *st, INode *want, int writable, int ind) {
    if (rcMemoCap == 0)
        return NULL;
    uint32_t at = rcMemoHash(st, want, writable, ind) & (rcMemoCap - 1);
    while (rcMemo[at].used) {
        if (rcMemo[at].st == st && rcMemo[at].want == want && rcMemo[at].writable == writable && rcMemo[at].ind == ind)
            return &rcMemo[at];
        at = (at + 1) & (rcMemoCap - 1);
    }
    return NULL;
}

static void rcMemoPut(INode *st, INode *want, int writable, int ind, unsigned bits) {
    if ((rcMemoUsed + 1) * 2 > rcMemoCap) {
        RcMemo *old = rcMemo;
        uint32_t oldcap = rcMemoCap;
        rcMemoCap = oldcap ? oldcap * 2 : 1024;
        rcMemo = (RcMemo *)memAllocBlk(rcMemoCap * sizeof(RcMemo));
        memset(rcMemo, 0, rcMemoCap * sizeof(RcMemo));
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (!old[i].used)
                continue;
            uint32_t at = rcMemoHash(old[i].st, old[i].want, old[i].writable, old[i].ind) & (rcMemoCap - 1);
            while (rcMemo[at].used)
                at = (at + 1) & (rcMemoCap - 1);
            rcMemo[at] = old[i];
        }
    }
    uint32_t at = rcMemoHash(st, want, writable, ind) & (rcMemoCap - 1);
    while (rcMemo[at].used)
        at = (at + 1) & (rcMemoCap - 1);
    rcMemo[at].st = st;
    rcMemo[at].want = want;
    rcMemo[at].writable = (uint8_t)writable;
    rcMemo[at].ind = (uint8_t)ind;
    rcMemo[at].bits = (uint8_t)bits;
    rcMemo[at].used = 1;
    ++rcMemoUsed;
}

// 'ind': a reference or pointer has been passed through on the way
static void rcWalk(INode *type, int ind) {
    if (type == NULL)
        return;
    switch (type->tag) {
    case NameUseTag:
        if (isTypeNode(type))
            rcWalk(itypeGetTypeDcl(type), ind);
        return;
    case AliasDclTag:
        rcWalk(((AliasDclNode *)type)->target, ind);
        return;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        // A read-only reference cannot write what it reaches, however many others may
        if (rcWritable && ref->region && itypeGetTypeDcl(ref->region) == borrowRef) {
            INode *perm = ref->perm == unknownType ? (INode *)roPerm : itypeGetTypeDcl(ref->perm);
            if (perm == (INode *)roPerm || perm == (INode *)immPerm)
                return;
        }
        if (type->tag == VirtRefTag) {
            rcBits |= ReachVirtual | ReachBorrowed;
            return;
        }
        rcWalk(ref->vtexp, 1);
        return;
    }
    case PtrTag:
        rcWalk(((StarNode *)type)->vtexp, 1);
        return;
    case ArrayTag:
        rcWalk(arrayElemType(type), ind);
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            rcWalk(*nodesp, ind);
        return;
    }
    case StructTag:
    {
        if (type == rcWant) {
            rcBits |= ind ? ReachBorrowed : ReachContained;
            return;
        }
        RcMemo *kept = rcMemoFind(type, rcWant, rcWritable, ind);
        if (kept) {
            rcBits |= kept->bits;
            return;
        }
        for (int i = 0; i < rcN; ++i) {
            if (rcSeen[i] == type && rcSeenInd[i] == ind) {
                // On the stack: its walk is not done, so what is above it leans on it.
                // Done but not kept: what it reached was cut short by one that was
                if (rcOnStack[i]) {
                    if (i < rcLow)
                        rcLow = i;
                }
                else
                    rcLow = 0;
                return;
            }
        }
        if (rcN >= RcSeenCap) {
            rcBits |= ReachGaveUp | ReachBorrowed;
            rcLow = 0;
            return;
        }
        int at = rcN++;
        rcSeen[at] = type;
        rcSeenInd[at] = (uint8_t)ind;
        rcOnStack[at] = 1;
        unsigned outer = rcBits;
        int outerLow = rcLow;
        rcBits = 0;
        rcLow = RcSeenCap;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp))
            rcWalk(((IExpNode *)*nodesp)->vtype, ind);
        if (((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp))
                rcWalk(*nodesp, ind);
        }
        rcOnStack[at] = 0;
        // Settled when nothing it walked leans on a struct above it
        if (rcLow >= at && (type->flags & TypeChecked))
            rcMemoPut(type, rcWant, rcWritable, ind, rcBits);
        rcBits |= outer;
        rcLow = rcLow < outerLow ? rcLow : outerLow;
        return;
    }
    default:
        return;
    }
}

unsigned reshapeReach(INode *type, INode *cont, int writable) {
    rcN = 0;
    rcBits = 0;
    rcLow = RcSeenCap;
    rcWant = cont;
    rcWritable = writable;
    rcWalk(type, 0);
    return rcBits;
}

// ---- Globals -----------------------------------------------------------------------

static VarDclNode **rgGlobals = NULL;
static uint32_t rgUsed = 0;
static uint32_t rgCap = 0;

void reshapeNoteGlobal(VarDclNode *var) {
    if (rgUsed == rgCap) {
        VarDclNode **old = rgGlobals;
        rgCap = rgCap ? rgCap * 2 : 64;
        rgGlobals = (VarDclNode **)memAllocBlk(rgCap * sizeof(VarDclNode *));
        if (old)
            memcpy(rgGlobals, old, rgUsed * sizeof(VarDclNode *));
    }
    rgGlobals[rgUsed++] = var;
}

// Per collection type: how many globals were looked at, and whether one reaches it
typedef struct {
    INode *cont;
    uint32_t looked;
    uint8_t reaches;
} RgMemo;
#define RgMemoCap 8
static RgMemo rgMemo[RgMemoCap];
static uint32_t rgMemoUsed = 0;
static uint32_t rgMemoNext = 0;

int reshapeGlobalsReach(INode *cont) {
    RgMemo *m = NULL;
    for (uint32_t i = 0; i < rgMemoUsed; ++i) {
        if (rgMemo[i].cont == cont)
            m = &rgMemo[i];
    }
    if (m == NULL) {
        m = &rgMemo[rgMemoNext];
        rgMemoNext = (rgMemoNext + 1) % RgMemoCap;
        if (rgMemoUsed < RgMemoCap)
            ++rgMemoUsed;
        m->cont = cont;
        m->looked = 0;
        m->reaches = 0;
    }
    for (; m->looked < rgUsed && !m->reaches; ++m->looked) {
        INode *type = rgGlobals[m->looked]->vtype;
        // A type not worked out yet could be anything
        if (type == NULL || type == unknownType || reshapeReach(type, cont, 0))
            m->reaches = 1;
    }
    return m->reaches;
}

// ---- Values the function owns whole ---------------------------------------------

int reshapeUniqueLocal(INode *e) {
    for (;;) {
        if (e->tag == CastTag && !(e->flags & FlagConvert))
            e = ((CastNode *)e)->exp;
        else if (e->tag == FldAccessTag)
            e = ((FnCallNode *)e)->objfn;
        else if (e->tag == ArrIndexTag && iexpGetTypeDcl(((FnCallNode *)e)->objfn)->tag == ArrayTag)
            e = ((FnCallNode *)e)->objfn;
        else
            break;
    }
    VarDclNode *var = siNamedVar(e);
    if (!var || var->scope == 0 || (var->flags & FlagStatic))
        return 0;
    INode *t = iexpGetTypeDcl(e);
    return t->tag != RefTag && t->tag != ArrayRefTag && t->tag != VirtRefTag && t->tag != PtrTag;
}

// ---- A call ----------------------------------------------------------------------

// What the callee may write through its parameter k: that parameter's own
// permission, which is what it was declared to need, not the argument's
static INode *reshapeParmType(FnDclNode *callee, uint32_t k, INode *arg) {
    if (callee && callee->vtype && callee->vtype->tag == FnSigTag && k < ((FnSigNode *)callee->vtype)->parms->used)
        return iexpGetTypeDcl(nodesGet(((FnSigNode *)callee->vtype)->parms, k));
    return iexpGetTypeDcl(arg);
}

int reshapeCall(FnCallNode *call, FnDclNode *meth, int recvunique, INode *cont, ReshapeVerdict *verdict) {
    INode *fnn = call->objfn;
    FnDclNode *callee = isNameUseNode(fnn) && ((NameUseNode *)fnn)->dclnode && ((NameUseNode *)fnn)->dclnode->tag == FnDclTag
        ? (FnDclNode *)((NameUseNode *)fnn)->dclnode : NULL;
    verdict->why = ReshapeNot;
    verdict->callee = callee;
    // An intrinsic acts on the memory it is handed and reshapes no collection;
    // an init builds a value that has no other name yet
    if (callee && ((callee->dclinfo.facts & DclIntrinsic) || (callee->value && callee->value->tag == IntrinsicTag)
            || fnDclIsInit(callee)))
        return 0;
    uint32_t nargs = call->args ? call->args->used : 0;
    int visible = !(call->flags & FlagVDisp) && siVisible(callee);
    uint32_t first = 0;

    // A call handed nothing it could write through (a read-only reference, a number)
    // can reshape only through a global: most calls, dismissed at once
    int capable = 0;
    for (uint32_t k = 0; k < nargs && !capable; ++k) {
        INode *t = iexpGetTypeDcl(nodesGet(call->args, k));
        capable = ((t->tag == RefTag || t->tag == ArrayRefTag) && (permGetFlags(((RefNode *)t)->perm) & MayWrite))
            || (t->tag == StructTag && ((StructNode *)t)->carriesborrow != CarriesBorrowNo)
            || t->tag == PtrTag || t->tag == VirtRefTag;
    }
    if (!capable) {
        if (visible && reshapeGlobalsReach(cont) && shapeTypeReshapes(callee, cont, 0)) {
            verdict->why = ReshapeGlobal;
            return 1;
        }
        return 0;
    }

    // The receiver, a method on the collection itself: layer 2 reads its body
    // (layer 1, assuming a method it cannot read reshapes), and a read-only
    // 'self' or a receiver nothing else reaches is no danger
    if (nargs > 0 && meth) {
        INode *t0 = iexpGetTypeDcl(nodesGet(call->args, 0));
        if (t0->tag == RefTag && itypeGetTypeDcl(((RefNode *)t0)->vtexp) == cont) {
            first = 1;
            INode *selft = iexpGetTypeDcl(nodesGet(((FnSigNode *)meth->vtype)->parms, 0));
            int writes = selft->tag == RefTag && (permGetFlags(((RefNode *)selft)->perm) & MayWrite) != 0;
            if (writes && !recvunique && !visible) {
                verdict->why = ReshapeReceiverUnseen;
                return 1;
            }
            if (writes && !recvunique && shapeParamReshapes(callee, 0)) {
                verdict->why = ReshapeReceiver;
                return 1;
            }
        }
    }

    // The other arguments: any that can reach the collection by a path the callee
    // may write by. A value this function owns whole (a local lent or handed
    // over) is not the shared one, but what it holds by reference still may be.
    int writable = 0;
    for (uint32_t k = first; k < nargs; ++k) {
        INode *arg = nodesGet(call->args, k);
        INode *at = iexpGetTypeDcl(arg);
        INode *ba = arg;
        while (ba->tag == CastTag && !(ba->flags & FlagConvert))
            ba = ((CastNode *)ba)->exp;
        INode *wt = reshapeParmType(callee, k, arg);
        unsigned bits;
        if (ba->tag == BorrowTag && at->tag == RefTag && reshapeUniqueLocal(((RefNode *)ba)->vtexp)) {
            // The callee may write what the borrow lends only if its parameter says so
            INode *wp = wt->tag == RefTag ? (((RefNode *)wt)->perm == unknownType ? (INode *)roPerm
                : itypeGetTypeDcl(((RefNode *)wt)->perm)) : NULL;
            bits = (wp == (INode *)roPerm || wp == (INode *)immPerm) ? 0
                : reshapeReach(((RefNode *)at)->vtexp, cont, 1) & (ReachBorrowed | ReachVirtual);
        }
        else if (at->tag != RefTag && at->tag != ArrayRefTag && at->tag != VirtRefTag && at->tag != PtrTag)
            bits = reshapeReach(wt, cont, 1) & (ReachBorrowed | ReachVirtual);
        else
            bits = reshapeReach(wt, cont, 1);
        if (bits)
            writable = 1;
    }
    if (writable) {
        if (!visible) {
            verdict->why = ReshapeUnseen;
            return 1;
        }
        if (shapeTypeReshapes(callee, cont, 1)) {
            verdict->why = ReshapeBody;
            return 1;
        }
        return 0;
    }
    // Handed nothing that reaches it: only a callee that does so through a global
    if (visible && reshapeGlobalsReach(cont) && shapeTypeReshapes(callee, cont, 0)) {
        verdict->why = ReshapeGlobal;
        return 1;
    }
    return 0;
}
