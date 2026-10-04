/** Handling for 'await' nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

// Create a new await node
AwaitNode *newAwaitNode() {
    AwaitNode *node;
    newNode(node, AwaitNode, AwaitTag);
    node->vtype = unknownType;
    node->exp = NULL;
    node->seamvars = NULL;
    node->nseamvars = 0;
    node->seamcap = 0;
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
