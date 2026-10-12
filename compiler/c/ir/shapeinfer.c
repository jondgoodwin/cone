/** Which types change shape, read from their methods
 *
 * shapeinfer.h says what the compiler takes a shape-changing type to be. This is
 * how it reads that from the typed bodies of a type's methods, and when.
 *
 * - siAsk asks of one function, in one of four modes (SiMode), whether its body
 *   (transitively) writes storage through one of its parameters (siParamWrites:
 *   what makes a type shape-changing), writes any field of what a parameter points
 *   at, or reshapes a value of a collection type that is not its own local, through
 *   a parameter or a global or only through a global. The last three are what
 *   reshape.h asks of a call (shapeParamReshapes, shapeTypeReshapes). It is
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

#include <stdio.h>
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

VarDclNode *siNamedVar(INode *node) {
    if (!(isNameUseNode(node) && isExpNode(node)))
        return NULL;
    INode *dcl = ((NameUseNode *)node)->dclnode;
    return dcl && dcl->tag == VarDclTag ? (VarDclNode *)dcl : NULL;
}

#define SiTaintMax 16
#define SiAliasMax 8
#define SiHeldMax 4

// What is asked of one function's body, in one of four modes. The first is
// whether it writes storage through parameter 'parm' (what it points at, when it
// is a reference): what makes a type shape-changing. The second is whether it
// writes any field of what the parameter points at (reshape.h: layer 2 of the
// check of a call). The last two ask of a function with no parameter in mind
// whether it reshapes a value of the collection type 'cont' that is not its own
// local, through a parameter or not (SiModeType), or only through something that
// is no parameter of its own, a global (SiModeOut).
enum SiMode {
    SiModeStorage,
    SiModeField,
    SiModeType,
    SiModeOut,
    // SiModeType and SiModeOut, for a borrow into a 'mut' global (shapeTypeWritesGlobal): a call
    // that could run any code (through a virtual reference or a function pointer) is taken to write
    // any global, whatever it is handed
    SiModeTypeG,
    SiModeOutG
};

typedef struct {
    int mode;               // SiMode, SiModeTypeG and SiModeOutG as SiModeType and SiModeOut
    int opaque;             // asked in one of those: a call into unknown code is taken to write a global
    INode *cont;            // type modes: the collection's type
    VarDclNode *parm;       // the parameter whose pointee is asked about
    int isref;              // it is a reference: a write through it reaches the caller's value
    INode *pointee;         // then, the type it points at
    int hit;                // a write found
    INode *hitnode;         // the node of this body that made it (a message names it)
    int pending;            // a body that could settle it is not type checked yet
    VarDclNode *taint[SiTaintMax];  // locals holding a copy of storage read from the parameter's pointee
    int ntaint;
    VarDclNode *alias[SiAliasMax];  // locals holding a reference to a place of the parameter's pointee
    int nalias;
    // SiModeField: an element read out of the storage (readRaw) into a local, which
    // leaves it unless the local is only written back (writeRaw), as a swap does
    VarDclNode *held[SiHeldMax];
    int heldwrites[SiHeldMax];
    int helduses[SiHeldMax];
    int nheld;
    INode *consumed[SiHeldMax];     // readRaw calls whose result is written back in place
    int nconsumed;
} SiCtx;

// Is this variable the parameter, or a local that holds a reference to a place of
// what the parameter points at? In SiModeOut, which asks of no parameter: a global
// (or a static), or a local holding a reference to a place of one
static int siIsRoot(SiCtx *c, VarDclNode *var) {
    if (var == NULL)
        return 0;
    if (var == c->parm)
        return 1;
    if (c->mode == SiModeOut && (var->scope == 0 || (var->flags & FlagStatic)))
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
int siVisit(INode *node, SiVisitFn fn, void *ctx) {
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
    case YieldTag:
        return siVisit(((YieldNode *)node)->exp, fn, ctx);
    case NamedValTag:
        return siVisit(((NamedValNode *)node)->val, fn, ctx);
    case OfEntryTag:
    case FillEntryTag:
    case PairEntryTag:
    case EachEntryTag:
    case YieldEntryTag:
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
int siVisible(FnDclNode *fn) {
    if (fn == NULL || fn->genericinfo != NULL || (fn->flags & FlagExtern) || (fn->dclinfo.facts & DclIntrinsic)
        || fn->value == NULL || fn->value->tag != BlockTag)
        return 0;
    INode *owner = inodeGetOwner((INode *)fn);
    return !(owner && owner->tag == StructTag && (owner->flags & TraitType));
}

int siBodyReady(FnDclNode *fn) {
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
    // Asked of an enum (a method taking it by reference): only storing over the whole
    // value reshapes it. A common field, or a field of a variant, is written within the
    // variant
    if (c->mode == SiModeField && reshapeIsSum(c->pointee))
        return iexpGetTypeDcl(lval) == c->pointee;
    if (c->mode == SiModeField)
        return 1;
    INode *t = iexpGetTypeDcl(lval);
    return t != NULL && siTypeHoldsStorage(t);
}

// ---- The type modes: a function that reshapes a value of one type --------------

// Is the place written, or some value it is inside, of the collection's type: its
// whole replaced, or a field of its header written?
static int siPlaceInCont(INode *e, INode *cont) {
    for (;;) {
        if (iexpGetTypeDcl(e) == cont)
            return 1;
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
            if (iexpGetTypeDcl(((FnCallNode *)e)->objfn)->tag != ArrayTag)
                return 0;
            e = ((FnCallNode *)e)->objfn;
            continue;
        default:
            return 0;
        }
    }
}

// A write at this place, in a type mode: a place of the collection's type, that is
// not a local of the function's own, and, in SiModeOut, one reached from a global
static int siTypeWriteHits(INode *lval, SiCtx *c) {
    if (lval->tag == VTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)lval)->elems, cnt, nodesp)) {
            if (siTypeWriteHits(*nodesp, c))
                return 1;
        }
        return 0;
    }
    if (reshapeUniqueLocalOf(lval, c->cont))
        return 0;
    // The place's value stored over: the collection or enum itself, a value holding one
    // by value, or an element that owns what a borrow may point at (reshapeStoreHits);
    // or, for a collection, one of its header fields written. An enum's common field,
    // or a field of its variant, is written within the variant: no reshape
    if (!reshapeStoreHits(lval, c->cont)
            && (reshapeIsSum(c->cont) || reshapeIsOwner(c->cont) || !siPlaceInCont(lval, c->cont)))
        return 0;
    return c->mode != SiModeOut || siRooted(lval, c);
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
static int siAsk(FnDclNode *fn, uint32_t k, int mode, INode *cont);

// ---- Elements that end or leave: SiModeField's other two tests ------------------
// A method reshapes a collection when it writes a field of it, when it runs a
// finalizer of an element (the storage then holds a value that is dead, and what the
// element owned is freed: a borrow into a String element's bytes dangles), or when
// it moves an element out of the storage. Writing an element in place, and moving
// elements about within the storage (a swap, a sort), end none and move none out.

// Which of the compiler's declared intrinsics (mem.finalize, mem.readRaw, ...) a
// function is, or -1; its type argument comes back too
static int siIntrinsicOf(FnDclNode *callee, INode **typearg) {
    if (callee == NULL || callee->value == NULL || callee->value->tag != IntrinsicTag)
        return -1;
    *typearg = ((IntrinsicNode *)callee->value)->typearg;
    return ((IntrinsicNode *)callee->value)->intrinsicFn;
}

static INode *siStripCasts(INode *e) {
    while (e->tag == CastTag && !(e->flags & FlagConvert))
        e = ((CastNode *)e)->exp;
    return e;
}

// 'e' is a call of mem.readRaw on storage read from the parameter's pointee, of a
// type that owns something (a plain number or struct moved out leaves nothing a
// borrow could be of: no finalizer will run for it, and its bytes stay where they are)
static FnCallNode *siReadRawCall(INode *e, SiCtx *c) {
    e = siStripCasts(e);
    if (e->tag != FnCallTag)
        return NULL;
    FnCallNode *call = (FnCallNode *)e;
    if (!isNameUseNode(call->objfn))
        return NULL;
    INode *dcl = ((NameUseNode *)call->objfn)->dclnode;
    INode *typearg = NULL;
    if (dcl == NULL || dcl->tag != FnDclTag || siIntrinsicOf((FnDclNode *)dcl, &typearg) != ReadRawIntrinsic)
        return NULL;
    if (typearg != NULL && !itypeNeedsFinal(typearg))
        return NULL;
    return call->args && call->args->used > 0 && siReadsStorage(nodesGet(call->args, 0), c) ? call : NULL;
}

// A call of mem.finalize, mem.readRaw or mem.writeRaw in SiModeField. Returns 1
// when the call is one of them (so it is judged here)
static int siElementCall(FnCallNode *call, FnDclNode *callee, SiCtx *c) {
    INode *typearg = NULL;
    int which = siIntrinsicOf(callee, &typearg);
    uint32_t nargs = call->args ? call->args->used : 0;
    if (which == FinalizeIntrinsic) {
        // Of an element: a pointer into the storage. An element type with nothing to run
        // when it dies (a number, a struct of numbers) finalizes nothing
        if (nargs > 0 && siReadsStorage(nodesGet(call->args, 0), c) && (typearg == NULL || itypeNeedsFinal(typearg)))
            c->hit = 1;
        return 1;
    }
    if (which == WriteRawIntrinsic) {
        if (nargs > 1) {
            INode *value = siStripCasts(nodesGet(call->args, 1));
            FnCallNode *read = siReadRawCall(value, c);
            VarDclNode *var = siNamedVar(value);
            if (read && c->nconsumed < SiHeldMax)
                c->consumed[c->nconsumed++] = (INode *)read;       // read and written back in one
            for (int i = 0; var && i < c->nheld; ++i) {
                if (c->held[i] == var)
                    ++c->heldwrites[i];
            }
        }
        return 1;
    }
    if (which == ReadRawIntrinsic) {
        for (int i = 0; i < c->nconsumed; ++i) {
            if (c->consumed[i] == (INode *)call)
                return 1;
        }
        // Read out of the storage and not put back: it leaves
        if (siReadRawCall((INode *)call, c))
            c->hit = 1;
        return 1;
    }
    return 0;
}

// A read-only reference (an element lent to a comparison, say): what it is handed
// to cannot free or move what it points at
static int siReadOnlyRef(INode *arg) {
    INode *t = iexpGetTypeDcl(arg);
    return t != NULL && t->tag == RefTag && !(permGetFlags(((RefNode *)t)->perm) & MayWrite);
}

// An element read into a local: it leaves unless that local is only written back
static void siHoldElement(VarDclNode *var, INode *value, SiCtx *c) {
    FnCallNode *read = siReadRawCall(value, c);
    if (read == NULL)
        return;
    if (c->nheld >= SiHeldMax || c->nconsumed >= SiHeldMax) {
        c->hit = 1;
        return;
    }
    c->held[c->nheld] = var;
    c->heldwrites[c->nheld] = 0;
    c->helduses[c->nheld++] = 0;
    c->consumed[c->nconsumed++] = (INode *)read;
}

// A held local used, or the end of the body: one used otherwise than written back left
static void siCountHeldUse(INode *node, SiCtx *c) {
    VarDclNode *var = siNamedVar(node);
    for (int i = 0; var && i < c->nheld; ++i) {
        if (c->held[i] == var)
            ++c->helduses[i];
    }
}

// A call, in a type mode (SiModeType, SiModeOut). Every argument that can reach
// a value of the collection's type, other than a value the function owns whole, is
// either that value lent to be written (the callee is asked about that parameter)
// or something holding it (the callee is asked about the type). With no argument
// that can, only a callee that reshapes one without any, through a global, is
// one that does. A callee the compiler cannot read is taken to reshape what its
// arguments reach.
static void siCallType(FnCallNode *call, SiCtx *c) {
    INode *fnn = call->objfn;
    FnDclNode *callee = isNameUseNode(fnn) && ((NameUseNode *)fnn)->dclnode && ((NameUseNode *)fnn)->dclnode->tag == FnDclTag
        ? (FnDclNode *)((NameUseNode *)fnn)->dclnode : NULL;
    if (callee && ((callee->dclinfo.facts & DclIntrinsic) || (callee->value && callee->value->tag == IntrinsicTag)
            || fnDclIsInit(callee)))
        return;
    // A call through a virtual reference marked '+ ShapeKeeping' reshapes nothing
    // (checked where the value became the reference)
    if (shapeKeepingCall(call))
        return;
    uint32_t nargs = call->args ? call->args->used : 0;
    if ((call->flags & FlagLvalOp) && nargs > 0 && siTypeWriteHits(nodesGet(call->args, 0), c)) {
        c->hit = 1;
        return;
    }
    int visible = !(call->flags & FlagVDisp) && siVisible(callee);
    // Into a borrow of a 'mut' global: a virtual call, or a call of a function value (a '&fn' held in
    // a field, a parameter, a global or a local), runs code that is not known, which may write any
    // global without being handed it; so does an 'extern' function handed anything that can reach
    // something callable (a '&fn', a virtual reference, a closure an owner holds), which C may call
    // back (a named function the compiler cannot read is otherwise asked as before: of what it is
    // handed)
    if (c->opaque && ((call->flags & FlagVDisp) || callee == NULL || shapeExternHandsCallable(call, callee))) {
        c->hit = 1;
        return;
    }
    int anyreach = 0;
    for (uint32_t k = 0; k < nargs && !c->hit; ++k) {
        INode *arg = nodesGet(call->args, k);
        INode *ba = arg;
        while (ba->tag == CastTag && !(ba->flags & FlagConvert))
            ba = ((CastNode *)ba)->exp;
        // A value the function owns whole, lent or handed over, is nobody else's
        if ((ba->tag == BorrowTag && reshapeUniqueLocalOf(((RefNode *)ba)->vtexp, c->cont))
                || reshapeUniqueLocalOf(ba, c->cont))
            continue;
        if (c->mode == SiModeOut && !siRooted(ba, c))
            continue;
        INode *at = iexpGetTypeDcl(arg);
        if (reshapeReach(at, c->cont, 1) == 0)
            continue;
        if (at->tag == RefTag && itypeGetTypeDcl(((RefNode *)at)->vtexp) == c->cont) {
            if (!siParmWritable(callee, k, arg))
                continue;
            if (!visible)
                c->hit = 1;
            else {
                int answer = siAsk(callee, k, SiModeField, NULL);
                if (answer == SiYes)
                    c->hit = 1;
                else if (answer == SiPending)
                    c->pending = 1;
            }
        }
        else
            anyreach = 1;
    }
    if (c->hit)
        return;
    if (anyreach && !visible)
        c->hit = 1;
    else if (visible && (anyreach || reshapeGlobalsReach(c->cont))) {
        // Handed what reaches the type: the callee through any parameter or global.
        // Handed nothing that does: only through a global, and only if some global
        // holds one
        int answer = siAsk(callee, 0, c->opaque ? (anyreach ? SiModeTypeG : SiModeOutG)
            : anyreach ? SiModeType : SiModeOut, c->cont);
        if (answer == SiYes)
            c->hit = 1;
        else if (answer == SiPending)
            c->pending = 1;
    }
}

static void siCall(FnCallNode *call, SiCtx *c) {
    if (c->mode >= SiModeType) {
        siCallType(call, c);
        return;
    }
    INode *fnn = call->objfn;
    FnDclNode *callee = isNameUseNode(fnn) && ((NameUseNode *)fnn)->dclnode && ((NameUseNode *)fnn)->dclnode->tag == FnDclTag
        ? (FnDclNode *)((NameUseNode *)fnn)->dclnode : NULL;
    uint32_t nargs = call->args ? call->args->used : 0;
    if ((call->flags & FlagLvalOp) && nargs > 0 && siWriteHits(nodesGet(call->args, 0), c)) {
        c->hit = 1;
        return;
    }
    if (c->mode == SiModeField && siElementCall(call, callee, c) && c->hit)
        return;
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
                if (at->tag == RefTag && (c->mode == SiModeField || siTypeHoldsStorage(((RefNode *)at)->vtexp)))
                    c->hit = 1;
                continue;
            }
            by = 1;
        }
        else if (!intrinsic && siReadsStorage(arg, c)
                && !(c->mode == SiModeField && siReadOnlyRef(arg)))
            by = 1;     // storage handed on by value: a free or a realloc, in a callee
        if (!by)
            continue;
        if (!visible)
            c->hit = 1;
        else {
            int answer = siAsk(callee, k, c->mode, NULL);
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
    int types = c->mode >= SiModeType;
    // What a local holds a reference to is followed to a parameter's pointee, and to a global
    int notes = !types || c->mode == SiModeOut;
    if (c->nheld > 0 && isNameUseNode(node) && isExpNode(node))
        siCountHeldUse(node, c);
    switch (node->tag) {
    case AssignTag:
        if (types ? siTypeWriteHits(((AssignNode *)node)->lval, c) : siWriteHits(((AssignNode *)node)->lval, c))
            c->hit = 1;
        else if (notes)
            siNote(siNamedVar(((AssignNode *)node)->lval), ((AssignNode *)node)->rval, c);
        break;
    case SwapTag:
        if (types ? siTypeWriteHits(((SwapNode *)node)->lval, c) || siTypeWriteHits(((SwapNode *)node)->rval, c)
                : siWriteHits(((SwapNode *)node)->lval, c) || siWriteHits(((SwapNode *)node)->rval, c))
            c->hit = 1;
        break;
    case VarDclTag:
        if (c->mode == SiModeField && ((VarDclNode *)node)->value)
            siHoldElement((VarDclNode *)node, ((VarDclNode *)node)->value, c);
        if (notes)
            siNote((VarDclNode *)node, ((VarDclNode *)node)->value, c);
        break;
    case FnCallTag:
        siCall((FnCallNode *)node, c);
        break;
    default:
        break;
    }
    if (c->hit && c->hitnode == NULL)
        c->hitnode = node;
    return c->hit;
}

typedef struct {
    FnDclNode *fn;
    uint32_t k;
    INode *cont;        // a type mode: the collection's type
    uint8_t mode;       // SiMode
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
static INode *siHitNode = NULL;     // the node of the outermost function asked that made the answer yes, when it was worked out

static uint32_t siMemoHash(FnDclNode *fn, uint32_t k, int mode, INode *cont) {
    uint64_t h = ((uint64_t)(uintptr_t)fn * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)k * 0xC2B2AE3D27D4EB4Full)
        ^ ((uint64_t)(uintptr_t)cont * 0xD6E8FEB86659FD93ull) ^ ((uint64_t)mode << 20);
    return (uint32_t)(h >> 32);
}

static SiMemo *siMemoFind(FnDclNode *fn, uint32_t k, int mode, INode *cont) {
    if ((siMemoUsed + 1) * 2 > siMemoCap) {
        SiMemo *old = siMemo;
        uint32_t oldcap = siMemoCap;
        siMemoCap = oldcap ? oldcap * 2 : 1024;
        siMemo = (SiMemo *)memAllocBlk(siMemoCap * sizeof(SiMemo));
        memset(siMemo, 0, siMemoCap * sizeof(SiMemo));
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (old[i].fn == NULL)
                continue;
            uint32_t at = siMemoHash(old[i].fn, old[i].k, old[i].mode, old[i].cont) & (siMemoCap - 1);
            while (siMemo[at].fn != NULL)
                at = (at + 1) & (siMemoCap - 1);
            siMemo[at] = old[i];
        }
    }
    uint32_t at = siMemoHash(fn, k, mode, cont) & (siMemoCap - 1);
    while (siMemo[at].fn != NULL) {
        if (siMemo[at].fn == fn && siMemo[at].k == k && siMemo[at].mode == mode && siMemo[at].cont == cont)
            return &siMemo[at];
        at = (at + 1) & (siMemoCap - 1);
    }
    siMemo[at].fn = fn;
    siMemo[at].k = k;
    siMemo[at].mode = (uint8_t)mode;
    siMemo[at].cont = cont;
    siMemo[at].state = SiMemoFree;
    ++siMemoUsed;
    return &siMemo[at];
}

// Does the body of 'fn' do what 'mode' asks, directly or through what it calls?
// The parameter modes: write storage (or, SiModeField, any field) through its
// parameter 'k' (what it points at, when it is a reference), or hand storage read
// from it to code that may free it. The type modes: reshape a value of the type
// 'cont' that is not its own local.
static int siAsk(FnDclNode *fn, uint32_t k, int mode, INode *cont) {
    if (!siVisible(fn))
        return SiYes;
    if (!siBodyReady(fn))
        return SiPending;
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    int base = mode == SiModeTypeG ? SiModeType : mode == SiModeOutG ? SiModeOut : mode;
    int ofparm = base < SiModeType;
    if (ofparm && k >= sig->parms->used)
        return SiYes;
    if (!ofparm)
        k = 0;
    SiMemo *m = siMemoFind(fn, k, mode, cont);
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
    ctx.mode = base;
    ctx.opaque = mode >= SiModeTypeG;
    ctx.cont = cont;
    if (ofparm) {
        ctx.parm = (VarDclNode *)nodesGet(sig->parms, k);
        INode *ptype = iexpGetTypeDcl((INode *)ctx.parm);
        ctx.isref = ptype != NULL && ptype->tag == RefTag;
        ctx.pointee = ctx.isref ? itypeGetTypeDcl(((RefNode *)ptype)->vtexp) : NULL;
    }
    siVisit(fn->value, siWalkNode, &ctx);
    // An element read into a local that is used otherwise than written back left
    for (int i = 0; i < ctx.nheld && !ctx.hit; ++i) {
        if (ctx.helduses[i] != ctx.heldwrites[i])
            ctx.hit = 1;
    }
    int answer = ctx.hit ? SiYes : ctx.pending ? SiPending : SiNo;

    --siDepth;
    // The outermost question keeps what it found, for a message
    if (siDepth == 0)
        siHitNode = ctx.hit ? ctx.hitnode : NULL;
    m = siMemoFind(fn, k, mode, cont);      // the table may have grown
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

static int siParamWrites(FnDclNode *fn, uint32_t k) {
    return siAsk(fn, k, SiModeStorage, NULL);
}

// The two questions a call's check asks (reshape.h). An answer not settled yet is
// taken as yes
uint32_t shapeUnsettled = 0;

int shapeParamReshapes(FnDclNode *fn, uint32_t k) {
    int answer = siAsk(fn, k, SiModeField, NULL);
    if (answer == SiPending)
        ++shapeUnsettled;
    return answer != SiNo;
}

int shapeTypeReshapes(FnDclNode *fn, INode *cont, int viaParams) {
    int answer = siAsk(fn, 0, viaParams ? SiModeType : SiModeOut, cont);
    if (answer == SiPending)
        ++shapeUnsettled;
    return answer != SiNo;
}

// Can a value of this type reach anything callable: a '&fn' (a reference to a function), a virtual
// reference ('&<Trait', '&<fn', '&<mut ...'), or a closure or trait object an owner holds ('So[fn]',
// 'Rc[Trait]')? By containment, a field, an element, or through a pointer or a reference: the same
// idea as reshapeReach, with code to call as the thing wanted. A type graph too large to follow is
// taken as reaching one. Only a positive answer is kept, for a struct, for a negative one found
// beneath a struct still being walked is not settled
#define SiFnSeenCap 512
static INode *siFnSeen[SiFnSeenCap];
static int siFnSeenN;
static INode **siFnYes = NULL;
static uint32_t siFnYesCap = 0;
static uint32_t siFnYesUsed = 0;

static int siFnYesHas(INode *st) {
    if (siFnYesCap == 0)
        return 0;
    uint32_t at = (uint32_t)(((uintptr_t)st >> 4) * 2654435761u) & (siFnYesCap - 1);
    while (siFnYes[at]) {
        if (siFnYes[at] == st)
            return 1;
        at = (at + 1) & (siFnYesCap - 1);
    }
    return 0;
}

static void siFnYesPut(INode *st) {
    if ((siFnYesUsed + 1) * 2 > siFnYesCap) {
        INode **old = siFnYes;
        uint32_t oldcap = siFnYesCap;
        siFnYesCap = oldcap ? oldcap * 2 : 256;
        siFnYes = (INode **)memAllocBlk(siFnYesCap * sizeof(INode *));
        memset(siFnYes, 0, siFnYesCap * sizeof(INode *));
        siFnYesUsed = 0;
        for (uint32_t i = 0; i < oldcap; ++i) {
            if (old[i])
                siFnYesPut(old[i]);
        }
    }
    uint32_t at = (uint32_t)(((uintptr_t)st >> 4) * 2654435761u) & (siFnYesCap - 1);
    while (siFnYes[at])
        at = (at + 1) & (siFnYesCap - 1);
    siFnYes[at] = st;
    ++siFnYesUsed;
}

static int siReachesFnRef(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? siReachesFnRef(itypeGetTypeDcl(type)) : 0;
    case AliasDclTag:
        return siReachesFnRef(((AliasDclNode *)type)->target);
    case FnSigTag:
    case VirtRefTag:
        return 1;
    case RefTag:
    case ArrayRefTag:
        return siReachesFnRef(((RefNode *)type)->vtexp);
    case PtrTag:
        return siReachesFnRef(((StarNode *)type)->vtexp);
    case ArrayTag:
        return siReachesFnRef(arrayElemType(type));
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (siReachesFnRef(*nodesp))
                return 1;
        }
        return 0;
    }
    case StructTag:
    {
        if (siFnYesHas(type))
            return 1;
        for (int i = 0; i < siFnSeenN; ++i) {
            if (siFnSeen[i] == type)
                return 0;
        }
        if (siFnSeenN >= SiFnSeenCap)
            return 1;
        siFnSeen[siFnSeenN++] = type;
        INode **nodesp;
        uint32_t cnt;
        int found = 0;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp)) {
            if (siReachesFnRef(((IExpNode *)*nodesp)->vtype)) {
                found = 1;
                break;
            }
        }
        if (!found && ((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp)) {
                if (siReachesFnRef(*nodesp)) {
                    found = 1;
                    break;
                }
            }
        }
        if (found)
            siFnYesPut(type);
        return found;
    }
    default:
        return 0;
    }
}

// Is this an 'extern' function handed anything that can reach something callable (a callback C can
// call back into Cone with, directly or held in a struct it is handed a pointer to)?
int shapeExternHandsCallable(FnCallNode *call, FnDclNode *callee) {
    if (callee == NULL || !(callee->flags & FlagExtern) || call->args == NULL)
        return 0;
    for (uint32_t k = 0; k < call->args->used; ++k) {
        siFnSeenN = 0;
        if (siReachesFnRef(iexpGetTypeDcl(nodesGet(call->args, k))))
            return 1;
    }
    return 0;
}

// The same, asked of a call made while a borrow into a 'mut' global is held (reshape.h,
// 'globalview'): only through a global, with a call into unknown code (a virtual call, a function
// value) taken to write any
int shapeTypeWritesGlobal(FnDclNode *fn, INode *cont) {
    int answer = siAsk(fn, 0, SiModeOutG, cont);
    if (answer == SiPending)
        ++shapeUnsettled;
    return answer != SiNo;
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

// ---- The free functions of the type's own module ------------------------------
// A type's methods are not the only code that lends from it or changes it: a free
// function of its module (a function beside it, taking it by reference) may too.
// Other modules reach a type only through its public methods, which are read here,
// and its public fields, which a store through another name covers (shapeinfer.h).
// The search is bound to the type's own module: a function there that names the
// type as the target of a parameter's reference. Generic functions and generic
// types are not read (their bodies are only checked when instantiated).

// Does this parameter's type, written or checked, make a reference to 'st'?
static int siParmRefersTo(VarDclNode *parm, StructNode *st) {
    INode *t = parm->vtype;
    if (t == NULL || t->tag != RefTag)
        return 0;
    t = ((RefNode *)t)->vtexp;
    return t != NULL && isTypeNode(t) && itypeGetTypeDcl(t) == (INode *)st;
}

// Call 'fn' on each free function of the module 'st' is declared in that takes it by reference
static void siEachFree(StructNode *st, void (*fn)(FnDclNode *, void *), void *ctx) {
    INode *owner = inodeGetOwner((INode *)st);
    if (owner == NULL || owner->tag != ModuleTag || st->genericinfo != NULL || dclIsInstance((INode *)st))
        return;
    Nodes *nodes = ((ModuleNode *)owner)->nodes;
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag)
            continue;
        FnDclNode *f = (FnDclNode *)*nodesp;
        if ((f->flags & FlagMethFld) || f->genericinfo != NULL || f->vtype == NULL || f->vtype->tag != FnSigTag)
            continue;
        Nodes *parms = ((FnSigNode *)f->vtype)->parms;
        for (uint32_t k = 0; parms && k < parms->used; ++k) {
            if (siParmRefersTo((VarDclNode *)nodesGet(parms, k), st)) {
                fn(f, ctx);
                break;
            }
        }
    }
}

typedef struct {
    StructNode *st;
    int *lender, *writer, *sigpending, *bodypending;
} SiFreeCtx;

static void siFreeFn(FnDclNode *fn, void *vc) {
    SiFreeCtx *c = (SiFreeCtx *)vc;
    // Its signature is checked once the declaration is under way
    if (!(fn->flags & (TypeChecked | TypeChecking))) {
        *c->sigpending = 1;
        return;
    }
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    if (sig->rettype && itypeCarriesBorrow(sig->rettype))
        *c->lender = 1;
    if (!siVisible(fn))
        return;
    for (uint32_t k = 0; k < sig->parms->used; ++k) {
        INode *pt = iexpGetTypeDcl(nodesGet(sig->parms, k));
        if (pt == NULL || pt->tag != RefTag || itypeGetTypeDcl(((RefNode *)pt)->vtexp) != (INode *)c->st
                || !(permGetFlags(((RefNode *)pt)->perm) & MayWrite))
            continue;
        int answer = siParamWrites(fn, k);
        if (answer == SiYes)
            *c->writer = 1;
        else if (answer == SiPending)
            *c->bodypending = 1;
    }
}

static void siDemandFree(FnDclNode *fn, void *vc) {
    (void)vc;
    fnCallDemandCandidates((INode *)fn);
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
    if (!(lender && writer)) {
        SiFreeCtx fc = { st, &lender, &writer, &sigpending, &bodypending };
        siEachFree(st, siFreeFn, &fc);
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
    siEachFree(st, siDemandFree, NULL);
}

// A type of a package whose include file is read here, and not generic: its
// methods have no bodies in this compile, and the file said what the package's
// own compile found (incfile.c)
static int siIsRecorded(StructNode *st) {
    return st->lexer != NULL && (st->lexer->flags & LexLineMarks) && !dclIsInstance((INode *)st)
        && !(st->dclinfo.facts & DclActorGen);
}

// How many fields the struct has besides the tag
static uint32_t siPayloadFields(StructNode *st) {
    uint32_t count = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&st->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsTagField))
            ++count;
    }
    return count;
}

// An enum changes shape when one of its variants holds more than the fields they
// share: a reference narrowed into that variant is a borrow into the value, and
// storing another variant over it rereads the same bytes as other types. Nothing is
// read from its methods (those declared on it are copied into its variants): a
// store of the whole value is what reshapes it, asked wherever a borrow into it is
// held (reshape.h). An enum of bare names holds nothing to borrow.
static int siSumHasPayload(StructNode *st) {
    uint32_t common = siPayloadFields(st);
    if (st->derived == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(st->derived, cnt, nodesp)) {
        if ((*nodesp)->tag == StructTag && siPayloadFields((StructNode *)*nodesp) > common)
            return 1;
    }
    return 0;
}

static int siSettle(StructNode *st, int demand) {
    if (st->lends == LendsShapeChanging || st->shapeinf == ShapeYes)
        return SiYes;
    if (reshapeIsSum((INode *)st))
        return !(st->flags & TypeChecked) ? SiPending : siSumHasPayload(st) ? SiYes : SiNo;
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
static void siNeedAdd(SiNeeds *needs, INode *recvtype) {
    INode *container = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode *)recvtype)->vtexp) : NULL;
    if (container == NULL || container->tag != StructTag || ((StructNode *)container)->lends != LendsLoaned)
        return;
    for (int i = 0; i < needs->nneed; ++i) {
        if (needs->need[i] == (StructNode *)container)
            return;
    }
    if (needs->nneed < SiNeedMax)
        needs->need[needs->nneed++] = (StructNode *)container;
}

static int siNeedNode(INode *node, void *vc) {
    if (node->tag != FnCallTag)
        return 0;
    SiNeeds *needs = (SiNeeds *)vc;
    FnCallNode *call = (FnCallNode *)node;
    if (call->args == NULL || call->args->used == 0 || !isNameUseNode(call->objfn))
        return 0;
    INode *fn = ((NameUseNode *)call->objfn)->dclnode;
    if (fn == NULL || fn->tag != FnDclTag || ((FnDclNode *)fn)->vtype == NULL
        || ((FnDclNode *)fn)->vtype->tag != FnSigTag)
        return 0;
    FnSigNode *sig = (FnSigNode *)((FnDclNode *)fn)->vtype;
    if (sig->rettype == NULL || !itypeCarriesBorrow(sig->rettype))
        return 0;
    if (fn->flags & FlagMethFld) {
        if (sig->parms->used == 0 || ((VarDclNode *)nodesGet(sig->parms, 0))->namesym != selfName)
            return 0;
        siNeedAdd(needs, iexpGetTypeDcl(nodesGet(call->args, 0)));
        return 0;
    }
    // A free function lends from each reference it is handed (flowpath.c, pwLentArg)
    for (uint32_t i = 0; i < call->args->used; ++i)
        siNeedAdd(needs, iexpGetTypeDcl(nodesGet(call->args, i)));
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
    int seams;
} SiDeferred;

static SiDeferred *siQueue = NULL;
static uint32_t siQueueUsed = 0;
static uint32_t siQueueCap = 0;

void shapeWalkDefer(FnDclNode *fn, int drops, int seams) {
    if (siQueueUsed == siQueueCap) {
        SiDeferred *old = siQueue;
        siQueueCap = siQueueCap ? siQueueCap * 2 : 64;
        siQueue = (SiDeferred *)memAllocBlk(siQueueCap * sizeof(SiDeferred));
        if (old)
            memcpy(siQueue, old, siQueueUsed * sizeof(SiDeferred));
    }
    siQueue[siQueueUsed].fn = fn;
    siQueue[siQueueUsed].drops = drops;
    siQueue[siQueueUsed].seams = seams;
    ++siQueueUsed;
}

// A function this one calls by name that was never asked for, because a call was
// lowered straight to it (a folded method of a lent body: 'conns.len()' is
// 'conns.view().len()'): its body is checked now, so what a call of it does can be
// read (reshape.h)
static int siDemandCallee(INode *node, void *vc) {
    (void)vc;
    if (node->tag != FnCallTag || !isNameUseNode(((FnCallNode *)node)->objfn))
        return 0;
    INode *fn = ((NameUseNode *)((FnCallNode *)node)->objfn)->dclnode;
    if (fn != NULL && fn->tag == FnDclTag && !(fn->flags & (TypeChecked | TypeChecking)) && siVisible((FnDclNode *)fn))
        fnCallDemandCandidates(fn);
    return 0;
}

void shapeDemandCallees(FnDclNode *fn) {
    if (!structLayoutInFlight())
        siVisit(fn->value, siDemandCallee, NULL);
}

void shapeWalkDeferred(void) {
    // A walk made may check a method on the way, which queues another
    for (uint32_t i = 0; i < siQueueUsed; ++i) {
        FnDclNode *fn = siQueue[i].fn;
        int drops = siQueue[i].drops;
        int seams = siQueue[i].seams;
        shapeWalkReady(fn, 0);
        shapeDemandCallees(fn);
        size_t svTimer = timerCurrent;
        if (timerFine)
            timerBegin(FlowTimer);
        // Nothing is checked later: an answer not settled now is taken as yes
        flowWalkFinal = 1;
        flowPathWalk(fn, 1, drops, seams);
        flowWalkFinal = 0;
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

// ---- '+ ShapeKeeping' ----------------------------------------------------------
// A virtual reference marked '+ ShapeKeeping' promises that a call through it
// reshapes nothing the caller holds a borrow into (shapeinfer.h). The question is
// reshape.c's, put to the value behind the reference where it becomes one: for each
// method the trait dispatches to, every shape-changing type its parameters (and, for
// a closure, the variables it borrows) reach by a writable path is asked of the
// method's body (SiModeType) as a call asks it of a callee. Nothing is inferred
// anew; the coarse answer is the one a visible callee already gets.

typedef struct {
    INode *seen[1024];
    uint8_t shared[1024];   // reached through a reference that may write by a path others share
    int n;
    int full;
} SkWalk;

// Does a hop through this reference reach what it points at by a shared mutable path?
// A permission that writes and is not 'uni' (the one holder's): a '&mut', an owner
// that may be aliased and written. A read-only reference is no hop, but what it
// points at is still walked: Cone writes through a '&' of the value holding a '&mut'
static int skWritingHop(RefNode *ref) {
    if (ref->perm == NULL || ref->perm == unknownType)
        return 0;
    INode *perm = itypeGetTypeDcl(ref->perm);
    return perm != (INode *)uniPerm && (permGetFlags(ref->perm) & MayWrite) != 0;
}

// Every struct a type holds or reaches, once each by way of each kind of path:
// 'shared' is set once a reference that writes by a shared path has been passed
static void skCollect(INode *type, int shared, SkWalk *w) {
    if (type == NULL)
        return;
    switch (type->tag) {
    case NameUseTag:
        if (isTypeNode(type))
            skCollect(itypeGetTypeDcl(type), shared, w);
        return;
    case AliasDclTag:
        skCollect(((AliasDclNode *)type)->target, shared, w);
        return;
    case RefTag:
    case ArrayRefTag:
        skCollect(((RefNode *)type)->vtexp, shared || skWritingHop((RefNode *)type), w);
        return;
    case PtrTag:
        skCollect(((StarNode *)type)->vtexp, 1, w);
        return;
    case ArrayTag:
        skCollect(arrayElemType(type), shared, w);
        return;
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            skCollect(*nodesp, shared, w);
        return;
    }
    case StructTag:
    {
        for (int i = 0; i < w->n; ++i) {
            if (w->seen[i] == type && w->shared[i] == shared)
                return;
        }
        if (w->n >= 1024) {
            w->full = 1;
            return;
        }
        w->seen[w->n] = type;
        w->shared[w->n++] = (uint8_t)shared;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp))
            skCollect(((IExpNode *)*nodesp)->vtype, shared, w);
        if (((StructNode *)type)->derived) {
            for (nodesFor(((StructNode *)type)->derived, cnt, nodesp))
                skCollect(*nodesp, shared, w);
        }
        return;
    }
    default:
        return;
    }
}

#define SkMax 160

// The shape-changing types a method's parameters reach by a path it may write by,
// and so share with whoever else holds the value. 'full' says too many to follow
static int skFrom(INode *start, int shared, StructNode **out, int n, int *full, int *pending) {
    static SkWalk skw;
    SkWalk *w = &skw;
    w->n = 0;
    w->full = 0;
    skCollect(start, shared, w);
    if (w->full)
        *full = 1;
    for (int i = 0; i < w->n; ++i) {
        StructNode *st = (StructNode *)w->seen[i];
        if (!w->shared[i])
            continue;
        int shaping = siSettle(st, 0);
        if (shaping == SiNo)
            continue;
        // Whether it changes shape is not settled yet (a method of its module not
        // checked): asked again at the end of type check
        if (shaping == SiPending) {
            *pending = 1;
            continue;
        }
        int dup = 0;
        for (int j = 0; j < n; ++j) {
            if (out[j] == st)
                dup = 1;
        }
        if (dup)
            continue;
        if (n >= SkMax) {
            *full = 1;
            return n;
        }
        out[n++] = st;
    }
    return n;
}

static int skReached(FnDclNode *meth, StructNode **out, int *full, int *pending) {
    FnSigNode *sig = (FnSigNode *)meth->vtype;
    int n = 0;
    for (uint32_t k = 0; k < sig->parms->used; ++k) {
        VarDclNode *parm = (VarDclNode *)nodesGet(sig->parms, k);
        INode *pt = iexpGetTypeDcl((INode *)parm);
        if (pt == NULL)
            continue;
        // The receiver's own state is its own; what it holds by reference is not. A
        // 'uni' reference is the one holder's, and a parameter held by value is owned:
        // neither makes what it holds shared (skCollect)
        INode *start = pt;
        if (pt->tag == RefTag && k == 0 && parm->namesym == selfName)
            start = itypeGetTypeDcl(((RefNode *)pt)->vtexp);
        n = skFrom(start, 0, out, n, full, pending);
    }
    // A global is reachable from every function, and shared by every holder of it
    for (uint32_t g = 0; g < reshapeGlobalCount(); ++g) {
        INode *gt = reshapeGlobalAt(g)->vtype;
        if (gt == NULL || gt == unknownType)
            *pending = 1;
        else
            n = skFrom(gt, 1, out, n, full, pending);
    }
    return n;
}

// The methods of the trait a virtual reference dispatches to (structMakeVtable's
// choice), as the value 'impl' fills them
static int skMethods(StructNode *impl, StructNode *trait, FnDclNode **out, int max) {
    int n = 0;
    if (trait->callsig) {
        INode *want = namespaceFind(&trait->namespace, parensName);
        INode *binding = namespaceFind(&impl->namespace, parensName);
        FnDclNode *m = want && want->tag == FnDclTag && binding ? iNsTypeFindVrefMethod(binding, (FnDclNode *)want, NULL) : NULL;
        if (m)
            out[n++] = m;
        return n;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld) || structIsWholeEnumMethod(*nodesp))
            continue;
        FnDclNode *meth = (FnDclNode *)*nodesp;
        if ((meth->value && (meth->genericinfo || meth->where || (meth->flags & FlagInline))) || inodeIsPrivate((INode *)meth))
            continue;
        INode *binding = namespaceFind(&impl->namespace, meth->namesym);
        FnDclNode *m = binding ? iNsTypeFindVrefMethod(binding, meth, NULL) : NULL;
        if (m && n < max)
            out[n++] = m;
    }
    return n;
}

typedef struct {
    StructNode *impl;
    StructNode *trait;
    uint8_t answer;         // ShapeKeepsYes or ShapeKeepsNo, once settled
    char *why;
    FnDclNode *fn;          // a deferred reference to a plain function (shapeKeepFnRef), then 'impl' is NULL
    INode *errnode;         // ... and where the refusal is reported
    char wanted[160];       // ... and the reference type as it is written
} SkMemo;

static SkMemo *skMemo = NULL;
static uint32_t skMemoUsed = 0;
static uint32_t skMemoCap = 0;
static SkMemo *skDeferred = NULL;
static uint32_t skDeferredUsed = 0;
static uint32_t skDeferredCap = 0;

static void skPush(SkMemo **list, uint32_t *used, uint32_t *cap, StructNode *impl, StructNode *trait, int answer, char *why) {
    if (*used == *cap) {
        SkMemo *old = *list;
        *cap = *cap ? *cap * 2 : 16;
        *list = (SkMemo *)memAllocBlk(*cap * sizeof(SkMemo));
        if (old)
            memcpy(*list, old, *used * sizeof(SkMemo));
    }
    SkMemo *m = &(*list)[(*used)++];
    m->impl = impl;
    m->trait = trait;
    m->answer = (uint8_t)answer;
    m->why = why;
    m->fn = NULL;
    m->errnode = NULL;
    m->wanted[0] = '\0';
}

static char *skDup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)memAllocBlk(len);
    memcpy(copy, s, len);
    return copy;
}

// What the method does that reshapes, in words: the node the question stopped at
static void skSay(FnDclNode *meth, StructNode *impl, StructNode *cont, INode *hit, char *buf, size_t size) {
    ClosureInfo *closure = impl ? closureOfStruct((INode *)impl) : NULL;
    char who[200];
    if (closure)
        snprintf(who, sizeof(who), "this closure");
    else if (impl)
        snprintf(who, sizeof(who), "%s's `%s`", &impl->namesym->namestr, &meth->namesym->namestr);
    else
        snprintf(who, sizeof(who), "the function `%s`", &meth->namesym->namestr);
    char what[200];
    if (hit && hit->tag == FnCallTag && isNameUseNode(((FnCallNode *)hit)->objfn))
        snprintf(what, sizeof(what), "calls `%s` (line %u)",
            &((NameUseNode *)((FnCallNode *)hit)->objfn)->namesym->namestr, hit->linenbr);
    else if (hit && (hit->tag == AssignTag || hit->tag == SwapTag))
        snprintf(what, sizeof(what), "stores over a place (line %u)", hit->linenbr);
    else
        snprintf(what, sizeof(what), "reshapes one");
    snprintf(buf, size,
        "%s %s on a `%s` that it reaches through a shared mutable path (a `&mut` parameter, a variable it borrows or holds by reference, or a global), which could be a collection the caller holds a borrow into",
        who, what, cont->namesym ? &cont->namesym->namestr : "collection");
}

// One function or method: does it reshape nothing its parameters (and what it holds)
// reach by a shared mutable path? 'impl' is the struct it is a method of, else NULL.
// 'final': no body will be checked later, so one not ready is taken as reshaping
static int skMethod(FnDclNode *meth, StructNode *impl, char *why, size_t size, int final) {
    if (!siVisible(meth)) {
        if (why)
            snprintf(why, size, "%s `%s` is %s whose body the compiler cannot read here (an include file left it out, an extern, a function reference held in a variable or a virtual call), so it is assumed to reshape what it reaches",
                impl && closureOfStruct((INode *)impl) ? "this closure's" : impl ? (const char *)&impl->namesym->namestr : "the function",
                &meth->namesym->namestr, impl ? "a method" : "a function");
        return ShapeKeepsNo;
    }
    if (!siBodyReady(meth) && !structLayoutInFlight())
        fnCallDemandCandidates((INode *)meth);
    if (!siBodyReady(meth)) {
        if (final && why)
            snprintf(why, size, "`%s` could not be read", &meth->namesym->namestr);
        return final ? ShapeKeepsNo : ShapeKeepsPending;
    }
    int pending = 0;
    StructNode *reached[SkMax];
    int full = 0;
    int n = skReached(meth, reached, &full, &pending);
    if (full) {
        if (why)
            snprintf(why, size, "`%s` reaches too many shape-changing types to follow, so it is assumed to reshape one", &meth->namesym->namestr);
        return ShapeKeepsNo;
    }
    for (int i = 0; i < n; ++i) {
        siHitNode = NULL;
        int answer = siAsk(meth, 0, SiModeType, (INode *)reached[i]);
        if (answer == SiYes) {
            if (why)
                skSay(meth, impl, reached[i], siHitNode, why, size);
            return ShapeKeepsNo;
        }
        if (answer == SiPending)
            pending = 1;
    }
    if (pending && final) {
        if (why)
            snprintf(why, size, "`%s` could not be read", &meth->namesym->namestr);
        return ShapeKeepsNo;
    }
    return pending ? ShapeKeepsPending : ShapeKeepsYes;
}

static int skAsk(StructNode *impl, StructNode *trait, char *why, size_t size, int final) {
    // A trait with variants: a call may reach any of them
    if ((impl->flags & TraitType) && impl->derived) {
        INode **nodesp;
        uint32_t cnt;
        int pending = 0;
        for (nodesFor(impl->derived, cnt, nodesp)) {
            if ((*nodesp)->tag != StructTag)
                continue;
            int a = skAsk((StructNode *)*nodesp, trait, why, size, final);
            if (a == ShapeKeepsNo)
                return a;
            if (a == ShapeKeepsPending)
                pending = 1;
        }
        return pending ? ShapeKeepsPending : ShapeKeepsYes;
    }
    FnDclNode *meths[24];
    int nmeths = skMethods(impl, trait, meths, 24);
    int pending = 0;
    for (int mi = 0; mi < nmeths; ++mi) {
        int a = skMethod(meths[mi], impl, why, size, final);
        if (a == ShapeKeepsNo)
            return a;
        if (a == ShapeKeepsPending)
            pending = 1;
    }
    return pending ? ShapeKeepsPending : ShapeKeepsYes;
}

int shapeKeepsVia(StructNode *impl, StructNode *trait, char *why, size_t size) {
    for (uint32_t i = 0; i < skMemoUsed; ++i) {
        if (skMemo[i].impl == impl && skMemo[i].trait == trait) {
            if (why && skMemo[i].answer == ShapeKeepsNo && skMemo[i].why)
                snprintf(why, size, "%s", skMemo[i].why);
            return skMemo[i].answer;
        }
    }
    char reason[900] = "";
    int answer = skAsk(impl, trait, reason, sizeof(reason), 0);
    if (answer != ShapeKeepsNo) {
        // Accepted, and asked again at the end of type check: a body may not have been
        // checked yet, and a global or a type that reshapes may be declared after it
        int seen = 0;
        for (uint32_t i = 0; i < skDeferredUsed; ++i) {
            if (skDeferred[i].impl == impl && skDeferred[i].trait == trait)
                seen = 1;
        }
        if (!seen)
            skPush(&skDeferred, &skDeferredUsed, &skDeferredCap, impl, trait, answer, NULL);
        if (answer == ShapeKeepsPending)
            return answer;
    }
    skPush(&skMemo, &skMemoUsed, &skMemoCap, impl, trait, answer, answer == ShapeKeepsNo ? skDup(reason) : NULL);
    if (why && answer == ShapeKeepsNo)
        snprintf(why, size, "%s", reason);
    return answer;
}

// The end of type check: the values accepted while a body they read was not checked
void shapeKeepDeferred(void) {
    // A body that failed its own check cannot be read, and says why itself
    if (errors > 0)
        skDeferredUsed = 0;
    for (uint32_t i = 0; i < skDeferredUsed; ++i) {
        StructNode *impl = skDeferred[i].impl;
        char reason[900] = "";
        if (skDeferred[i].fn) {
            if (skMethod(skDeferred[i].fn, NULL, reason, sizeof(reason), 1) == ShapeKeepsNo)
                errorMsgNode(skDeferred[i].errnode, ErrorNotShapeKeeping,
                    "`%s` says a call through it keeps the shape of what the caller has borrowed, and %s. Leave '+ ShapeKeeping' off, which ends the caller's borrows into a collection at the call, or don't reshape it there.",
                    skDeferred[i].wanted, reason);
            continue;
        }
        if (skAsk(impl, skDeferred[i].trait, reason, sizeof(reason), 1) == ShapeKeepsNo) {
            char wanted[200];
            snprintf(wanted, sizeof(wanted), "%s", &skDeferred[i].trait->namesym->namestr);
            errorMsgNode((INode *)impl, ErrorNotShapeKeeping,
                "A reference marked `+ ShapeKeeping` to `%s` says calls through it reshape nothing the caller holds a borrow into, and %s: %s.",
                wanted, impl->namesym ? &impl->namesym->namestr : "this value", reason);
        }
    }
    skDeferredUsed = 0;
}

// A reference to a plain function, '&growWorld', made a virtual reference marked
// '+ ShapeKeeping' (iexp.c): the reference's type carries only the signature, so the
// function is read off the expression. A reference the compiler cannot trace to a
// function (a parameter holding a function reference) is refused: its code is unseen
void shapeKeepFnRef(INode *from, INode *to) {
    INode *todcl = itypeGetTypeDcl(to);
    if (todcl->tag != VirtRefTag || !(((RefNode *)todcl)->marks & RefMarkShapeKeeping))
        return;
    INode *fromtype = isExpNode(from) ? iexpGetTypeDcl(from) : NULL;
    if (fromtype == NULL || fromtype->tag != RefTag || itypeGetTypeDcl(((RefNode *)fromtype)->vtexp)->tag != FnSigTag)
        return;
    char wanted[300] = "";
    itypeSpellCat(wanted, sizeof(wanted), todcl, 0);
    INode *e = from;
    while (e->tag == CastTag && !(e->flags & FlagConvert))
        e = ((CastNode *)e)->exp;
    if (e->tag == BorrowTag || e->tag == ArrayBorrowTag)
        e = ((RefNode *)e)->vtexp;
    INode *dcl = isNameUseNode(e) ? ((NameUseNode *)e)->dclnode : NULL;
    if (dcl == NULL || dcl->tag != FnDclTag) {
        errorMsgNode(from, ErrorNotShapeKeeping,
            "`%s` says a call through it keeps the shape of what the caller has borrowed, and the compiler cannot tell what function this reference holds, so it is assumed to reshape what it reaches. Pass the function by name, '&name', or a closure.",
            wanted);
        return;
    }
    char reason[900] = "";
    int answer = skMethod((FnDclNode *)dcl, NULL, reason, sizeof(reason), 0);
    if (answer == ShapeKeepsNo)
        errorMsgNode(from, ErrorNotShapeKeeping,
            "`%s` says a call through it keeps the shape of what the caller has borrowed, and %s. Leave '+ ShapeKeeping' off, which ends the caller's borrows into a collection at the call, or don't reshape it there.",
            wanted, reason);
    else {
        // Accepted, and asked again at the end of type check (shapeKeepsVia)
        skPush(&skDeferred, &skDeferredUsed, &skDeferredCap, NULL, NULL, answer, NULL);
        SkMemo *m = &skDeferred[skDeferredUsed - 1];
        m->fn = (FnDclNode *)dcl;
        m->errnode = from;
        snprintf(m->wanted, sizeof(m->wanted), "%s", wanted);
    }
}

int shapeKeepingCall(FnCallNode *call) {
    if (!(call->flags & FlagVDisp) || call->args == NULL || call->args->used == 0)
        return 0;
    INode *t = iexpGetTypeDcl(nodesGet(call->args, 0));
    return t != NULL && t->tag == VirtRefTag && (((RefNode *)t)->marks & RefMarkShapeKeeping) != 0;
}
