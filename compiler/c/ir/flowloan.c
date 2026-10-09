/** Borrow freezing: loans, their conflicts, and the pending conflicts that
 * fire at a holder's next use
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

// How a loan restricts its source, from the borrow's permission and from the
// path the source is reached by. Cone's 'mut' is shared: other references may
// read and change the value too, so only a source reached as 'uni' (a local,
// or through 'uni' references) can be frozen against them. The refperm.html
// manual page, "From 'uni'", is the rule; flow.md, "The loan walk", the note.
enum LoanKind {
    LoanShared,     // read-only: 'ro' or 'imm' of a 'uni' source; '&imm' of a shared one
    LoanExcl,       // may write: 'mut', 'uni', 'mut1' of a 'uni' source; '&uni' of any
    LoanAlias,      // 'mut', 'ro', 'mut1' of a shared source: it needs the source only alive
    LoanPin,        // neither: 'opaq', which holds only the address
    LoanCaller,     // what the caller lent through a parameter: frozen by the caller, nothing here conflicts
};

typedef struct {
    INode *site;        // the borrow that made it
    Name *by;           // the method whose returned borrow carries it, for the message, or NULL
    Place place;        // what it borrows
    uint32_t next;      // the next loan with the same root variable
    uint32_t mayhold;   // where in 'maypool' every holder that may hold it on some path is
    uint16_t nmay;
    uint16_t maycap;
    uint8_t kind;       // LoanKind
    uint8_t writes;     // the borrow may write, for the message
    uint8_t part;       // a caller loan: the part of its parameter it lends (LifePart)
    INode *shapecont;   // a borrow into a shape-changing value through a shared path: its struct, else NULL
} Loan;

// A pending conflict: 'access' conflicted with 'loan', held by 'holder'. On a
// GPU target, one of kind PendingChosen is instead a holder given where 'loan'
// and 'other' point on paths that joined (loanChosenPending)
typedef struct {
    INode *access;
    uint32_t loan;
    uint32_t other;
    uint32_t holder;
    uint8_t kind;       // PathAccess, or PendingChosen
    uint8_t fired;
} Pending;
#define PendingChosen 0xFF
// A call that could reshape the collection a holder's borrow points into
// (loanShapePend): fired by a use of the holder; 'other' is its ShapeNote
#define PendingShape 0xFD
// A variable's live mark at a seam (loanSeamLive): fired, it records the
// variable as used after the seam, and reports nothing
#define PendingSeamLive 0xFE

static Loan *loans = NULL;
static uint32_t nloans = 0;
static uint32_t loancap = 0;
static Pending *pendings = NULL;
static uint32_t npendings = 0;
static uint32_t pendingcap = 0;

// Each loan's holders, a run apiece
static uint32_t *maypool = NULL;
static uint32_t nmaypool = 0;
static uint32_t maypoolcap = 0;

// Holders whose loans were widened to all of them, by a loop that would not settle
static uint32_t nflights;

static uint32_t *saturated = NULL;
static uint32_t nsaturated = 0;
static uint32_t saturatedcap = 0;

// The loans into a shape-changing value through a shared path (loanShapeMark),
// and what each call that could reshape one was (loanShapePend), a pending
// conflict's 'other'
typedef struct {
    INode *call;
    Name *callee;
    uint8_t why;        // ReshapeWhy
} ShapeNote;

static uint32_t *shapeloans = NULL;
static uint32_t nshapeloans = 0;
static uint32_t shapeloancap = 0;
static ShapeNote *shapenotes = NULL;
static uint32_t nshapenotes = 0;
static uint32_t shapenotecap = 0;

// *********************
// A small map from a node (with two numbers) to an id: which loan a borrow
// made, which pending conflict an access recorded against a holder, and which
// accesses were already reported. Slots from an earlier function are told by
// their generation, so a walk does not clear the table.
// *********************

typedef struct {
    void *node;
    uint32_t a;
    uint32_t b;
    uint32_t id;
    uint32_t gen;
} MapSlot;

static MapSlot *map = NULL;
static uint32_t mapcap = 0;     // a power of two
static uint32_t mapused = 0;
static uint32_t mapgen = 0;

static uint32_t mapHash(void *node, uint32_t a, uint32_t b) {
    uint64_t h = (uint64_t)(uintptr_t)node * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)a << 32 | b) * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h >> 32) ^ (uint32_t)h;
}

static MapSlot *mapSlot(void *node, uint32_t a, uint32_t b) {
    uint32_t mask = mapcap - 1;
    uint32_t i = mapHash(node, a, b) & mask;
    while (map[i].gen == mapgen) {
        if (map[i].node == node && map[i].a == a && map[i].b == b)
            return &map[i];
        i = (i + 1) & mask;
    }
    return &map[i];
}

// The map, like every buffer the walk keeps, comes from the compiler's arena,
// small at first: a buffer grown freshly from the system costs a compile more,
// in first touches, than the walk itself does
static void mapGrow() {
    MapSlot *old = map;
    uint32_t oldcap = mapcap;
    mapcap = oldcap ? oldcap << 1 : 64;
    map = (MapSlot *)memAllocBlk(mapcap * sizeof(MapSlot));
    memset(map, 0, mapcap * sizeof(MapSlot));
    uint32_t oldgen = mapgen;
    mapgen = 1;
    mapused = 0;
    for (uint32_t i = 0; i < oldcap; ++i) {
        if (old[i].gen == oldgen && oldgen != 0) {
            MapSlot *slot = mapSlot(old[i].node, old[i].a, old[i].b);
            *slot = old[i];
            slot->gen = mapgen;
            ++mapused;
        }
    }
}

// The id stored for this key, or 0
static uint32_t mapGet(void *node, uint32_t a, uint32_t b) {
    MapSlot *slot = mapSlot(node, a, b);
    return slot->gen == mapgen ? slot->id : 0;
}

static void mapPut(void *node, uint32_t a, uint32_t b, uint32_t id) {
    if ((mapused + 1) * 2 > mapcap)
        mapGrow();
    MapSlot *slot = mapSlot(node, a, b);
    if (slot->gen != mapgen)
        ++mapused;
    slot->node = node;
    slot->a = a;
    slot->b = b;
    slot->id = id;
    slot->gen = mapgen;
}

void loanWalkBegin() {
    if (mapcap == 0)
        mapGrow();
    // A new generation empties the map; at the wrap, clear it for real
    if (++mapgen == 0) {
        memset(map, 0, mapcap * sizeof(MapSlot));
        mapgen = 1;
    }
    mapused = 0;
    nloans = 1;         // loan 0 is "none"
    npendings = 1;
    nmaypool = 0;
    nsaturated = 0;
    nflights = 0;
    nshapeloans = 0;
    nshapenotes = 0;
}

// *********************
// Loans
// *********************

int loanBorrowAccess(INode *perm) {
    uint16_t flags = permGetFlags(perm);
    if (flags & MayWrite)
        return (flags & MayAlias) ? AccessBorrowMut : AccessBorrowUni;
    if (flags & MayRead)
        return itypeGetTypeDcl(perm) == (INode *)immPerm ? AccessBorrowImm : AccessBorrow;
    return AccessBorrowOpaq;
}

static uint8_t loanKindOf(INode *perm, Place *pl) {
    switch (loanBorrowAccess(perm)) {
    case AccessBorrowUni:
        return LoanExcl;
    case AccessBorrowMut:
        return pl->shared ? LoanAlias : LoanExcl;
    case AccessBorrowImm:
        return LoanShared;
    case AccessBorrow:
        return pl->shared ? LoanAlias : LoanShared;
    default:
        return LoanPin;
    }
}

uint32_t loanMake(INode *site, Place *pl, INode *perm) {
    uint32_t id = mapGet(site, 0, 0);
    if (id)
        return id;
    if (nloans > LoanIdMask) {
        errorUnreachable(site, "more loans in one function than an entry of a loan set can name");
        return 0;
    }
    if (nloans >= loancap)
        loans = (Loan *)pathGrow(loans, &loancap, sizeof(Loan));
    id = nloans++;
    Loan *loan = &loans[id];
    loan->site = site;
    loan->by = NULL;
    loan->place = *pl;
    loan->mayhold = 0;
    loan->nmay = 0;
    loan->maycap = 0;
    loan->kind = loanKindOf(perm, pl);
    loan->writes = (permGetFlags(perm) & MayWrite) != 0;
    loan->part = 0;
    loan->shapecont = NULL;
    // Linked from its root variable, so an access finds it
    loan->next = pathVars[pl->var].loans;
    pathVars[pl->var].loans = id;
    mapPut(site, 0, 0, id);
    return id;
}

// A caller loan is not linked from its parameter: no access here meets it.
// Each part's is keyed by the parameter and the part, apart from every other
// key the map holds: a borrow's (site, 0, 0), a report's (node, 0, 1) and a
// pending conflict's (access, loan, holder), whose loan is never 0.
#define loanCallerKey(part) (0x80000000u | (part))
uint32_t loanCaller(uint32_t var, uint32_t part) {
    VarDclNode *parm = pathVars[var].var;
    uint32_t id = mapGet(parm, 0, loanCallerKey(part));
    if (id)
        return id;
    if (nloans > LoanIdMask) {
        errorUnreachable((INode *)parm, "more loans in one function than an entry of a loan set can name");
        return 0;
    }
    if (nloans >= loancap)
        loans = (Loan *)pathGrow(loans, &loancap, sizeof(Loan));
    id = nloans++;
    Loan *loan = &loans[id];
    loan->site = (INode *)parm;
    loan->by = NULL;
    memset(&loan->place, 0, sizeof(Place));
    loan->place.var = var;
    loan->place.deref = 1;
    loan->next = 0;
    loan->mayhold = 0;
    loan->nmay = 0;
    loan->maycap = 0;
    loan->kind = LoanCaller;
    loan->writes = 0;
    loan->part = (uint8_t)part;
    loan->shapecont = NULL;
    mapPut(parm, 0, loanCallerKey(part), id);
    return id;
}

uint32_t loanRoot(uint32_t loan) {
    return loans[loan].place.var;
}

int loanThrough(uint32_t loan) {
    return loans[loan].place.deref;
}

int loanNamesThrough(uint32_t loan, uint32_t var) {
    return loans[loan].kind != LoanCaller && loans[loan].place.deref && loans[loan].place.var == var;
}

int loanWhole(uint32_t loan) {
    return loans[loan].place.nsteps == 0 && !loans[loan].place.far;
}

// A variable whose storage outlives every call: a global, or a static
static int loanVarIsGlobal(VarDclNode *var) {
    return var->scope == 0 || (var->flags & FlagStatic);
}

int loanIsLocal(uint32_t id) {
    Loan *loan = &loans[id];
    return loan->kind != LoanCaller && !loan->place.deref
        && !loanVarIsGlobal(pathVars[loan->place.var].var);
}

uint32_t loanLocalIn(PathSet *set) {
    if (set == &pathSetAll) {
        for (uint32_t id = 1; id < nloans; ++id) {
            if (loanIsLocal(id))
                return id;
        }
        return 0;
    }
    if (set == NULL)
        return 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (loanIsLocal(loanOf(set->ids[i])))
            return loanOf(set->ids[i]);
    }
    return 0;
}

// Is this the type of a borrowed reference?
static int loanIsBorrowType(INode *type) {
    return (type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef;
}

// May a reference to a 'referent' point at what this near loan borrows? Where
// the walk could not tell near from far -- the reference is a call's result,
// or was read through another reference -- its near loans hold some of what
// it points at holds too. A loan of a struct of another type than the
// referent is one of those (a borrow is coerced only to a trait, its own or a
// base's, or to a slice), and so is the caller loan of a parameter that is no
// reference, which stands for the borrows it holds. Anything not known to be
// one may be what it points at.
static int loanMayBePointee(Loan *loan, INode *referent) {
    if (referent == NULL)
        return 1;
    INode *borrowed;
    if (loan->kind == LoanCaller) {
        INode *parmtype = itypeGetTypeDcl(pathVars[loan->place.var].var->vtype);
        if (!loanIsBorrowType(parmtype))
            return 0;
        // What a parameter's reference points at holds: a reference holding
        // that may point anywhere in it
        if (loan->part != LifePartOwn)
            return 1;
        borrowed = ((RefNode *)parmtype)->vtexp;
    }
    else {
        INode *sitetype = iexpGetTypeDcl(loan->site);
        if (!loanIsBorrowType(sitetype))
            return 1;
        borrowed = ((RefNode *)sitetype)->vtexp;
    }
    INode *b = itypeGetTypeDcl(borrowed);
    INode *r = itypeGetTypeDcl(referent);
    return b == r || b->tag != StructTag || r->tag != StructTag
        || (b->flags & TraitType) || (r->flags & TraitType);
}

int loanMayPointOut(PathSet *refholds, INode *referent, int beyond) {
    if (refholds == NULL || refholds == &pathSetAll)
        return 1;
    int local = 0;
    for (uint32_t i = 0; i < refholds->cnt; ++i) {
        // A far loan is held where the reference points, not pointed at
        if ((refholds->ids[i] & LoanFar) && !beyond)
            continue;
        uint32_t id = loanOf(refholds->ids[i]);
        Loan *loan = &loans[id];
        // A reborrow through another reference points where that one does,
        // and what that one held came along with it
        if ((loan->kind != LoanCaller && loan->place.deref) || !loanMayBePointee(loan, referent))
            continue;
        if (!loanIsLocal(id) || loan->place.owned)
            return 1;
        local = 1;
    }
    return !local;
}

uint32_t loanAt(INode *site) {
    return mapGet(site, 0, 0);
}

// The parameter a caller loan stands for
static VarDclNode *loanParm(uint32_t id) {
    return pathVars[loans[id].place.var].var;
}

uint32_t loanCallerApart(FnSigNode *sig, PathSet *set, INode *wanted) {
    if (set == NULL)
        return 0;
    uint32_t n = set == &pathSetAll ? nloans : set->cnt;
    for (uint32_t i = set == &pathSetAll ? 1 : 0; i < n; ++i) {
        uint32_t id = set == &pathSetAll ? i : loanOf(set->ids[i]);
        if (loans[id].kind == LoanCaller && !lifePartFlows(sig, loanParm(id)->vtype, loans[id].part, wanted))
            return id;
    }
    return 0;
}

// The type of what a reference holding the caller loan 'id' near points at:
// what the parameter's own reference points at, or, for what that holds,
// what the held borrows point at -- for a struct declaring lifetimes, the
// struct itself, whose slots each hold their own
static INode *loanCallerPlace(uint32_t id) {
    INode *parmtype = loanParm(id)->vtype;
    if (loans[id].part == LifePartOwn)
        return lifePointee(parmtype);
    INode *held = lifeHeld(parmtype);
    if (held == NULL)
        return lifePointee(parmtype);
    return loans[id].part == LifePartHeld ? lifePointee(held) : held;
}

uint32_t loanStoredApart(FnSigNode *sig, PathSet *stored, PathSet *refholds, int beyond, uint32_t landing,
        VarDclNode **through) {
    if (refholds == NULL || stored == NULL)
        return 0;
    uint32_t n = refholds == &pathSetAll ? nloans : refholds->cnt;
    uint32_t nstored = stored == &pathSetAll ? nloans : stored->cnt;
    for (uint32_t i = refholds == &pathSetAll ? 1 : 0; i < n; ++i) {
        uint32_t id = refholds == &pathSetAll ? i : refholds->ids[i];
        if ((id & LoanFar) && !beyond)
            continue;
        id = loanOf(id);
        if (loans[id].kind != LoanCaller)
            continue;
        VarDclNode *parm = loanParm(id);
        INode *place = loanCallerPlace(id);
        int slotted = lifeSlotted(place) != NULL;
        // The field a store lands in is known only where it is in what the
        // parameter's own reference points at
        uint32_t slots = loans[id].part == LifePartOwn && !beyond ? landing : 0;
        for (uint32_t k = stored == &pathSetAll ? 1 : 0; k < nstored; ++k) {
            uint32_t sid = stored == &pathSetAll ? k : loanOf(stored->ids[k]);
            if (loans[sid].kind != LoanCaller)
                continue;
            VarDclNode *sparm = loanParm(sid);
            uint32_t spart = loans[sid].part;
            int flows;
            if (slotted && slots)
                flows = lifePartFlowsSlots(sig, sparm->vtype, spart, place, slots, 0);
            // What the parameter's own struct holds, moved within it to a
            // field not known: the caller's view of its slots keeps it where
            // it was, so it must be at home in every one
            else if (slotted && sparm == parm && spart != LifePartOwn)
                flows = lifePartFlowsSlots(sig, sparm->vtype, spart, place, LifeAllSlots, 1);
            else
                flows = lifePartFlows(sig, sparm->vtype, spart, place);
            if (!flows) {
                *through = parm;
                return sid;
            }
        }
    }
    return 0;
}

uint32_t loanNotGlobalIn(PathSet *set) {
    if (set == NULL)
        return 0;
    uint32_t n = set == &pathSetAll ? nloans : set->cnt;
    for (uint32_t i = set == &pathSetAll ? 1 : 0; i < n; ++i) {
        uint32_t id = set == &pathSetAll ? i : loanOf(set->ids[i]);
        if (loans[id].kind == LoanCaller || loanIsLocal(id))
            return id;
    }
    return 0;
}

int loanIsGlobal(uint32_t id) {
    return loans[id].kind != LoanCaller && !loanIsLocal(id);
}

uint32_t loanNotBoundIn(FnSigNode *sig, PathSet *set, Name *bound) {
    if (set == NULL || set == &pathSetAll)
        return 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (!(set->ids[i] & LoanFar))
            continue;
        uint32_t id = loanOf(set->ids[i]);
        if (loanIsLocal(id))
            return id;
        if (loans[id].kind == LoanCaller && !lifePartOutlives(sig, loanParm(id)->vtype, loans[id].part, bound))
            return id;
    }
    return 0;
}

uint32_t loanNotStaticIn(FnSigNode *sig, PathSet *set) {
    if (set == NULL)
        return 0;
    uint32_t n = set == &pathSetAll ? nloans : set->cnt;
    for (uint32_t i = set == &pathSetAll ? 1 : 0; i < n; ++i) {
        uint32_t id = set == &pathSetAll ? i : loanOf(set->ids[i]);
        if (loanIsLocal(id))
            return id;
        if (loans[id].kind == LoanCaller
            && !lifePartOutlives(sig, loanParm(id)->vtype, loans[id].part, staticLifeName))
            return id;
    }
    return 0;
}

uint32_t loanNotGlobalInAs(PathSet *set, int near, int far) {
    if (set == NULL || set == &pathSetAll)
        return near || far ? loanNotGlobalIn(set) : 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if ((set->ids[i] & LoanFar) ? !far : !near)
            continue;
        uint32_t id = loanOf(set->ids[i]);
        if (loans[id].kind == LoanCaller || loanIsLocal(id))
            return id;
    }
    return 0;
}

void loanHeldBy(uint32_t var, PathSet *holds) {
    if (holds == &pathSetAll) {
        for (uint32_t i = 0; i < nsaturated; ++i) {
            if (saturated[i] == var)
                return;
        }
        if (nsaturated == saturatedcap)
            saturated = (uint32_t *)pathGrow(saturated, &saturatedcap, sizeof(uint32_t));
        saturated[nsaturated++] = var;
        return;
    }
    for (uint32_t i = 0; i < holds->cnt; ++i) {
        Loan *loan = &loans[loanOf(holds->ids[i])];
        uint16_t k;
        for (k = 0; k < loan->nmay; ++k) {
            if (maypool[loan->mayhold + k] == var)
                break;
        }
        if (k < loan->nmay)
            continue;
        // A full run moves to the pool's end, twice the size
        if (loan->nmay == loan->maycap) {
            uint16_t cap = loan->maycap ? loan->maycap << 1 : 2;
            while (nmaypool + cap > maypoolcap)
                maypool = (uint32_t *)pathGrow(maypool, &maypoolcap, sizeof(uint32_t));
            memcpy(&maypool[nmaypool], &maypool[loan->mayhold], loan->nmay * sizeof(uint32_t));
            loan->mayhold = nmaypool;
            loan->maycap = cap;
            nmaypool += cap;
        }
        maypool[loan->mayhold + loan->nmay++] = var;
    }
}

// Does this access, of the place 'pl', conflict with this live loan? A
// read-only loan lets its source be read and borrowed read-only again; a loan
// that may write lets nothing touch the source but itself; a loan of a source
// reached through a shared path, which others may read and change anyway,
// needs the source only to stay alive, and to promise no more than it does,
// so it refuses only an '&uni' or '&imm' borrow of it besides moving,
// replacing and ending it; an '&opaq' loan holds only the address, so only
// moving, replacing or ending the source conflicts with it.
static int loanConflictsAs(int access, uint8_t kind, Loan *loan, Place *pl) {
    if (kind == LoanCaller)
        return 0;
    // A place on the way to the shared path -- the field holding a 'Rc[mut, T]'
    // owner the loan was reached through -- the loan reads: changing the
    // owner there would end what it borrows
    if (kind == LoanAlias && pl->nsteps < loan->place.sharedlen)
        kind = LoanShared;
    switch (access) {
    case AccessRead:
    case AccessBorrow:
        return kind == LoanExcl;
    case AccessBorrowImm:
        return kind == LoanExcl || kind == LoanAlias;
    case AccessWrite:
    case AccessBorrowMut:
        return kind == LoanExcl || kind == LoanShared;
    case AccessBorrowUni:
        return kind != LoanPin;
    case AccessMove:
    case AccessReplace:
    case AccessEnd:
        return 1;
    default:    // AccessBorrowOpaq reads and writes nothing
        return 0;
    }
}

static int loanConflicts(int access, Loan *loan, Place *pl) {
    return loanConflictsAs(access, loan->kind, loan, pl);
}

// Do two paths from one root overlap? Only two different fields are disjoint.
static int placeOverlaps(Place *a, Place *b) {
    uint8_t n = a->nsteps < b->nsteps ? a->nsteps : b->nsteps;
    for (uint8_t i = 0; i < n; ++i) {
        uintptr_t sa = a->steps[i];
        uintptr_t sb = b->steps[i];
        if (sa != sb && (sa & 1) == 0 && (sb & 1) == 0)
            return 0;
    }
    return 1;
}

// The pending conflict of this access with this loan, held by this holder
static uint32_t loanPending(INode *access, int kind, uint32_t loan, uint32_t holder) {
    uint32_t id = mapGet(access, loan, holder);
    if (id)
        return id;
    if (npendings >= pendingcap)
        pendings = (Pending *)pathGrow(pendings, &pendingcap, sizeof(Pending));
    id = npendings++;
    Pending *pend = &pendings[id];
    pend->access = access;
    pend->loan = loan;
    pend->other = 0;
    pend->holder = holder;
    pend->kind = (uint8_t)kind;
    pend->fired = 0;
    mapPut(access, loan, holder, id);
    return id;
}

static void loanPend(INode *node, int access, uint32_t loan, uint32_t holder) {
    PathVar *hv = &pathVars[holder];
    if (!pathSetHasLoan(hv->holds, loan))
        return;
    uint32_t pend = loanPending(node, access, loan, holder);
    if (!pathSetHas(hv->pending, pend))
        pathSetFacts(holder, hv->holds, pathSetAdd(hv->pending, pend));
}

void loanAccess(Place *pl, int access, INode *node) {
    if (nflights)
        loanFlightAccess(pl, access, node);
    for (uint32_t id = pathVars[pl->var].loans; id; id = loans[id].next) {
        Loan *loan = &loans[id];
        if (loan->place.deref != pl->deref || !loanConflicts(access, loan, pl)
            || !placeOverlaps(&loan->place, pl))
            continue;
        for (uint16_t k = 0; k < loan->nmay; ++k)
            loanPend(node, access, id, maypool[loan->mayhold + k]);
        for (uint32_t k = 0; k < nsaturated; ++k)
            loanPend(node, access, id, saturated[k]);
    }
}

uint32_t loanSeamEnds(PathSet *holds) {
    if (loanNotGlobalIn(holds) == 0)
        return 0;
    if (holds == &pathSetAll)
        return loanNotGlobalIn(holds);
    // The borrow the holder was given is the one to name: a near loan, which
    // is where it points -- one of this function's own storage, else a
    // reborrow through a reference, whose own loans end with it
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < holds->cnt; ++i) {
            uint32_t id = loanOf(holds->ids[i]);
            if ((holds->ids[i] & LoanFar) || loans[id].kind == LoanCaller)
                continue;
            if (pass == 0 ? loanIsLocal(id) : loans[id].place.deref)
                return id;
        }
    }
    return loanNotGlobalIn(holds);
}

uint32_t loanSeamPending(INode *seam, uint32_t loan, uint32_t holder) {
    return loanPending(seam, AccessSeam, loan, holder);
}

// Keyed by the seam, the holder, and a middle number no loan's id reaches:
// apart from a pending conflict's key, a report's (node, 0, 1), a caller
// loan's and a choice's (bit 30)
uint32_t loanSeamLive(INode *seam, uint32_t var) {
    uint32_t id = mapGet(seam, 0x20000000u, var);
    if (id)
        return id;
    if (npendings >= pendingcap)
        pendings = (Pending *)pathGrow(pendings, &pendingcap, sizeof(Pending));
    id = npendings++;
    Pending *pend = &pendings[id];
    pend->access = seam;
    pend->loan = 0;
    pend->other = 0;
    pend->holder = var;
    pend->kind = PendingSeamLive;
    pend->fired = 0;
    mapPut(seam, 0x20000000u, var, id);
    return id;
}

// *********************
// Reporting
// *********************

static uint32_t loanColumn(INode *node) {
    return (uint32_t)(node->srcp - node->linep) + 1;
}

// Is a loan's source a temporary: one that ends with its statement (a
// stand-in, pwTemp), or one a variable's initializer keeps to the end of its
// block (a hidden local, varDclExtend)?
enum LoanTemp {
    LoanTempNone,
    LoanTempStatement,
    LoanTempBlock,
};
static int loanTempKind(Place *pl) {
    PathVar *pv = &pathVars[pl->var];
    if (pv->temp)
        return LoanTempStatement;
    return pv->var->namesym == tempLocalName ? LoanTempBlock : LoanTempNone;
}

// The name of a loan's source, as the reader would write it
static char *loanSourceName(Place *pl, char *buf, size_t size) {
    VarDclNode *var = pathVars[pl->var].var;
    if (loanTempKind(pl) != LoanTempNone)
        snprintf(buf, size, "the temporary made at %u:%u", var->linenbr, loanColumn((INode *)var));
    else
        snprintf(buf, size, "%s%s", pl->deref ? "*" : "", &var->namesym->namestr);
    return buf;
}

// How long a temporary lasts, for a message
static char *loanTempEnds(int kind) {
    return kind == LoanTempStatement ? "ends with its statement" : "lasts to the end of its block";
}

// Where a loan was made, as the reader would find it: at the borrow, or, for
// one a method's returned borrow carries, at the call ("by 'alloc' at 5:11")
static char *loanWhere(Loan *loan, char *buf, size_t size) {
    if (loan->by)
        snprintf(buf, size, "by '%s' at %u:%u", &loan->by->namestr, loan->site->linenbr, loanColumn(loan->site));
    else
        snprintf(buf, size, "at %u:%u", loan->site->linenbr, loanColumn(loan->site));
    return buf;
}

// What an access attempts, for the message
static char *loanAttempt(int access) {
    switch (access) {
    case AccessRead: return "read";
    case AccessBorrow: return "borrowed";
    case AccessBorrowMut: return "borrowed mutably";
    case AccessBorrowImm: return "borrowed as 'imm'";
    case AccessBorrowUni: return "borrowed as 'uni'";
    case AccessMove: return "moved";
    case AccessEnd: return "ended";
    default: return "changed";
    }
}

void loanEscape(INode *node, uint32_t loan, int how) {
    // Once, though a loop's body is walked again
    if (mapGet(node, 0, 1))
        return;
    mapPut(node, 0, 1, 1);
    // A caller loan escapes only by a store into a global (pwStoreEscapes)
    if (loans[loan].kind == LoanCaller) {
        errorMsgNode(node, ErrorEscape,
            "Stored into a global, the value carries the borrow the caller lent through '%s', which the global would outlive: only a global borrow may be stored in a global.",
            &loanParm(loan)->namesym->namestr);
        return;
    }
    char srcname[128];
    char where[160];
    loanSourceName(&loans[loan].place, srcname, sizeof(srcname));
    loanWhere(&loans[loan], where, sizeof(where));
    int temp = loanTempKind(&loans[loan].place);
    if (temp != LoanTempNone) {
        if (how == LoanEscapeReturn)
            errorMsgNode(node, ErrorEscape,
                "Returned value carries a borrow of a temporary (made %s), which %s: the borrow would outlive it.",
                where, loanTempEnds(temp));
        else
            errorMsgNode(node, how == LoanEscapeStore ? ErrorEscape : ErrorCallEscape,
                "%s where it may outlive this function, a value carries a borrow of a temporary (made %s), which %s.",
                how == LoanEscapeStore ? "Stored" : "Handed to a call that could store it", where,
                loanTempEnds(temp));
        return;
    }
    switch (how) {
    case LoanEscapeReturn:
        errorMsgNode(node, ErrorEscape,
            "Returned value carries a borrow of '%s' (made %s), which it would outlive: '%s' belongs to this function.",
            srcname, where, srcname);
        break;
    case LoanEscapeStore:
        errorMsgNode(node, ErrorEscape,
            "Stored where it may outlive this function, the value carries a borrow of '%s' (made %s), which belongs to this function.",
            srcname, where);
        break;
    default:
        errorMsgNode(node, ErrorCallEscape,
            "Call could store a borrowed reference where it would outlive the value it points to: an argument carries a borrow of '%s' (made %s), and another reaches beyond this function.",
            srcname, where);
        break;
    }
}

// How a caller loan is named in a message: its parameter, and, for a slot of
// a struct declaring lifetimes, the slot's lifetime there
static char *loanLentThrough(uint32_t loan, char *buf, size_t size) {
    VarDclNode *parm = loanParm(loan);
    Name *life = NULL;
    if (loans[loan].part >= LifePartSlot) {
        INode *held = lifeHeld(parm->vtype);
        life = held ? lifeSlotName(held, loans[loan].part - LifePartSlot) : NULL;
    }
    if (life)
        snprintf(buf, size, "'%s' (its '%s')", &parm->namesym->namestr, &life->namestr);
    else
        snprintf(buf, size, "'%s'", &parm->namesym->namestr);
    return buf;
}

void loanApart(INode *node, uint32_t loan, VarDclNode *through, int how) {
    if (mapGet(node, 0, 1))
        return;
    mapPut(node, 0, 1, 1);
    char lent[160];
    loanLentThrough(loan, lent, sizeof(lent));
    switch (how) {
    case LoanEscapeReturn:
        errorMsgNode(node, ErrorEscape,
            "Returned value carries the borrow the caller lent through %s, whose lifetime the result's type does not name, nor one ordered shorter by its 'where' clause or by what its types imply. Lifetimes named apart are unrelated: only a borrow of a lifetime the result names, or a global one, may be returned.",
            lent);
        break;
    case LoanEscapeStore:
        errorMsgNode(node, ErrorEscape,
            "Stored where '%s' points, the value carries the borrow the caller lent through %s, whose lifetime is not one held there, nor ordered longer than one by a 'where' clause or by what the signature's types imply. Lifetimes named apart are unrelated.",
            &through->namesym->namestr, lent);
        break;
    default:
        errorMsgNode(node, ErrorCallEscape,
            "Call could store the borrow the caller lent through %s where '%s' points, which holds no borrow of its lifetime, nor of one ordered shorter by a 'where' clause or by what the signature's types imply. Lifetimes named apart are unrelated.",
            lent, &through->namesym->namestr);
        break;
    }
}

void loanNotGlobal(INode *node, uint32_t loan, Name *tparm) {
    if (mapGet(node, 0, 1))
        return;
    mapPut(node, 0, 1, 1);
    char srcname[128];
    char where[160];
    char why[200];
    loanSourceName(&loans[loan].place, srcname, sizeof(srcname));
    if (tparm)
        snprintf(why, sizeof(why), "The type parameter %s is bounded by ''static' ('%s + 'static'), so what is handed in for it must hold only global borrows, but it carries",
            &tparm->namestr, &tparm->namestr);
    else
        snprintf(why, sizeof(why), "This parameter's lifetime is ''static', so what is handed to it must be global, but it carries");
    if (loans[loan].kind == LoanCaller)
        errorMsgNode(node, tparm ? ErrorLifetimeBound : ErrorCallEscape,
            "%s the borrow the caller lent through '%s'.",
            why, &loanParm(loan)->namesym->namestr);
    else
        errorMsgNode(node, tparm ? ErrorLifetimeBound : ErrorCallEscape,
            "%s a borrow of '%s' (made %s).",
            why, srcname, loanWhere(&loans[loan], where, sizeof(where)));
}

void loanNotBound(INode *node, uint32_t loan, Name *bound) {
    if (mapGet(node, 0, 1))
        return;
    mapPut(node, 0, 1, 1);
    char srcname[128];
    char where[160];
    if (loans[loan].kind == LoanCaller) {
        char lent[160];
        errorMsgNode(node, ErrorLifetimeBound,
            "A virtual reference bounded by '%s' points at a value whose borrows all last '%s', but this one holds the borrow the caller lent through %s, whose lifetime is not ordered at least as long by a 'where' clause or by what the signature's types imply.",
            &bound->namestr, &bound->namestr, loanLentThrough(loan, lent, sizeof(lent)));
        return;
    }
    loanSourceName(&loans[loan].place, srcname, sizeof(srcname));
    errorMsgNode(node, ErrorLifetimeBound,
        "A virtual reference bounded by '%s' points at a value whose borrows all last '%s', but this one holds a borrow of '%s' (made %s), which belongs to this function.",
        &bound->namestr, &bound->namestr, srcname, loanWhere(&loans[loan], where, sizeof(where)));
}

void loanNotBoxable(INode *node, uint32_t loan) {
    if (mapGet(node, 0, 1))
        return;
    mapPut(node, 0, 1, 1);
    char srcname[128];
    char where[160];
    if (loans[loan].kind == LoanCaller) {
        char lent[160];
        errorMsgNode(node, ErrorLifetimeBound,
            "An owning virtual reference ('So[Trait]', 'Rc[Trait]') names no lifetime, so it may outlive any borrow: the value made one may hold only global borrows, as if bounded by ''static'. This one holds the borrow the caller lent through %s, which is not ordered to last ''static'.",
            loanLentThrough(loan, lent, sizeof(lent)));
        return;
    }
    loanSourceName(&loans[loan].place, srcname, sizeof(srcname));
    errorMsgNode(node, ErrorLifetimeBound,
        "An owning virtual reference ('So[Trait]', 'Rc[Trait]') names no lifetime, so it may outlive any borrow: the value made one may hold only global borrows, as if bounded by ''static'. This one holds a borrow of '%s' (made %s), which belongs to this function.",
        srcname, loanWhere(&loans[loan], where, sizeof(where)));
}

// What a borrow a seam ends is of, as the reader would say it: what the caller
// lent through a parameter, what a lock's guard holds the lock for (the guard
// a borrow through a lock permission reads through), a temporary, or a place
static char *loanSeamOf(uint32_t id, char *buf, size_t size) {
    Loan *loan = &loans[id];
    VarDclNode *root = pathVars[loan->place.var].var;
    INode *roottype = root->vtype ? itypeGetTypeDcl(root->vtype) : NULL;
    char srcname[128];
    if (loan->kind == LoanCaller)
        snprintf(buf, size, "of what the caller lent through '%s'", &root->namesym->namestr);
    else if (!loan->place.deref && roottype && roottype->tag == RefTag && permHeldKind(((RefNode *)roottype)->perm))
        snprintf(buf, size, "through the lock taken at %u:%u", root->linenbr, loanColumn((INode *)root));
    else if (loanTempKind(&loan->place) != LoanTempNone)
        snprintf(buf, size, "of %s", loanSourceName(&loan->place, srcname, sizeof(srcname)));
    else
        snprintf(buf, size, "of '%s'", loanSourceName(&loan->place, srcname, sizeof(srcname)));
    return buf;
}

// A use of a holder is a node naming it, or, where its value dies with a
// finalizer that may read what it holds, its declaration (pwHolderDies)
static void loanReport(Pending *pend, INode *usenode) {
    Loan *loan = &loans[pend->loan];
    char srcname[128];
    char where[160];
    char used[160];
    loanSourceName(&loan->place, srcname, sizeof(srcname));
    loanWhere(loan, where, sizeof(where));
    VarDclNode *holder = pathVars[pend->holder].var;
    if (usenode->tag == VarDclTag)
        snprintf(used, sizeof(used), "when '%s''s value is finalized", &holder->namesym->namestr);
    else
        snprintf(used, sizeof(used), "at %u:%u", usenode->linenbr, loanColumn(usenode));
    char *mutably = loan->writes ? " mutably" : "";
    // A seam ends the borrow itself, whatever its source, as a scope's end
    // ends the borrows of what it declared: the ordinary diagnostic, at the
    // seam as where the borrow ended
    if (pend->kind == AccessSeam) {
        char of[200];
        errorMsgNode(pend->access, ErrorFrozen,
            "The borrow '%s' holds %s (made %s) ends at this 'await', where the method returns to its actor's dispatcher to wait, and '%s' is used again %s.",
            &holder->namesym->namestr, loanSeamOf(pend->loan, of, sizeof(of)), where,
            &holder->namesym->namestr, used);
        return;
    }
    int temp = loanTempKind(&loan->place);
    if (pend->kind == AccessEnd && temp == LoanTempStatement) {
        errorMsgNode(pend->access, ErrorFrozen,
            "The temporary made here ends with its statement while '%s' still holds a borrow of it (made %s), used again %s. A temporary lasts to the end of its block only where a variable's initializer borrows it ('imm r = &make();'); keep it in a variable first.",
            &holder->namesym->namestr, where, used);
        return;
    }
    if (pend->kind == AccessEnd && temp == LoanTempBlock) {
        errorMsgNode(pend->access, ErrorFrozen,
            "The temporary made here, kept to the end of its block, goes out of scope while '%s' still holds a borrow of it (made %s), used again %s.",
            &holder->namesym->namestr, where, used);
        return;
    }
    if (pend->kind == AccessEnd) {
        errorMsgNode(pend->access, ErrorFrozen,
            "'%s', declared here, goes out of scope while '%s' still holds a borrow of it (made %s), used again %s.",
            srcname, &holder->namesym->namestr, where, used);
        return;
    }
    errorMsgNode(pend->access, ErrorFrozen,
        "'%s' is borrowed%s (%s), and that borrow is used again %s. It may not be %s until after that last use.",
        srcname, mutably, where, used, loanAttempt(pend->kind));
}

static void loanChosenReport(INode *node, char *what, uint32_t la, uint32_t lb);

static void loanShapeReport(Pending *pend, INode *usenode);

void loanUse(uint32_t var, INode *usenode) {
    PathSet *pending = pathVars[var].pending;
    if (pending == NULL)
        return;
    for (uint32_t i = 0; i < pending->cnt; ++i) {
        Pending *pend = &pendings[pending->ids[i]];
        if (pend->fired)
            continue;
        if (pend->kind == PendingSeamLive) {
            pend->fired = 1;
            pathSeamLive(pend->access, pend->holder);
            continue;
        }
        if (pend->kind == PendingChosen) {
            pend->fired = 1;
            char what[160];
            snprintf(what, sizeof(what), usenode->tag == VarDclTag ? "'%s', finalized as it dies,"
                : "'%s', used here,", &pathVars[var].var->namesym->namestr);
            loanChosenReport(usenode, what, pend->loan, pend->other);
            continue;
        }
        if (pend->kind == PendingShape) {
            pend->fired = 1;
            if (mapGet(pend->access, 0, 1))
                continue;
            mapPut(pend->access, 0, 1, 1);
            loanShapeReport(pend, usenode);
            continue;
        }
        pend->fired = 1;
        // One error per access, however many borrows it conflicts with
        if (mapGet(pend->access, 0, 1))
            continue;
        mapPut(pend->access, 0, 1, 1);
        loanReport(pend, usenode);
    }
}

void loanReturnedBy(uint32_t loan, Name *method) {
    if (loans[loan].by == NULL)
        loans[loan].by = method;
}

// A loan of a place reached through a shared path, which the borrow it made
// carried out of a call, freezes that place as a local's loan does: a
// read-only borrow lets it be read, a borrow that writes lets nothing touch it
void loanFreezeShared(uint32_t loan) {
    if (loans[loan].kind == LoanAlias)
        loans[loan].kind = loans[loan].writes ? LoanExcl : LoanShared;
}

// *********************
// Loans in flight: those an operand of a call or a literal carries, walked and
// waiting for the call to be made or the value to be built. That value is
// certainly used, so an access conflicting with one is reported at once
// rather than left pending. A method receiver's mutable borrow is two-phase
// (Rust's RFC 2025): reserved while the arguments are walked, it meets their
// accesses as a read-only borrow would, so 'v.push(v.len())' compiles, and it
// is activated at the call, where it must not conflict with what the other
// arguments carry.
// *********************

typedef struct {
    PathSet *loans;     // what one operand carries
    uint32_t reserved;  // a two-phase receiver's own loan among them, or 0
    INode *operand;     // the operand, where its pusher named it, or NULL
} Flight;

static Flight *flights = NULL;
static uint32_t flightcap = 0;

uint32_t loanFlightMark() {
    return nflights;
}

void loanFlightPushOf(PathSet *carried, uint32_t reserved, INode *operand) {
    if (carried == NULL || carried == &pathSetAll)
        return;
    if (nflights == flightcap)
        flights = (Flight *)pathGrow(flights, &flightcap, sizeof(Flight));
    flights[nflights].loans = carried;
    flights[nflights].reserved = reserved;
    flights[nflights].operand = operand;
    ++nflights;
}

void loanFlightPush(PathSet *carried, uint32_t reserved) {
    loanFlightPushOf(carried, reserved, NULL);
}

void loanFlightPop(uint32_t mark) {
    nflights = mark;
}

// Report once at 'node', unless something there is reported already
static int loanReportOnce(INode *node) {
    if (mapGet(node, 0, 1))
        return 0;
    mapPut(node, 0, 1, 1);
    return 1;
}

void loanFlightAccess(Place *pl, int access, INode *node) {
    for (uint32_t f = 0; f < nflights; ++f) {
        PathSet *set = flights[f].loans;
        for (uint32_t i = 0; i < set->cnt; ++i) {
            uint32_t id = loanOf(set->ids[i]);
            Loan *loan = &loans[id];
            if (loan->place.var != pl->var || loan->place.deref != pl->deref)
                continue;
            // A reserved receiver meets them as a read-only borrow would, until the call
            uint8_t kind = id == flights[f].reserved && loan->kind == LoanExcl ? LoanShared : loan->kind;
            if (!loanConflictsAs(access, kind, loan, pl) || !placeOverlaps(&loan->place, pl))
                continue;
            if (!loanReportOnce(node))
                return;
            char srcname[128];
            char where[160];
            loanSourceName(&loan->place, srcname, sizeof(srcname));
            loanWhere(loan, where, sizeof(where));
            int temp = loanTempKind(&loan->place);
            if (access == AccessEnd && temp != LoanTempNone)
                errorMsgNode(node, ErrorFrozen,
                    "The temporary made here %s, while a value still being handed on carries a borrow of it (made %s).",
                    loanTempEnds(temp), where);
            else
                errorMsgNode(node, ErrorFrozen,
                    "'%s' is borrowed%s (%s) for a call or value still being made, which uses that borrow. It may not be %s before then.",
                    srcname, loan->writes ? " mutably" : "", where, loanAttempt(access));
            return;
        }
    }
}

void loanFlightActivate(uint32_t mark, uint32_t receiver, int access, INode *node) {
    Loan *recv = &loans[receiver];
    for (uint32_t f = mark; f < nflights; ++f) {
        if (flights[f].reserved == receiver)
            continue;
        PathSet *set = flights[f].loans;
        for (uint32_t i = 0; i < set->cnt; ++i) {
            uint32_t id = loanOf(set->ids[i]);
            Loan *loan = &loans[id];
            if (id == receiver || loan->place.var != recv->place.var
                || loan->place.deref != recv->place.deref
                || !loanConflicts(access, loan, &recv->place) || !placeOverlaps(&loan->place, &recv->place))
                continue;
            if (!loanReportOnce(node))
                return;
            char srcname[128];
            char where[160];
            loanSourceName(&loan->place, srcname, sizeof(srcname));
            loanWhere(loan, where, sizeof(where));
            errorMsgNode(node, ErrorFrozen,
                "'%s' is borrowed%s (%s) by another argument of this call, which the call uses. It may not be %s as the call's receiver too.",
                srcname, loan->writes ? " mutably" : "", where, loanAttempt(access));
            return;
        }
    }
}

void loanSeamFlight(INode *seam) {
    for (uint32_t f = 0; f < nflights; ++f) {
        PathSet *set = flights[f].loans;
        for (uint32_t i = 0; i < set->cnt; ++i) {
            uint32_t id = loanOf(set->ids[i]);
            if (loanIsGlobal(id))
                continue;
            if (!loanReportOnce(seam))
                return;
            // A plain path is reached again after the seam (awaitReReached),
            // so what is in flight was made by a call or a temporary
            INode *operand = flights[f].operand;
            if (operand && !awaitIsPath(operand)) {
                awaitLeftCallMsg(operand, "This borrow");
                return;
            }
            char of[200];
            char where[160];
            loanWhere(&loans[id], where, sizeof(where));
            errorMsgNode(seam, ErrorFrozen,
                "A borrow %s (made %s) ends at this 'await', where the method returns to its actor's dispatcher to wait, and a call or value still being made around it uses that borrow after it.",
                loanSeamOf(id, of, sizeof(of)), where);
            return;
        }
    }
}

// *********************
// GPU targets: a reference chosen at run time (flowloan.h)
// *********************

// Does 'set' hold the loan 'id' near?
static int loanHasNear(PathSet *set, uint32_t id) {
    if (set == NULL)
        return 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (!(set->ids[i] & LoanFar) && loanOf(set->ids[i]) == id)
            return 1;
    }
    return 0;
}

// A near loan of 'a' that 'b' does not hold near, or 0
static uint32_t loanNearNotIn(PathSet *a, PathSet *b) {
    if (a == NULL)
        return 0;
    for (uint32_t i = 0; i < a->cnt; ++i) {
        if (!(a->ids[i] & LoanFar) && !loanHasNear(b, loanOf(a->ids[i])))
            return loanOf(a->ids[i]);
    }
    return 0;
}

// Any near loan of 'set', or 0
static uint32_t loanNearAny(PathSet *set) {
    if (set == NULL || set == &pathSetAll)
        return 0;
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (!(set->ids[i] & LoanFar))
            return loanOf(set->ids[i]);
    }
    return 0;
}

int loanNearApart(PathSet *a, PathSet *b, uint32_t *la, uint32_t *lb) {
    // Widened to every loan by a loop that would not settle: not known to agree
    if (a == &pathSetAll || b == &pathSetAll) {
        *la = loanNearAny(a);
        *lb = loanNearAny(b);
        return a != b;
    }
    uint32_t x = loanNearNotIn(a, b);
    uint32_t y = loanNearNotIn(b, a);
    if (x == 0 && y == 0)
        return 0;
    *la = x ? x : loanNearAny(a);
    *lb = y ? y : loanNearAny(b);
    return 1;
}

// The memory a loan's place is in, where the loan itself says: a local's
// (SPIR-V's Function storage class) or a global's (Private). What a caller
// lent, or what a reference points at, is in whatever memory its origin is.
enum LoanMemory {
    LoanMemUnknown,
    LoanMemLocal,
    LoanMemGlobal,
};
static int loanMemory(uint32_t id) {
    if (id == 0 || loans[id].kind == LoanCaller || loans[id].place.deref)
        return LoanMemUnknown;
    return loanVarIsGlobal(pathVars[loans[id].place.var].var) ? LoanMemGlobal : LoanMemLocal;
}

// Where a loan points, for the message: its variable, what kind it is, and
// the line and column of the borrow
static char *loanOrigin(uint32_t id, char *buf, size_t size) {
    if (id == 0) {
        snprintf(buf, size, "somewhere else");
        return buf;
    }
    Loan *loan = &loans[id];
    VarDclNode *var = pathVars[loan->place.var].var;
    if (loan->kind == LoanCaller) {
        snprintf(buf, size, "what the parameter '%s' points at (declared at %u:%u)", &var->namesym->namestr,
            var->linenbr, loanColumn((INode *)var));
        return buf;
    }
    char where[160];
    loanWhere(loan, where, sizeof(where));
    if (loanTempKind(&loan->place) != LoanTempNone) {
        char srcname[128];
        snprintf(buf, size, "%s (borrowed %s)", loanSourceName(&loan->place, srcname, sizeof(srcname)), where);
    }
    else if (loan->place.deref)
        snprintf(buf, size, "what '%s' points at (borrowed %s)", &var->namesym->namestr, where);
    else
        snprintf(buf, size, "%s '%s' (borrowed %s)", loanMemory(id) == LoanMemGlobal ? "the global" : "the local",
            &var->namesym->namestr, where);
    return buf;
}

static void loanChosenReport(INode *node, char *what, uint32_t la, uint32_t lb) {
    if (!loanReportOnce(node))
        return;
    char a[320];
    char b[320];
    int ka = loanMemory(la);
    int kb = loanMemory(lb);
    errorMsgNode(node, ErrorGpuRefChoice,
        "On a GPU target a reference may not be chosen at run time, but %s may point at %s or at %s.%s Choose the index instead ('&buf[if c {i;} else {j;}]'), or the value ('if c {a;} else {b;}').",
        what, loanOrigin(la, a, sizeof(a)), loanOrigin(lb, b, sizeof(b)),
        ka && kb && ka != kb ? " These are two kinds of GPU memory, a local's and a global's, which no one pointer can reach." : "");
}

void loanChosen(INode *node, uint32_t la, uint32_t lb) {
    loanChosenReport(node, "this value", la, lb);
}

void loanIndexedRefs(INode *node) {
    if (!loanReportOnce(node))
        return;
    errorMsgNode(node, ErrorGpuRefIndexed,
        "On a GPU target an array whose elements hold references may be indexed only by a literal or a constant: a pointer cannot be kept in GPU memory, so the array must break into separate values. Index an array of the values instead.");
}

// Keyed by the holder's declaration and the two loans, bit 30 set on each:
// apart from a pending conflict's key, whose loan is never above LoanIdMask,
// and from a caller loan's and a report's, whose middle number is 0
uint32_t loanChosenPending(uint32_t holder, uint32_t la, uint32_t lb) {
    INode *key = (INode *)pathVars[holder].var;
    uint32_t a = 0x40000000u | la;
    uint32_t b = 0x40000000u | lb;
    uint32_t id = mapGet(key, a, b);
    if (id)
        return id;
    if (npendings >= pendingcap)
        pendings = (Pending *)pathGrow(pendings, &pendingcap, sizeof(Pending));
    id = npendings++;
    Pending *pend = &pendings[id];
    pend->access = key;
    pend->loan = la;
    pend->other = lb;
    pend->holder = holder;
    pend->kind = PendingChosen;
    pend->fired = 0;
    mapPut(key, a, b, id);
    return id;
}
// *********************
// Shape-changing values (reshape.h). A borrow into a collection that changes
// shape, reached through a shared path, is marked: the same name changing the
// collection is the loan's own freezing (loanFreezeShared), and a call that could
// change it through another name is asked of every call made while the loan is
// held (flowpath.c, pwShapeCall). A call met while a variable holds the loan is a
// pending conflict, fired by that variable's next use as an ordinary freezing
// conflict is; one met while the loan is in flight, or handed the borrow itself,
// is reported at once.
// *********************

void loanShapeMark(uint32_t loan, INode *container) {
    if (loans[loan].shapecont == NULL) {
        if (nshapeloans == shapeloancap)
            shapeloans = (uint32_t *)pathGrow(shapeloans, &shapeloancap, sizeof(uint32_t));
        shapeloans[nshapeloans++] = loan;
    }
    loans[loan].shapecont = container;
}

uint32_t loanShapeCount() {
    return nshapeloans;
}

uint32_t loanShapeId(uint32_t i) {
    return shapeloans[i];
}

INode *loanShapeContainer(uint32_t loan) {
    return loans[loan].shapecont;
}

int loanShapeInFlight(uint32_t loan) {
    for (uint32_t k = 0; k < nflights; ++k) {
        if (pathSetHasLoan(flights[k].loans, loan))
            return 1;
    }
    return 0;
}

// What the call does, for the message
static char *loanShapeReason(ShapeNote *note, char *cont, char *buf, size_t size) {
    const char *name = note->callee ? &note->callee->namestr : "this call";
    switch (note->why) {
    case ReshapeReceiver:
        snprintf(buf, size, "'%s' reshapes the %s it is called on: it writes a field of it, runs a finalizer of an element, or moves an element out", name, cont);
        break;
    case ReshapeReceiverUnseen:
        snprintf(buf, size, "'%s' may change the %s it is called on, and its body is not visible here, so it is assumed to change its shape", name, cont);
        break;
    case ReshapeUnseen:
        snprintf(buf, size, "'%s' is handed something that reaches a %s it may change, and its body is not visible here, so it is assumed to change its shape", name, cont);
        break;
    case ReshapeBody:
        snprintf(buf, size, "'%s' is handed something that reaches a %s, and it, or something it calls, changes the shape of one", name, cont);
        break;
    default:
        snprintf(buf, size, "'%s' changes the shape of a %s through a global", name, cont);
        break;
    }
    return buf;
}

static void loanShapeContName(uint32_t loan, char *buf, size_t size) {
    StructNode *st = (StructNode *)loans[loan].shapecont;
    snprintf(buf, size, "'%s'", st->namesym ? &st->namesym->namestr : "collection");
}

static void loanShapeEmit(INode *node, char *msg) {
    errorMsgNode(node, ErrorShapeReshape, "%s", msg);
}

static void loanShapeReport(Pending *pend, INode *usenode) {
    Loan *loan = &loans[pend->loan];
    ShapeNote *note = &shapenotes[pend->other];
    char cont[100];
    char reason[300];
    char where[160];
    char used[160];
    loanShapeContName(pend->loan, cont, sizeof(cont));
    loanWhere(loan, where, sizeof(where));
    VarDclNode *holder = pathVars[pend->holder].var;
    if (usenode->tag == VarDclTag)
        snprintf(used, sizeof(used), "when '%s''s value is finalized", &holder->namesym->namestr);
    else
        snprintf(used, sizeof(used), "at %u:%u", usenode->linenbr, loanColumn(usenode));
    char msg[1400];
    snprintf(msg, sizeof(msg),
        "This call could change the shape of a %s while '%s' still holds a borrow into it (made %s, through a path other references share), used again %s: %s. A %s that changes shape may move or free what it lent. Reach it through an 'imm' or 'uni' reference, or copy what is needed out before this call.",
        cont, &holder->namesym->namestr, where, used, loanShapeReason(note, cont, reason, sizeof(reason)), cont);
    loanShapeEmit(pend->access, msg);
}

void loanShapeNow(INode *call, uint32_t loan, int why, Name *callee) {
    if (!loanReportOnce(call))
        return;
    ShapeNote note = { call, callee, (uint8_t)why };
    char cont[100];
    char reason[300];
    char where[160];
    loanShapeContName(loan, cont, sizeof(cont));
    loanWhere(&loans[loan], where, sizeof(where));
    char msg[1400];
    snprintf(msg, sizeof(msg),
        "This call could change the shape of a %s while a borrow into it (made %s, through a path other references share) is still to be used by this call or by a value around it: %s. A %s that changes shape may move or free what it lent. Reach it through an 'imm' or 'uni' reference, or copy what is needed out before this call.",
        cont, where, loanShapeReason(&note, cont, reason, sizeof(reason)), cont);
    loanShapeEmit(call, msg);
}

// The call at 'call' could reshape the collection the loan borrows into: every
// holder that holds the loan gets a pending conflict, which its next use fires
void loanShapePend(INode *call, uint32_t loan, int why, Name *callee) {
    Loan *l = &loans[loan];
    uint32_t note = 0;
    int have = 0;
    for (uint32_t k = 0; k < (uint32_t)l->nmay + nsaturated; ++k) {
        uint32_t holder = k < l->nmay ? maypool[l->mayhold + k] : saturated[k - l->nmay];
        PathVar *hv = &pathVars[holder];
        if (!pathSetHasLoan(hv->holds, loan))
            continue;
        uint32_t key = 0x08000000u | loan;
        uint32_t id = mapGet(call, key, holder);
        if (!id) {
            if (!have) {
                if (nshapenotes == shapenotecap)
                    shapenotes = (ShapeNote *)pathGrow(shapenotes, &shapenotecap, sizeof(ShapeNote));
                note = nshapenotes++;
                shapenotes[note].call = call;
                shapenotes[note].callee = callee;
                shapenotes[note].why = (uint8_t)why;
                have = 1;
            }
            if (npendings >= pendingcap)
                pendings = (Pending *)pathGrow(pendings, &pendingcap, sizeof(Pending));
            id = npendings++;
            Pending *pend = &pendings[id];
            pend->access = call;
            pend->loan = loan;
            pend->other = note;
            pend->holder = holder;
            pend->kind = PendingShape;
            pend->fired = 0;
            mapPut(call, key, holder, id);
        }
        if (!pathSetHas(hv->pending, id))
            pathSetFacts(holder, hv->holds, pathSetAdd(hv->pending, id));
    }
}

// Does this set entry's loan keep every other holder from writing what it
// borrows while it is used: a read-only or exclusive loan of a place that
// nothing else reaches, as against one through a shared path or a caller's?
int loanExcludesWriters(uint32_t entry) {
    uint8_t kind = loans[entry & LoanIdMask].kind;
    return kind == LoanShared || kind == LoanExcl;
}

// Is the place 'pl' the very place (or part of it, or holding it) the loan borrows?
int loanShapeSamePlace(uint32_t loan, Place *pl) {
    Loan *l = &loans[loan];
    return l->place.var == pl->var && l->place.deref == pl->deref && placeOverlaps(&l->place, pl);
}

// Is this set entry's loan a mutable borrow of a source nothing else reaches (a
// local, or what 'uni' references reach)? Whoever holds it holds the only path
int loanIsExclusive(uint32_t entry) {
    return loans[entry & LoanIdMask].kind == LoanExcl;
}

// Is the place 'pl' a different part of the very root the loan borrows from (another
// field of the same struct, through the same reference)? Nothing reached by a path
// that diverges from the loan's can be what the loan points into
int loanShapeDisjoint(uint32_t loan, Place *pl) {
    Loan *l = &loans[loan];
    return l->place.var == pl->var && l->place.deref == pl->deref && !placeOverlaps(&l->place, pl);
}
