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
    node->message = NULL;
    node->walked = 0;
    return node;
}

SelfActorNode *newSelfActorNode() {
    SelfActorNode *node;
    newNode(node, SelfActorNode, SelfActorTag);
    node->vtype = unknownType;
    return node;
}

static AwaitReplyNode *newAwaitReplyNode(AwaitNode *await, INode *type) {
    AwaitReplyNode *node;
    newNode(node, AwaitReplyNode, AwaitReplyTag);
    inodeLexCopy((INode *)node, (INode *)await);
    node->vtype = type;
    node->await = await;
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
    newnode->message = NULL;
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

// Where may 'selfactor' stand, and a behaviour be sent through 'self'? In a
// method of an actor's state, where the state is in the actor. An actor's
// 'init' runs on the thread making the actor, before the actor exists, and
// its 'final' as it dies (refconccomm.html: an actor's body is fields, an
// init, behaviours, synchronous methods, a final).
static char *actorMethodNotPlaced(FnDclNode *fn) {
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

// Where may 'await' stand? In a behaviour, a method declared 'async do': a
// seam is a return to the actor's dispatcher, which runs a behaviour as a
// message. Every 'fn' of an actor -- its 'init', its 'final', a helper -- is
// synchronous, run inside whatever called it, which a seam cannot cut. NULL
// where it may, and otherwise why not, written into 'buf'
static char *awaitNotPlaced(FnDclNode *fn, char *buf, size_t size) {
    if (fn == NULL)
        return "This one is outside any function.";
    if (actorOfBehaviour(fn))
        return NULL;
    ActorInfo *info = (fn->flags & FlagMethFld) ? actorOfState(inodeGetOwner((INode *)fn)) : NULL;
    if (info == NULL)
        return "This one is in a function that is not an actor's behaviour.";
    Name *actorname = info->handle->namesym;
    if (fn->namesym == initName || fn->overloadsym == initName)
        snprintf(buf, size, "This one is in actor %s's initializer, which runs on the thread making the actor, before any dispatcher has it.",
            &actorname->namestr);
    else if (fn->namesym == finalName)
        snprintf(buf, size, "This one is in actor %s's finalizer, which runs as the actor dies.", &actorname->namestr);
    else
        snprintf(buf, size, "This one is in %s, a 'fn' of actor %s's: synchronous, run inside the method that calls it, which cannot pause partway through a call. An 'await' belongs at the top of a behaviour: await there, and push the synchronous work down, onto a helper like this one or a type the actor owns, to run between the awaits.",
            &fn->namesym->namestr, &actorname->namestr);
    return buf;
}

// What is awaited is a behaviour of an actor -- a call of a handle's method
// that sends one -- when the behaviour returns a value: it is sent awaited.
// The call is made the handle's second method for it, which takes the
// reply's envelope as well, and the envelope is the call's last argument;
// the 'await''s value is what the behaviour returns. Its signature, as the
// author wrote it, is unchanged: the envelope is in the message, not in any
// parameter list. A behaviour that returns nothing sends no reply: whether
// one is sent is the behaviour's signature to say, never the caller's
static void awaitMessage(TypeCheckState *pstate, AwaitNode *node) {
    // Checked again: the call is the awaited one already
    if (node->message) {
        node->vtype = ((FnSigNode *)node->message->vtype)->rettype;
        return;
    }
    if (node->exp->tag != FnCallTag)
        return;
    FnCallNode *call = (FnCallNode *)node->exp;
    if (call->objfn == NULL || !isNameUseNode(call->objfn))
        return;
    INode *dcl = ((NameUseNode *)call->objfn)->dclnode;
    if (dcl == NULL || dcl->tag != FnDclTag)
        return;
    ActorInfo *callee;
    ActorMessage *msg = actorMessageOfSend((FnDclNode *)dcl, &callee);
    if (msg == NULL)
        return;
    if (msg->ask == NULL) {
        errorMsgNode((INode *)node, ErrorAwaitVoid,
            "'await' waits for a behaviour's reply, and %s returns nothing, so it sends none: a behaviour with no return type owes its caller nothing. If the caller needs to know %s finished, declare a return type -- a status saying whether it worked -- and await that.",
            &msg->method->namesym->namestr, &msg->method->namesym->namestr);
        node->vtype = errorType;
        return;
    }
    ActorInfo *awaiter = actorOfState(inodeGetOwner((INode *)pstate->fn));
    if (awaiter == NULL || awaiter->replyfn == NULL || awaiter->replyidfn == NULL) {
        errorUnreachable((INode *)node, "an 'await' in an actor generated no functions for its seams");
        return;
    }
    for (int i = 0; i < ActorRtCount; ++i) {
        if (actorRuntime[i] == NULL) {
            errorMsgNode((INode *)node, ErrorActorRuntime,
                "'await' needs the actors package's %s, which the package this module imports as actors does not have.",
                actorRuntimeNames[i]);
            return;
        }
        fnCallDemandCandidates((INode *)actorRuntime[i]);
    }
    fnCallDemandCandidates((INode *)msg->ask);
    fnCallDemandCandidates((INode *)msg->method);
    fnCallDemandCandidates((INode *)awaiter->replyfn);
    fnCallDemandCandidates((INode *)awaiter->replyidfn);

    FnSigNode *asksig = (FnSigNode *)msg->ask->vtype;
    NameUseNode *askuse = (NameUseNode *)call->objfn;
    askuse->dclnode = (INode *)msg->ask;
    askuse->vtype = (INode *)asksig;
    INode *envtype = ((VarDclNode *)nodesGet(asksig->parms, asksig->parms->used - 1))->vtype;
    nodesAdd(&call->args, (INode *)newAwaitReplyNode(node, envtype));
    node->message = msg->method;
    node->vtype = ((FnSigNode *)msg->method->vtype)->rettype;
}

// Type check await
void awaitTypeCheck(TypeCheckState *pstate, AwaitNode *node, INode *expectType) {
    char buf[600];
    char *where = awaitNotPlaced(pstate->fn, buf, sizeof(buf));
    if (where)
        errorMsgNode((INode *)node, ErrorAwaitPlace,
            "'await' stands only in an actor's behaviour, declared 'async do', where the behaviour is cut and returns to the actor's dispatcher to wait. %s",
            where);
    if (!iexpTypeCheckAny(pstate, &node->exp))
        return;
    // Refused, it has no value, which is not reported again where it is used
    if (where) {
        node->vtype = errorType;
        return;
    }
    node->vtype = ((IExpNode *)node->exp)->vtype;
    awaitMessage(pstate, node);
}

// 'selfactor' is the actor's own handle: another owner of its mailbox, made
// from the state by the function the actor's declaration generated for it.
// It stands in any method of the actor's but two: its 'init' runs before the
// actor exists, and its 'final' as it dies, when nothing may own it again
void selfActorTypeCheck(TypeCheckState *pstate, INode **nodep) {
    INode *node = *nodep;
    char *where = actorMethodNotPlaced(pstate->fn);
    if (where) {
        errorMsgNode(node, ErrorSelfActorPlace,
            "'selfactor', the actor's own handle, stands only in an actor's method; this one is %s.", where);
        ((IExpNode *)node)->vtype = errorType;
        return;
    }
    ActorInfo *info = actorOfState(inodeGetOwner((INode *)pstate->fn));
    if (info == NULL || info->selffn == NULL) {
        errorUnreachable(node, "a 'selfactor' whose actor generated no function for it");
        ((IExpNode *)node)->vtype = errorType;
        return;
    }
    VarDclNode *self = (VarDclNode *)nodesGet(((FnSigNode *)pstate->fn->vtype)->parms, 0);
    NameUseNode *selfuse = newNameUseNode(selfName);
    inodeLexCopy((INode *)selfuse, node);
    selfuse->dclnode = (INode *)self;
    NameUseNode *fnuse = newNameUseNode(info->selffn->namesym);
    inodeLexCopy((INode *)fnuse, node);
    fnuse->dclnode = (INode *)info->selffn;
    FnCallNode *call = newFnCallLower(node, (INode *)fnuse, 1);
    nodesAdd(&call->args, (INode *)selfuse);
    *nodep = (INode *)call;
    inodeTypeCheckAny(pstate, nodep);
}

// A behaviour runs only as its dispatcher calls it, for a message: so
// 'self.m()', m one of the actor's behaviours, written in any of its other
// methods, is a send to the actor itself, as 'selfactor.m()' is. The caller
// goes on at once, with no seam; m runs later, when the actor takes the
// message up. The receiver becomes the actor's handle, made from the state
// by the function 'selfactor' is (Counter.self'), and the call then selects
// the handle's method sending m. Only the method's own 'self' is the state of
// an actor the runtime holds: a state made with 'new Self(...)' in a method
// is in no actor, so a behaviour reached through it is refused, as it is in
// the actor's 'init' and 'final', where 'selfactor' is.
int selfActorSend(TypeCheckState *pstate, FnCallNode *call, INode *objdereftype) {
    ActorInfo *info = actorOfState(objdereftype);
    if (info == NULL || pstate->fn == info->dispatch
        || call->methfld == NULL || !isNameUseNode(call->methfld))
        return 0;
    Name *methsym = ((NameUseNode *)call->methfld)->namesym;
    if (actorMessageNamed(info, methsym) == NULL)
        return 0;
    Name *actorname = info->handle->namesym;
    char *where = actorMethodNotPlaced(pstate->fn);
    if (where == NULL && actorOfState(inodeGetOwner((INode *)pstate->fn)) != info)
        where = "in a method of another actor";
    if (where) {
        errorMsgNode((INode *)call, ErrorBehaviourSend,
            "%s is a behaviour of actor %s's, so calling it sends it to the actor, as a message; this call is %s, where there is no actor to send it to.",
            &methsym->namestr, &actorname->namestr, where);
        call->vtype = errorType;
        return -1;
    }
    VarDclNode *self = (VarDclNode *)nodesGet(((FnSigNode *)pstate->fn->vtype)->parms, 0);
    INode *obj = call->objfn;
    if (!isNameUseNode(obj) || ((NameUseNode *)obj)->dclnode != (INode *)self) {
        errorMsgNode((INode *)call, ErrorBehaviourSend,
            "%s is a behaviour of actor %s's, so calling it sends it to the actor whose state it is reached through, and only this method's own 'self' is the state of an actor the runtime holds. Send it through 'self', 'selfactor' or another of the actor's handles.",
            &methsym->namestr, &actorname->namestr);
        call->vtype = errorType;
        return -1;
    }
    if (info->selffn == NULL) {
        errorUnreachable((INode *)call, "a behaviour whose actor generated no function for its handle");
        call->vtype = errorType;
        return -1;
    }
    NameUseNode *selfuse = newNameUseNode(selfName);
    inodeLexCopy((INode *)selfuse, obj);
    selfuse->dclnode = (INode *)self;
    NameUseNode *fnuse = newNameUseNode(info->selffn->namesym);
    inodeLexCopy((INode *)fnuse, obj);
    fnuse->dclnode = (INode *)info->selffn;
    FnCallNode *handle = newFnCallLower(obj, (INode *)fnuse, 1);
    nodesAdd(&handle->args, (INode *)selfuse);
    call->objfn = (INode *)handle;
    inodeTypeCheckAny(pstate, &call->objfn);
    return 1;
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
        char *unbuilt = "'await' is not built yet, so nothing is generated for it.";
        if (!node->walked) {
            errorMsgNode((INode *)node, ErrorUnbuiltAwait, "%s No path reaches this seam.", unbuilt);
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
            "%s Its seam would end the borrows held by [%s]; give back [%s]; move into the continuation's record [%s], finalized as if the method were not cut, in the order [%s]; and leave behind [%s].",
            unbuilt, ended, locks, record, finals, behind);
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

// A behaviour: a method of an actor's state declared 'async do', which its
// dispatcher runs for a message sent to it. An 'await' anywhere else was
// refused where it stands (awaitTypeCheck), so only a behaviour is split
static int awaitIsMessage(FnDclNode *fn) {
    return actorOfBehaviour(fn) != NULL;
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

// A continuation waiting in the pending table is off the stack, where the
// collector's roots are not: the table is not traced yet, so a record that
// would hold a traced reference is refused
static int awaitRecordTraced(AwaitNode *node) {
    int bad = 0;
    for (uint32_t i = 0; i < node->nseamvars; ++i) {
        SeamVar *sv = &node->seamvars[i];
        if (!(sv->flags & SeamOpen) || !(sv->flags & (SeamLive | SeamDies)) || (sv->flags & SeamGuard))
            continue;
        if ((sv->flags & SeamParm) && sv->var->namesym == selfName)
            continue;
        if (!itypeHoldsTraced(sv->var->vtype))
            continue;
        errorMsgNode((INode *)node, ErrorUnbuiltAwait,
            "'await' whose continuation holds a traced reference is not built: %s would wait in the actor's pending table, which the collector does not trace yet.",
            &sv->var->namesym->namestr);
        bad = 1;
    }
    return bad;
}

void awaitSplitOrReport(FnDclNode *fn, Nodes *awaits) {
    if (!awaitIsMessage(fn)) {
        awaitReportUnbuilt(awaits);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    // What is awaited is a behaviour returning a value, whose reply calls the
    // second half; any other seam is built only under '--await-direct', for
    // tests
    Nodes *unbuilt = NULL;
    for (nodesFor(awaits, cnt, nodesp)) {
        AwaitNode *node = (AwaitNode *)*nodesp;
        if (node->message == NULL && !awaitDirect) {
            if (unbuilt == NULL)
                unbuilt = newNodes(4);
            nodesAdd(&unbuilt, (INode *)node);
        }
    }
    if (unbuilt) {
        awaitReportUnbuilt(unbuilt);
        return;
    }
    uint32_t bad = 0;
    awaitWalk(fn->value, 1, &bad);
    for (nodesFor(awaits, cnt, nodesp)) {
        AwaitNode *node = (AwaitNode *)*nodesp;
        if (node->message && awaitRecordTraced(node))
            ++bad;
    }
    if (bad)
        return;
    // Each seam numbered in the order written, and each lock's guard a seam
    // gives back followed by a flag (VarSeamHeld): the code after the seam
    // reaches the guard's scope's end too
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
