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

FnDclNode *actorRuntime[ActorRtCount];
char *actorRuntimeNames[ActorRtCount] = {
    "parkReserve", "parked", "unpark", "recordFree", "answerTo", "answered"
};

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
        || itypeGetTypeDcl(type)->tag == VoidTag || !itypeThreadBound(type, NULL))
        return;
    char typename[256] = "";
    itypeSpellCat(typename, sizeof(typename), type, 0);
    char what[512], reason[512];
    genericNotSendableWhy(type, what, reason);
    errorMsgNode(((FnSigNode *)fn->vtype)->rettype, ErrorNotSendable,
        "Actor %s's behaviour %s returns a %s, which is not Sendable: %s %s. The value goes back to the actor that awaits it, on the thread that runs that actor, so it must be Sendable.",
        &info->handle->namesym->namestr, &fn->namesym->namestr, typename, what, reason);
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
            if (!itypeThreadBound(type, NULL))
                continue;
            char typename[256] = "";
            itypeSpellCat(typename, sizeof(typename), type, 0);
            char what[512], reason[512];
            genericNotSendableWhy(type, what, reason);
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
    }
}

int actorIsState(INode *type) {
    return actorOfState(type) != NULL;
}

ActorInfo *actorOfState(INode *type) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i]->state == type)
            return actors[i];
    }
    return NULL;
}

ActorMessage *actorMessageOfSend(FnDclNode *send, ActorInfo **info) {
    INode *owner = inodeGetOwner((INode *)send);
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
