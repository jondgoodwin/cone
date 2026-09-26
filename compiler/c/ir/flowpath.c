/** The path walk: flow state along each path, with joins
 * @file
 *
 * flowpath.h says what it is for. The state is one mutable current state plus
 * an undo log: a fork remembers the log's position; an arm's end pops the log
 * back to it, keeping what the arm changed as its delta; a join sets each fact
 * any arm changed to the union over the arms, an arm that did not change it
 * contributing its value at the fork. So a fork costs nothing and a join costs
 * what the arms changed, not the size of the state.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

// *********************
// Sets of ids
// *********************

PathSet pathSetAll = { UINT32_MAX };

static PathSet *pathSetNew(uint32_t cnt) {
    PathSet *set = (PathSet *)memAllocBlk(sizeof(PathSet) + cnt * sizeof(uint32_t));
    set->cnt = cnt;
    return set;
}

int pathSetHas(PathSet *set, uint32_t id) {
    if (set == NULL)
        return 0;
    if (set == &pathSetAll)
        return 1;
    uint32_t lo = 0;
    uint32_t hi = set->cnt;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (set->ids[mid] == id)
            return 1;
        if (set->ids[mid] < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

PathSet *pathSetAdd(PathSet *set, uint32_t id) {
    if (pathSetHas(set, id))
        return set;
    uint32_t cnt = set ? set->cnt : 0;
    PathSet *added = pathSetNew(cnt + 1);
    uint32_t i = 0;
    uint32_t k = 0;
    while (i < cnt && set->ids[i] < id)
        added->ids[k++] = set->ids[i++];
    added->ids[k++] = id;
    while (i < cnt)
        added->ids[k++] = set->ids[i++];
    return added;
}

// Is every id of 'a' in 'b'?
static int pathSetWithin(PathSet *a, PathSet *b) {
    if (a == NULL || a == b || b == &pathSetAll)
        return 1;
    if (b == NULL || a == &pathSetAll)
        return 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->cnt; ++i) {
        while (k < b->cnt && b->ids[k] < a->ids[i])
            ++k;
        if (k == b->cnt || b->ids[k] != a->ids[i])
            return 0;
    }
    return 1;
}

PathSet *pathSetUnion(PathSet *a, PathSet *b) {
    if (pathSetWithin(b, a))
        return a;
    if (pathSetWithin(a, b))
        return b;
    PathSet *both = pathSetNew(a->cnt + b->cnt);
    uint32_t i = 0, k = 0, n = 0;
    while (i < a->cnt && k < b->cnt) {
        if (a->ids[i] < b->ids[k])
            both->ids[n++] = a->ids[i++];
        else if (a->ids[i] > b->ids[k])
            both->ids[n++] = b->ids[k++];
        else {
            both->ids[n++] = a->ids[i++];
            ++k;
        }
    }
    while (i < a->cnt)
        both->ids[n++] = a->ids[i++];
    while (k < b->cnt)
        both->ids[n++] = b->ids[k++];
    both->cnt = n;
    return both;
}

static int pathSetEqual(PathSet *a, PathSet *b) {
    return pathSetWithin(a, b) && pathSetWithin(b, a);
}

// *********************
// The walk's state
// *********************

// A fact as it was (the undo log), or as an arm left it (a delta)
typedef struct {
    uint32_t var;
    PathSet *holds;
    PathSet *pending;
} PathEntry;

// What one path changed since a mark, and the value it hands its target
typedef struct PathDelta {
    struct PathDelta *next;
    PathSet *value;
    uint32_t cnt;
    PathEntry ents[];
} PathDelta;

// A block being walked: where it started, and what jumped to it
typedef struct {
    BlockNode *blk;
    uint32_t declstart;     // its first variable on the declared-variable stack
    uint32_t mark;          // the log's position at its start (a loop: at its head)
    PathDelta *exits;       // each 'break' leaving it
    PathDelta *loops;       // each 'continue' to it, and its body's end (a loop)
} PathFrame;

PathVar *pathVars = NULL;
static uint32_t nvars = 0;
static uint32_t varcap = 0;

static PathEntry *pathLog = NULL;
static uint32_t nlog = 0;
static uint32_t logcap = 0;

// The variables declared by the blocks being walked, innermost last
static uint32_t *decls = NULL;
static uint32_t ndecls = 0;
static uint32_t declcap = 0;

static PathFrame *frames = NULL;
static uint32_t nframes = 0;
static uint32_t framecap = 0;

// The variables a join is gathering
static uint32_t *touched = NULL;
static uint32_t touchedcap = 0;

static uint32_t stamp = 0;
static int dead = 0;            // the current path has jumped away: nothing after it runs

// -V 2 tallies
static uint32_t statFns = 0;
static uint32_t statLoops = 0;
static uint32_t statRewalks = 0;
static uint32_t statCapped = 0;

// A loop walked this many times without settling has its holders widened to
// every loan, which settles it (conservatively) within a walk or two more
#define PathLoopCap 4

void *pathGrow(void *buf, uint32_t *cap, size_t size) {
    uint32_t old = *cap;
    *cap = old ? old << 1 : 16;
    void *grown = memAllocBlk(*cap * size);
    if (old)
        memcpy(grown, buf, old * size);
    return grown;
}

// Is this type a borrowed reference: one whose referent belongs to another?
static int pwIsBorrowed(INode *typedcl) {
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef;
}

static int pwIsBorrowedType(INode *type) {
    return !flowGateCarriesNone(type) && pwIsBorrowed(itypeGetTypeDcl(type));
}

// The walk's index for a variable, registering it the first time
static uint32_t pathVar(VarDclNode *var) {
    if (var->flowindex)
        return var->flowindex;
    if (nvars == varcap)
        pathVars = (PathVar *)pathGrow(pathVars, &varcap, sizeof(PathVar));
    uint32_t index = nvars++;
    PathVar *pv = &pathVars[index];
    pv->var = var;
    pv->holds = NULL;
    pv->pending = NULL;
    pv->loans = 0;
    pv->xstamp = 0;
    pv->jstamp = 0;
    // A holder is a local variable or a parameter whose type is a borrowed
    // reference. A borrow held inside another value -- a struct, a tuple, an
    // array, an 'Option' -- is not tracked yet. What a parameter holds, the
    // caller lent and froze; it is tracked only once something here is stored
    // into it. The temporary an operator changing its operand in place
    // borrows it through ('x += 1', 'v <- (a, b)') is the operator's own, as
    // a method's receiver is: 'k += k' reads 'k' while it is borrowed.
    pv->holder = var->scope > 0 && !(var->flags & FlagStatic) && var->namesym != tempName
        && var->vtype && pwIsBorrowedType(var->vtype);
    var->flowindex = index;
    return index;
}

void pathSetFacts(uint32_t var, PathSet *holds, PathSet *pending) {
    PathVar *pv = &pathVars[var];
    if (pv->holds == holds && pv->pending == pending)
        return;
    if (nlog == logcap)
        pathLog = (PathEntry *)pathGrow(pathLog, &logcap, sizeof(PathEntry));
    PathEntry *entry = &pathLog[nlog++];
    entry->var = var;
    entry->holds = pv->holds;
    entry->pending = pv->pending;
    if (holds && holds != pv->holds)
        loanHeldBy(var, holds);
    pv->holds = holds;
    pv->pending = pending;
}

// Undo every change since 'mark'
static void pathRollback(uint32_t mark) {
    while (nlog > mark) {
        PathEntry *entry = &pathLog[--nlog];
        pathVars[entry->var].holds = entry->holds;
        pathVars[entry->var].pending = entry->pending;
    }
}

// What the current path changed since 'mark': each variable once, as it is now
static PathDelta *pathDelta(uint32_t mark, PathSet *value) {
    ++stamp;
    uint32_t cnt = 0;
    for (uint32_t i = mark; i < nlog; ++i) {
        PathVar *pv = &pathVars[pathLog[i].var];
        if (pv->xstamp != stamp) {
            pv->xstamp = stamp;
            ++cnt;
        }
    }
    PathDelta *delta = (PathDelta *)memAllocBlk(sizeof(PathDelta) + cnt * sizeof(PathEntry));
    delta->next = NULL;
    delta->value = value;
    delta->cnt = cnt;
    ++stamp;
    cnt = 0;
    for (uint32_t i = mark; i < nlog; ++i) {
        uint32_t var = pathLog[i].var;
        PathVar *pv = &pathVars[var];
        if (pv->xstamp != stamp) {
            pv->xstamp = stamp;
            delta->ents[cnt].var = var;
            delta->ents[cnt].holds = pv->holds;
            delta->ents[cnt].pending = pv->pending;
            ++cnt;
        }
    }
    return delta;
}

// Join the paths in 'deltas', each relative to the current state: a fact takes
// the union of every path's value, a path that did not change it contributing
// the current value -- as the current path itself does when 'withcurrent' is
// set. Returns whether anything changed.
static int pathJoin(PathDelta *deltas, int withcurrent) {
    ++stamp;
    uint32_t npaths = 0;
    uint32_t ntouched = 0;
    for (PathDelta *delta = deltas; delta; delta = delta->next) {
        ++npaths;
        for (uint32_t i = 0; i < delta->cnt; ++i) {
            PathEntry *entry = &delta->ents[i];
            PathVar *pv = &pathVars[entry->var];
            if (pv->jstamp != stamp) {
                pv->jstamp = stamp;
                pv->jcnt = 0;
                pv->jholds = NULL;
                pv->jpending = NULL;
                if (ntouched == touchedcap)
                    touched = (uint32_t *)pathGrow(touched, &touchedcap, sizeof(uint32_t));
                touched[ntouched++] = entry->var;
            }
            ++pv->jcnt;
            pv->jholds = pathSetUnion(pv->jholds, entry->holds);
            pv->jpending = pathSetUnion(pv->jpending, entry->pending);
        }
    }
    int changed = 0;
    for (uint32_t i = 0; i < ntouched; ++i) {
        PathVar *pv = &pathVars[touched[i]];
        PathSet *holds = pv->jholds;
        PathSet *pending = pv->jpending;
        if (withcurrent || pv->jcnt < npaths) {
            holds = pathSetUnion(holds, pv->holds);
            pending = pathSetUnion(pending, pv->pending);
        }
        if (!pathSetEqual(holds, pv->holds) || !pathSetEqual(pending, pv->pending)) {
            pathSetFacts(touched[i], holds, pending);
            changed = 1;
        }
    }
    return changed;
}

static PathDelta *pathDeltaPush(PathDelta *list, PathDelta *delta) {
    delta->next = list;
    return delta;
}

// *********************
// The walk
// *********************

static PathSet *pwValue(INode **nodep, int move);
static PathSet *pwBlock(BlockNode *blk, int fnblock, int move);

// An access to a place, asked of borrow freezing only when some loan is rooted
// at the place's variable: what kind of access it is is not worked out otherwise
#define pwAccess(pl, access, node) \
    do { if (pathVars[(pl)->var].loans) loanAccess((pl), (access), (node)); } while (0)

// Does a value of this type carry a borrow? Asked only where the answer
// decides a loan set, and remembered per struct (itypeCarriesBorrow).
static int pwCarries(INode *type) {
    return type && !flowGateCarriesNone(type) && itypeCarriesBorrow(type);
}

// A variable named as an expression, or NULL
static VarDclNode *pwNamedVar(INode *node) {
    if (!(isNameUseNode(node) && isExpNode(node)))
        return NULL;
    INode *dcl = ((NameUseNode *)node)->dclnode;
    return dcl && dcl->tag == VarDclTag ? (VarDclNode *)dcl : NULL;
}

// The loans a value read from this place carries, if its type carries any:
// everything its root variable holds (holders are whole variables)
static PathSet *pwPlaceHolds(Place *pl, INode *node) {
    PathVar *pv = &pathVars[pl->var];
    return pv->holder && pwCarries(((IExpNode *)node)->vtype) ? pv->holds : NULL;
}

static void pwStep(Place *pl, uintptr_t step) {
    if (pl->nsteps < PlaceMaxSteps)
        pl->steps[pl->nsteps++] = step;
}

static int pwPlace(INode **nodep, Place *pl, PathSet **base);

// The place reached through the reference 'ref' evaluates to. Through a
// borrowed reference it is a root of its own, what the reference points at,
// keyed by the variable the reference is read from; through an owning one, a
// step further along the reference's own place. Nothing is tracked through a
// raw pointer, or through a reference that is not read from a variable.
static int pwThrough(INode **refp, Place *pl, PathSet **base) {
    INode *reftype = iexpGetTypeDcl(*refp);
    if (reftype->tag != RefTag && reftype->tag != ArrayRefTag && reftype->tag != VirtRefTag) {
        *base = pwValue(refp, 0);
        return 0;
    }
    Place refpl;
    if (!pwPlace(refp, &refpl, base))
        return 0;
    if (pwIsBorrowed(reftype)) {
        // The reference itself is read
        pwAccess(&refpl, AccessRead, *refp);
        pl->var = refpl.var;
        pl->deref = 1;
        pl->nsteps = 0;
        return 1;
    }
    *pl = refpl;
    pwStep(pl, PlaceStepDeref);
    return 1;
}

// Walk a place expression: the values inside it (an index, the reference it
// is reached through) are walked, and each holder named along it is used.
// Returns 1 and fills 'pl' when it is a place the walk tracks; otherwise walks
// it as a value, whose loans go in 'base'. The place itself is not accessed.
static int pwPlace(INode **nodep, Place *pl, PathSet **base) {
    INode *node = *nodep;
    *base = NULL;
    VarDclNode *var = pwNamedVar(node);
    if (var) {
        uint32_t index = pathVar(var);
        if (pathVars[index].holder)
            loanUse(index, node);
        pl->var = index;
        pl->deref = 0;
        pl->nsteps = 0;
        return 1;
    }
    switch (node->tag) {
    case CastTag:
        if (node->flags & FlagConvert)
            break;
        return pwPlace(&((CastNode *)node)->exp, pl, base);
    case FldAccessTag:
    {
        FnCallNode *fld = (FnCallNode *)node;
        // A virtual reference's field is read through it, with no dereference injected
        int found = iexpGetTypeDcl(fld->objfn)->tag == VirtRefTag
            ? pwThrough(&fld->objfn, pl, base) : pwPlace(&fld->objfn, pl, base);
        if (!found)
            return 0;
        INode *methfld = fld->methfld;
        if (methfld->tag == ULitTag)
            pwStep(pl, ((uintptr_t)((ULitNode *)methfld)->uintlit << 2) | 2);
        else
            pwStep(pl, (uintptr_t)((NameUseNode *)methfld)->namesym);
        return 1;
    }
    case ArrIndexTag:
    {
        // A reference to an array and a slice are indexed with no dereference injected
        FnCallNode *index = (FnCallNode *)node;
        uint16_t objtag = iexpGetTypeDcl(index->objfn)->tag;
        int found = objtag == RefTag || objtag == ArrayRefTag || objtag == PtrTag
            ? pwThrough(&index->objfn, pl, base) : pwPlace(&index->objfn, pl, base);
        INode **argsp;
        uint32_t cnt;
        for (nodesFor(index->args, cnt, argsp))
            pwValue(argsp, 0);
        if (found)
            pwStep(pl, PlaceStepElem);
        return found;
    }
    case DerefTag:
        return pwThrough(&((StarNode *)node)->vtexp, pl, base);
    default:
        break;
    }
    *base = pwValue(nodep, 0);
    return 0;
}

// A place read, or moved when 'move' says the value goes to a new holder
static PathSet *pwPlaceValue(INode **nodep, int move) {
    Place pl;
    PathSet *base;
    if (!pwPlace(nodep, &pl, &base))
        return base && pwCarries(((IExpNode *)*nodep)->vtype) ? base : NULL;
    pwAccess(&pl, move && iexpIsMove(*nodep) ? AccessMove : AccessRead, *nodep);
    return pwPlaceHolds(&pl, *nodep);
}

// A borrow: an access to what it borrows, and a new loan of it. What is
// borrowed through a holder -- a reborrow '&mut *r', or a borrow of the holder
// itself -- keeps everything that holder holds. 'aswrite' is the borrow an
// operator changing its operand in place takes ('++', '+='), which is a write.
static PathSet *pwBorrow(INode *node, int aswrite) {
    RefNode *borrow = (RefNode *)node;
    Place pl;
    PathSet *base;
    if (!pwPlace(&borrow->vtexp, &pl, &base))
        return base;
    INode *perm = ((RefNode *)iexpGetTypeDcl(node))->perm;
    uint16_t flags = permGetFlags(perm);
    int access = aswrite ? AccessWrite
        : (flags & MayWrite) ? AccessBorrowMut
        : (flags & MayRead) ? AccessBorrow : AccessBorrowOpaq;
    pwAccess(&pl, access, node);
    uint32_t loan = loanMake(node, &pl, perm);
    PathVar *pv = &pathVars[pl.var];
    return pathSetAdd(pv->holder ? pv->holds : NULL, loan);
}

// A call: the function reference it calls through, then each argument in
// order. What a call returns carries no loan yet.
static void pwCall(FnCallNode *call) {
    INode *objfn = call->objfn;
    if (pwNamedVar(objfn) || objfn->tag == DerefTag || objfn->tag == FldAccessTag)
        pwValue(&call->objfn, 0);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(call->args, cnt, argsp)) {
        if (argsp == &nodesGet(call->args, 0) && (call->flags & FlagLvalOp) && (*argsp)->tag == BorrowTag)
            pwBorrow(*argsp, 1);
        else
            pwValue(argsp, 1);
    }
}

// Store a value carrying 'holds' into an lval
static void pwStore(INode **lvalp, PathSet *holds) {
    VarDclNode *var = pwNamedVar(*lvalp);
    if (var) {
        if (var->namesym == anonName)
            return;
        // The whole variable is replaced: not a use of it. A holder reassigned
        // was not live before, so what its old value was pending on is dropped.
        uint32_t index = pathVar(var);
        Place pl = { index, 0, 0 };
        pwAccess(&pl, itypeNeedsFinal(var->vtype) ? AccessReplace : AccessWrite, *lvalp);
        if (pathVars[index].holder)
            pathSetFacts(index, holds, NULL);
        return;
    }
    Place pl;
    PathSet *base;
    if (!pwPlace(lvalp, &pl, &base))
        return;
    pwAccess(&pl, AccessWrite, *lvalp);
    // Part of a holder's own value: it holds what it did and the new loans
    PathVar *pv = &pathVars[pl.var];
    if (!pl.deref && pv->holder && holds)
        pathSetFacts(pl.var, pathSetUnion(pv->holds, holds), pv->pending);
}

static PathSet *pwAssign(AssignNode *node) {
    PathSet *holds = pwValue(&node->rval, 1);
    if (node->lval->tag == VTupleTag) {
        INode **lvalp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)node->lval)->elems, cnt, lvalp))
            pwStore(lvalp, holds);
    }
    else
        pwStore(&node->lval, holds);
    return holds;
}

// Each side of a swap is stored over with the other's value
static void pwSwap(SwapNode *node) {
    INode **sides[2] = { &node->lval, &node->rval };
    uint32_t whole[2] = { 0, 0 };
    PathSet *holds[2] = { NULL, NULL };
    for (int i = 0; i < 2; ++i) {
        VarDclNode *var = pwNamedVar(*sides[i]);
        Place pl;
        PathSet *base;
        if (var) {
            whole[i] = pathVar(var);
            pl.var = whole[i];
            pl.deref = 0;
            pl.nsteps = 0;
            holds[i] = pathVars[whole[i]].holds;
            pwAccess(&pl, itypeNeedsFinal(var->vtype) ? AccessReplace : AccessWrite, *sides[i]);
        }
        else if (pwPlace(sides[i], &pl, &base)) {
            holds[i] = pwPlaceHolds(&pl, *sides[i]);
            pwAccess(&pl, AccessWrite, *sides[i]);
        }
    }
    // Two holders exchange what they hold, and what each is pending on goes
    // with its value; a part of a holder gains what the other side held
    PathSet *pending0 = whole[0] ? pathVars[whole[0]].pending : NULL;
    PathSet *pending1 = whole[1] ? pathVars[whole[1]].pending : NULL;
    for (int i = 0; i < 2; ++i) {
        uint32_t other = 1 - i;
        if (whole[i] && pathVars[whole[i]].holder) {
            if (whole[other])
                pathSetFacts(whole[i], holds[other], i == 0 ? pending1 : pending0);
            else
                pathSetFacts(whole[i], pathSetUnion(holds[i], holds[other]), pathVars[whole[i]].pending);
        }
    }
}

// A variable declared: it holds what its initializer carries
static void pwVarDcl(VarDclNode *var) {
    // A static's storage outlives every call, as a global's does
    if (var->flags & FlagStatic)
        return;
    uint32_t index = pathVar(var);
    if (ndecls == declcap)
        decls = (uint32_t *)pathGrow(decls, &declcap, sizeof(uint32_t));
    decls[ndecls++] = index;
    PathSet *holds = NULL;
    if (var->value) {
        // An operator changing a value in place through a borrow it keeps in a
        // temporary writes it: 'x += 1' is '{imm tmp = &mut x; *tmp = *tmp + 1}',
        // and 'v <- (a, b)' appends each element through one
        if (var->namesym == tempName && var->value->tag == BorrowTag)
            holds = pwBorrow(var->value, 1);
        else
            holds = pwValue(&var->value, 1);
    }
    if (pathVars[index].holder)
        pathSetFacts(index, holds, NULL);
}

// The variables declared since 'from' leave scope: each holder among them is
// dead, and what it was pending on is dropped; then each ends, which is an
// access conflicting with any loan of it still held by a holder outside
static void pwScopeEnd(uint32_t from) {
    for (uint32_t i = ndecls; i > from; --i) {
        PathVar *pv = &pathVars[decls[i - 1]];
        if (pv->holder && (pv->holds || pv->pending))
            pathSetFacts(decls[i - 1], NULL, NULL);
    }
    for (uint32_t i = ndecls; i > from; --i) {
        uint32_t index = decls[i - 1];
        if (pathVars[index].loans) {
            Place pl = { index, 0, 0 };
            pwAccess(&pl, AccessEnd, (INode *)pathVars[index].var);
        }
    }
}

// A 'break' or 'continue' to 'target', handing it 'value': the blocks it
// leaves end, and the state goes to the target
static void pwJump(BlockNode *target, PathSet *value, int iscontinue) {
    uint32_t f = nframes;
    while (f > 0 && frames[f - 1].blk != target)
        --f;
    if (f == 0) {
        dead = 1;
        return;
    }
    PathFrame *frame = &frames[f - 1];
    pwScopeEnd(frame->declstart);
    PathDelta *delta = pathDelta(frame->mark, value);
    if (iscontinue)
        frame->loops = pathDeltaPush(frame->loops, delta);
    else
        frame->exits = pathDeltaPush(frame->exits, delta);
    dead = 1;
}

// Walk a block's statements, stopping where the path jumps away. Returns the
// value its final expression hands back, unless it is a loop's, which loops;
// 'move' says that value goes to a new holder.
static PathSet *pwStmts(BlockNode *blk, int move) {
    PathSet *value = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        if (dead)
            break;
        switch ((*nodesp)->tag) {
        case VarDclTag:
            pwVarDcl((VarDclNode *)*nodesp);
            break;
        case SwapTag:
            pwSwap((SwapNode *)*nodesp);
            break;
        case BreakTag:
        {
            BreakRetNode *brk = (BreakRetNode *)*nodesp;
            PathSet *brkval = brk->exp->tag == NilLitTag ? NULL : pwValue(&brk->exp, 1);
            pwJump(brk->block, brkval, 0);
            break;
        }
        case ContinueTag:
            pwJump(((BreakRetNode *)*nodesp)->block, NULL, 1);
            break;
        case ReturnTag:
        {
            // Every holder is a local of this function, and dies with it
            BreakRetNode *ret = (BreakRetNode *)*nodesp;
            if (ret->exp && ret->exp != unknownType && isExpNode(ret->exp))
                pwValue(&ret->exp, 1);
            dead = 1;
            break;
        }
        case BlockRetTag:
        {
            BreakRetNode *ret = (BreakRetNode *)*nodesp;
            if (ret->exp->tag != NilLitTag) {
                int loop = blk->flags & FlagLoop;
                value = pwValue(&ret->exp, move && !loop);
                if (loop)
                    value = NULL;
            }
            break;
        }
        default:
            if (isExpNode(*nodesp))
                pwValue(nodesp, 0);
            break;
        }
    }
    return value;
}

static PathFrame *pwFramePush(BlockNode *blk) {
    if (nframes == framecap)
        frames = (PathFrame *)pathGrow(frames, &framecap, sizeof(PathFrame));
    PathFrame *frame = &frames[nframes++];
    frame->blk = blk;
    frame->declstart = ndecls;
    frame->mark = nlog;
    frame->exits = NULL;
    frame->loops = NULL;
    return frame;
}

// The join of the paths that left a block by 'break', or fell out of its end
static PathSet *pwBlockExits(PathFrame *frame, PathSet *value, int fallthrough) {
    if (frame->exits == NULL)
        return value;
    PathDelta *paths = frame->exits;
    if (fallthrough)
        paths = pathDeltaPush(paths, pathDelta(frame->mark, value));
    pathRollback(frame->mark);
    value = NULL;
    for (PathDelta *path = paths; path; path = path->next)
        value = pathSetUnion(value, path->value);
    pathJoin(paths, 0);
    dead = 0;
    return value;
}

static PathSet *pwBlock(BlockNode *blk, int fnblock, int move) {
    uint32_t f = nframes;
    pwFramePush(blk);
    PathSet *value = pwStmts(blk, move);
    if (!dead && !fnblock)
        pwScopeEnd(frames[f].declstart);
    int fallthrough = !dead;
    value = pwBlockExits(&frames[f], value, fallthrough);
    ndecls = frames[f].declstart;
    --nframes;
    return value;
}

// A loop: its body walked from the state at its head, then again from the
// join of that state with every path back to the head, until the head stops
// growing. Facts only grow, so it settles; errors fire the first time they
// arise and are reported once. Only a 'break' leaves it.
static PathSet *pwLoop(BlockNode *blk) {
    uint32_t f = nframes;
    pwFramePush(blk);
    ++statLoops;
    uint32_t walks = 0;
    while (1) {
        PathFrame *frame = &frames[f];
        frame->mark = nlog;
        frame->exits = NULL;
        frame->loops = NULL;
        ++walks;
        pwStmts(blk, 0);
        frame = &frames[f];
        if (!dead) {
            pwScopeEnd(frame->declstart);
            frame->loops = pathDeltaPush(frame->loops, pathDelta(frame->mark, NULL));
        }
        ndecls = frame->declstart;
        dead = 0;
        pathRollback(frame->mark);
        if (!pathJoin(frame->loops, 1))
            break;
        ++statRewalks;
        // A loop that will not settle has every holder that changed around it
        // widened to every loan: conservative, and it settles
        if (walks >= PathLoopCap) {
            if (walks == PathLoopCap)
                ++statCapped;
            for (PathDelta *path = frame->loops; path; path = path->next) {
                for (uint32_t i = 0; i < path->cnt; ++i) {
                    PathVar *pv = &pathVars[path->ents[i].var];
                    if (pv->holder && pv->holds != &pathSetAll)
                        pathSetFacts(path->ents[i].var, &pathSetAll, pv->pending);
                }
            }
        }
    }
    PathFrame *frame = &frames[f];
    PathSet *value = NULL;
    if (frame->exits) {
        for (PathDelta *path = frame->exits; path; path = path->next)
            value = pathSetUnion(value, path->value);
        pathJoin(frame->exits, 0);
    }
    else
        dead = 1;
    --nframes;
    return value;
}

// An 'if': each branch is a path from the state its conditions leave, and the
// paths join after it. A missing 'else' is a path that runs no branch.
static PathSet *pwIf(IfNode *ifnode, int move) {
    INode **nodesp;
    uint32_t cnt;
    uint32_t mark = 0;
    int first = 1;
    int haselse = 0;
    PathDelta *paths = NULL;
    PathSet *value = NULL;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (*nodesp == elseCond)
            haselse = 1;
        else
            pwValue(nodesp, 0);
        if (first) {
            // What the first condition did holds on every path
            mark = nlog;
            first = 0;
        }
        nodesp++; cnt--;
        uint32_t condmark = nlog;
        PathSet *armval = pwBlock((BlockNode *)*nodesp, 0, move);
        if (!dead) {
            paths = pathDeltaPush(paths, pathDelta(mark, armval));
            value = pathSetUnion(value, armval);
        }
        dead = 0;
        pathRollback(condmark);
    }
    if (!haselse)
        paths = pathDeltaPush(paths, pathDelta(mark, NULL));
    pathRollback(mark);
    if (paths == NULL)
        dead = 1;
    else
        pathJoin(paths, 0);
    return value;
}

// Walk an expression as a value, returning the loans it may carry. 'move'
// says the value goes to a new holder, which moves it when its type moves.
static PathSet *pwValue(INode **nodep, int move) {
    INode *node = *nodep;
    if (pwNamedVar(node) || node->tag == DerefTag || node->tag == ArrIndexTag
        || node->tag == FldAccessTag)
        return pwPlaceValue(nodep, move);
    if (isNameUseNode(node))
        return NULL;
    switch (node->tag) {
    case BlockTag:
        return (node->flags & FlagLoop) ? pwLoop((BlockNode *)node) : pwBlock((BlockNode *)node, 0, move);
    case IfTag:
        return pwIf((IfNode *)node, move);
    case AssignTag:
        return pwAssign((AssignNode *)node);
    case FnCallTag:
        pwCall((FnCallNode *)node);
        return NULL;
    case BorrowTag:
    case ArrayBorrowTag:
        return pwBorrow(node, 0);
    case AllocateTag:
    case ArrayAllocTag:
        return pwValue(&((RefNode *)node)->vtexp, 1);
    case VTupleTag:
    case TypeLitTag:
    {
        PathSet *holds = NULL;
        INode **nodesp;
        uint32_t cnt;
        Nodes *elems = node->tag == VTupleTag ? ((TupleNode *)node)->elems : ((FnCallNode *)node)->args;
        for (nodesFor(elems, cnt, nodesp)) {
            INode **valp = (*nodesp)->tag == NamedValTag ? &((NamedValNode *)*nodesp)->val : nodesp;
            holds = pathSetUnion(holds, pwValue(valp, node->tag == TypeLitTag || move));
        }
        return holds;
    }
    case ArrayLitTag:
    {
        PathSet *holds = NULL;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((ArrayNode *)node)->elems, cnt, nodesp))
            holds = pathSetUnion(holds, pwValue(nodesp, 1));
        return holds;
    }
    case CastTag:
        if (node->flags & FlagConvert) {
            pwValue(&((CastNode *)node)->exp, 0);
            return NULL;
        }
        return pwValue(&((CastNode *)node)->exp, move);
    case IsTag:
        pwValue(&((CastNode *)node)->exp, 0);
        return NULL;
    case NotLogicTag:
        pwValue(&((LogicNode *)node)->lexp, 0);
        return NULL;
    case OrLogicTag:
    case AndLogicTag:
    {
        // The right operand runs on one path only
        LogicNode *logic = (LogicNode *)node;
        pwValue(&logic->lexp, 0);
        uint32_t mark = nlog;
        pwValue(&logic->rexp, 0);
        PathDelta *path = pathDelta(mark, NULL);
        pathRollback(mark);
        pathJoin(path, 1);
        return NULL;
    }
    case RefCountTag:
        return pwValue(&((RefCountNode *)node)->exp, move);
    case HollowTag:
        return ((HollowNode *)node)->exp ? pwValue(&((HollowNode *)node)->exp, move) : NULL;
    case SizeofTag:
    case NilLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
    case AbsenceTag:
    case UnknownTag:
        return NULL;
    default:
        errorUnreachable(node, "a value node the loan walk has no case for");
        return NULL;
    }
}

void flowPathWalk(FnDclNode *fndcl) {
    // The walk's state is file-static, as flow's variable stack is: safe
    // because flow never runs re-entrantly, which this holds it to
    static int walking = 0;
    if (walking) {
        errorUnreachable((INode *)fndcl, "a function the loan walk was asked to walk while walking another");
        return;
    }
    walking = 1;
    ++statFns;
    loanWalkBegin();
    nvars = 1;          // index 0 is "not yet met"
    if (varcap == 0)
        pathVars = (PathVar *)pathGrow(pathVars, &varcap, sizeof(PathVar));
    nlog = 0;
    ndecls = 0;
    nframes = 0;
    dead = 0;

    // The parameters are variables of the function's block; what a caller lent
    // through one is the caller's to freeze
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((FnSigNode *)fndcl->vtype)->parms, cnt, nodesp))
        pathVar((VarDclNode *)*nodesp);
    pwBlock((BlockNode *)fndcl->value, 1, 1);

    // A variable's index is the walk's own
    for (uint32_t i = 1; i < nvars; ++i)
        pathVars[i].var->flowindex = 0;
    walking = 0;
}

void flowPathPrint() {
    printf("Loan walk: %u functions, %u loops, %u walked again, %u widened\n\n",
        statFns, statLoops, statRewalks, statCapped);
}
