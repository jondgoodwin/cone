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
static uint32_t nflights;

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
    nflights = 0;
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
    // Linked from its root variable, so an access finds it
    loan->next = pathVars[pl->var].loans;
    pathVars[pl->var].loans = id;
    mapPut(site, 0, 0, id);
    return id;
}

// A caller loan is not linked from its parameter: no access here meets it
uint32_t loanCaller(uint32_t var) {
    VarDclNode *parm = pathVars[var].var;
    uint32_t id = mapGet(parm, 0, 0);
    if (id)
        return id;
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
    mapPut(parm, 0, 0, id);
    return id;
}

uint32_t loanRoot(uint32_t loan) {
    return loans[loan].place.var;
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
        if (loanIsLocal(set->ids[i]))
            return set->ids[i];
    }
    return 0;
}

// Is this the type of a borrowed reference?
static int loanIsBorrowType(INode *type) {
    return (type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef;
}

// May a reference to a 'referent' point at what this loan borrows? A
// reference's loans are what it points at and what that holds, in one set; a
// loan of a struct of another type than the referent is one of the second
// kind (a borrow is coerced only to a trait, its own or a base's, or to a
// slice), and so is the caller loan of a parameter that is no reference,
// which stands for the borrows it holds. Anything not known to be one may be
// the first.
static int loanMayBePointee(Loan *loan, INode *referent) {
    if (referent == NULL)
        return 1;
    INode *borrowed;
    if (loan->kind == LoanCaller) {
        INode *parmtype = itypeGetTypeDcl(pathVars[loan->place.var].var->vtype);
        if (!loanIsBorrowType(parmtype))
            return 0;
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

int loanMayPointOut(PathSet *refholds, INode *referent) {
    if (refholds == NULL || refholds == &pathSetAll)
        return 1;
    int local = 0;
    for (uint32_t i = 0; i < refholds->cnt; ++i) {
        Loan *loan = &loans[refholds->ids[i]];
        // A reborrow through another reference points where that one does,
        // and what that one held came along with it
        if ((loan->kind != LoanCaller && loan->place.deref) || !loanMayBePointee(loan, referent))
            continue;
        if (!loanIsLocal(refholds->ids[i]) || loan->place.owned)
            return 1;
        local = 1;
    }
    return !local;
}

uint32_t loanAt(INode *site) {
    return mapGet(site, 0, 0);
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
    char srcname[128];
    char where[160];
    loanSourceName(&loans[loan].place, srcname, sizeof(srcname));
    loanWhere(&loans[loan], where, sizeof(where));
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

void loanReturnedBy(uint32_t loan, Name *method) {
    if (loans[loan].by == NULL)
        loans[loan].by = method;
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
} Flight;

static Flight *flights = NULL;
static uint32_t flightcap = 0;

uint32_t loanFlightMark() {
    return nflights;
}

void loanFlightPush(PathSet *carried, uint32_t reserved) {
    if (carried == NULL || carried == &pathSetAll)
        return;
    if (nflights == flightcap)
        flights = (Flight *)pathGrow(flights, &flightcap, sizeof(Flight));
    flights[nflights].loans = carried;
    flights[nflights].reserved = reserved;
    ++nflights;
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
            uint32_t id = set->ids[i];
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
            Loan *loan = &loans[set->ids[i]];
            if (set->ids[i] == receiver || loan->place.var != recv->place.var
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