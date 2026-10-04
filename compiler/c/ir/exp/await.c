/** Handling for 'await' nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

int awaitDirect = 0;

// Create a new await node
AwaitNode *newAwaitNode() {
    AwaitNode *node;
    newNode(node, AwaitNode, AwaitTag);
    node->vtype = unknownType;
    node->exp = NULL;
    node->seamvars = NULL;
    node->nseamvars = 0;
    node->seamcap = 0;
    node->seamno = 0;
    node->genseam = NULL;
    node->walked = 0;
    return node;
}

// Clone await: what a walk found at the seam is the original's own
INode *cloneAwaitNode(CloneState *cstate, AwaitNode *node) {
    AwaitNode *newnode;
    newnode = memAllocBlk(sizeof(AwaitNode));
    memcpy(newnode, node, sizeof(AwaitNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->seamvars = NULL;
    newnode->nseamvars = 0;
    newnode->seamcap = 0;
    newnode->seamno = 0;
    newnode->genseam = NULL;
    newnode->walked = 0;
    return (INode *)newnode;
}

// Serialize await
void awaitPrint(AwaitNode *node) {
    inodeFprint("(await, ");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of await
void awaitNameRes(NameResState *pstate, AwaitNode *node) {
    inodeNameRes(pstate, &node->exp);
}

// Where may 'await' stand? In a method of an actor's state: a seam is a return
// to the actor's dispatcher, which runs only its methods. An actor's 'init'
// runs on the thread making the actor, before any dispatcher has it, and its
// 'final' as it dies; neither is a method the dispatcher runs
// (refconccomm.html: an actor's body is fields, an init, methods, a final).
static char *awaitNotPlaced(FnDclNode *fn) {
    if (fn == NULL)
        return "outside any function";
    if (!(fn->flags & FlagMethFld) || !actorIsState(inodeGetOwner((INode *)fn)))
        return "in a function that is not a method of an actor";
    if (fn->namesym == initName || fn->overloadsym == initName)
        return "in an actor's initializer, which runs on the thread making the actor, before any dispatcher has it";
    if (fn->namesym == finalName)
        return "in an actor's finalizer, which runs as the actor dies";
    return NULL;
}

// Type check await
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType) {
    char *where = awaitNotPlaced(pstate->fn);
    if (where)
        errorMsgNode((INode *)node, ErrorAwaitPlace,
            "'await' stands only in an actor's method, where the method is cut and returns to the actor's dispatcher to wait; this one is %s.",
            where);
    if (!iexpTypeCheckAny(pstate, &node->exp))
        return;
    node->vtype = ((IExpNode *)node->exp)->vtype;
}

// *********************
// The unbuilt seam's report
// *********************

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    int count;
} SeamList;

static void seamListPut(SeamList *list, char *text) {
    if (list->len >= list->size)
        return;
    int n = snprintf(list->buf + list->len, list->size - list->len, "%s%s", list->count ? ", " : "", text);
    if (n > 0)
        list->len += (size_t)n < list->size - list->len ? (size_t)n : list->size - list->len - 1;
    ++list->count;
}

// A variable as the reader would name it: by its name, or a temporary, which
// no source names, by where it was made
static void seamListVar(SeamList *list, SeamVar *sv, int guard) {
    VarDclNode *var = sv->var;
    char name[160];
    if (guard)
        snprintf(name, sizeof(name), "the lock taken at %u:%u", var->linenbr, (uint32_t)(var->srcp - var->linep) + 1);
    else if (var->namesym == tempLocalName || var->namesym == tempName)
        snprintf(name, sizeof(name), "the temporary made at %u:%u", var->linenbr, (uint32_t)(var->srcp - var->linep) + 1);
    else
        snprintf(name, sizeof(name), "%s", &var->namesym->namestr);
    seamListPut(list, name);
}

// Is this the actor's own 'self', which the dispatcher lends the second half
// afresh rather than the record carrying it?
static int seamIsSelf(SeamVar *sv) {
    return (sv->flags & SeamParm) && sv->var->namesym == selfName;
}

void awaitReportUnbuilt(Nodes *awaits) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(awaits, cnt, nodesp)) {
        AwaitNode *node = (AwaitNode *)*nodesp;
        if (!node->walked) {
            errorMsgNode((INode *)node, ErrorUnbuiltAwait,
                "'await' is not built yet, so nothing is generated for it. No path reaches this seam.");
            continue;
        }
        char record[512], finals[512], ended[512], locks[512], behind[512];
        SeamList lrecord = {record, sizeof(record), 0, 0};
        SeamList lfinals = {finals, sizeof(finals), 0, 0};
        SeamList lended = {ended, sizeof(ended), 0, 0};
        SeamList llocks = {locks, sizeof(locks), 0, 0};
        SeamList lbehind = {behind, sizeof(behind), 0, 0};
        record[0] = finals[0] = ended[0] = locks[0] = behind[0] = '\0';
        for (uint32_t i = 0; i < node->nseamvars; ++i) {
            SeamVar *sv = &node->seamvars[i];
            if (seamIsSelf(sv))
                continue;
            if (sv->flags & SeamEnds)
                seamListVar(&lended, sv, 0);
            if (!(sv->flags & SeamOpen))
                continue;
            // The seam's order: every borrow ends, a lock's guard with its
            // borrow; then what is used after it, and every droppable still
            // holding its value, moves into the record; the rest is left
            if (sv->flags & SeamGuard)
                seamListVar(&llocks, sv, 1);
            else if (sv->flags & (SeamLive | SeamDies))
                seamListVar(&lrecord, sv, 0);
            else
                seamListVar(&lbehind, sv, 0);
        }
        // What the record holds dies where the single-scope reading says, so in
        // the order it would have: the statement's temporaries, then the locals
        // and the parameters, the last declared first
        for (uint32_t i = node->nseamvars; i > 0; --i) {
            SeamVar *sv = &node->seamvars[i - 1];
            if (!seamIsSelf(sv) && (sv->flags & SeamOpen) && (sv->flags & SeamDies) && !(sv->flags & SeamGuard))
                seamListVar(&lfinals, sv, 0);
        }
        errorMsgNode((INode *)node, ErrorUnbuiltAwait,
            "'await' is not built yet, so nothing is generated for it. Its seam would end the borrows held by [%s]; give back [%s]; move into the continuation's record [%s], finalized as if the method were not cut, in the order [%s]; and leave behind [%s].",
            ended, locks, record, finals, behind);
    }
}

// *********************
// The split: a message's seams, generated as the method's first half and a
// second half for each seam (genllvm/genlawait.c)
// *********************

int awaitIsGuardType(INode *type) {
    INode *typedcl = type ? itypeGetTypeDcl(type) : NULL;
    return typedcl && typedcl->tag == RefTag && permHeldKind(((RefNode *)typedcl)->perm);
}

// The split functions and their seams
static FnDclNode **splitfns = NULL;
static Nodes **splitawaits = NULL;
static uint32_t splitcnt = 0;
static uint32_t splitmax = 0;

Nodes *awaitSplitOf(FnDclNode *fn) {
    for (uint32_t i = 0; i < splitcnt; ++i) {
        if (splitfns[i] == fn)
            return splitawaits[i];
    }
    return NULL;
}

static void awaitSplitAdd(FnDclNode *fn, Nodes *awaits) {
    if (splitcnt == splitmax) {
        uint32_t newmax = splitmax ? splitmax * 2 : 16;
        FnDclNode **fns = (FnDclNode **)memAllocBlk(newmax * sizeof(FnDclNode *));
        Nodes **lists = (Nodes **)memAllocBlk(newmax * sizeof(Nodes *));
        if (splitcnt) {
            memcpy(fns, splitfns, splitcnt * sizeof(FnDclNode *));
            memcpy(lists, splitawaits, splitcnt * sizeof(Nodes *));
        }
        splitfns = fns;
        splitawaits = lists;
        splitmax = newmax;
    }
    splitfns[splitcnt] = fn;
    splitawaits[splitcnt++] = awaits;
}

// A message: a 'pub' method of an actor's state, which its dispatcher runs for
// a message sent to it. Whether an 'await' in any other method of the actor
// cuts its callers, or is refused, is not settled, so only a message is split
static int awaitIsMessage(FnDclNode *fn) {
    return (fn->flags & FlagMethFld) && (fn->flags & FlagPub)
        && actorIsState(inodeGetOwner((INode *)fn));
}

static int awaitWalk(INode *node, int check, uint32_t *bad);

// Each 'await' in 'node' -- in what it awaits too -- reported once as standing
// where the split is not built, for the reason 'why' its construct gives
static void awaitReportIn(INode *node, char *why, uint32_t *bad);

static void awaitReportNodes(Nodes *nodes, char *why, uint32_t *bad) {
    INode **nodesp;
    uint32_t cnt;
    if (nodes == NULL)
        return;
    for (nodesFor(nodes, cnt, nodesp))
        awaitReportIn(*nodesp, why, bad);
}

// The children of a node, each handed to 'awaitWalk'; answers whether any holds an 'await'
static int awaitWalkNodes(Nodes *nodes, int check, uint32_t *bad) {
    if (nodes == NULL)
        return 0;
    int found = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp))
        found |= awaitWalk(*nodesp, check, bad);
    return found;
}

// A part of a construct where a seam cannot be split yet: what generating the
// construct holds across the part is an address, or memory being filled, not
// a value a record can carry. Each 'await' in the part is reported
static int awaitWalkUnsplit(INode *part, int check, uint32_t *bad, char *why) {
    if (!awaitWalk(part, 0, bad))
        return 0;
    if (check)
        awaitReportIn(part, why, bad);
    return 1;
}

// Walk 'node', answering whether an 'await' stands in it. Where 'check', each
// 'await' standing where the split is not built is reported, and counted in '*bad'
static int awaitWalk(INode *node, int check, uint32_t *bad) {
    if (node == NULL)
        return 0;
    switch (node->tag) {
    case AwaitTag:
        awaitWalk(((AwaitNode *)node)->exp, check, bad);
        return 1;
    // A function declared inside is lifted out: its seams are its own
    case FnDclTag:
        return 0;
    case VarDclTag:
        return awaitWalk(((VarDclNode *)node)->value, check, bad);
    case BlockTag:
        return awaitWalkNodes(((BlockNode *)node)->stmts, check, bad);
    case IfTag:
        return awaitWalkNodes(((IfNode *)node)->condblk, check, bad);
    case BreakTag:
    case ContinueTag:
    case BlockRetTag:
    case ReturnTag:
        return awaitWalk(((BreakRetNode *)node)->exp, check, bad);
    // The value is made first, then the place's address is taken: an 'await'
    // in the place would wait while that address is held
    case AssignTag:
    {
        int found = awaitWalk(((AssignNode *)node)->rval, check, bad);
        found |= awaitWalkUnsplit(((AssignNode *)node)->lval, check, bad,
            "in the place an assignment stores into, whose address would be held across the seam");
        return found;
    }
    case SwapTag:
    {
        int found = awaitWalkUnsplit(((SwapNode *)node)->lval, check, bad,
            "in a place a swap exchanges, whose address would be held across the seam");
        found |= awaitWalkUnsplit(((SwapNode *)node)->rval, check, bad,
            "in a place a swap exchanges, whose address would be held across the seam");
        return found;
    }
    case VTupleTag:
        return awaitWalkNodes(((TupleNode *)node)->elems, check, bad);
    // An array's contents that repeat a value, fill a size, or are written with
    // several sizes are filled in memory an element at a time
    case ArrayLitTag:
    {
        ArrayNode *lit = (ArrayNode *)node;
        if (lit->repeats || lit->nsizes > 1 || (lit->dimens && lit->dimens->used > 0)) {
            int found = 0;
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(lit->elems, cnt, nodesp))
                found |= awaitWalkUnsplit(*nodesp, check, bad,
                    "in an array's contents that repeat a value or fill a size, which are filled in memory across the seam");
            return found;
        }
        return awaitWalkNodes(lit->elems, check, bad);
    }
    case FnCallTag:
    case TypeLitTag:
    case FldAccessTag:
    {
        FnCallNode *call = (FnCallNode *)node;
        int found = awaitWalk(call->objfn, check, bad);
        found |= awaitWalkNodes(call->args, check, bad);
        return found;
    }
    // The array's address is taken before its index is made
    case ArrIndexTag:
    {
        FnCallNode *call = (FnCallNode *)node;
        int found = awaitWalk(call->objfn, check, bad);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(call->args, cnt, nodesp))
            found |= awaitWalkUnsplit(*nodesp, check, bad,
                "in the index of a place, whose address would be held across the seam");
        return found;
    }
    case CastTag:
    case IsTag:
        return awaitWalk(((CastNode *)node)->exp, check, bad);
    case DerefTag:
        return awaitWalk(((StarNode *)node)->vtexp, check, bad);
    case BorrowTag:
    case ArrayBorrowTag:
        return awaitWalk(((RefNode *)node)->vtexp, check, bad);
    // An allocation filled with an array's contents is made before they are
    case AllocateTag:
        if (node->flags & FlagAllocFill)
            return awaitWalkUnsplit(((RefNode *)node)->vtexp, check, bad,
                "in an array's contents an allocation is filled with, which would be held part filled across the seam");
        return awaitWalk(((RefNode *)node)->vtexp, check, bad);
    case NotLogicTag:
        return awaitWalk(((LogicNode *)node)->lexp, check, bad);
    case OrLogicTag:
    case AndLogicTag:
    {
        int found = awaitWalk(((LogicNode *)node)->lexp, check, bad);
        found |= awaitWalk(((LogicNode *)node)->rexp, check, bad);
        return found;
    }
    case NamedValTag:
        return awaitWalk(((NamedValNode *)node)->val, check, bad);
    case OfEntryTag: case FillEntryTag: case PairEntryTag:
    {
        int found = awaitWalk(((EntryNode *)node)->first, check, bad);
        found |= awaitWalk(((EntryNode *)node)->val, check, bad);
        return found;
    }
    case RefCountTag:
        return awaitWalk(((RefCountNode *)node)->exp, check, bad);
    case HollowTag:
        return awaitWalk(((HollowNode *)node)->exp, check, bad);
    case DropFlagTag:
        return awaitWalk(((DropFlagNode *)node)->release, check, bad);
    case TempTag:
        return awaitWalk(((TempNode *)node)->exp, check, bad);
    default:
        return 0;
    }
}

// Where the last 'await' reported as not split stands: a value an array's
// contents repeat is copied for each element it fills, each copy an 'await'
// of its own at the one place in the source, reported once
static char *awaitReportedAt = NULL;

static void awaitReportIn(INode *node, char *why, uint32_t *bad) {
    if (node == NULL)
        return;
    switch (node->tag) {
    case AwaitTag:
    {
        AwaitNode *await = (AwaitNode *)node;
        if (await->seamno == 0) {
            await->seamno = UINT32_MAX;
            if (node->srcp != awaitReportedAt)
                errorMsgNode(node, ErrorUnbuiltAwait,
                    "'await' is not built here yet, so nothing is generated for it: it stands %s.", why);
            awaitReportedAt = node->srcp;
            ++*bad;
        }
        awaitReportIn(await->exp, why, bad);
        return;
    }
    case FnDclTag:
        return;
    case VarDclTag:
        awaitReportIn(((VarDclNode *)node)->value, why, bad); return;
    case BlockTag:
        awaitReportNodes(((BlockNode *)node)->stmts, why, bad); return;
    case IfTag:
        awaitReportNodes(((IfNode *)node)->condblk, why, bad); return;
    case BreakTag: case ContinueTag: case BlockRetTag: case ReturnTag:
        awaitReportIn(((BreakRetNode *)node)->exp, why, bad); return;
    case AssignTag:
        awaitReportIn(((AssignNode *)node)->lval, why, bad);
        awaitReportIn(((AssignNode *)node)->rval, why, bad);
        return;
    case SwapTag:
        awaitReportIn(((SwapNode *)node)->lval, why, bad);
        awaitReportIn(((SwapNode *)node)->rval, why, bad);
        return;
    case VTupleTag:
        awaitReportNodes(((TupleNode *)node)->elems, why, bad); return;
    case ArrayLitTag:
        awaitReportNodes(((ArrayNode *)node)->elems, why, bad); return;
    case FnCallTag: case TypeLitTag: case FldAccessTag: case ArrIndexTag:
        awaitReportIn(((FnCallNode *)node)->objfn, why, bad);
        awaitReportNodes(((FnCallNode *)node)->args, why, bad);
        return;
    case CastTag: case IsTag:
        awaitReportIn(((CastNode *)node)->exp, why, bad); return;
    case DerefTag:
        awaitReportIn(((StarNode *)node)->vtexp, why, bad); return;
    case BorrowTag: case ArrayBorrowTag: case AllocateTag:
        awaitReportIn(((RefNode *)node)->vtexp, why, bad); return;
    case NotLogicTag:
        awaitReportIn(((LogicNode *)node)->lexp, why, bad); return;
    case OrLogicTag: case AndLogicTag:
        awaitReportIn(((LogicNode *)node)->lexp, why, bad);
        awaitReportIn(((LogicNode *)node)->rexp, why, bad);
        return;
    case NamedValTag:
        awaitReportIn(((NamedValNode *)node)->val, why, bad); return;
    case OfEntryTag: case FillEntryTag: case PairEntryTag:
        awaitReportIn(((EntryNode *)node)->first, why, bad);
        awaitReportIn(((EntryNode *)node)->val, why, bad);
        return;
    case RefCountTag:
        awaitReportIn(((RefCountNode *)node)->exp, why, bad); return;
    case HollowTag:
        awaitReportIn(((HollowNode *)node)->exp, why, bad); return;
    case DropFlagTag:
        awaitReportIn(((DropFlagNode *)node)->release, why, bad); return;
    case TempTag:
        awaitReportIn(((TempNode *)node)->exp, why, bad); return;
    default:
        return;
    }
}

int awaitWithin(INode *node) {
    uint32_t bad = 0;
    return awaitWalk(node, 0, &bad);
}

void awaitSplitOrReport(FnDclNode *fn, Nodes *awaits) {
    if (!awaitDirect || !awaitIsMessage(fn)) {
        awaitReportUnbuilt(awaits);
        return;
    }
    uint32_t bad = 0;
    awaitWalk(fn->value, 1, &bad);
    if (bad)
        return;
    // Each seam numbered in the order written, and each lock's guard a seam
    // gives back followed by a flag (VarSeamHeld): the code after the seam
    // reaches the guard's scope's end too
    INode **nodesp;
    uint32_t cnt;
    uint32_t seamno = 0;
    for (nodesFor(awaits, cnt, nodesp)) {
        AwaitNode *node = (AwaitNode *)*nodesp;
        node->seamno = ++seamno;
        for (uint32_t i = 0; i < node->nseamvars; ++i) {
            SeamVar *sv = &node->seamvars[i];
            if ((sv->flags & SeamGuard) && !(sv->flags & SeamTemp))
                sv->var->flowtempflags |= VarSeamHeld;
        }
    }
    awaitSplitAdd(fn, awaits);
}
