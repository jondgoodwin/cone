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

// The set without 'id'
static PathSet *pathSetWithout(PathSet *set, uint32_t id) {
    if (set == &pathSetAll || !pathSetHas(set, id))
        return set;
    if (set->cnt == 1)
        return NULL;
    PathSet *less = pathSetNew(set->cnt - 1);
    uint32_t k = 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (set->ids[i] != id)
            less->ids[k++] = set->ids[i];
    }
    return less;
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
    uint8_t state;
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
static int pathLoans = 1;       // borrow freezing is walking
static int pathDrops = 0;       // drop flags are walking

// -V 2 tallies
static uint32_t statFns = 0;
static uint32_t statLoanFns = 0;
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

// Does a value of this type carry a borrow? Asked only where the answer
// decides a loan set, and remembered per struct (itypeCarriesBorrow).
static int pwCarries(INode *type) {
    return type && !flowGateCarriesNone(type) && itypeCarriesBorrow(type);
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
    pv->state = 0;
    pv->flagged = 0;
    pv->tracked = pathDrops && flowDropTracked(var) && !flowMatchBound((INode *)var);
    pv->dies = pv->tracked && itypeNeedsFinal(var->vtype);
    // A holder is a local variable or a parameter whose type carries a
    // borrow: a borrowed reference, or a struct, tuple, array, 'Option' or
    // collection holding one (itypeCarriesBorrow). What a parameter holds,
    // the caller lent and froze; it is tracked only once something here is
    // stored into it. The temporary an operator changing its operand in place
    // borrows it through ('x += 1', 'v <- (a, b)') is the operator's own, as
    // a method's receiver is: 'k += k' reads 'k' while it is borrowed.
    pv->holder = var->scope > 0 && !(var->flags & FlagStatic) && var->namesym != tempName
        && pwCarries(var->vtype);
    var->flowindex = index;
    return index;
}

static void pathLogVar(uint32_t var) {
    PathVar *pv = &pathVars[var];
    if (nlog == logcap)
        pathLog = (PathEntry *)pathGrow(pathLog, &logcap, sizeof(PathEntry));
    PathEntry *entry = &pathLog[nlog++];
    entry->var = var;
    entry->holds = pv->holds;
    entry->pending = pv->pending;
    entry->state = pv->state;
}

void pathSetState(uint32_t var, uint8_t state) {
    if (pathVars[var].state == state)
        return;
    pathLogVar(var);
    pathVars[var].state = state;
}

void pathSetFacts(uint32_t var, PathSet *holds, PathSet *pending) {
    PathVar *pv = &pathVars[var];
    if (pv->holds == holds && pv->pending == pending)
        return;
    pathLogVar(var);
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
        pathVars[entry->var].state = entry->state;
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
            delta->ents[cnt].state = pv->state;
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
                pv->jstate = 0;
                if (ntouched == touchedcap)
                    touched = (uint32_t *)pathGrow(touched, &touchedcap, sizeof(uint32_t));
                touched[ntouched++] = entry->var;
            }
            ++pv->jcnt;
            pv->jholds = pathSetUnion(pv->jholds, entry->holds);
            pv->jpending = pathSetUnion(pv->jpending, entry->pending);
            pv->jstate |= entry->state;
        }
    }
    int changed = 0;
    for (uint32_t i = 0; i < ntouched; ++i) {
        PathVar *pv = &pathVars[touched[i]];
        PathSet *holds = pv->jholds;
        PathSet *pending = pv->jpending;
        uint8_t state = pv->jstate;
        if (withcurrent || pv->jcnt < npaths) {
            holds = pathSetUnion(holds, pv->holds);
            pending = pathSetUnion(pending, pv->pending);
            state |= pv->state;
        }
        if (!pathSetEqual(holds, pv->holds) || !pathSetEqual(pending, pv->pending)) {
            pathSetFacts(touched[i], holds, pending);
            changed = 1;
        }
        if (state != pv->state) {
            pathSetState(touched[i], state);
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
static PathSet *pwLend(INode *site, Place *pl, INode *perm, int access, uint32_t *loan);

// An access to a place, asked of borrow freezing only when some loan is rooted
// at the place's variable: what kind of access it is is not worked out otherwise
#define pwAccess(pl, access, node) \
    do { if (pathVars[(pl)->var].loans) loanAccess((pl), (access), (node)); } while (0)

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

// Set by pwPlace when the place's own variable was a marked move's: its use
// was the move, checked there
static int pwMoved = 0;

// The drop-flag client's check of a use of a place's root variable, the
// owner of the value for a match's binding
static void pwDropUse(Place *pl, INode *node, int borrow) {
    if (!pathDrops || pl->deref)
        return;
    VarDclNode *var = pathVars[pl->var].var;
    VarDclNode *owner = flowDropOwner(var);
    dropUse(owner == var ? pl->var : pathVar(owner), pl->use ? pl->use : node, borrow);
}

// May other references reach what this reference points at? Every permission
// but 'uni' may alias.
static int pwMayAlias(INode *reftype) {
    return (permGetFlags(((RefNode *)reftype)->perm) & MayAlias) != 0;
}

// The place reached through the reference 'ref' evaluates to. Through a
// borrowed reference it is a root of its own, what the reference points at,
// keyed by the variable the reference is read from; through an owning one, a
// step further along the reference's own place. Nothing is tracked through a
// raw pointer, or through a reference that is not read from a variable. A
// reference that may alias makes the path shared from there on.
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
        pl->use = refpl.use;
        pl->deref = 1;
        pl->nsteps = 0;
        pl->shared = pwMayAlias(reftype);
        pl->sharedlen = 0;
        return 1;
    }
    *pl = refpl;
    // A dereference cut off by the step limit is not marked shared: the place
    // then stands for more than itself, and is held to the stricter rule
    if (pl->nsteps < PlaceMaxSteps) {
        pwStep(pl, PlaceStepDeref);
        if (!pl->shared && pwMayAlias(reftype)) {
            pl->shared = 1;
            pl->sharedlen = pl->nsteps;
        }
    }
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
        // A value moves out of it, or out through it, here (flowMoveSource
        // marked where): the variable owning the value no longer holds it
        if (pathDrops && (node->flags & (FlagMoveOut | FlagHollowOut))) {
            VarDclNode *owner = flowDropOwner(var);
            dropMove(owner == var ? index : pathVar(owner), node, (node->flags & FlagHollowOut) != 0);
            pwMoved = 1;
        }
        pl->var = index;
        pl->deref = 0;
        pl->nsteps = 0;
        pl->shared = 0;
        pl->sharedlen = 0;
        pl->use = node;
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
    int svmoved = pwMoved;
    pwMoved = 0;
    int found = pwPlace(nodep, &pl, &base);
    int moved = pwMoved;
    pwMoved = svmoved;
    if (!found)
        return base && pwCarries(((IExpNode *)*nodep)->vtype) ? base : NULL;
    if (!moved)
        pwDropUse(&pl, *nodep, 0);
    int moves = move && iexpIsMove(*nodep);
    pwAccess(&pl, moves ? AccessMove : AccessRead, *nodep);
    PathSet *holds = pwPlaceHolds(&pl, *nodep);
    // A holder moved away whole holds nothing here: its value, and the
    // finalizer that might read its borrows, went with the move
    if (moves && !pl.deref && pl.nsteps == 0 && pathVars[pl.var].holder)
        pathSetFacts(pl.var, NULL, NULL);
    return holds;
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
    return pwLend(node, &pl, perm, aswrite ? AccessWrite : loanBorrowAccess(perm), NULL);
}

// A loan of the place 'pl', already walked, made by 'site' with the
// permission 'perm': the access it makes, and the loans its value carries --
// the new one and whatever the holder at the place's root holds. 'loan'
// returns the new loan's id, if asked.
static PathSet *pwLend(INode *site, Place *pl, INode *perm, int access, uint32_t *loan) {
    // A borrow needs the value there, not moved out; a variable never given
    // one may be borrowed, so that a method can fill it in
    pwDropUse(pl, site, 1);
    if (!pathLoans) {
        if (loan)
            *loan = 0;
        return NULL;
    }
    pwAccess(pl, access, site);
    // A container declaring 'NoLoanMut' (an arena) never moves what it lent,
    // whatever is done to it, so a borrow of the container itself needs it only
    // alive, as a borrow through a shared path does: two '&mut a' may be live
    // at once, and 'a.alloc(Spawner[2, &mut a])' is one call. '&uni' and
    // '&imm' still promise what they say.
    Place lent = *pl;
    if (access == AccessBorrow || access == AccessBorrowMut || access == AccessWrite) {
        INode *reftype = iexpGetTypeDcl(site);
        INode *referent = reftype->tag == RefTag ? itypeGetTypeDcl(((RefNode *)reftype)->vtexp) : NULL;
        if (referent && referent->tag == StructTag && ((StructNode *)referent)->lends == LendsNoLoanMut
            && !lent.shared) {
            lent.shared = 1;
            lent.sharedlen = lent.nsteps;
        }
    }
    uint32_t id = loanMake(site, &lent, perm);
    if (loan)
        *loan = id;
    PathVar *pv = &pathVars[pl->var];
    return pathSetAdd(pv->holder ? pv->holds : NULL, id);
}

// An owning reference coerced to a borrowed one ('imm b &i32 = u', 'u' a
// 'So[i32]'; or 'imm b &<mut App = v', 'v' a 'So[App]'): a borrow of what it
// owns, so 'u' may not be moved, replaced or ended while the borrow is used
static int pwIsOwnedLent(CastNode *cast) {
    INode *to = iexpGetTypeDcl((INode *)cast);
    INode *from = iexpGetTypeDcl(cast->exp);
    return (to->tag == RefTag || to->tag == VirtRefTag) && from->tag == to->tag
        && pwIsBorrowed(to) && !pwIsBorrowed(from);
}

static PathSet *pwOwnedLent(CastNode *cast) {
    Place pl;
    PathSet *base;
    if (!pwThrough(&cast->exp, &pl, &base))
        return base;
    INode *perm = ((RefNode *)iexpGetTypeDcl((INode *)cast))->perm;
    return pwLend((INode *)cast, &pl, perm, loanBorrowAccess(perm), NULL);
}

// The method a call calls, when its first argument is the method's receiver
static FnDclNode *pwMethod(FnCallNode *call) {
    if (call->args == NULL || call->args->used == 0 || !isNameUseNode(call->objfn))
        return NULL;
    INode *fn = ((NameUseNode *)call->objfn)->dclnode;
    if (fn == NULL || fn->tag != FnDclTag || !(fn->flags & FlagMethFld))
        return NULL;
    Nodes *parms = ((FnSigNode *)((FnDclNode *)fn)->vtype)->parms;
    return parms->used > 0 && ((VarDclNode *)nodesGet(parms, 0))->namesym == selfName ? (FnDclNode *)fn : NULL;
}

// Does a receiver's borrow with this access change it, so that it is
// two-phase: reserved while the arguments are walked, activated at the call?
static int pwTwoPhase(int access) {
    return access == AccessBorrowMut || access == AccessBorrowUni || access == AccessWrite;
}

// The access a receiver's borrow makes while it is reserved: a read-only
// borrow's, when it is two-phase
static int pwReserved(int access) {
    return pwTwoPhase(access) ? AccessBorrow : access;
}

// A method's receiver taken by reference: a loan of the receiver's place,
// whether borrowed here ('list.push(x)' is '(&mut list).push(x)', the borrow
// injected) or reached through a reference it is handed ('r.push(x)' for 'r
// &mut List' reborrows '*r', with the permission the method declared for
// 'self'). Its place, its loan and its access come back through 'pl', 'loan'
// and 'access'; 'loan' is 0 when the receiver is no place the walk tracks.
static PathSet *pwReceiver(FnCallNode *call, FnDclNode *meth, Place *pl, uint32_t *loan, int *access) {
    INode **recvp = &nodesGet(call->args, 0);
    INode *recv = *recvp;
    PathSet *base;
    *loan = 0;
    if (recv->tag == BorrowTag) {
        RefNode *borrow = (RefNode *)recv;
        if (!pwPlace(&borrow->vtexp, pl, &base))
            return base;
        INode *perm = ((RefNode *)iexpGetTypeDcl(recv))->perm;
        // An operator changing its operand in place writes it ('v <- x')
        *access = (call->flags & FlagLvalOp) ? AccessWrite : loanBorrowAccess(perm);
        return pwLend(recv, pl, perm, pwReserved(*access), loan);
    }
    // An owner lent as the receiver ('a.bump()', 'a' a 'So[R]' or a
    // 'So[App]'): a borrow of what it owns, as pwOwnedLent reads any other lent
    // owner, and not a borrowed reference read through, which would make the
    // path shared
    if (recv->tag == CastTag && pwIsOwnedLent((CastNode *)recv)) {
        if (!pwThrough(&((CastNode *)recv)->exp, pl, &base))
            return base;
        INode *perm = ((RefNode *)iexpGetTypeDcl(recv))->perm;
        *access = loanBorrowAccess(perm);
        return pwLend(recv, pl, perm, pwReserved(*access), loan);
    }
    INode *selftype = iexpGetTypeDcl(nodesGet(((FnSigNode *)meth->vtype)->parms, 0));
    if (!pwIsBorrowed(iexpGetTypeDcl(recv)) || selftype->tag != RefTag)
        return pwValue(recvp, 1);
    if (!pwThrough(recvp, pl, &base))
        return base;
    INode *perm = ((RefNode *)selftype)->perm;
    *access = loanBorrowAccess(perm);
    return pwLend(recv, pl, perm, pwReserved(*access), loan);
}

// A holder's value dies -- at its scope's end, or stored over whole: if its
// finalizer may read a borrow it holds, that is a use of the holder
static void pwHolderDies(uint32_t var) {
    PathVar *pv = &pathVars[var];
    if (pv->pending && itypeDropReadsBorrow(pv->var->vtype))
        loanUse(var, (INode *)pv->var);
}

// A value carrying 'holds' stored into part of a place rooted at 'var' -- its
// own value, or, for 'deref', what that variable (a borrowed reference) points
// at: the holder there holds those loans too, besides what it held. Stored
// through a reference, they go wherever it may point: the reference's holder
// holds them, since what is read back through it carries them, and so does
// each holder one of its loans borrows from, so that after 'imm r = &mut
// outer; *r = v' or 'stash(&mut outer, v)', using 'outer' uses 'v''s loans.
static void pwStoreInto(uint32_t var, int deref, PathSet *holds) {
    PathVar *pv = &pathVars[var];
    if (holds == NULL || !pv->holder)
        return;
    PathSet *held = pv->holds;
    pathSetFacts(var, pathSetUnion(held, holds), pv->pending);
    if (!deref || held == NULL || held == &pathSetAll)
        return;
    for (uint32_t i = 0; i < held->cnt; ++i) {
        PathVar *root = &pathVars[loanRoot(held->ids[i])];
        if (root != pv && root->holder)
            pathSetFacts((uint32_t)(root - pathVars), pathSetUnion(root->holds, holds), root->pending);
    }
}

// Where a store through the reference argument 'ref' lands, keyed as pwPlace
// keys it, found from the node alone (the argument is walked already): for a
// borrow ('&mut outer', '&mut h.list', '&mut *r'), the root of what it
// borrows; for a reference read from a place ('r', 'h.r'), what it points at.
// Returns the root variable's index, or 0 for nothing the walk tracks.
static uint32_t pwStoreTarget(INode *ref, int *deref) {
    while (ref->tag == CastTag && !(ref->flags & FlagConvert))
        ref = ((CastNode *)ref)->exp;
    INode *place = ref;
    *deref = 1;
    if (ref->tag == BorrowTag || ref->tag == ArrayBorrowTag) {
        place = ((RefNode *)ref)->vtexp;
        *deref = 0;
    }
    while (1) {
        VarDclNode *var = pwNamedVar(place);
        if (var)
            return (var->flags & FlagStatic) ? 0 : pathVar(var);
        switch (place->tag) {
        case CastTag:
            if (place->flags & FlagConvert)
                return 0;
            place = ((CastNode *)place)->exp;
            break;
        case FldAccessTag:
        case ArrIndexTag:
            // A virtual reference, a reference to an array and a slice are
            // reached through with no dereference injected
            place = ((FnCallNode *)place)->objfn;
            if (pwIsBorrowed(iexpGetTypeDcl(place)))
                *deref = 1;
            break;
        case DerefTag:
            place = ((StarNode *)place)->vtexp;
            if (pwIsBorrowed(iexpGetTypeDcl(place)))
                *deref = 1;
            break;
        default:
            return 0;
        }
    }
}

// The borrow an argument is, where one is written or lent there: a borrow, or
// an owner lent by a recast (pwOwnedLent), the site its loan is keyed by
static INode *pwLendSite(INode *arg) {
    while (1) {
        if (arg->tag == BorrowTag || arg->tag == ArrayBorrowTag)
            return arg;
        if (arg->tag != CastTag || (arg->flags & FlagConvert))
            return NULL;
        if (pwIsOwnedLent((CastNode *)arg))
            return arg;
        arg = ((CastNode *)arg)->exp;
    }
}

// A call handed a writable borrow of a place that can hold a borrow --
// 'l.push(x)', 'stash(&mut outer, v)', 'fill(r, v)' for 'r &mut Option[&T]'
// -- may store there anything its other arguments carry: every borrow in an
// unannotated signature shares one lifetime (reflifefn.html), as
// fnCallFlowStoredBorrow reads it. So that place's holder holds those loans
// from here on. 'argsets' is what each argument carries; 'recvpl' the place
// of a receiver taken by reference, or NULL.
static void pwCallStores(FnCallNode *call, PathSet **argsets, Place *recvpl) {
    if (call->args == NULL || call->args->used < 2)
        return;
    uint32_t nargs = call->args->used;
    for (uint32_t at = 0; at < nargs; ++at) {
        INode *arg = nodesGet(call->args, at);
        RefNode *argtype = (RefNode *)iexpGetTypeDcl(arg);
        if ((argtype->tag != RefTag && argtype->tag != ArrayRefTag) || !pwIsBorrowed((INode *)argtype)
            || !(permGetFlags(argtype->perm) & MayWrite) || !pwCarries(argtype->vtexp))
            continue;
        PathSet *others = NULL;
        for (uint32_t i = 0; i < nargs; ++i) {
            if (i == at || argsets[i] == NULL)
                continue;
            // A borrow written as the argument is stored itself only where
            // the place's type can hold a borrow of what it borrows: the
            // world's pool and the list of its creatures, each lent '&mut' to
            // one call, are not stored in each other
            PathSet *carried = argsets[i];
            INode *site = pwLendSite(nodesGet(call->args, i));
            uint32_t own = site ? loanAt(site) : 0;
            if (own && pathSetHas(carried, own) && !itypeHoldsBorrowOf(argtype->vtexp,
                    itypeGetTypeDcl(((RefNode *)iexpGetTypeDcl(site))->vtexp)))
                carried = pathSetWithout(carried, own);
            others = pathSetUnion(others, carried);
        }
        if (others == NULL)
            continue;
        int deref = 0;
        uint32_t target;
        if (at == 0 && recvpl) {
            target = recvpl->var;
            deref = recvpl->deref;
        }
        else
            target = pwStoreTarget(arg, &deref);
        if (target)
            pwStoreInto(target, deref, others);
    }
}

// A call: the function reference it calls through, then each argument in
// order, each carrying its loans in flight until the call is made. A borrow
// the call returns (or a value holding one) carries every argument's loans --
// Cone's rule for an unannotated signature is that every borrow in it shares
// one lifetime (reflifefn.html) -- so a borrow a method returns keeps its
// receiver loaned while it is used. A container declaring a no-loan kind
// ('NoLoanMut', an arena; 'NoLoanRead', for a read-only borrow) lends with
// no loan on it: its receiver's place gets only a pin, which forbids moving,
// replacing or ending it.
static PathSet *pwCall(FnCallNode *call) {
    INode *objfn = call->objfn;
    if (pwNamedVar(objfn) || objfn->tag == DerefTag || objfn->tag == FldAccessTag)
        pwValue(&call->objfn, 0);
    FnDclNode *meth = pwMethod(call);
    int carries = pwCarries(call->vtype);
    uint32_t mark = loanFlightMark();
    PathSet *result = NULL;
    PathSet *recvholds = NULL;
    Place recvpl;
    uint32_t recvloan = 0;
    int recvaccess = AccessRead;
    int twophase = 0;
    // What each argument carries, for a call that may store some of them
    // through another (pwCallStores)
    PathSet *localsets[8];
    PathSet **argsets = call->args == NULL || call->args->used <= 8 ? localsets
        : (PathSet **)memAllocBlk(call->args->used * sizeof(PathSet *));
    INode **argsp;
    uint32_t cnt;
    uint32_t argi = 0;
    for (nodesFor(call->args, cnt, argsp)) {
        PathSet *carried;
        if (meth && argsp == &nodesGet(call->args, 0)) {
            carried = pwReceiver(call, meth, &recvpl, &recvloan, &recvaccess);
            twophase = recvloan && pwTwoPhase(recvaccess);
            loanFlightPush(carried, twophase ? recvloan : 0);
            recvholds = carried;
            argsets[argi++] = carried;
            continue;
        }
        carried = pwValue(argsp, 1);
        loanFlightPush(carried, 0);
        argsets[argi++] = carried;
        if (carries)
            result = pathSetUnion(result, carried);
    }
    // The receiver's borrow activated: against what the other arguments carry,
    // and then as the access it is, against every loan still held
    if (twophase)
        loanFlightActivate(mark, recvloan, recvaccess, nodesGet(call->args, 0));
    loanFlightPop(mark);
    if (twophase)
        pwAccess(&recvpl, recvaccess, nodesGet(call->args, 0));
    if (pathLoans)
        pwCallStores(call, argsets, recvloan ? &recvpl : NULL);
    if (!carries)
        return NULL;
    if (recvloan) {
        INode *recvtype = iexpGetTypeDcl(nodesGet(call->args, 0));
        INode *container = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode *)recvtype)->vtexp) : NULL;
        uint8_t lends = container && container->tag == StructTag ? ((StructNode *)container)->lends : LendsLoaned;
        INode *rettype = iexpGetTypeDcl((INode *)call);
        if (lends == LendsNoLoanMut || (lends == LendsNoLoanRead && rettype->tag == RefTag
                && !(permGetFlags(((RefNode *)rettype)->perm) & MayWrite))) {
            // The receiver's own loan ends with the call; what its root holds stays
            PathVar *pv = &pathVars[recvpl.var];
            uint32_t pin = loanMake((INode *)call, &recvpl, (INode *)opaqPerm);
            loanReturnedBy(pin, ((NameUseNode *)call->objfn)->namesym);
            recvholds = pathSetAdd(pv->holder ? pv->holds : NULL, pin);
        }
        else
            loanReturnedBy(recvloan, ((NameUseNode *)call->objfn)->namesym);
    }
    return pathSetUnion(result, recvholds);
}

// Store a value carrying 'holds' into an lval. 'rvalp' is the value's slot,
// or NULL where the value is one element of another (a destructuring).
static void pwStore(INode **lvalp, PathSet *holds, INode **rvalp) {
    VarDclNode *var = pwNamedVar(*lvalp);
    if (var) {
        if (var->namesym == anonName)
            return;
        // The whole variable is replaced: not a use of it. A holder reassigned
        // was not live before, so what its old value was pending on is dropped.
        uint32_t index = pathVar(var);
        Place pl = { index, 0, 0 };
        pwAccess(&pl, itypeNeedsFinal(var->vtype) ? AccessReplace : AccessWrite, *lvalp);
        if (pathVars[index].holder) {
            pwHolderDies(index);
            pathSetFacts(index, holds, NULL);
        }
        // What it held is released here, if it held anything; now it holds its value
        if (pathVars[index].tracked) {
            dropStore(index, *lvalp, rvalp);
            pathSetState(index, DropWhole);
        }
        return;
    }
    Place pl;
    PathSet *base;
    if (!pwPlace(lvalp, &pl, &base))
        return;
    pwAccess(&pl, AccessWrite, *lvalp);
    // Part of a local's own value: its old value is released if the local holds one
    if (pathDrops && !pl.deref && pathVars[pl.var].tracked && flowLvalRootVar(*lvalp) == pathVars[pl.var].var)
        dropPartStore(pl.var, *lvalp);
    pwStoreInto(pl.var, pl.deref, holds);
}

static PathSet *pwAssign(AssignNode *node) {
    PathSet *holds = pwValue(&node->rval, 1);
    if (node->lval->tag == VTupleTag) {
        INode **lvalp;
        uint32_t cnt;
        uint32_t index = 0;
        // A parallel assignment stores each value in its own slot
        Nodes *rvals = node->rval->tag == VTupleTag ? ((TupleNode *)node->rval)->elems : NULL;
        for (nodesFor(((TupleNode *)node->lval)->elems, cnt, lvalp)) {
            pwStore(lvalp, holds, rvals && index < rvals->used ? &nodesGet(rvals, index) : NULL);
            ++index;
        }
    }
    else
        pwStore(&node->lval, holds, node->rval->tag == VTupleTag && !assignOneTakesTuple(node) ? &nodesGet(((TupleNode *)node->rval)->elems, 0) : &node->rval);
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
            pl.shared = 0;
            pl.sharedlen = 0;
            pl.use = *sides[i];
            holds[i] = pathVars[whole[i]].holds;
            pwAccess(&pl, itypeNeedsFinal(var->vtype) ? AccessReplace : AccessWrite, *sides[i]);
            pwDropUse(&pl, *sides[i], 0);
        }
        else if (pwPlace(sides[i], &pl, &base)) {
            pwDropUse(&pl, *sides[i], 0);
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
    // It holds nothing until its value is stored, again on each pass of a loop
    if (pathVars[index].tracked)
        pathSetState(index, DropUninit);
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
    if (var->value && pathVars[index].tracked)
        pathSetState(index, DropWhole);
}

// The variables declared since 'from' leave scope, the last declared first.
// Each holder among them dies -- its finalizer's use of what it holds, then
// what it was pending on is dropped -- and each variable ends, which is an
// access conflicting with any loan of it a holder still alive holds: one
// declared outside, or before it here and finalized after it
static void pwScopeEnd(uint32_t from) {
    for (uint32_t i = ndecls; i > from; --i) {
        uint32_t index = decls[i - 1];
        PathVar *pv = &pathVars[index];
        if (pv->holder && (pv->holds || pv->pending)) {
            pwHolderDies(index);
            pathSetFacts(index, NULL, NULL);
        }
        if (pv->loans) {
            Place pl = { index, 0, 0 };
            pwAccess(&pl, AccessEnd, (INode *)pv->var);
        }
        // Out of scope, it holds nothing on any path, so the paths leaving it
        // agree about it where they join
        if (pathVars[index].state)
            pathSetState(index, 0);
    }
}

// The same, as a block hands on 'value' to the holder it goes to: that value
// is still being made, so a variable ending while it carries a loan of it is
// reported at once ('imm h = { imm x = ..; new H(&x); }')
static void pwScopeEndHanding(uint32_t from, PathSet *value) {
    uint32_t mark = loanFlightMark();
    loanFlightPush(value, 0);
    pwScopeEnd(from);
    loanFlightPop(mark);
}

// A scope's exit, which releases each variable declared since 'from': what
// each may hold here is recorded for its release
static void pwExit(INode *exit, uint32_t from) {
    if (pathDrops && ndecls > from)
        dropExit(exit, &decls[from], ndecls - from);
}

// A 'break' or 'continue' to 'target', handing it 'value': the blocks it
// leaves end, and the state goes to the target
static void pwJump(BlockNode *target, PathSet *value, int iscontinue, INode *exit) {
    uint32_t f = nframes;
    while (f > 0 && frames[f - 1].blk != target)
        --f;
    if (f == 0) {
        dead = 1;
        return;
    }
    PathFrame *frame = &frames[f - 1];
    pwExit(exit, frame->declstart);
    pwScopeEndHanding(frame->declstart, value);
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
            pwJump(brk->block, brkval, 0, *nodesp);
            break;
        }
        case ContinueTag:
            pwJump(((BreakRetNode *)*nodesp)->block, NULL, 1, *nodesp);
            break;
        case ReturnTag:
        {
            // Every variable is a local of this function, and dies with it
            BreakRetNode *ret = (BreakRetNode *)*nodesp;
            if (ret->exp && ret->exp != unknownType && isExpNode(ret->exp))
                pwValue(&ret->exp, 1);
            pwExit(*nodesp, 0);
            pwScopeEnd(0);
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
            if (!dead)
                pwExit(*nodesp, frames[nframes - 1].declstart);
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

// The parameters of the function being walked: variables of its block
static Nodes *pwParms = NULL;

static PathSet *pwBlock(BlockNode *blk, int fnblock, int move) {
    uint32_t f = nframes;
    pwFramePush(blk);
    if (fnblock && pathDrops) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(pwParms, cnt, nodesp)) {
            uint32_t index = pathVar((VarDclNode *)*nodesp);
            if (ndecls == declcap)
                decls = (uint32_t *)pathGrow(decls, &declcap, sizeof(uint32_t));
            decls[ndecls++] = index;
            if (pathVars[index].tracked)
                pathSetState(index, DropWhole);
        }
    }
    PathSet *value = pwStmts(blk, move);
    if (!dead && !fnblock)
        pwScopeEndHanding(frames[f].declstart, move ? value : NULL);
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
        return pwCall((FnCallNode *)node);
    case BorrowTag:
    case ArrayBorrowTag:
        return pwBorrow(node, 0);
    case AllocateTag:
        return pwValue(&((RefNode *)node)->vtexp, 1);
    case VTupleTag:
    case TypeLitTag:
    {
        // Each element's loans are in flight until the value is built
        PathSet *holds = NULL;
        INode **nodesp;
        uint32_t cnt;
        uint32_t mark = loanFlightMark();
        Nodes *elems = node->tag == VTupleTag ? ((TupleNode *)node)->elems : ((FnCallNode *)node)->args;
        for (nodesFor(elems, cnt, nodesp)) {
            INode **valp = (*nodesp)->tag == NamedValTag ? &((NamedValNode *)*nodesp)->val : nodesp;
            PathSet *carried = pwValue(valp, node->tag == TypeLitTag || move);
            loanFlightPush(carried, 0);
            holds = pathSetUnion(holds, carried);
        }
        loanFlightPop(mark);
        return holds;
    }
    case ArrayLitTag:
    {
        PathSet *holds = NULL;
        INode **nodesp;
        uint32_t cnt;
        uint32_t mark = loanFlightMark();
        for (nodesFor(((ArrayNode *)node)->elems, cnt, nodesp)) {
            PathSet *carried = pwValue(nodesp, 1);
            loanFlightPush(carried, 0);
            holds = pathSetUnion(holds, carried);
        }
        loanFlightPop(mark);
        return holds;
    }
    case CastTag:
    {
        if (!(node->flags & FlagConvert) && pwIsOwnedLent((CastNode *)node))
            return pwOwnedLent((CastNode *)node);
        // A conversion moves nothing but an owner it carries; whatever borrow
        // its value holds came from its operand ('case imm e Some' converts
        // the matched 'Option[&T]' to its 'Some[&T]')
        PathSet *holds = pwValue(&((CastNode *)node)->exp, flowCastCarries(node) ? move : 0);
        return pwCarries(((IExpNode *)node)->vtype) ? holds : NULL;
    }
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
    case TempTag:
        return pwValue(&((TempNode *)node)->exp, move);
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

void flowPathWalk(FnDclNode *fndcl, int loans, int drops) {
    // The walk's state is file-static, as flow's variable stack is: safe
    // because flow never runs re-entrantly, which this holds it to
    static int walking = 0;
    if (walking) {
        errorUnreachable((INode *)fndcl, "a function the loan walk was asked to walk while walking another");
        return;
    }
    walking = 1;
    ++statFns;
    if (loans)
        ++statLoanFns;
    pathLoans = loans;
    pathDrops = drops;
    loanWalkBegin();
    if (drops)
        dropWalkBegin();
    int errorsOnEntry = errors;
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
    pwParms = ((FnSigNode *)fndcl->vtype)->parms;
    for (nodesFor(pwParms, cnt, nodesp))
        pathVar((VarDclNode *)*nodesp);
    pwBlock((BlockNode *)fndcl->value, 1, 1);
    if (drops)
        dropWalkEnd(errors == errorsOnEntry);

    // A variable's index is the walk's own
    for (uint32_t i = 1; i < nvars; ++i)
        pathVars[i].var->flowindex = 0;
    walking = 0;
}

void flowPathPrint() {
    printf("Path walk: %u functions (%u for loans), %u loops, %u walked again, %u widened\n\n",
        statFns, statLoanFns, statLoops, statRewalks, statCapped);
}
