/** Actors: what an 'actor' declaration became
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

static ActorInfo **actors = NULL;
static uint32_t actorCnt = 0;
static uint32_t actorMax = 0;

static ActorTraitInfo **actorTraits = NULL;
static uint32_t actorTraitCnt = 0;
static uint32_t actorTraitMax = 0;

FnDclNode *actorRuntime[ActorRtCount];
char *actorRuntimeNames[ActorRtCount] = {
    "parkReserve", "parked", "unpark", "recordFree", "answerTo", "answered", "startAwait",
    "futureReady", "futureRegister", "futureOpen", "futureTaken", "parBlock", "parSeam"
};

StructNode *actorAwaitable = NULL;
StructNode *actorFuture = NULL;
INode *actorAwaitOperand = NULL;

// The one type argument of 'type', where it is an instance of 'generic'
static INode *actorInstanceArg(StructNode *generic, INode *type) {
    if (generic == NULL || type == NULL || generic->genericinfo == NULL
        || generic->genericinfo->memonodes == NULL)
        return NULL;
    type = itypeGetTypeDcl(type);
    if (type == NULL || type->tag != StructTag)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(generic->genericinfo->memonodes, cnt, nodesp)) {
        ++nodesp; --cnt;  // pairs: the call, then its instance
        if (*nodesp != type)
            continue;
        Nodes *args = itypeInstanceTypeArgs(type);
        return args && args->used == 1 ? nodesGet(args, 0) : NULL;
    }
    return NULL;
}

INode *actorAwaitableResult(INode *type) {
    return actorInstanceArg(actorAwaitable, type);
}

INode *actorFutureResult(INode *type) {
    return actorInstanceArg(actorFuture, type);
}

void actorFutureCall(TypeCheckState *pstate, FnCallNode *call) {
    if (call->objfn == NULL || !isNameUseNode(call->objfn))
        return;
    INode *dcl = ((NameUseNode *)call->objfn)->dclnode;
    if (dcl == NULL || dcl->tag != FnDclTag)
        return;
    ActorInfo *callee;
    ActorMessage *msg = actorMessageOfSend((FnDclNode *)dcl, &callee);
    if (msg == NULL)
        return;
    // Whether a value comes back is the behaviour's declaration to say, as
    // for an 'await' (ErrorAwaitVoid)
    if (msg->future == NULL) {
        Name *name = msg->method->namesym;
        errorMsgNode((INode *)call, ErrorFutureVoid,
            "%s returns nothing, so its call gives no future to keep: a behaviour with no return type owes its caller nothing, and calling it sends a message and forgets it. Call it as a statement of its own; or, if the caller needs to know %s finished, declare a return type -- a status saying whether it worked -- and keep or await that.",
            &name->namestr, &name->namestr);
        return;
    }
    fnCallDemandCandidates((INode *)msg->future);
    FnSigNode *sig = (FnSigNode *)msg->future->vtype;
    NameUseNode *use = (NameUseNode *)call->objfn;
    use->dclnode = (INode *)msg->future;
    use->vtype = (INode *)sig;
    call->vtype = sig->rettype;
}

void actorRegister(ActorInfo *info) {
    if (actorCnt == actorMax) {
        uint32_t newmax = actorMax ? actorMax * 2 : 16;
        ActorInfo **grown = (ActorInfo **)memAllocBlk(newmax * sizeof(ActorInfo *));
        if (actorCnt)
            memcpy(grown, actors, actorCnt * sizeof(ActorInfo *));
        actors = grown;
        actorMax = newmax;
    }
    actors[actorCnt++] = info;
}

// What a message returns goes back to the actor that awaited it, on another
// thread, in its reply
static void actorCheckReturn(ActorInfo *info, FnDclNode *fn) {
    INode *type = ((FnSigNode *)fn->vtype)->rettype;
    if (type == NULL || type == unknownType || type->tag == UnknownTag
        || itypeGetTypeDcl(type)->tag == VoidTag || !itypeThreadBoundHow(type, NULL, StaticWritten))
        return;
    char typename[256] = "";
    itypeSpellCat(typename, sizeof(typename), type, 0);
    char what[512], reason[512];
    genericNotSendableWhy(type, what, reason, StaticWritten);
    errorMsgNode(((FnSigNode *)fn->vtype)->rettype, ErrorNotSendable,
        "Actor %s's behaviour %s returns a %s, which is not Sendable: %s %s. The value goes back to the actor that awaits it, on the thread that runs that actor, so it must be Sendable.",
        &info->handle->namesym->namestr, &fn->namesym->namestr, typename, what, reason);
}

// Is this parameter one whose argument crosses to the actor (so the thread
// check has judged it)?
static int actorParmCrosses(ActorInfo *info, VarDclNode *parm) {
    for (uint32_t j = 0; j + 1 < info->crossing->used; j += 2) {
        if (nodesGet(info->crossing, j + 1) == (INode *)parm)
            return 1;
    }
    return 0;
}

// A stopgap until the collector is per actor (the regions work): an actor makes,
// holds or copies no traced ('Gc') reference, whatever the thread it is run on.
// Its state's fields, its methods' parameters and everything its methods' bodies
// make are asked, behaviours, synchronous methods, 'init' and 'final' alike.
// (A behaviour's or initializer's parameter, that crosses, is the thread check's.)
static void actorRefuseGc(ActorInfo *info) {
    StructNode *state = info->state;
    INode **nodesp;
    uint32_t cnt;
    // The state is a root of the actor's own collector, handed to it between
    // messages: one holding a traced reference that its trace cannot reach (a
    // raw pointer to a value holding one, declaring no GcTraceElements) would
    // leave that reference to dangle when the collector moves what it points at
    if (itypeHoldsTraced((INode *)state) && itypeCannotTrace((INode *)state))
        errorMsgNode((INode *)state, ErrorTraceRoot,
            "Actor %s's state holds a traced reference and cannot be traced: it holds a raw pointer to a value that may hold one, or an owner a trace cannot pass through, and no GcTraceElements reaches what it points at. The actor's collector starts from its state, so a reference it cannot find would not be moved with the rest. A type that holds values this way declares GcTraceElements, and a 'gcTraceElements' giving a cursor over a writable reference to each.",
            &info->handle->namesym->namestr);
    for (nodelistFor(&state->fields, cnt, nodesp)) {
        if ((*nodesp)->tag == FieldDclTag)
            gcStopgapCheckDcl(*nodesp, ((IExpNode *)*nodesp)->vtype, "an actor's state");
    }
    for (nodelistFor(&state->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag)
            continue;
        FnDclNode *fn = (FnDclNode *)*nodesp;
        if (fn->genericinfo != NULL || fn->vtype == NULL || fn->vtype->tag != FnSigTag)
            continue;
        INode **parmp;
        uint32_t pcnt;
        Nodes *parms = ((FnSigNode *)fn->vtype)->parms;
        if (parms) {
            for (nodesFor(parms, pcnt, parmp)) {
                if ((*parmp)->tag == VarDclTag && !actorParmCrosses(info, (VarDclNode *)*parmp))
                    gcStopgapCheckDcl(*parmp, ((IExpNode *)*parmp)->vtype, "an actor's method");
            }
        }
        if (fn->value != NULL && fn->value->tag == BlockTag) {
            gcStopgapCheckBody(fn->value, "an actor's method");
            gcPieceCheckBody(fn->value, "a 'parallel each' body");
        }
    }
}

// A message carries its arguments to whichever worker thread runs the actor,
// and an initializer's arguments become the state, which the workers then run
// on: so each must be Sendable, as a channel's element or a thread's start
// value is. Asked here, of the parameters as written, rather than of the
// generated message enum: the enum declares Sendable on the strength of this
// check, so each refusal names the parameter, where the author can fix it.
// A message's returned value goes back in a reply, so it is asked too.
void actorCheckAll() {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        ActorInfo *info = actors[i];
        Name *actorname = info->handle->namesym;
        for (uint32_t j = 0; j + 1 < info->crossing->used; j += 2) {
            FnDclNode *fn = (FnDclNode *)nodesGet(info->crossing, j);
            VarDclNode *parm = (VarDclNode *)nodesGet(info->crossing, j + 1);
            INode *type = parm->vtype;
            if (type == NULL || type == unknownType || type->tag == UnknownTag)
                continue;
            if (!itypeThreadBoundHow(type, NULL, StaticWritten))
                continue;
            char typename[256] = "";
            itypeSpellCat(typename, sizeof(typename), type, 0);
            char what[512], reason[512];
            genericNotSendableWhy(type, what, reason, StaticWritten);
            int isinit = fn->namesym == initName || fn->overloadsym == initName;
            if (isinit)
                errorMsgNode((INode *)parm, ErrorNotSendable,
                    "Actor %s's initializer takes %s, a %s, which is not Sendable: %s %s. An actor's state is made from its initializer's arguments and then run on other threads, so each must be Sendable.",
                    &actorname->namestr, &parm->namesym->namestr, typename, what, reason);
            else
                errorMsgNode((INode *)parm, ErrorNotSendable,
                    "Actor %s's behaviour %s takes %s, a %s, which is not Sendable: %s %s. A message carries its arguments to the thread the actor runs on, so each must be Sendable.",
                    &actorname->namestr, &fn->namesym->namestr, &parm->namesym->namestr, typename, what, reason);
        }
        for (uint32_t j = 0; j < info->nmsgs; ++j)
            actorCheckReturn(info, info->msgs[j].method);
        actorRefuseGc(info);
    }
    // An actor trait's behaviours are sent to whatever actor the handle holds,
    // so each parameter and each returned type crosses to that actor's thread
    for (uint32_t i = 0; i < actorTraitCnt; ++i) {
        ActorTraitInfo *info = actorTraits[i];
        Name *traitname = info->handle->namesym;
        for (uint32_t j = 0; j + 1 < info->crossing->used; j += 2) {
            FnDclNode *fn = (FnDclNode *)nodesGet(info->crossing, j);
            VarDclNode *parm = (VarDclNode *)nodesGet(info->crossing, j + 1);
            INode *type = parm->vtype;
            if (type == NULL || type == unknownType || type->tag == UnknownTag)
                continue;
            if (!itypeThreadBoundHow(type, NULL, StaticWritten))
                continue;
            char typename[256] = "";
            itypeSpellCat(typename, sizeof(typename), type, 0);
            char what[512], reason[512];
            genericNotSendableWhy(type, what, reason, StaticWritten);
            errorMsgNode((INode *)parm, ErrorNotSendable,
                "Actor trait %s's behaviour %s takes %s, a %s, which is not Sendable: %s %s. A message carries its arguments to the thread the actor runs on, so each must be Sendable.",
                &traitname->namestr, &fn->namesym->namestr, &parm->namesym->namestr, typename, what, reason);
        }
        for (uint32_t j = 0; j < info->nmsgs; ++j) {
            FnDclNode *fn = info->msgs[j].method;
            INode *type = ((FnSigNode *)fn->vtype)->rettype;
            if (type == NULL || type == unknownType || type->tag == UnknownTag
                || itypeGetTypeDcl(type)->tag == VoidTag || !itypeThreadBoundHow(type, NULL, StaticWritten))
                continue;
            char typename[256] = "";
            itypeSpellCat(typename, sizeof(typename), type, 0);
            char what[512], reason[512];
            genericNotSendableWhy(type, what, reason, StaticWritten);
            errorMsgNode(((FnSigNode *)fn->vtype)->rettype, ErrorNotSendable,
                "Actor trait %s's behaviour %s returns a %s, which is not Sendable: %s %s. The value goes back to the actor that awaits it, on the thread that runs that actor, so it must be Sendable.",
                &traitname->namestr, &fn->namesym->namestr, typename, what, reason);
        }
    }
}

int actorIsState(INode *type) {
    return actorOfState(type) != NULL;
}

int actorIsAllocator(FnDclNode *fn) {
    if (fn->namesym != allocRegionMethodName && fn->overloadsym != allocRegionMethodName)
        return 0;
    INode *owner = inodeGetOwner((INode *)fn);
    return owner != NULL && actorOfState(owner) != NULL;
}

ActorInfo *actorOfState(INode *type) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i]->state == type)
            return actors[i];
    }
    return NULL;
}

ActorInfo *actorOfHandle(INode *type) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i]->handle == type)
            return actors[i];
    }
    return NULL;
}

ActorInfo *actorOfSelfFn(FnDclNode *fn) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if (fn != NULL && actors[i]->selffn == fn)
            return actors[i];
    }
    return NULL;
}

ActorMessage *actorMessageOfSend(FnDclNode *send, ActorInfo **info) {
    INode *owner = inodeGetOwner((INode *)send);
    // A trait's handle sends through its table: the same four shapes, but no
    // actor of its own to answer
    for (uint32_t i = 0; i < actorTraitCnt; ++i) {
        if ((INode *)actorTraits[i]->handle != owner)
            continue;
        for (uint32_t j = 0; j < actorTraits[i]->nmsgs; ++j) {
            if (actorTraits[i]->msgs[j].send == send) {
                *info = NULL;
                return &actorTraits[i]->msgs[j];
            }
        }
        return NULL;
    }
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i]->handle != owner)
            continue;
        for (uint32_t j = 0; j < actors[i]->nmsgs; ++j) {
            if (actors[i]->msgs[j].send == send) {
                *info = actors[i];
                return &actors[i]->msgs[j];
            }
        }
        return NULL;
    }
    return NULL;
}

int actorMethodAwaits(ActorInfo *info, FnDclNode *method) {
    INode **nodesp;
    uint32_t cnt;
    if (info->awaiting == NULL)
        return 0;
    for (nodesFor(info->awaiting, cnt, nodesp)) {
        if (*nodesp == (INode *)method)
            return 1;
    }
    return 0;
}

ActorInfo *actorOfBehaviour(FnDclNode *fn) {
    ActorInfo *info = actorOfState(inodeGetOwner((INode *)fn));
    if (info == NULL || info->behaviours == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(info->behaviours, cnt, nodesp)) {
        if (*nodesp == (INode *)fn)
            return info;
    }
    return NULL;
}

ActorMessage *actorMessageNamed(ActorInfo *info, Name *name) {
    for (uint32_t j = 0; j < info->nmsgs; ++j) {
        FnDclNode *method = info->msgs[j].method;
        if (method->namesym == name || (method->overloadsym && method->overloadsym == name))
            return &info->msgs[j];
    }
    return NULL;
}

INode *actorStateMember(INode *type, Name *name, StructNode **state) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i]->handle != type)
            continue;
        INode *member = iNsTypeFindFnField((INsTypeNode *)actors[i]->state, name);
        if (member == NULL || member->tag == StructTag)
            return NULL;
        *state = actors[i]->state;
        return member;
    }
    return NULL;
}

// ---- Actor traits ------------------------------------------------------------

void actorTraitRegister(ActorTraitInfo *info) {
    if (actorTraitCnt == actorTraitMax) {
        uint32_t newmax = actorTraitMax ? actorTraitMax * 2 : 8;
        ActorTraitInfo **grown = (ActorTraitInfo **)memAllocBlk(newmax * sizeof(ActorTraitInfo *));
        if (actorTraitCnt)
            memcpy(grown, actorTraits, actorTraitCnt * sizeof(ActorTraitInfo *));
        actorTraits = grown;
        actorTraitMax = newmax;
    }
    actorTraits[actorTraitCnt++] = info;
}

ActorTraitInfo *actorTraitOfHandle(INode *type) {
    for (uint32_t i = 0; i < actorTraitCnt; ++i) {
        if ((INode *)actorTraits[i]->handle == type)
            return actorTraits[i];
    }
    return NULL;
}

int actorTraitWanted(INode *from, INode *to) {
    return actorTraitCnt != 0 && to != NULL && from != NULL
        && actorTraitOfHandle(to) != NULL && actorOfHandle(from) != NULL;
}

// Do two types written as a parameter's or a return, either maybe none, agree?
static int actorSameType(INode *a, INode *b) {
    int avoid = a == NULL || itypeGetTypeDcl(a)->tag == VoidTag;
    int bvoid = b == NULL || itypeGetTypeDcl(b)->tag == VoidTag;
    if (avoid || bvoid)
        return avoid && bvoid;
    return itypeIsSame(a, b);
}

// A behaviour as 'name(a: A, b: B) R', its 'self' left out
static void actorSigCat(char *buf, size_t size, FnDclNode *fn) {
    FnSigNode *sig = (FnSigNode *)fn->vtype;
    size_t len = strlen(buf);
    snprintf(buf + len, size - len, "%s(", &fn->namesym->namestr);
    int first = 1;
    INode **parmp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, parmp)) {
        VarDclNode *parm = (VarDclNode *)*parmp;
        if (parm->namesym == selfName)
            continue;
        len = strlen(buf);
        snprintf(buf + len, size - len, "%s%s: ", first ? "" : ", ", &parm->namesym->namestr);
        first = 0;
        itypeSpellCat(buf, size, parm->vtype, 0);
    }
    len = strlen(buf);
    snprintf(buf + len, size - len, ")");
    if (sig->rettype != NULL && itypeGetTypeDcl(sig->rettype)->tag != VoidTag) {
        len = strlen(buf);
        snprintf(buf + len, size - len, " ");
        itypeSpellCat(buf, size, sig->rettype, 0);
    }
}

// Does the actor have each behaviour of the trait, 'pub', by its name, with the
// same parameter types and the same returned type? If not, say each that it does
// not at 'at'
static int actorTraitConforms(ActorTraitInfo *trait, ActorInfo *actor, INode *at) {
    char why[2400];
    why[0] = '\0';
    int bad = 0;
    for (uint32_t j = 0; j < trait->nmsgs; ++j) {
        FnDclNode *req = trait->msgs[j].method;
        fnCallDemandCandidates((INode *)req);
        char want[512];
        want[0] = '\0';
        actorSigCat(want, sizeof(want), req);
        size_t len = strlen(why);
        ActorMessage *have = actorMessageNamed(actor, req->namesym);
        if (have == NULL) {
            snprintf(why + len, sizeof(why) - len, " It has no behaviour 'pub async fn %s'.", want);
            ++bad;
            continue;
        }
        fnCallDemandCandidates((INode *)have->method);
        char has[512];
        has[0] = '\0';
        actorSigCat(has, sizeof(has), have->method);
        if (!(have->method->flags & FlagPub)) {
            snprintf(why + len, sizeof(why) - len, " Its behaviour %s is not 'pub', and a sender outside the actor reaches only a 'pub' one.", has);
            ++bad;
            continue;
        }
        FnSigNode *reqsig = (FnSigNode *)req->vtype;
        FnSigNode *havesig = (FnSigNode *)have->method->vtype;
        int same = reqsig->parms->used == havesig->parms->used && actorSameType(reqsig->rettype, havesig->rettype);
        for (uint32_t k = 1; same && k < reqsig->parms->used; ++k) {
            INode *a = ((VarDclNode *)nodesGet(reqsig->parms, k))->vtype;
            INode *b = ((VarDclNode *)nodesGet(havesig->parms, k))->vtype;
            same = a != NULL && b != NULL && itypeIsSame(a, b);
        }
        if (!same) {
            snprintf(why + len, sizeof(why) - len, " Its behaviour %s does not take and return what the trait's %s does.", has, want);
            ++bad;
        }
    }
    if (bad) {
        errorMsgNode(at, ErrorActorTraitConform,
            "Actor %s does not conform to actor trait %s, which it is converted to here:%s An actor conforms when it has every behaviour of the trait, declared 'pub', under the same name, with the same parameter types and the same returned type.",
            &actor->handle->namesym->namestr, &trait->handle->namesym->namestr, why);
        return 0;
    }
    return 1;
}

int actorTraitCoerce(TypeCheckState *pstate, INode **nodep, INode *totype) {
    INode *totypedcl = itypeGetTypeDcl(totype);
    ActorTraitInfo *trait = actorTraitOfHandle(totypedcl);
    INode *fromtype = iexpGetTypeDcl(*nodep);
    ActorInfo *actor = actorOfHandle(fromtype);
    INode *orig = *nodep;
    // The trait's handle reads the actor's mailbox from the place of the actor's
    // handle, which holds that and nothing else (parseactor.c)
    if (actor->handle->fields.used != 1)
        errorUnreachable(orig, "an actor's handle holds more than its mailbox, which an actor trait's handle reads it from");
    if (!actorTraitConforms(trait, actor, orig)) {
        // The conversion is built anyway, so what uses the value says nothing more
        INode *conv = (INode *)newConvCastNode(orig, totypedcl);
        inodeLexCopy(conv, orig);
        *nodep = conv;
        return 1;
    }
    // The handle is lent to 'wrap' for the call, and the virtual reference that
    // makes is what carries the table of the actor's sending methods. A value
    // that is no place is lent as a temporary, to the end of its statement
    INode *perm = newPermUseNode(roPerm);
    if (iexpIsLval(orig))
        borrowMutRef(nodep, fromtype, perm);
    else if (pstate != NULL && pstate->scope >= 2)
        borrowTempRef(nodep, fromtype, perm, borrowTempScope(pstate));
    else
        return 0;
    fnCallDemandCandidates((INode *)trait->wrap);
    FnCallNode *call = newFnCallLower(orig, newNameUseFromDclNode((INode *)trait->wrap, orig), 1);
    nodesAdd(&call->args, *nodep);
    fnCallFinalizeArgs(pstate, call);
    *nodep = (INode *)call;
    return 1;
}
