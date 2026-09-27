/** Drop flags: what each variable may hold at each release, and the releases
 * rebuilt from it
 * @file
 *
 * flowdrop.h says what it is for; compiler/c/doc/phases/flow.md, "Drop flags",
 * is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>
#include <stdio.h>

// A release the walk met: a scope's exit, a store over a whole variable, or a
// store over part of one. What each variable it releases may hold there is
// gathered over every walk of it (a loop body is walked more than once).
enum DropSiteKind {
    DropSiteExit,
    DropSiteStore,
    DropSitePart,
};

typedef struct {
    INode *node;        // the exit (a BreakRetNode), or the assignment's target
    INode **rvalp;      // a whole store: the value's slot, or NULL
    uint32_t first;     // its variables in 'entries'
    uint32_t cnt;
    uint8_t kind;
} DropSite;

typedef struct {
    uint32_t var;
    uint8_t state;
} DropEntry;

static DropSite *sites = NULL;
static uint32_t nsites = 0;
static uint32_t sitecap = 0;
static DropEntry *entries = NULL;
static uint32_t nentries = 0;
static uint32_t entrycap = 0;

// Which site a node is, and which uses were reported: a small map from a node
// to an id, told from an earlier function's by its generation
typedef struct {
    INode *node;
    uint32_t id;
    uint32_t gen;
} DropSlot;

static DropSlot *map = NULL;
static uint32_t mapcap = 0;
static uint32_t mapused = 0;
static uint32_t mapgen = 0;

// -V 2 tallies
static uint32_t statFns = 0;
static uint32_t statFlags = 0;

static DropSlot *mapSlot(INode *node) {
    uint32_t mask = mapcap - 1;
    uint64_t h = (uint64_t)(uintptr_t)node * 0x9E3779B97F4A7C15ull;
    uint32_t i = ((uint32_t)(h >> 32) ^ (uint32_t)h) & mask;
    while (map[i].gen == mapgen) {
        if (map[i].node == node)
            return &map[i];
        i = (i + 1) & mask;
    }
    return &map[i];
}

static void mapGrow() {
    DropSlot *old = map;
    uint32_t oldcap = mapcap;
    uint32_t oldgen = mapgen;
    mapcap = oldcap ? oldcap << 1 : 64;
    map = (DropSlot *)memAllocBlk(mapcap * sizeof(DropSlot));
    memset(map, 0, mapcap * sizeof(DropSlot));
    mapgen = 1;
    mapused = 0;
    for (uint32_t i = 0; i < oldcap; ++i) {
        if (old[i].gen == oldgen && oldgen != 0) {
            DropSlot *slot = mapSlot(old[i].node);
            *slot = old[i];
            slot->gen = mapgen;
            ++mapused;
        }
    }
}

// The id recorded for a node, or 0; 'make' records 'id' if none is
static uint32_t mapFind(INode *node, uint32_t id) {
    if (mapcap == 0 || (mapused + 1) * 2 > mapcap)
        mapGrow();
    DropSlot *slot = mapSlot(node);
    if (slot->gen == mapgen)
        return slot->id;
    if (id) {
        slot->node = node;
        slot->id = id;
        slot->gen = mapgen;
        ++mapused;
    }
    return 0;
}

void dropWalkBegin() {
    ++statFns;
    nsites = 0;
    nentries = 0;
    if (mapcap == 0)
        mapGrow();
    // A new generation empties the map without clearing it
    ++mapgen;
    mapused = 0;
}

// Does a variable's state here hold more than one kind of thing -- its value,
// a hollowed one, nothing -- so that only a flag can say which?
static int dropMixed(uint8_t state) {
    int kinds = ((state & DropWhole) != 0) + ((state & DropHollow) != 0)
        + ((state & (DropUninit | DropMoved)) != 0);
    return kinds > 1;
}

// A use of a value some path reaching it has not got: refused once, however
// many times a loop's walk meets it. A reported node is recorded under an id no
// site has (UINT32_MAX).
static void dropRefuse(INode *node, uint8_t state, int borrow) {
    if (mapFind(node, 0))
        return;
    mapFind(node, UINT32_MAX);
    if (state & (DropMoved | DropHollow))
        errorMsgNode(node, ErrorMove, "This variable's value may have been moved out on a path that reaches here (a branch, or an earlier pass of a loop). It is not certainly there to use.");
    else if (!borrow)
        errorMsgNode(node, ErrorMove, "This variable may not have been given a value on every path that reaches here. There is not certainly a value to use.");
}

void dropMove(uint32_t var, INode *node, int hollow) {
    PathVar *pv = &pathVars[var];
    if (!pv->tracked)
        return;
    if (pv->state != DropWhole && pv->state != 0)
        dropRefuse(node, pv->state, 0);
    pathSetState(var, hollow ? DropHollow : DropMoved);
}

void dropUse(uint32_t var, INode *node, int borrow) {
    PathVar *pv = &pathVars[var];
    if (!pv->tracked || pv->state == DropWhole || pv->state == 0)
        return;
    uint8_t missing = pv->state & (borrow ? (DropMoved | DropHollow) : (DropMoved | DropHollow | DropUninit));
    if (missing)
        dropRefuse(node, pv->state, borrow);
}

static DropSite *dropSite(INode *node, uint8_t kind, uint32_t cnt, int *isnew) {
    uint32_t id = mapFind(node, 0);
    if (id && id != UINT32_MAX) {
        *isnew = 0;
        return &sites[id - 1];
    }
    if (nsites == sitecap)
        sites = (DropSite *)pathGrow(sites, &sitecap, sizeof(DropSite));
    while (nentries + cnt > entrycap)
        entries = (DropEntry *)pathGrow(entries, &entrycap, sizeof(DropEntry));
    DropSite *site = &sites[nsites++];
    mapFind(node, nsites);
    site->node = node;
    site->rvalp = NULL;
    site->first = nentries;
    site->cnt = cnt;
    site->kind = kind;
    nentries += cnt;
    *isnew = 1;
    return site;
}

// Record one variable's state at a site of one variable
static void dropOne(INode *node, uint8_t kind, uint32_t var, INode **rvalp) {
    if (!pathVars[var].dies)
        return;
    int isnew;
    DropSite *site = dropSite(node, kind, 1, &isnew);
    DropEntry *entry = &entries[site->first];
    if (isnew) {
        entry->var = var;
        entry->state = 0;
        site->rvalp = rvalp;
    }
    entry->state |= pathVars[var].state;
}

void dropStore(uint32_t var, INode *lval, INode **rvalp) {
    dropOne(lval, DropSiteStore, var, rvalp);
}

void dropPartStore(uint32_t var, INode *lval) {
    dropOne(lval, DropSitePart, var, NULL);
}

void dropExit(INode *exit, uint32_t *vars, uint32_t n) {
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (pathVars[vars[i]].dies)
            ++cnt;
    }
    if (cnt == 0)
        return;
    int isnew;
    DropSite *site = dropSite(exit, DropSiteExit, cnt, &isnew);
    DropEntry *entry = &entries[site->first];
    for (uint32_t i = 0; i < n; ++i) {
        PathVar *pv = &pathVars[vars[i]];
        if (!pv->dies)
            continue;
        // The variables in scope at an exit are the same on every walk
        if (isnew) {
            entry->var = vars[i];
            entry->state = 0;
        }
        else if (entry->var != vars[i]) {
            errorUnreachable(exit, "an exit whose variables in scope differ between walks");
            return;
        }
        entry->state |= pv->state;
        ++entry;
    }
}

// A whole store's release, as its old value's state says
static void dropApplyStore(DropSite *site, DropEntry *entry) {
    INode *lval = site->node;
    VarDclNode *var = pathVars[entry->var].var;
    uint8_t state = entry->state;
    int mixed = dropMixed(state);
    lval->flags &= 0xFFFF - (FlagFirstAssign | FlagDropTest);
    if (!(state & DropWhole))
        lval->flags |= FlagFirstAssign;
    else if (mixed)
        lval->flags |= FlagDropTest;
    // A hollowed variable's old allocation goes after the new value is
    // evaluated, through the HollowNode round it (assignSingleFlow). The main
    // walk wrapped one where it saw the variable hollow; the paths decide.
    INode **rvalp = site->rvalp;
    if (rvalp == NULL)
        return;
    HollowNode *hnode = (*rvalp)->tag == HollowTag && ((HollowNode *)*rvalp)->var == var
        ? (HollowNode *)*rvalp : NULL;
    if (state & DropHollow) {
        if (hnode == NULL) {
            hnode = flowNewHollow(var);
            hnode->exp = *rvalp;
            hnode->vtype = ((IExpNode *)*rvalp)->vtype;
            *rvalp = (INode *)hnode;
        }
        // Every move that hollowed it, on any path
        hnode->moved = newNodes(2);
        INode **nodesp;
        uint32_t cnt;
        if (var->hollowall) {
            for (nodesFor(var->hollowall, cnt, nodesp))
                nodesAdd(&hnode->moved, *nodesp);
        }
        hnode->test = mixed;
    }
    else if (hnode)
        *rvalp = hnode->exp;
}

// A part store's release, as the variable's state says
static void dropApplyPart(DropSite *site, DropEntry *entry) {
    INode *lval = site->node;
    uint8_t state = entry->state;
    lval->flags &= 0xFFFF - (FlagPartNoPrior | FlagDropTest);
    if (!(state & DropWhole))
        lval->flags |= FlagPartNoPrior;
    else if (dropMixed(state))
        lval->flags |= FlagDropTest;
}

// An exit's release list, rebuilt from what each variable may hold there,
// last declared first
static void dropApplyExit(DropSite *site) {
    BreakRetNode *exit = (BreakRetNode *)site->node;
    INode *result = exit->flowresult;
    INode *dropat = result != NULL ? result : (INode *)exit;
    Nodes *list = NULL;
    for (uint32_t i = site->cnt; i > 0; --i) {
        DropEntry *entry = &entries[site->first + i - 1];
        VarDclNode *var = pathVars[entry->var].var;
        uint8_t state = entry->state;
        if (!(state & (DropWhole | DropHollow)))
            continue;
        // A variable the scope hands back is the caller's. One whose value
        // left on a path to here (a marked move: a block or an 'if' handing it
        // on) has its flag to say so, and is not exempt.
        Nodes *hollow = NULL;
        if (!(state & DropMoved) && flowIsScopeResultOf(result, var, &hollow))
            continue;
        int whole = (state & DropWhole) && hollow == NULL;
        int hollowed = (state & DropHollow) || (hollow != NULL && (state & DropWhole));
        flowVarRelease(var, dropat, whole, hollowed, (state & DropHollow) ? var->hollowall : NULL, hollow,
            dropMixed(state), &list);
    }
    exit->dealias = list;
}

void dropWalkEnd(int ok) {
    if (!ok)
        return;
    // A variable whose state differs by path at any release has a flag
    for (uint32_t i = 0; i < nentries; ++i) {
        if (dropMixed(entries[i].state))
            pathVars[entries[i].var].flagged = 1;
    }
    for (uint32_t i = 0; i < nentries; ++i) {
        PathVar *pv = &pathVars[entries[i].var];
        if (pv->flagged && !(pv->var->flowtempflags & VarDropFlag)) {
            pv->var->flowtempflags |= VarDropFlag;
            ++statFlags;
        }
    }
    for (uint32_t s = 0; s < nsites; ++s) {
        DropSite *site = &sites[s];
        switch (site->kind) {
        case DropSiteExit:
            dropApplyExit(site);
            break;
        case DropSiteStore:
            dropApplyStore(site, &entries[site->first]);
            break;
        case DropSitePart:
            dropApplyPart(site, &entries[site->first]);
            break;
        }
    }
}

void dropPrint() {
    printf("Drop walk: %u functions, %u variables flagged\n\n", statFns, statFlags);
}
