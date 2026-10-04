/** Actors: what an 'actor' declaration became
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

typedef struct ActorInfo {
    StructNode *handle;     // What the actor's name names
    StructNode *state;      // Its fields and methods, reached only by the runtime
    Nodes *crossing;        // FnDclNode, VarDclNode pairs: each argument that crosses to the actor
} ActorInfo;

static ActorInfo *actors = NULL;
static uint32_t actorCnt = 0;
static uint32_t actorMax = 0;

void actorRegister(StructNode *handle, StructNode *state, Nodes *crossing) {
    if (actorCnt == actorMax) {
        uint32_t newmax = actorMax ? actorMax * 2 : 16;
        ActorInfo *grown = (ActorInfo *)memAllocBlk(newmax * sizeof(ActorInfo));
        if (actorCnt)
            memcpy(grown, actors, actorCnt * sizeof(ActorInfo));
        actors = grown;
        actorMax = newmax;
    }
    ActorInfo *info = &actors[actorCnt++];
    info->handle = handle;
    info->state = state;
    info->crossing = crossing;
}

// A message carries its arguments to whichever worker thread runs the actor,
// and an initializer's arguments become the state, which the workers then run
// on: so each must be Sendable, as a channel's element or a thread's start
// value is. Asked here, of the parameters as written, rather than of the
// generated message enum: the enum declares Sendable on the strength of this
// check, so each refusal names the parameter, where the author can fix it.
void actorCheckAll() {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        ActorInfo *info = &actors[i];
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
                    "Actor %s's message %s takes %s, a %s, which is not Sendable: %s %s. A message carries its arguments to the thread the actor runs on, so each must be Sendable.",
                    &actorname->namestr, &fn->namesym->namestr, &parm->namesym->namestr, typename, what, reason);
        }
    }
}

int actorIsState(INode *type) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i].state == type)
            return 1;
    }
    return 0;
}

INode *actorStateMember(INode *type, Name *name, StructNode **state) {
    for (uint32_t i = 0; i < actorCnt; ++i) {
        if ((INode *)actors[i].handle != type)
            continue;
        INode *member = iNsTypeFindFnField((INsTypeNode *)actors[i].state, name);
        if (member == NULL || member->tag == StructTag)
            return NULL;
        *state = actors[i].state;
        return member;
    }
    return NULL;
}
