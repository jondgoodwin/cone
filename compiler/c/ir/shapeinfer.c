/** Which types change shape, read from their methods
 *
 * shapeinfer.h says what the compiler takes a shape-changing type to be. This is
 * how it reads that from the typed bodies of a type's methods, and when.
 *
 * - siParamWrites asks of one function and one of its parameters whether the
 *   function's body (transitively) writes storage through that parameter. It is
 *   memoised; a function in the middle of being asked about, reached again by a
 *   recursion, answers no, and an answer that leaned on such a guess is not kept.
 * - siInferStruct combines the answers over a type's methods.
 * - siSettle caches the answer on the type, asking for the bodies it reads to be
 *   type checked first where the caller is outside any loan walk.
 * - The queue of loan walks that had to wait for an answer, run at the end of type
 *   check, and the check of the types that declare 'ShapeChanging'.
 *
 * Every answer is yes, no, or not yet: a body that is not type checked yet (it is
 * suspended further up the stack, waiting on a name it demanded) can neither
 * confirm nor rule out a write. A write found elsewhere is yes whatever else is
 * not yet known.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../shared/timer.h"

#include <string.h>

enum SiAnswer {
    SiNo,
    SiYes,
    SiPending
};

// ---- What a type holds that a write could free -------------------------------

#define SiSeenCap 64
typedef struct {
    INode *seen[SiSeenCap];
    int nseen;
} SiSeen;

// Does a value of this type hold storage it owns: a raw pointer, or an owning
// reference (an owner's drop frees what it points at), by containment? A borrow
// holds none. Replacing such a value, or writing it, ends the life of that storage
static int siHoldsStorage(INode *type, SiSeen *seen) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? siHoldsStorage(itypeGetTypeDcl(type), seen) : 0;
    case AliasDclTag:
        return siHoldsStorage(((AliasDclNode *)type)->target, seen);
    case PtrTag:
        return 1;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        return ref->region != NULL && regionIsOwning(ref->region);
    }
    case ArrayTag:
        return siHoldsStorage(arrayElemType(type), seen);
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (siHoldsStorage(*nodesp, seen))
                return 1;
        }
        return 0;
    }
    case StructTag:
    {
        for (int i = 0; i < seen->nseen; ++i) {
            if (seen->seen[i] == type)
                return 0;
        }
        if (seen->nseen >= SiSeenCap)
            return 1;       // too large a type graph to say: taken as holding some
        seen->seen[seen->nseen++] = type;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp)) {
            if (!((*nodesp)->flags & IsTagField) && siHoldsStorage(((IExpNode *)*nodesp)->vtype, seen))
                return 1;
        }
        if (((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp)) {
                if (siHoldsStorage(*nodesp, seen))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int siTypeHoldsStorage(INode *type) {
    SiSeen seen;
    seen.nseen = 0;
    return siHoldsStorage(type, &seen);
}

// ---- Places ----------------------------------------------------------------

static VarDclNode *siNamedVar(INode *node) {
    if (!(isNameUseNode(node) && isExpNode(node)))
        return NULL;
    INode *dcl = ((NameUseNode *)node)->dclnode;
    return dcl && dcl->tag == VarDclTag ? (VarDclNode *)dcl : NULL;
}

#define SiTaintMax 16
#define SiAliasMax 8

// What is asked of one function's body: whether it writes storage through
// parameter 'parm' (what it points at, when it is a reference)
typedef struct {
    VarDclNode *parm;       // the parameter whose pointee is asked about
    int isref;              // it is a reference: a write through it reaches the caller's value
    int hit;                // a write found
    int pending;            // a body that could settle it is not type checked yet
    VarDclNode *taint[SiTaintMax];  // locals holding a copy of storage read from the parameter's pointee
    int ntaint;
    VarDclNode *alias[SiAliasMax];  // locals holding a reference to a place of the parameter's pointee
    int nalias;
} SiCtx;

// Is this variable the parameter, or a local that holds a reference to a place of
// what the parameter points at?
static int siIsRoot(SiCtx *c, VarDclNode *var) {
    if (var == NULL)
        return 0;
    if (var == c->parm)
        return 1;
    for (int i = 0; i < c->nalias; ++i) {
        if (c->alias[i] == var)
            return 1;
    }
    return 0;
}

// Is 'e' a place inside what the parameter points at, reached by field steps and
// by-value array elements alone, or the parameter itself?
static int siRooted(INode *e, SiCtx *c) {
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
        {
            INode *in = ((StarNode *)e)->vtexp;
            while (in->tag == CastTag && !(in->flags & FlagConvert))
                in = ((CastNode *)in)->exp;
            return siIsRoot(c, siNamedVar(in));
        }
        case FldAccessTag:
            e = ((FnCallNode *)e)->objfn;
            continue;
        case ArrIndexTag:
        {
            INode *obj = ((FnCallNode *)e)->objfn;
            if (iexpGetTypeDcl(obj)->tag != ArrayTag)
                return 0;
            e = obj;
            continue;
        }
        default:
            return siIsRoot(c, siNamedVar(e));
        }
    }
}

// ---- Reading a body ---------------------------------------------------------

typedef int (*SiVisitFn)(INode *node, void *ctx);

static int siVisit(INode *node, SiVisitFn fn, void *ctx);

static int siVisitNodes(Nodes *nodes, SiVisitFn fn, void *ctx) {
    if (nodes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp)) {
        if (siVisit(*nodesp, fn, ctx))
            return 1;
    }
    return 0;
}

// Every node of an expression tree, each before what it holds, as the tree
// check (checktree.c) reaches them. 'fn' answering nonzero stops the walk.
static int siVisit(INode *node, SiVisitFn fn, void *ctx) {
    if (node == NULL)
        return 0;
    if (fn(node, ctx))
        return 1;
    switch (node->tag) {
    case BlockTag:
        return siVisitNodes(((BlockNode *)node)->stmts, fn, ctx);
    case IfTag:
        return siVisitNodes(((IfNode *)node)->condblk, fn, ctx);
    case BreakTag:
    case ContinueTag:
    case BlockRetTag:
    case ReturnTag:
        return siVisit(((BreakRetNode *)node)->exp, fn, ctx);
    case VarDclTag:
        return siVisit(((VarDclNode *)node)->value, fn, ctx);
    case AssignTag:
        return siVisit(((AssignNode *)node)->lval, fn, ctx) || siVisit(((AssignNode *)node)->rval, fn, ctx);
    case SwapTag:
        return siVisit(((SwapNode *)node)->lval, fn, ctx) || siVisit(((SwapNode *)node)->rval, fn, ctx);
    case VTupleTag:
        return siVisitNodes(((TupleNode *)node)->elems, fn, ctx);
    case ArrayLitTag:
        return siVisitNodes(((ArrayNode *)node)->elems, fn, ctx);
    case FnCallTag:
    case ArrIndexTag:
    case FldAccessTag:
    case TypeLitTag:
        return siVisit(((FnCallNode *)node)->objfn, fn, ctx) || siVisitNodes(((FnCallNode *)node)->args, fn, ctx);
    case CastTag:
    case IsTag:
        return siVisit(((CastNode *)node)->exp, fn, ctx);
    case DerefTag:
        return siVisit(((StarNode *)node)->vtexp, fn, ctx);
    case BorrowTag:
    case ArrayBorrowTag:
    case AllocateTag:
        return siVisit(((RefNode *)node)->vtexp, fn, ctx);
    case NotLogicTag:
        return siVisit(((LogicNode *)node)->lexp, fn, ctx);
    case OrLogicTag:
    case AndLogicTag:
        return siVisit(((LogicNode *)node)->lexp, fn, ctx) || siVisit(((LogicNode *)node)->rexp, fn, ctx);
    case AwaitTag:
        return siVisit(((AwaitNode *)node)->exp, fn, ctx);
    case NamedValTag:
        return siVisit(((NamedValNode *)node)->val, fn, ctx);
    case OfEntryTag:
    case FillEntryTag:
    case PairEntryTag:
        return siVisit(((EntryNode *)node)->first, fn, ctx) || siVisit(((EntryNode *)node)->val, fn, ctx);
    case RefCountTag:
        return siVisit(((RefCountNode *)node)->exp, fn, ctx);
    case HollowTag:
        return siVisit(((HollowNode *)node)->exp, fn, ctx);
    case DropFlagTag:
        return siVisit(((DropFlagNode *)node)->release, fn, ctx);
    case TempTag:
        return siVisit(((TempNode *)node)->exp, fn, ctx);
    default:
        return 0;
    }
}

// ---- Does a function write storage through one of its parameters? -------------

// Can the compiler read what this function does: it has a body here, which is a
// call of this very function and not of one of a trait's (a virtual call)?
static int siVisible(FnDclNode *fn) {
    if (fn == NULL || fn->genericinfo != NULL || (fn->flags & FlagExtern) || (fn->dclinfo.facts & DclIntrinsic)
        || fn->value == NULL || fn->value->tag != BlockTag)
        return 0;
    INode *owner = inodeGetOwner((INode *)fn);
    return !(owner && owner->tag == StructTag && (owner->flags & TraitType));
}

static int siBodyReady(FnDclNode *fn) {
    return (fn->dclinfo.facts & DclBodyTyped) != 0;
}

static int siParamWrites(FnDclNode *fn, uint32_t k);

static int siTainted(SiCtx *c, VarDclNode *var) {
    for (int i = 0; i < c->ntaint; ++i) {
        if (c->taint[i] == var)
            return 1;
    }
    return 0;
}

// Does this expression read storage out of the parameter's pointee, or out of a
// local that holds a copy of it: a pointer, or a value holding one?
static int siReadsStorage(INode *e, SiCtx *c) {
    if (e == NULL)
        return 0;
    if (siRooted(e, c)) {
        INode *t = iexpGetTypeDcl(e);
        return t != NULL && siTypeHoldsStorage(t);
    }
    VarDclNode *var = siNamedVar(e);
    if (var != NULL)
        return siTainted(c, var);
    switch (e->tag) {
    case CastTag:
        return siReadsStorage(((CastNode *)e)->exp, c);
    case BorrowTag:
    case ArrayBorrowTag:
        return siReadsStorage(((RefNode *)e)->vtexp, c);
    case DerefTag:
        return siReadsStorage(((StarNode *)e)->vtexp, c);
    case FldAccessTag:
        return siReadsStorage(((FnCallNode *)e)->objfn, c);
    case FnCallTag:
    case ArrIndexTag:
    {
        FnCallNode *call = (FnCallNode *)e;
        INode **argsp;
        uint32_t cnt;
        if (siReadsStorage(call->objfn, c))
            return 1;
        if (call->args) {
            for (nodesFor(call->args, cnt, argsp)) {
                if (siReadsStorage(*argsp, c))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

// Does a write to this place write storage of the parameter's pointee: a place in
// it, or all of it, that holds storage?
static int siWriteHits(INode *lval, SiCtx *c) {
    if (!c->isref)
        return 0;
    if (lval->tag == VTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)lval)->elems, cnt, nodesp)) {
            if (siWriteHits(*nodesp, c))
                return 1;
        }
        return 0;
    }
    if (!siRooted(lval, c))
        return 0;
    INode *t = iexpGetTypeDcl(lval);
    return t != NULL && siTypeHoldsStorage(t);
}

// May the callee write through its parameter 'k'?
static int siParmWritable(FnDclNode *callee, uint32_t k, INode *arg) {
    INode *ptype;
    if (callee && callee->vtype && callee->vtype->tag == FnSigTag && k < ((FnSigNode *)callee->vtype)->parms->used)
        ptype = iexpGetTypeDcl(nodesGet(((FnSigNode *)callee->vtype)->parms, k));
    else
        ptype = iexpGetTypeDcl(arg);
    return ptype && ptype->tag == RefTag && (permGetFlags(((RefNode *)ptype)->perm) & MayWrite);
}

// A call: each argument that reaches the parameter's pointee as a place the
// callee may write, or as storage handed on by value, is the callee's to answer
// for. A callee the compiler cannot read is taken to write.
static void siCall(FnCallNode *call, SiCtx *c) {
    INode *fnn = call->objfn;
    FnDclNode *callee = isNameUseNode(fnn) && ((NameUseNode *)fnn)->dclnode && ((NameUseNode *)fnn)->dclnode->tag == FnDclTag
        ? (FnDclNode *)((NameUseNode *)fnn)->dclnode : NULL;
    uint32_t nargs = call->args ? call->args->used : 0;
    if ((call->flags & FlagLvalOp) && nargs > 0 && siWriteHits(nodesGet(call->args, 0), c)) {
        c->hit = 1;
        return;
    }
    int intrinsic = callee && ((callee->dclinfo.facts & DclIntrinsic) || (callee->value && callee->value->tag == IntrinsicTag));
    int visible = !(call->flags & FlagVDisp) && siVisible(callee);
    for (uint32_t k = 0; k < nargs && !c->hit; ++k) {
        INode *arg = nodesGet(call->args, k);
        int rooted = siRooted(arg, c);
        int by = 0;     // the callee is asked about its parameter k
        if (rooted && c->isref && siParmWritable(callee, k, arg)) {
            // A place of the pointee lent to be written
            if (intrinsic) {
                INode *at = iexpGetTypeDcl(arg);
                if (at->tag == RefTag && siTypeHoldsStorage(((RefNode *)at)->vtexp))
                    c->hit = 1;
                continue;
            }
            by = 1;
        }
        else if (!intrinsic && siReadsStorage(arg, c))
            by = 1;     // storage handed on by value: a free or a realloc, in a callee
        if (!by)
            continue;
        if (!visible)
            c->hit = 1;
        else {
            int answer = siParamWrites(callee, k);
            if (answer == SiYes)
                c->hit = 1;
            else if (answer == SiPending)
                c->pending = 1;
        }
    }
}

// A local is given a value: a reference to a place of the parameter's pointee
// makes it another name for that place, and a copy of storage read from it makes
// it hold that storage. What is then done through it is done to the parameter's
static void siNote(VarDclNode *var, INode *value, SiCtx *c) {
    if (var == NULL || value == NULL || siIsRoot(c, var) || siTainted(c, var))
        return;
    INode *vt = iexpGetTypeDcl((INode *)var);
    if (vt != NULL && vt->tag == RefTag) {
        if (c->nalias < SiAliasMax && siRooted(value, c))
            c->alias[c->nalias++] = var;
    }
    else if (c->ntaint < SiTaintMax && siReadsStorage(value, c))
        c->taint[c->ntaint++] = var;
}

static int siWalkNode(INode *node, void *vc) {
    SiCtx *c = (SiCtx *)vc;
    switch (node->tag) {
    case AssignTag:
        if (siWriteHits(((AssignNode *)node)->lval, c))
            c->hit = 1;
        else
            siNote(siNamedVar(((AssignNode *)node)->lval), ((AssignNode *)node)->rval, c);
        break;
    case SwapTag:
        if (siWriteHits(((SwapNode *)node)->lval, c) || siWriteHits(((SwapNode *)node)->rval, c))
            c->hit = 1;
        break;
    case VarDclTag:
        siNote((VarDclNode *)node, ((VarDclNode *)node)->value, c);
        break;
    case FnCallTag:
        siCall((FnCallNode *)node, c);
        break;
    default:
        break;
    }
    return c->hit;
}

typedef struct {
    FnDclNode *fn;
    uint32_t k;
    uint8_t state;      // SiMemoBusy or SiMemoDone, else a key held for another try
    uint8_t answer;
    uint32_t depth;
} SiMemo;

enum { SiMemoFree, SiMemoBusy, SiMemoDone };

static SiMemo *siMemo = NULL;
static uint32_t siMemoCap = 0;
static uint32_t siMemoUsed = 0;
static uint32_t siDepth = 0;        // how many functions are being asked about
static uint32_t siGuess = UINT32_MAX;   // the shallowest function a guess of 'no' was made for

static uint32_t siMemoHash(FnDclNode *fn, uint32_t k) {
    uint64_t h = ((uint64_t)(uintptr_t)fn * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)k * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)(h >> 32);
}

static SiMemo *siMemoFind(FnDclNode *fn, uint32_t k) {
    if ((siMemoUsed + 1) * 2 > siMemoCap) {
        SiMemo *old = siMemo;
        uint32_t oldcap = siMemoCap;
        siMemoCap = oldcap ? oldcap * 2 : 1024;
        siMemo = (SiMemo *)memAllocBlk(siMemoCap * sizeof(SiMemo));
        memset(siMemo, 0, siMemoCap * sizeof(SiMemo));
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (old[i].fn == NULL)
                continue;
            uint32_t at = siMemoHash(old[i].fn, old[i].k) & (siMemoCap - 1);
            while (siMemo[at].fn != NULL)
                at = (at + 1) & (siMemoCap - 1);
            siMemo[at] = old[i];
        }
    }
    uint32_t at = siMemoHash(fn, k) & (siMemoCap - 1);
    while (siMemo[at].fn != NULL) {
        if (siMemo[at].fn == fn && siMemo[at].k == k)
            return &siMemo[at];
        at = (at + 1) & (siMemoCap - 1);
    }
    siMemo[at].fn = fn;
    siMemo[at].k = k;
    siMemo[at].state = SiMemoFree;
    ++siMemoUsed;
    return &siMemo[at];
}

// Does the body of 'fn' write storage through its parameter 'k' (what it points
// at, when it is a reference), or hand storage read from it to code that may free
// it, directly or through what it calls?
static int siParamWrites(FnDclNode *fn, uint32_t k) {
    if (!siVisible(fn))
        return SiYes;
    if (!siBodyReady(fn))
        return SiPending;
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    if (k >= sig->parms->used)
        return SiYes;
    SiMemo *m = siMemoFind(fn, k);
    if (m->state == SiMemoDone)
        return m->answer;
    if (m->state == SiMemoBusy) {
        // A recursion: no write found on the way round, which holds only if
        // nothing else in the cycle writes either
        if (m->depth < siGuess)
            siGuess = m->depth;
        return SiNo;
    }
    uint32_t depth = ++siDepth;
    m->state = SiMemoBusy;
    m->depth = depth;
    uint32_t savedGuess = siGuess;
    siGuess = UINT32_MAX;

    SiCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.parm = (VarDclNode *)nodesGet(sig->parms, k);
    INode *ptype = iexpGetTypeDcl((INode *)ctx.parm);
    ctx.isref = ptype != NULL && ptype->tag == RefTag;
    siVisit(fn->value, siWalkNode, &ctx);
    int answer = ctx.hit ? SiYes : ctx.pending ? SiPending : SiNo;

    --siDepth;
    m = siMemoFind(fn, k);      // the table may have grown
    if (answer == SiNo && siGuess < depth) {
        // It leaned on a guess about a function still being asked about: asked again
        m->state = SiMemoFree;
        siGuess = siGuess < savedGuess ? siGuess : savedGuess;
    }
    else {
        if (answer == SiPending)
            m->state = SiMemoFree;
        else {
            m->state = SiMemoDone;
            m->answer = (uint8_t)answer;
        }
        siGuess = savedGuess;
    }
    return answer;
}

// ---- Does a type change shape? ----------------------------------------------

static int siIsRecorded(StructNode *st);

static void siMethod(FnDclNode *fn, int *lender, int *writer, int *sigpending, int *bodypending) {
    if (!(fn->flags & FlagMethFld) || fn->vtype == NULL || fn->vtype->tag != FnSigTag)
        return;
    // Its signature is checked once the declaration is under way
    if (!(fn->flags & (TypeChecked | TypeChecking))) {
        *sigpending = 1;
        return;
    }
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    if (sig->parms->used == 0 || ((VarDclNode *)nodesGet(sig->parms, 0))->namesym != selfName)
        return;
    INode *selft = iexpGetTypeDcl(nodesGet(sig->parms, 0));
    if (selft->tag != RefTag)
        return;
    if (sig->rettype && itypeCarriesBorrow(sig->rettype))
        *lender = 1;
    // A value's own death frees its storage, and no borrow of it outlives that
    if (!(permGetFlags(((RefNode *)selft)->perm) & MayWrite) || fnDclIsInit(fn)
        || fn->namesym == finalName || fn->namesym == typeDropName || fn->namesym == enumFinalName)
        return;
    int answer = siParamWrites(fn, 0);
    if (answer == SiYes)
        *writer = 1;
    else if (answer == SiPending)
        *bodypending = 1;
}

static int siInferStruct(StructNode *st) {
    int lender = 0, writer = 0, sigpending = 0, bodypending = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&st->nodelist, cnt, nodesp)) {
        INode *m = *nodesp;
        if (m->tag == FnDclTag)
            siMethod((FnDclNode *)m, &lender, &writer, &sigpending, &bodypending);
        else if (m->tag == FnOverloadDclTag) {
            INode **op;
            uint32_t oc;
            for (nodesFor(((FnOverloadDclNode *)m)->overloads, oc, op)) {
                if ((*op)->tag == FnDclTag)
                    siMethod((FnDclNode *)*op, &lender, &writer, &sigpending, &bodypending);
            }
        }
    }
    if (lender && writer)
        return SiYes;
    if (!sigpending && (!lender || !bodypending))
        return SiNo;
    return SiPending;
}

// Ask for every method of the type to be type checked: reaching a name does so,
// and this reaches them all. Only where nothing is mid-way through a layout, which
// members must wait for.
static void siDemandMethods(StructNode *st) {
    if (structLayoutInFlight())
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&st->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag || (*nodesp)->tag == FnOverloadDclTag)
            fnCallDemandCandidates(*nodesp);
    }
}

// A type of a package whose include file is read here, and not generic: its
// methods have no bodies in this compile, and the file said what the package's
// own compile found (incfile.c)
static int siIsRecorded(StructNode *st) {
    return st->lexer != NULL && (st->lexer->flags & LexLineMarks) && !dclIsInstance((INode *)st)
        && !(st->dclinfo.facts & DclActorGen);
}

static int siSettle(StructNode *st, int demand) {
    if (st->lends == LendsShapeChanging || st->shapeinf == ShapeYes)
        return SiYes;
    if (st->lends != LendsLoaned || st->shapeinf == ShapeNo || (st->flags & (TraitType | EnumType)))
        return SiNo;
    if (st->shapeinf == ShapeAsking || !(st->flags & TypeChecked))
        return SiPending;
    if (siIsRecorded(st)) {
        st->shapeinf = ShapeNo;
        return SiNo;
    }
    st->shapeinf = ShapeAsking;
    if (demand)
        siDemandMethods(st);
    int answer = siInferStruct(st);
    st->shapeinf = answer == SiYes ? ShapeYes : answer == SiNo ? ShapeNo : ShapeUnknown;
    return answer;
}

int shapeChanging(StructNode *st) {
    return siSettle(st, 0) != SiNo;
}

int shapeRecordable(StructNode *st) {
    if (st->lends != LendsLoaned || (st->flags & (TraitType | EnumType)) || dclIsInstance((INode *)st)
        || siIsRecorded(st))
        return 0;
    return siSettle(st, 0) == SiYes;
}

// ---- Settling what a loan walk will ask --------------------------------------

#define SiNeedMax 32

typedef struct {
    StructNode *need[SiNeedMax];
    int nneed;
} SiNeeds;

// A call of a method that returns a borrow, on a type that declares nothing
// about its shape: the loan walk asks (pwCall) whether it is shape-changing
static int siNeedNode(INode *node, void *vc) {
    if (node->tag != FnCallTag)
        return 0;
    SiNeeds *needs = (SiNeeds *)vc;
    FnCallNode *call = (FnCallNode *)node;
    if (call->args == NULL || call->args->used == 0 || !isNameUseNode(call->objfn))
        return 0;
    INode *fn = ((NameUseNode *)call->objfn)->dclnode;
    if (fn == NULL || fn->tag != FnDclTag || !(fn->flags & FlagMethFld) || ((FnDclNode *)fn)->vtype == NULL
        || ((FnDclNode *)fn)->vtype->tag != FnSigTag)
        return 0;
    FnSigNode *sig = (FnSigNode *)((FnDclNode *)fn)->vtype;
    if (sig->parms->used == 0 || ((VarDclNode *)nodesGet(sig->parms, 0))->namesym != selfName
        || sig->rettype == NULL || !itypeCarriesBorrow(sig->rettype))
        return 0;
    INode *recvtype = iexpGetTypeDcl(nodesGet(call->args, 0));
    INode *container = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode *)recvtype)->vtexp) : NULL;
    if (container == NULL || container->tag != StructTag || ((StructNode *)container)->lends != LendsLoaned)
        return 0;
    for (int i = 0; i < needs->nneed; ++i) {
        if (needs->need[i] == (StructNode *)container)
            return 0;
    }
    if (needs->nneed < SiNeedMax)
        needs->need[needs->nneed++] = (StructNode *)container;
    return 0;
}

int shapeWalkReady(FnDclNode *fn, int maydefer) {
    SiNeeds needs;
    needs.nneed = 0;
    siVisit(fn->value, siNeedNode, &needs);
    int settled = 1;
    for (int i = 0; i < needs.nneed; ++i) {
        if (siSettle(needs.need[i], 1) == SiPending)
            settled = 0;
    }
    return settled || !maydefer;
}

// ---- Walks that waited ---------------------------------------------------------

typedef struct {
    FnDclNode *fn;
    int drops;
} SiDeferred;

static SiDeferred *siQueue = NULL;
static uint32_t siQueueUsed = 0;
static uint32_t siQueueCap = 0;

void shapeWalkDefer(FnDclNode *fn, int drops) {
    if (siQueueUsed == siQueueCap) {
        SiDeferred *old = siQueue;
        siQueueCap = siQueueCap ? siQueueCap * 2 : 64;
        siQueue = (SiDeferred *)memAllocBlk(siQueueCap * sizeof(SiDeferred));
        if (old)
            memcpy(siQueue, old, siQueueUsed * sizeof(SiDeferred));
    }
    siQueue[siQueueUsed].fn = fn;
    siQueue[siQueueUsed].drops = drops;
    ++siQueueUsed;
}

void shapeWalkDeferred(void) {
    // A walk made may check a method on the way, which queues another
    for (uint32_t i = 0; i < siQueueUsed; ++i) {
        FnDclNode *fn = siQueue[i].fn;
        int drops = siQueue[i].drops;
        shapeWalkReady(fn, 0);
        size_t svTimer = timerCurrent;
        if (timerFine)
            timerBegin(FlowTimer);
        flowPathWalk(fn, 1, drops, 0);
        if (timerFine)
            timerBegin(svTimer);
    }
    siQueueUsed = 0;
}

// ---- Types that declare 'ShapeChanging' ----------------------------------------

static StructNode **siDeclared = NULL;
static uint32_t siDeclaredUsed = 0;
static uint32_t siDeclaredCap = 0;

void shapeDeclared(StructNode *st) {
    if (st->genericinfo != NULL || (st->flags & (TraitType | EnumType)))
        return;
    if (siDeclaredUsed == siDeclaredCap) {
        StructNode **old = siDeclared;
        siDeclaredCap = siDeclaredCap ? siDeclaredCap * 2 : 32;
        siDeclared = (StructNode **)memAllocBlk(siDeclaredCap * sizeof(StructNode *));
        if (old)
            memcpy(siDeclared, old, siDeclaredUsed * sizeof(StructNode *));
    }
    siDeclared[siDeclaredUsed++] = st;
}

void shapeDeclaredCheck(void) {
    for (uint32_t i = 0; i < siDeclaredUsed; ++i) {
        StructNode *st = siDeclared[i];
        // A package's include file carries the type without the bodies that
        // would show it; its own compile checked them
        if (siIsRecorded(st))
            continue;
        if (siInferStruct(st) == SiNo)
            errorMsgNode((INode *)st, ErrorShapeMark,
                "%s declares ShapeChanging, but the compiler finds nothing in its methods that makes it so: it is shape-changing when one method writes storage it owns (a pointer, or a field holding one) and one lends a borrow. A declaration only asserts what the compiler finds, so remove it, or make the methods say it.",
                &st->namesym->namestr);
    }
    siDeclaredUsed = 0;
}
