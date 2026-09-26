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

// How a loan restricts its source, from the borrow's permission
enum LoanKind {
    LoanShared,     // may read, not write: 'ro', 'imm'
    LoanExcl,       // may write: 'mut', 'uni', 'mut1'
    LoanPin,        // neither: 'opaq', which holds only the address
};

typedef struct {
    INode *site;        // the borrow that made it
    Place place;        // what it borrows
    uint32_t next;      // the next loan with the same root variable
    uint32_t mayhold;   // where in 'maypool' every holder that may hold it on some path is
    uint16_t nmay;
    uint16_t maycap;
    uint8_t kind;       // LoanKind
} Loan;

// A pending conflict: 'access' conflicted with 'loan', held by 'holder'
typedef struct {
    INode *access;
    uint32_t loan;
    uint32_t holder;
    uint8_t kind;       // PathAccess
    uint8_t fired;
} Pending;

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
static uint32_t *saturated = NULL;
static uint32_t nsaturated = 0;
static uint32_t saturatedcap = 0;

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
}

// *********************
// Loans
// *********************

static uint8_t loanKindOf(INode *perm) {
    uint16_t flags = permGetFlags(perm);
    if (flags & MayWrite)
        return LoanExcl;
    return (flags & MayRead) ? LoanShared : LoanPin;
}

uint32_t loanMake(INode *site, Place *pl, INode *perm) {
    uint32_t id = mapGet(site, 0, 0);
    if (id)
        return id;
    if (nloans >= loancap)
        loans = (Loan *)pathGrow(loans, &loancap, sizeof(Loan));
    id = nloans++;
    Loan *loan = &loans[id];
    loan->site = site;
    loan->place = *pl;
    loan->mayhold = 0;
    loan->nmay = 0;
    loan->maycap = 0;
    loan->kind = loanKindOf(perm);
    // Linked from its root variable, so an access finds it
    loan->next = pathVars[pl->var].loans;
    pathVars[pl->var].loans = id;
    mapPut(site, 0, 0, id);
    return id;
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
        Loan *loan = &loans[holds->ids[i]];
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

// Does this access conflict with a live loan of this kind? A read-only loan
// lets its source be read and borrowed read-only again; a loan that may write
// lets nothing touch the source but itself; an '&opaq' loan holds only the
// address, so only moving, replacing or ending the source conflicts with it.
static int loanConflicts(int access, uint8_t kind) {
    switch (access) {
    case AccessRead:
    case AccessBorrow:
        return kind == LoanExcl;
    case AccessWrite:
    case AccessBorrowMut:
        return kind != LoanPin;
    case AccessMove:
    case AccessReplace:
    case AccessEnd:
        return 1;
    default:    // AccessBorrowOpaq reads and writes nothing
        return 0;
    }
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
    pend->holder = holder;
    pend->kind = (uint8_t)kind;
    pend->fired = 0;
    mapPut(access, loan, holder, id);
    return id;
}

static void loanPend(INode *node, int access, uint32_t loan, uint32_t holder) {
    PathVar *hv = &pathVars[holder];
    if (!pathSetHas(hv->holds, loan))
        return;
    uint32_t pend = loanPending(node, access, loan, holder);
    if (!pathSetHas(hv->pending, pend))
        pathSetFacts(holder, hv->holds, pathSetAdd(hv->pending, pend));
}

void loanAccess(Place *pl, int access, INode *node) {
    for (uint32_t id = pathVars[pl->var].loans; id; id = loans[id].next) {
        Loan *loan = &loans[id];
        if (loan->place.deref != pl->deref || !loanConflicts(access, loan->kind)
            || !placeOverlaps(&loan->place, pl))
            continue;
        for (uint16_t k = 0; k < loan->nmay; ++k)
            loanPend(node, access, id, maypool[loan->mayhold + k]);
        for (uint32_t k = 0; k < nsaturated; ++k)
            loanPend(node, access, id, saturated[k]);
    }
}

// *********************
// Reporting
// *********************

static uint32_t loanColumn(INode *node) {
    return (uint32_t)(node->srcp - node->linep) + 1;
}

// The name of a loan's source, as the reader would write it
static char *loanSourceName(Place *pl, char *buf, size_t size) {
    VarDclNode *var = pathVars[pl->var].var;
    snprintf(buf, size, "%s%s", pl->deref ? "*" : "", &var->namesym->namestr);
    return buf;
}

static void loanReport(Pending *pend, INode *usenode) {
    Loan *loan = &loans[pend->loan];
    char srcname[128];
    loanSourceName(&loan->place, srcname, sizeof(srcname));
    uint32_t bline = loan->site->linenbr;
    uint32_t bcol = loanColumn(loan->site);
    uint32_t uline = usenode->linenbr;
    uint32_t ucol = loanColumn(usenode);
    char *mutably = loan->kind == LoanExcl ? " mutably" : "";
    char *attempt;
    switch (pend->kind) {
    case AccessEnd:
    {
        VarDclNode *holder = pathVars[pend->holder].var;
        errorMsgNode(pend->access, ErrorFrozen,
            "'%s', declared here, goes out of scope while '%s' still holds a borrow of it (made at %u:%u), used again at %u:%u.",
            srcname, &holder->namesym->namestr, bline, bcol, uline, ucol);
        return;
    }
    case AccessRead: attempt = "read"; break;
    case AccessBorrow: attempt = "borrowed"; break;
    case AccessBorrowMut: attempt = "borrowed mutably"; break;
    case AccessMove: attempt = "moved"; break;
    default: attempt = "changed"; break;
    }
    errorMsgNode(pend->access, ErrorFrozen,
        "'%s' is borrowed%s (at %u:%u), and that borrow is used again at %u:%u. It may not be %s until after that last use.",
        srcname, mutably, bline, bcol, uline, ucol, attempt);
}

void loanUse(uint32_t var, INode *usenode) {
    PathSet *pending = pathVars[var].pending;
    if (pending == NULL)
        return;
    for (uint32_t i = 0; i < pending->cnt; ++i) {
        Pending *pend = &pendings[pending->ids[i]];
        if (pend->fired)
            continue;
        pend->fired = 1;
        // One error per access, however many borrows it conflicts with
        if (mapGet(pend->access, 0, 1))
            continue;
        mapPut(pend->access, 0, 1, 1);
        loanReport(pend, usenode);
    }
}
