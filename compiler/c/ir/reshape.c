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
static int rcLocalOnly;     // a value lent from a local: an owning virtual reference holds no borrow of one
static int rcMode;          // what the kept answers are kept by: rcWritable and rcLocalOnly
static int rcLow;           // the lowest stack position a struct below reached back to
// Asked about an owner (reshapeReachOwner): rcWant is the struct that holds the collection as a
// field, and this the collection. The collection reached inside any other struct is another
// value than the one the borrow is into, and is not reached; it is reached through a reference
// or pointer that points at it (or at an array of it), which may point at that very one.
// NULL where the collection itself is asked for
static INode *rcDirect;

// What one struct reaches, kept: asked of nearly every call made while a borrow is
// held, and the same types come round. Kept only where the answer was settled
// without leaning on a struct still being walked above it
typedef struct {
    INode *st;
    INode *want;
    INode *direct;
    uint8_t writable;
    uint8_t ind;
    uint8_t bits;
    uint8_t used;
} RcMemo;

static RcMemo *rcMemo = NULL;
static uint32_t rcMemoCap = 0;
static uint32_t rcMemoUsed = 0;

static uint32_t rcMemoHash(INode *st, INode *want, INode *direct, int writable, int ind) {
    uint64_t h = ((uint64_t)(uintptr_t)st * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)want * 0xC2B2AE3D27D4EB4Full)
        ^ ((uint64_t)(uintptr_t)direct * 0xD6E8FEB86659FD93ull) ^ ((uint64_t)writable << 3) ^ ((uint64_t)ind << 5);
    return (uint32_t)(h >> 32);
}

static RcMemo *rcMemoFind(INode *st, INode *want, int writable, int ind) {
    if (rcMemoCap == 0)
        return NULL;
    uint32_t at = rcMemoHash(st, want, rcDirect, writable, ind) & (rcMemoCap - 1);
    while (rcMemo[at].used) {
        if (rcMemo[at].st == st && rcMemo[at].want == want && rcMemo[at].direct == rcDirect
                && rcMemo[at].writable == writable && rcMemo[at].ind == ind)
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
            uint32_t at = rcMemoHash(old[i].st, old[i].want, old[i].direct, old[i].writable, old[i].ind) & (rcMemoCap - 1);
            while (rcMemo[at].used)
                at = (at + 1) & (rcMemoCap - 1);
            rcMemo[at] = old[i];
        }
    }
    uint32_t at = rcMemoHash(st, want, rcDirect, writable, ind) & (rcMemoCap - 1);
    while (rcMemo[at].used)
        at = (at + 1) & (rcMemoCap - 1);
    rcMemo[at].st = st;
    rcMemo[at].want = want;
    rcMemo[at].direct = rcDirect;
    rcMemo[at].writable = (uint8_t)writable;
    rcMemo[at].ind = (uint8_t)ind;
    rcMemo[at].bits = (uint8_t)bits;
    rcMemo[at].used = 1;
    ++rcMemoUsed;
}

// Asked about an owner: does a reference or pointer point at the collection itself, or at an
// array of it? Such a reference may point at the very value the borrow is into, whatever struct
// holds it. A reference to a struct that holds one as a field does not, unless that struct is the
// owner (rcWant), which the walk meets on its own.
static int rcPointsAtDirect(INode *pointee) {
    if (rcDirect == NULL || pointee == NULL)
        return 0;
    INode *t = itypeGetTypeDcl(pointee);
    if (t && t->tag == ArrayTag)
        t = itypeGetTypeDcl(arrayElemType(t));
    return t == rcDirect;
}

// The collection reached through a reference, as the walk reaches a struct it wants
static void rcReachDirect(int ind) {
    if (!(ind & 2))
        rcBits |= ReachBorrowed | ((ind & 4) ? 0 : ReachOwned);
}

// 'ind': bit 0, a reference or pointer has been passed through on the way; bit 1, the walk is
// under a read-only view, through which nothing is written but by a reference beneath it
// that is itself shared mutable; bit 2, a reference that does not own what it points at
// (a borrow, a raw pointer) has been passed through
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
        // The owner itself is what is wanted (reshapeOwnerCont): reached as a struct is
        if ((type->tag == RefTag || type->tag == VirtRefTag) && rcWant && type->tag == rcWant->tag
                && itypeIsSame(type, rcWant)) {
            if (!(ind & 2))
                rcBits |= (ind & 1) ? (ReachBorrowed | ((ind & 4) ? 0 : ReachOwned)) : ReachContained;
            return;
        }
        INode *perm = ref->perm == unknownType ? (INode *)roPerm : itypeGetTypeDcl(ref->perm);
        // A read-only reference cannot write what it reaches, however many others may; but
        // a 'mut' reference it reaches (a field of a struct it points at: 'Guard' holding a
        // '&mut List') can be written through, and is followed
        if (rcWritable && ref->region && itypeGetTypeDcl(ref->region) == borrowRef) {
            if (perm == (INode *)immPerm)
                return;
            if (perm == (INode *)roPerm) {
                // Followed: what it points at may hold a '&mut' ('& &mut Ed', a closure's capture
                // of a '&mut' parameter), which still reaches a writable path
                if (type->tag != VirtRefTag)
                    rcWalk(ref->vtexp, 1 | 2);
                return;
            }
        }
        if (type->tag == VirtRefTag) {
            // An owning virtual reference holds only global borrows (it names no lifetime), so
            // it cannot reach a local of this function
            if (rcLocalOnly && ref->region && itypeGetTypeDcl(ref->region) != borrowRef)
                return;
            rcBits |= ReachVirtual | ReachBorrowed;
            return;
        }
        // A 'mut' reference beneath a read-only view can be written through
        uint16_t pflags = perm && perm->tag == PermTag ? ((PermNode *)perm)->permflags : 0;
        // An owning reference ('Uni', 'Rc', 'Arc', a user region) keeps what it points at alive:
        // the walk stays 'owned' through it (ReachOwned); a borrow does not
        int owning = ref->region && itypeGetTypeDcl(ref->region) != borrowRef;
        int nind = 1 | (owning ? (ind & 4) : 4) | ((ind & 2) && !(pflags & MayAliasWrite) ? 2 : 0);
        if (rcPointsAtDirect(ref->vtexp)) {
            rcReachDirect(nind);
            return;
        }
        rcWalk(ref->vtexp, nind);
        return;
    }
    case PtrTag:
        if (rcPointsAtDirect(((StarNode *)type)->vtexp)) {
            rcReachDirect(1 | 4 | (ind & 2));
            return;
        }
        rcWalk(((StarNode *)type)->vtexp, 1 | 4 | (ind & 2));
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
            // Under a read-only view it is reached and not written
            if (!(ind & 2))
                rcBits |= (ind & 1) ? (ReachBorrowed | ((ind & 4) ? 0 : ReachOwned)) : ReachContained;
            return;
        }
        RcMemo *kept = rcMemoFind(type, rcWant, rcMode, ind);
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
            rcMemoPut(type, rcWant, rcMode, ind, rcBits);
        rcBits |= outer;
        rcLow = rcLow < outerLow ? rcLow : outerLow;
        return;
    }
    default:
        return;
    }
}

// 'owner' (or NULL): the struct that holds the collection 'cont' as a field, where a borrow
// into the collection was taken through that field. The value the borrow is into is then
// reached by reaching the owner, or by a reference that points at a collection of that
// type; the same type held as a field of any other struct is another value (rcDirect)
static unsigned reshapeReachOwnerMode(INode *type, INode *cont, INode *owner, int writable, int local) {
    rcN = 0;
    rcBits = 0;
    rcLow = RcSeenCap;
    rcWant = owner ? owner : cont;
    rcDirect = owner ? cont : NULL;
    rcWritable = writable;
    rcLocalOnly = local;
    rcMode = (writable ? 1 : 0) | (local ? 2 : 0);
    rcWalk(type, 0);
    rcDirect = NULL;
    return rcBits;
}

static unsigned reshapeReachMode(INode *type, INode *cont, int writable, int local) {
    return reshapeReachOwnerMode(type, cont, NULL, writable, local);
}

unsigned reshapeReach(INode *type, INode *cont, int writable) {
    return reshapeReachMode(type, cont, writable, 0);
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

// The same, of the globals that can be written ('var' ones): a 'let' global cannot change under a borrow
static RgMemo rgMutMemo[RgMemoCap];
static uint32_t rgMutMemoUsed = 0;
static uint32_t rgMutMemoNext = 0;

int reshapeMutGlobalsReach(INode *cont) {
    RgMemo *m = NULL;
    for (uint32_t i = 0; i < rgMutMemoUsed; ++i) {
        if (rgMutMemo[i].cont == cont)
            m = &rgMutMemo[i];
    }
    if (m == NULL) {
        m = &rgMutMemo[rgMutMemoNext];
        rgMutMemoNext = (rgMutMemoNext + 1) % RgMemoCap;
        if (rgMutMemoUsed < RgMemoCap)
            ++rgMutMemoUsed;
        m->cont = cont;
        m->looked = 0;
        m->reaches = 0;
    }
    for (; m->looked < rgUsed && !m->reaches; ++m->looked) {
        VarDclNode *var = rgGlobals[m->looked];
        if (var->perm && !(permGetFlags(var->perm) & MayWrite))
            continue;
        INode *type = var->vtype;
        // A type not worked out yet could be anything: taken as reaching, but not kept
        if (type == NULL || type == unknownType)
            return 1;
        if (reshapeReach(type, cont, 0))
            m->reaches = 1;
    }
    return m->reaches;
}

uint32_t reshapeGlobalCount(void) {
    return rgUsed;
}

VarDclNode *reshapeGlobalAt(uint32_t i) {
    return i < rgUsed ? rgGlobals[i] : NULL;
}

// ---- Values the function owns whole ---------------------------------------------

// The local a place is of, when it is one this function owns whole, else NULL
static VarDclNode *reshapeLocalRoot(INode *e, INode *cont) {
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
        return NULL;
    INode *t = iexpGetTypeDcl(e);
    // A local holding an owning reference is the owner's own place, when the container asked of
    // is an owner: no other name reaches it but a borrow of it (the loan's own, 'mine' below)
    if (reshapeIsOwner(cont) && (t->tag == RefTag || t->tag == VirtRefTag) && ((RefNode *)t)->region
            && itypeGetTypeDcl(((RefNode *)t)->region) != borrowRef)
        return var;
    return t->tag != RefTag && t->tag != ArrayRefTag && t->tag != VirtRefTag && t->tag != PtrTag ? var : NULL;
}

int reshapeUniqueLocalOf(INode *e, INode *cont) {
    return reshapeLocalRoot(e, cont) != NULL;
}

int reshapeUniqueLocal(INode *e) {
    return reshapeLocalRoot(e, NULL) != NULL;
}

// The same, but not one of the locals 'mine' (n of them): a local a loan points into has
// other names, the loan's own borrow among them
static int reshapeUniqueLocalExcept(INode *e, VarDclNode **mine, int n, INode *cont) {
    VarDclNode *var = reshapeLocalRoot(e, cont);
    for (int i = 0; var && i < n; ++i) {
        if (mine[i] == var)
            return 0;
    }
    return var != NULL;
}

// ---- A store ---------------------------------------------------------------------

int reshapeIsSum(INode *type) {
    return type && type->tag == StructTag && (type->flags & TraitType) && (type->flags & (HasTagField | SameSize));
}

// Is this place an element of a collection of type 'cont': reached by indexing one
// ('m[0]', '&mut m[i].name', 'self.items[k]'), a call of one of its methods that hands
// out a reference to what it holds, and field steps and dereferences from there?
static int reshapeElementOf(INode *e, INode *cont) {
    for (;;) {
        switch (e->tag) {
        case CastTag:
            if (e->flags & FlagConvert)
                return 0;
            e = ((CastNode *)e)->exp;
            continue;
        case BorrowTag:
        case ArrayBorrowTag:
            e = ((RefNode *)e)->vtexp;
            continue;
        case DerefTag:
            e = ((StarNode *)e)->vtexp;
            continue;
        case FldAccessTag:
            e = ((FnCallNode *)e)->objfn;
            continue;
        case ArrIndexTag:
            e = ((FnCallNode *)e)->objfn;
            continue;
        case FnCallTag:
        {
            FnCallNode *call = (FnCallNode *)e;
            if (call->args == NULL || call->args->used == 0)
                return 0;
            INode *recv = nodesGet(call->args, 0);
            INode *rt = iexpGetTypeDcl(recv);
            if (rt->tag == RefTag && itypeGetTypeDcl(((RefNode *)rt)->vtexp) == cont)
                return 1;
            e = recv;
            continue;
        }
        default:
            return 0;
        }
    }
}

// ---- An owner as the container ----------------------------------------------------

int reshapeIsOwner(INode *cont) {
    return cont && (cont->tag == RefTag || cont->tag == VirtRefTag);
}

// A borrow into what an owning reference ('Uni', 'Rc', 'Arc', a program's region) points at
// is a borrow into a value the owner frees when it is stored over, as a list is when its
// block is replaced. The owner's type stands for it, one node for all the types that are
// the same (the inference keeps its answers by the node)
static INode **roConts = NULL;
static uint32_t roUsed = 0;
static uint32_t roCap = 0;

INode *reshapeOwnerCont(INode *ownertype) {
    for (uint32_t i = 0; i < roUsed; ++i) {
        if (roConts[i] == ownertype || itypeIsSame(roConts[i], ownertype))
            return roConts[i];
    }
    if (roUsed == roCap) {
        INode **old = roConts;
        roCap = roCap ? roCap * 2 : 64;
        roConts = (INode **)memAllocBlk(roCap * sizeof(INode *));
        if (old)
            memcpy(roConts, old, roUsed * sizeof(INode *));
    }
    roConts[roUsed++] = ownertype;
    return ownertype;
}

int reshapeStoreHits(INode *lval, INode *cont) {
    INode *stored = iexpGetTypeDcl(lval);
    if (stored != NULL && reshapeIsOwner(cont)) {
        // The owner replaced whole, or a value holding it by value or through owners alone
        return ((stored->tag == RefTag || stored->tag == VirtRefTag) && itypeIsSame(stored, cont))
            || ((stored->tag == StructTag || stored->tag == ArrayTag || stored->tag == TTupleTag
                || ((stored->tag == RefTag || stored->tag == VirtRefTag) && ((RefNode *)stored)->region
                    && itypeGetTypeDcl(((RefNode *)stored)->region) != borrowRef))
                && (reshapeReach(stored, cont, 0) & (ReachContained | ReachOwned)) != 0);
    }
    if (stored == NULL || stored == cont)
        return stored != NULL;
    if (stored->tag != StructTag && stored->tag != ArrayTag && stored->tag != TTupleTag
            && !((stored->tag == RefTag || stored->tag == ArrayRefTag) && ((RefNode *)stored)->region
                && itypeGetTypeDcl(((RefNode *)stored)->region) != borrowRef))
        return 0;
    // A value holding the other by value, or through owning references alone ('Uni', 'Rc',
    // 'Arc'): replaced whole, it takes the old one with it (the last owner's going frees it)
    if (reshapeReach(stored, cont, 0) & (ReachContained | ReachOwned))
        return 1;
    // An element that owns something (a String), replaced inside a collection: what the
    // old one owned is freed
    return !reshapeIsSum(cont) && stored->tag == StructTag && itypeNeedsFinal(stored)
        && reshapeReach(cont, stored, 0) != 0 && reshapeElementOf(lval, cont);
}

// ---- A call ----------------------------------------------------------------------

// What the callee may write through its parameter k: that parameter's own
// permission, which is what it was declared to need, not the argument's
static INode *reshapeParmType(FnDclNode *callee, uint32_t k, INode *arg) {
    if (callee && callee->vtype && callee->vtype->tag == FnSigTag && k < ((FnSigNode *)callee->vtype)->parms->used)
        return iexpGetTypeDcl(nodesGet(((FnSigNode *)callee->vtype)->parms, k));
    return iexpGetTypeDcl(arg);
}

// The function a call names, when it names one
static FnDclNode *reshapeCallee(FnCallNode *call) {
    INode *fnn = call->objfn;
    return isNameUseNode(fnn) && ((NameUseNode *)fnn)->dclnode && ((NameUseNode *)fnn)->dclnode->tag == FnDclTag
        ? (FnDclNode *)((NameUseNode *)fnn)->dclnode : NULL;
}

// A call no borrow is asked of: an intrinsic acts on the memory it is handed and reshapes no
// collection, an init builds a value that has no other name yet, and a call through a virtual
// reference marked '+ ShapeKeeping' is one the caller's borrows survive: the value behind it was
// held to reshape nothing it reaches through a shared mutable path where it became the reference
// (shapeKeepsVia)
static int reshapeExempt(FnCallNode *call, FnDclNode *callee) {
    return (callee && ((callee->dclinfo.facts & DclIntrinsic) || (callee->value && callee->value->tag == IntrinsicTag)
            || fnDclIsInit(callee)))
        || shapeKeepingCall(call);
}

static int reshapeCallBody(FnCallNode *call, FnDclNode *meth, int recvunique, INode *cont, INode *owner,
        INode *recvowner, ReshapeVerdict *verdict, VarDclNode **mine, int nmine, int skiprecv) {
    FnDclNode *callee = reshapeCallee(call);
    verdict->why = ReshapeNot;
    verdict->callee = callee;
    if (reshapeExempt(call, callee))
        return 0;
    uint32_t nargs = call->args ? call->args->used : 0;
    int visible = !(call->flags & FlagVDisp) && siVisible(callee);
    // 'skiprecv': the borrow is the receiver's own (an owner lent to its method as 'self'), which is
    // the value the method works on and no other name for the owner
    uint32_t first = skiprecv && nargs > 0 ? 1 : 0;

    // A call handed nothing it could write through (a read-only reference, a number)
    // can reshape only through a global: most calls, dismissed at once
    int capable = 0;
    for (uint32_t k = first; k < nargs && !capable; ++k) {
        INode *t = iexpGetTypeDcl(nodesGet(call->args, k));
        // A read-only reference to a value holding borrows can write through a 'var' one it holds
        INode *pointee = nmine > 0 && (t->tag == RefTag || t->tag == ArrayRefTag) ? itypeGetTypeDcl(((RefNode *)t)->vtexp) : NULL;
        capable = ((t->tag == RefTag || t->tag == ArrayRefTag) && (permGetFlags(((RefNode *)t)->perm) & MayWrite))
            || (pointee && pointee->tag == StructTag && ((StructNode *)pointee)->carriesborrow != CarriesBorrowNo)
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
            // The receiver is a field of one struct, the borrow is into a field of another (or an
            // element of a list that is no field of the receiver's struct): two values, whatever
            // paths reach the structs. The borrow's value is the receiver only if it is the same
            // struct's field (or a name or call result, which says nothing of where it is held)
            if (owner && recvowner && owner != recvowner && !itypeIsSame(owner, recvowner))
                writes = 0;
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
        // A virtual call's receiver ('&<mut Trait', a '&<mut fn' called) hides the type behind
        // it, and the trait method's declared 'self &mut Trait' names no fields: the call is
        // asked of the virtual reference it was made through, which may reach any container
        if (k == 0 && (call->flags & FlagVDisp) && at->tag == VirtRefTag && wt->tag == RefTag
                && (permGetFlags(((RefNode *)wt)->perm) & MayWrite))
            wt = at;
        unsigned bits;
        if (ba->tag == BorrowTag && at->tag == RefTag
                && reshapeUniqueLocalExcept(((RefNode *)ba)->vtexp, mine, nmine, cont)) {
            // The callee may write what the borrow lends only if its parameter says so: a
            // read-only one, through what the local holds by a 'mut' reference
            INode *wp = wt->tag == RefTag ? (((RefNode *)wt)->perm == unknownType ? (INode *)roPerm
                : itypeGetTypeDcl(((RefNode *)wt)->perm)) : NULL;
            bits = wp == (INode *)immPerm ? 0
                : wp == (INode *)roPerm ? (nmine > 0 ? reshapeReachOwnerMode(wt, cont, owner, 1, 1) & (ReachBorrowed | ReachVirtual) : 0)
                : reshapeReachOwnerMode(((RefNode *)at)->vtexp, cont, owner, 1, nmine > 0) & (ReachBorrowed | ReachVirtual);
        }
        else if (at->tag != RefTag && at->tag != ArrayRefTag && at->tag != VirtRefTag && at->tag != PtrTag)
            bits = reshapeReachOwnerMode(wt, cont, owner, 1, nmine > 0) & (ReachBorrowed | ReachVirtual);
        else
            bits = reshapeReachOwnerMode(wt, cont, owner, 1, nmine > 0);
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

int reshapeCall(FnCallNode *call, FnDclNode *meth, int recvunique, INode *cont, INode *owner, INode *recvowner,
        int globalview, ReshapeVerdict *verdict, VarDclNode **mine, int nmine, int skiprecv) {
    if (reshapeCallBody(call, meth, recvunique, cont, owner, recvowner, verdict, mine, nmine, skiprecv))
        return 1;
    // The borrow points into a 'var' global, or into something reached from one: a call that may
    // reshape the container's type through a global does, by the inference that judges every call
    // (shapeTypeWritesGlobal). A callee whose body cannot be read (extern, virtual, a function value,
    // another package's) may do anything. No summary is made of which global a function writes.
    if (globalview && !reshapeExempt(call, verdict->callee)) {
        FnDclNode *callee = verdict->callee;
        // Code that could be anything: a virtual call, a function value, a Cone function whose body
        // is not here, an 'extern' C function handed something that can reach anything callable (a
        // '&fn', a virtual reference, a closure an owner holds) that it may call back. Any other
        // 'extern' is asked as any call is, of what it is handed (the ordinary check above)
        int opaque = (call->flags & FlagVDisp) || callee == NULL || shapeExternHandsCallable(call, callee)
            || (!(callee->flags & FlagExtern) && !siVisible(callee));
        if (opaque || (!(callee->flags & FlagExtern) && shapeTypeWritesGlobal(callee, cont))) {
            verdict->why = globalview == 2 ? ReshapeGlobalMaybe : ReshapeGlobalView;
            return 1;
        }
    }
    return 0;
}
