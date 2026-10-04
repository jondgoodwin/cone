/** Generation of a split method: an actor's behaviour holding an 'await'
 * @file
 *
 * A seam ('await') is a return to the actor's dispatcher, not to the author:
 * the author wrote one method with one scope, so what the author can see --
 * when each value dies, and in which order -- is what the method would do if
 * it were not cut. So a split method is generated whole, more than once:
 *
 * - its first half is the method itself, entered as it always is. At each
 *   seam it gives back the locks whose borrows end there, builds the seam's
 *   record by moving into it every value the method still holds (flow's
 *   list, AwaitNode.seamvars, with the statement's temporaries and the values
 *   in flight across the seam), and returns. What follows the seam is
 *   generated into a block no path reaches;
 * - each seam's second half is the method again, as a function of its own
 *   whose entry moves the record's values back where they were and jumps to
 *   the block after that seam, the value awaited arriving as its result. What
 *   comes before the seam is generated too and reached by no path, unless a
 *   loop leads back to it. Every scope's end after the seam is the method's
 *   own, so each value dies where and in the order it would have.
 *
 * A second half is '<method>'<n>(self, record, result)': the record is one
 * struct, moved in, its values moved out into the locals they came from as it
 * begins; with nothing in it there is no record parameter, and an 'await'
 * whose value is void gives no result.
 *
 * A seam awaiting a message (AwaitNode.message) waits for its reply. What is
 * awaited is the handle's method sending the message awaited, whose last
 * argument is the request's envelope (genlAwaitReply): made as it is passed,
 * once the seam is laid out, it takes a slot in the actor's pending table for
 * a record that is not empty, and makes the reply's node, whose resume
 * message names the seam's resume function and the slot's id. The seam then
 * moves its record into the slot's block and returns. The reply's dispatch
 * calls the resume function (genlSeamResume), which takes the record out of
 * the table and calls the second half with it and the value returned; a
 * message that returns a value is answered there too, when its second half
 * returns one. The envelope such a message answers waits in its actor's state
 * while it runs (an Answer slot), and its seams carry it in their records.
 *
 * Under '--await-direct', which is for tests, a seam awaiting anything else
 * hands its record straight to its second half, with the value it awaited,
 * and returns what the second half returns. compiler/c/doc/phases/
 * generation.md, "A split method" and "A message's reply", is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
#include "genllvm.h"

#include <stdio.h>
#include <string.h>

// ---- Values in flight --------------------------------------------------------

int genlHasSeam(GenState *gen, INode *node) {
    return gen->seams != NULL && awaitWithin(node);
}

// A value a node generates that is a constant in every half alike, which
// need not be kept: it is the same after the seam
static int genlFlightConst(INode *node) {
    return node->tag == ULitTag || node->tag == FLitTag || node->tag == NilLitTag || node->tag == NullLitTag;
}

static LLVMValueRef genlFlightKeep(GenState *gen, LLVMValueRef val, INode *type) {
    LLVMValueRef slot = genlAlloca(gen, LLVMTypeOf(val), "flight");
    genlRootNote(gen, slot, type);
    LLVMBuildStore(gen->builder, val, slot);
    if (gen->flightcnt == gen->flightmax) {
        uint32_t newmax = gen->flightmax ? gen->flightmax * 2 : 16;
        GenFlight *flights = (GenFlight *)memAllocBlk(newmax * sizeof(GenFlight));
        if (gen->flightcnt)
            memcpy(flights, gen->flights, gen->flightcnt * sizeof(GenFlight));
        gen->flights = flights;
        gen->flightmax = newmax;
    }
    gen->flights[gen->flightcnt].slot = slot;
    gen->flights[gen->flightcnt++].type = type;
    return slot;
}

// Each node's value in order. Where a later node holds a seam, each value made
// before it -- 'x' in 'two(x, await give())' -- is no variable's, and is used
// after the seam: it waits in a slot of its own, which the seam's record
// carries, and is read back once the last of them is made
void genlExprsAcross(GenState *gen, Nodes *nodes, LLVMValueRef *vals) {
    uint32_t cnt = nodes->used;
    uint32_t across = 0;    // One past the last node holding a seam
    if (gen->seams) {
        for (uint32_t i = cnt; i > 0; --i) {
            if (awaitWithin(nodesGet(nodes, i - 1))) {
                across = i;
                break;
            }
        }
    }
    uint32_t mark = gen->flightcnt;
    LLVMValueRef *slots = across > 1 ? (LLVMValueRef *)memAllocBlk(across * sizeof(LLVMValueRef)) : NULL;
    for (uint32_t i = 0; i < cnt; ++i) {
        INode *node = nodesGet(nodes, i);
        vals[i] = genlExpr(gen, node);
        if (i + 1 < across)
            slots[i] = genlFlightConst(node) ? NULL : genlFlightKeep(gen, vals[i], ((IExpNode *)node)->vtype);
    }
    if (across > 1) {
        for (uint32_t i = 0; i + 1 < across; ++i) {
            if (slots[i])
                vals[i] = LLVMBuildLoad2(gen->builder, LLVMGetAllocatedType(slots[i]), slots[i], "inflight");
        }
        gen->flightcnt = mark;
    }
}

// ---- Lock guards a seam gives back -------------------------------------------

LLVMValueRef genlHeldBegin(GenState *gen) {
    LLVMTypeRef i8 = LLVMInt8TypeInContext(gen->context);
    LLVMValueRef held = genlAlloca(gen, i8, "lock.held");
    LLVMBuildStore(gen->builder, LLVMConstInt(i8, 1, 0), held);
    return held;
}

LLVMBasicBlockRef genlHeldIf(GenState *gen, LLVMValueRef held) {
    LLVMTypeRef i8 = LLVMInt8TypeInContext(gen->context);
    LLVMValueRef now = LLVMBuildLoad2(gen->builder, i8, held, "held");
    LLVMValueRef is = LLVMBuildICmp(gen->builder, LLVMIntNE, now, LLVMConstInt(i8, 0, 0), "isheld");
    LLVMBasicBlockRef doblk = genlInsertBlock(gen, "lockheld");
    LLVMBasicBlockRef endblk = genlInsertBlock(gen, "lockdone");
    LLVMMoveBasicBlockBefore(doblk, endblk);
    LLVMBuildCondBr(gen->builder, is, doblk, endblk);
    LLVMPositionBuilderAtEnd(gen->builder, doblk);
    return endblk;
}

// Is this variable the actor's 'self', which the dispatcher lends afresh to
// each half rather than the record carrying it?
static int genlSeamIsSelf(SeamVar *sv) {
    return (sv->flags & SeamParm) && sv->var->namesym == selfName;
}

// ---- The seam ----------------------------------------------------------------

// The method's 'self', its first parameter
static VarDclNode *genlSplitSelf(FnDclNode *fnnode) {
    return (VarDclNode *)nodesGet(((FnSigNode *)itypeGetTypeDcl(fnnode->vtype))->parms, 0);
}

// The actor whose method is being split
static ActorInfo *genlSplitActor(FnDclNode *fnnode) {
    return actorOfState(inodeGetOwner((INode *)fnnode));
}

// Does the split method answer a request through its actor's Answer slot,
// which its seams carry? A message returning a value does
static int genlSplitAnswers(FnDclNode *fnnode) {
    ActorInfo *info = genlSplitActor(fnnode);
    INode *rettype = itypeGetTypeDcl(((FnSigNode *)itypeGetTypeDcl(fnnode->vtype))->rettype);
    return info && info->answer && rettype->tag != VoidTag;
}

// The address of one of the state's hidden fields, from the state's address
static LLVMValueRef genlStateField(GenState *gen, ActorInfo *info, LLVMValueRef st, FieldDclNode *field) {
    return LLVMBuildStructGEP2(gen->builder, genlType(gen, (INode *)info->state), st, field->index,
        &field->namesym->namestr);
}

// The state's address, in the half being generated
static LLVMValueRef genlSplitState(GenState *gen) {
    VarDclNode *self = genlSplitSelf(gen->fndcl);
    return LLVMBuildLoad2(gen->builder, genlType(gen, self->vtype), self->llvmvar, "self");
}

// A call of a Cone function generation names itself: the actors package's,
// or one an actor's declaration generated
static LLVMValueRef genlCallFn(GenState *gen, FnDclNode *fn, LLVMValueRef *args, unsigned nargs, const char *name) {
    if (fn->llvmvar == NULL)
        genlGloFnName(gen, fn);
    return LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(fn->llvmvar), fn->llvmvar, args, nargs, name);
}

// Whether what a seam awaits gives a value, which its second half receives
static int genlSeamHasResult(AwaitNode *node) {
    INode *type = itypeGetTypeDcl(node->vtype);
    return type != unknownType && type->tag != VoidTag;
}

static void genlSeamField(GenSeamField **fields, uint32_t *cnt, uint32_t *max, GenSeamField *field) {
    if (*cnt == *max) {
        uint32_t newmax = *max ? *max * 2 : 8;
        GenSeamField *grown = (GenSeamField *)memAllocBlk(newmax * sizeof(GenSeamField));
        if (*cnt)
            memcpy(grown, *fields, *cnt * sizeof(GenSeamField));
        *fields = grown;
        *max = newmax;
    }
    (*fields)[(*cnt)++] = *field;
}

// The LLVM type of a record's field
static LLVMTypeRef genlSeamFieldType(GenState *gen, GenSeamField *field) {
    switch (field->kind) {
    case GenSeamFlag:
        return LLVMInt8TypeInContext(gen->context);
    case GenSeamFlight:
        return LLVMGetAllocatedType(gen->flights[gen->flightbase + field->at].slot);
    default:
        return genlType(gen, field->type);
    }
}

// Where a record's field is, in the half being generated
static LLVMValueRef genlSeamFieldAt(GenState *gen, GenSeamField *field) {
    switch (field->kind) {
    case GenSeamVar:
        return field->var->llvmvar;
    case GenSeamFlag:
        return field->var->llvmflag;
    case GenSeamTemp:
        return gen->temps[gen->tempbase + field->at].slot;
    case GenSeamAnswer:
    {
        ActorInfo *info = genlSplitActor(gen->fndcl);
        return genlStateField(gen, info, genlSplitState(gen), info->answer);
    }
    default:
        return gen->flights[gen->flightbase + field->at].slot;
    }
}

// A function a seam needs -- its second half, or its resume function --
// private to the object, its debug information the method's
static LLVMValueRef genlSeamFn(GenState *gen, char *symbol, LLVMTypeRef fntype, AwaitNode *node) {
    LLVMValueRef fn = LLVMAddFunction(gen->module, symbol, fntype);
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    genlComdat(gen, fn);
    if (!gen->opt->release) {
        char *fnname = &gen->fndcl->namesym->namestr;
        LLVMMetadataRef ditype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder, gen->difile, NULL, 0, 0);
        LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
            fnname, strlen(fnname), symbol, strlen(symbol),
            gen->difile, node->linenbr, ditype, 1, 1, node->linenbr, LLVMDIFlagPrivate, 0);
        LLVMSetSubprogram(fn, sp);
    }
    return fn;
}

// Lay a seam out, the first time a half generates it: what its record holds,
// in the order the values would die, and its second half, declared; and,
// where it awaits a message, its resume function. 'nflights' values are in
// flight across it
static GenSeam *genlSeamLayout(GenState *gen, AwaitNode *node, uint32_t nflights) {
    GenSeam *seam = (GenSeam *)memAllocBlk(sizeof(GenSeam));
    GenSeamField *fields = NULL;
    uint32_t cnt = 0, max = 0;
    GenSeamField field;
    memset(&field, 0, sizeof(field));

    // The request's envelope a message returning a value answers, which no
    // author's value is: it travels with the continuation, to whichever half
    // returns the value
    if (genlSplitAnswers(gen->fndcl)) {
        field.kind = GenSeamAnswer;
        field.type = genlSplitActor(gen->fndcl)->answer->vtype;
        genlSeamField(&fields, &cnt, &max, &field);
    }
    memset(&field, 0, sizeof(field));

    // The values in flight across it, the newest first
    seam->nflights = nflights;
    for (uint32_t i = seam->nflights; i > 0; --i) {
        field.kind = GenSeamFlight;
        field.at = i - 1;
        field.type = gen->flights[gen->flightbase + i - 1].type;
        genlSeamField(&fields, &cnt, &max, &field);
    }
    memset(&field, 0, sizeof(field));
    // The temporaries waiting for their statement's end, the newest first: each
    // that does something as it dies, but a lock's guard, which gives its lock
    // back at the seam instead
    seam->ntemps = gen->tempcnt - gen->tempbase;
    for (uint32_t i = seam->ntemps; i > 0; --i) {
        GenTemp *entry = &gen->temps[gen->tempbase + i - 1];
        if (entry->held || !itypeNeedsFinal(entry->temp->vtype))
            continue;
        field.kind = GenSeamTemp;
        field.at = i - 1;
        field.temp = entry->temp;
        field.type = entry->temp->vtype;
        genlSeamField(&fields, &cnt, &max, &field);
    }
    memset(&field, 0, sizeof(field));
    // The variables, the last declared first: each holding its value that is
    // used after the seam or does something as it dies (flow's record, but for
    // 'self', which is lent afresh, and a lock's guard, given back). A variable
    // whose value differs by path brings its drop flag
    for (uint32_t i = node->nseamvars; i > 0; --i) {
        SeamVar *sv = &node->seamvars[i - 1];
        if (genlSeamIsSelf(sv) || (sv->flags & (SeamTemp | SeamGuard)) || !(sv->flags & SeamOpen)
            || !(sv->flags & (SeamLive | SeamDies)))
            continue;
        field.kind = GenSeamVar;
        field.var = sv->var;
        field.type = sv->var->vtype;
        genlSeamField(&fields, &cnt, &max, &field);
        if (sv->var->flowtempflags & VarDropFlag) {
            field.kind = GenSeamFlag;
            field.type = NULL;
            genlSeamField(&fields, &cnt, &max, &field);
        }
    }
    seam->fields = fields;
    seam->nfields = cnt;
    // A record parked in the pending table is off the stack, where the
    // collector's roots are not; flow refused a variable holding a traced
    // reference there (awaitRecordTraced), and here is a value in flight or a
    // temporary holding one
    for (uint32_t i = 0; node->message && i < cnt; ++i) {
        if ((fields[i].kind == GenSeamFlight || fields[i].kind == GenSeamTemp) && itypeHoldsTraced(fields[i].type))
            errorMsgNode((INode *)node, ErrorUnbuiltAwait,
                "'await' whose continuation holds a traced reference is not built: a value made before it and used after would wait in the actor's pending table, which the collector does not trace yet.");
    }

    // The second half: '<method>'<n>', private to the object
    FnDclNode *fnnode = gen->fndcl;
    size_t namelen;
    const char *method = LLVMGetValueName2(fnnode->llvmvar, &namelen);
    char symbol[2100];
    snprintf(symbol, sizeof(symbol), "%.*s'%u", (int)namelen, method, node->seamno);
    if (cnt > 0) {
        char recname[2120];
        snprintf(recname, sizeof(recname), "%s.record", symbol);
        LLVMTypeRef *ftypes = (LLVMTypeRef *)memAllocBlk(cnt * sizeof(LLVMTypeRef));
        for (uint32_t i = 0; i < cnt; ++i)
            ftypes[i] = genlSeamFieldType(gen, &fields[i]);
        seam->record = LLVMStructCreateNamed(gen->context, recname);
        LLVMStructSetBody(seam->record, ftypes, cnt, 0);
    }
    else
        seam->record = NULL;
    LLVMTypeRef parms[3];
    unsigned nparms = 0;
    parms[nparms++] = genlType(gen, genlSplitSelf(fnnode)->vtype);
    if (seam->record)
        parms[nparms++] = seam->record;
    if (genlSeamHasResult(node))
        parms[nparms++] = genlType(gen, node->vtype);
    INode *rettype = ((FnSigNode *)itypeGetTypeDcl(fnnode->vtype))->rettype;
    LLVMTypeRef halftype = LLVMFunctionType(genlType(gen, rettype), parms, nparms, 0);
    seam->half = genlSeamFn(gen, symbol, halftype, node);

    // Its resume function, '<method>'<n>.resume(state, [id,] data)': the id
    // only where the record is not empty, which it is the pending table's
    // key for (the actors package's resume and resumeId)
    seam->resume = NULL;
    if (node->message) {
        char resumename[2120];
        snprintf(resumename, sizeof(resumename), "%s.resume", symbol);
        LLVMTypeRef ptr = LLVMPointerTypeInContext(gen->context, 0);
        LLVMTypeRef rparms[3] = {ptr, LLVMInt64TypeInContext(gen->context), ptr};
        if (seam->record == NULL)
            rparms[1] = ptr;
        LLVMTypeRef resumetype = LLVMFunctionType(LLVMVoidTypeInContext(gen->context), rparms, seam->record ? 3 : 2, 0);
        seam->resume = genlSeamFn(gen, resumename, resumetype, node);
    }
    return seam;
}

// Step 1 of a seam: every borrow ends there, and a lock's guard gives its lock
// back as its borrow ends -- a local's (VarSeamHeld) and a temporary's alike.
// Its flag says so after, for the scope's end the code after the seam reaches
static void genlSeamGiveBack(GenState *gen, AwaitNode *node) {
    for (uint32_t i = 0; i < node->nseamvars; ++i) {
        SeamVar *sv = &node->seamvars[i];
        if (!(sv->flags & SeamGuard) || (sv->flags & SeamTemp) || !(sv->flags & SeamOpen))
            continue;
        VarDclNode *var = sv->var;
        if (var->llvmflag == NULL) {
            errorUnreachable((INode *)var, "a lock's guard at a seam that was given no flag");
            return;
        }
        LLVMBasicBlockRef endblk = genlDropFlagIf(gen, var, DropFlagWhole);
        genlFinalizeAt(gen, var->llvmvar, var->vtype);
        genlDropFlagEnd(gen, endblk);
        genlDropFlagSet(gen, var, DropFlagEmpty);
    }
    LLVMTypeRef i8 = LLVMInt8TypeInContext(gen->context);
    for (uint32_t i = gen->tempcnt; i > gen->tempbase; --i) {
        GenTemp entry = gen->temps[i - 1];
        if (entry.held == NULL)
            continue;
        genlTempRelease(gen, &entry);
        LLVMBuildStore(gen->builder, LLVMConstInt(i8, 0, 0), entry.held);
    }
}

// The record, each value moved into it: nothing in this half touches the
// places it was read from again
static LLVMValueRef genlSeamRecord(GenState *gen, GenSeam *seam) {
    LLVMValueRef record = LLVMGetUndef(seam->record);
    for (uint32_t i = 0; i < seam->nfields; ++i) {
        GenSeamField *field = &seam->fields[i];
        LLVMTypeRef type = genlSeamFieldType(gen, field);
        LLVMValueRef at = genlSeamFieldAt(gen, field);
        LLVMValueRef val = LLVMBuildLoad2(gen->builder, type, at, "tosave");
        record = LLVMBuildInsertValue(gen->builder, record, val, i, "record");
        // The Answer slot is the state's: left empty, the envelope gone with
        // the continuation, so that the dispatcher sees the method parked
        if (field->kind == GenSeamAnswer)
            LLVMBuildStore(gen->builder, LLVMConstNull(type), at);
    }
    return record;
}

// The envelope of the request a message 'await' sends, the last argument of
// the handle's method that sends it awaited. The seam is laid out here, as
// the record's id is needed in the request before the seam is reached: what
// is in flight across it are the values in flight as its 'await' began, and
// its temporaries are all made by now. A record that is not empty gets a
// slot in the pending table, its id; the envelope's reply node names the
// seam's resume function and that id (the actor's generated reply' and
// replyid' functions). The reply comes only once this method has returned to
// the dispatcher, which runs its actor's messages one at a time, so the
// record is in its slot by then
LLVMValueRef genlAwaitReply(GenState *gen, AwaitReplyNode *node) {
    AwaitNode *await = node->await;
    if (gen->seams == NULL || await->seamno == 0 || await->seamno == UINT32_MAX || await->message == NULL) {
        errorUnreachable((INode *)node, "a request's envelope generated where no message seam is split");
        return NULL;
    }
    GenSeam *seam = await->genseam;
    if (seam == NULL)
        seam = await->genseam = genlSeamLayout(gen, await, gen->awaitflights - gen->flightbase);
    ActorInfo *info = genlSplitActor(gen->fndcl);
    LLVMValueRef st = genlSplitState(gen);
    LLVMTypeRef usize = genlUsize(gen);
    LLVMTypeRef rtype = genlType(gen, await->vtype);
    LLVMValueRef size = LLVMConstInt(usize, LLVMABISizeOfType(gen->datalayout, rtype), 0);
    LLVMValueRef align = LLVMConstInt(usize, LLVMABIAlignmentOfType(gen->datalayout, rtype), 0);
    LLVMValueRef args[5];
    unsigned nargs = 0;
    args[nargs++] = st;
    args[nargs++] = seam->resume;
    if (seam->record) {
        LLVMValueRef rargs[2];
        rargs[0] = genlStateField(gen, info, st, info->pending);
        rargs[1] = LLVMConstInt(usize, LLVMABISizeOfType(gen->datalayout, seam->record), 0);
        gen->awaitid = genlCallFn(gen, actorRuntime[ActorRtParkReserve], rargs, 2, "id");
        args[nargs++] = gen->awaitid;
    }
    args[nargs++] = size;
    args[nargs++] = align;
    return genlCallFn(gen, seam->record ? info->replyidfn : info->replyfn, args, nargs, "reply");
}

LLVMValueRef genlAwait(GenState *gen, AwaitNode *node) {
    // What is awaited runs before the seam
    uint32_t svflights = gen->awaitflights;
    LLVMValueRef svid = gen->awaitid;
    gen->awaitflights = gen->flightcnt;
    gen->awaitid = NULL;
    LLVMValueRef value = genlExpr(gen, node->exp);
    LLVMValueRef id = gen->awaitid;
    gen->awaitflights = svflights;
    gen->awaitid = svid;
    if (gen->seams == NULL || node->seamno == 0 || node->seamno == UINT32_MAX) {
        errorUnreachable((INode *)node, "an 'await' generated in a method that is not split");
        return NULL;
    }
    GenSeam *seam = node->genseam;
    if (seam == NULL)
        seam = node->genseam = genlSeamLayout(gen, node, gen->flightcnt - gen->flightbase);
    else if (seam->ntemps != gen->tempcnt - gen->tempbase || seam->nflights != gen->flightcnt - gen->flightbase) {
        errorUnreachable((INode *)node, "a seam generated in two halves, or laid out by its envelope, with different temporaries or values in flight");
        return NULL;
    }
    int hasresult = genlSeamHasResult(node);

    // 1, 2: every borrow ends, a lock's guard giving its lock back; what the
    // ended borrows froze is free (nothing at run time)
    genlSeamGiveBack(gen, node);
    // 3: the record, every value still held moved in. 4: what is left is
    // nothing that does anything as it dies, so nothing is dropped
    LLVMValueRef record = seam->record ? genlSeamRecord(gen, seam) : NULL;
    // 5: the return to the dispatcher. Awaiting a message, the record is
    // parked in the block its slot in the pending table holds, and the method
    // returns no value: a seam took the envelope it answers, if it answers
    // one. Under '--await-direct', the record is first handed straight to the
    // second half, with what was awaited as the result, and what the second
    // half returns is returned
    INode *rettype = ((FnSigNode *)itypeGetTypeDcl(gen->fndcl->vtype))->rettype;
    LLVMValueRef retval = LLVMGetUndef(genlType(gen, rettype));
    if (node->message) {
        if (record) {
            if (id == NULL) {
                errorUnreachable((INode *)node, "a message seam with a record whose envelope reserved no slot");
                return NULL;
            }
            ActorInfo *info = genlSplitActor(gen->fndcl);
            LLVMValueRef pargs[2] = {genlStateField(gen, info, genlSplitState(gen), info->pending), id};
            LLVMValueRef block = genlCallFn(gen, actorRuntime[ActorRtParked], pargs, 2, "parked");
            LLVMBuildStore(gen->builder, record, block);
        }
    }
    else {
        LLVMValueRef args[3];
        unsigned nargs = 0;
        args[nargs++] = genlSplitState(gen);
        if (record)
            args[nargs++] = record;
        if (hasresult)
            args[nargs++] = value;
        LLVMValueRef ret = LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(seam->half), seam->half, args, nargs, "");
        if (itypeGetTypeDcl(rettype)->tag != VoidTag)
            retval = ret;
    }
    genlFnDclReturn(gen, gen->fndcl, retval);

    // What follows the seam: reached only in its second half, which enters here
    LLVMBasicBlockRef resume = genlInsertBlock(gen, "resume");
    LLVMPositionBuilderAtEnd(gen->builder, resume);
    if (gen->resumeat != node)
        return hasresult ? LLVMGetPoison(genlType(gen, node->vtype)) : LLVMGetUndef(gen->emptyStructType);
    gen->resumeblk = resume;
    // Where each of the record's values goes, but the Answer slot's, which
    // the entry finds itself
    gen->resumedest = (LLVMValueRef *)memAllocBlk((seam->nfields ? seam->nfields : 1) * sizeof(LLVMValueRef));
    for (uint32_t i = 0; i < seam->nfields; ++i)
        gen->resumedest[i] = seam->fields[i].kind == GenSeamAnswer ? NULL : genlSeamFieldAt(gen, &seam->fields[i]);
    gen->resumeheldcnt = 0;
    gen->resumeheld = (LLVMValueRef *)memAllocBlk((seam->ntemps ? seam->ntemps : 1) * sizeof(LLVMValueRef));
    for (uint32_t i = gen->tempbase; i < gen->tempcnt; ++i) {
        if (gen->temps[i].held)
            gen->resumeheld[gen->resumeheldcnt++] = gen->temps[i].held;
    }
    return hasresult ? LLVMGetParam(gen->fn, seam->record ? 2 : 1) : LLVMGetUndef(gen->emptyStructType);
}

// ---- The second halves -------------------------------------------------------

// The entry of a second half: the record's values moved back where they were;
// every drop flag of a variable in scope at the seam that the record does not
// carry says it holds nothing, a lock's guard's that it holds no lock; then
// on to just after the seam
static void genlSeamEntry(GenState *gen, AwaitNode *node, GenSeam *seam) {
    LLVMTypeRef i8 = LLVMInt8TypeInContext(gen->context);
    LLVMValueRef record = seam->record ? LLVMGetParam(gen->fn, 1) : NULL;
    for (uint32_t i = 0; i < seam->nfields; ++i) {
        LLVMValueRef dest = gen->resumedest[i] ? gen->resumedest[i] : genlSeamFieldAt(gen, &seam->fields[i]);
        LLVMBuildStore(gen->builder, LLVMBuildExtractValue(gen->builder, record, i, "saved"), dest);
    }
    for (uint32_t i = 0; i < node->nseamvars; ++i) {
        VarDclNode *var = node->seamvars[i].var;
        if (var->llvmflag == NULL || genlSeamIsSelf(&node->seamvars[i]))
            continue;
        int carried = 0;
        for (uint32_t f = 0; f < seam->nfields; ++f)
            carried |= seam->fields[f].kind == GenSeamFlag && seam->fields[f].var == var;
        if (!carried)
            LLVMBuildStore(gen->builder, LLVMConstInt(i8, DropFlagEmpty, 0), var->llvmflag);
    }
    for (uint32_t i = 0; i < gen->resumeheldcnt; ++i)
        LLVMBuildStore(gen->builder, LLVMConstInt(i8, 0, 0), gen->resumeheld[i]);
    LLVMBuildBr(gen->builder, gen->resumeblk);
}

// One second half: the method generated again, entered just after the seam
static void genlSplitHalf(GenState *gen, FnDclNode *fnnode, AwaitNode *node) {
    GenSeam *seam = node->genseam;
    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    INode *svfnblock = gen->fnblock;
    FnDclNode *svfndcl = gen->fndcl;
    int svexitzero = gen->exitzero;
    GenRoots svroots;
    genlRootsSave(gen, &svroots);
    uint32_t svtempbase = gen->tempbase;
    gen->tempbase = gen->tempcnt;
    Nodes *svseams = gen->seams;
    AwaitNode *svresumeat = gen->resumeat;
    uint32_t svflightbase = gen->flightbase;
    gen->flightbase = gen->flightcnt;

    gen->fn = seam->half;
    gen->fnblock = fnnode->value;
    gen->fndcl = fnnode;
    gen->exitzero = 0;
    gen->seams = awaitSplitOf(fnnode);
    gen->resumeat = node;
    gen->resumeblk = NULL;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(gen->builder, entry);
    LLVMValueRef allocaPoint = LLVMBuildAlloca(gen->builder, LLVMInt32TypeInContext(gen->context), "alloca_point");
    gen->allocaPoint = allocaPoint;

    // The method's parameters: 'self' is the dispatcher's, lent afresh; any
    // other is a local the record fills, or one never used again
    FnSigNode *fnsig = (FnSigNode *)itypeGetTypeDcl(fnnode->vtype);
    uint32_t cnt;
    INode **nodesp;
    int first = 1;
    for (nodesFor(fnsig->parms, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode *)*nodesp;
        var->llvmvar = genlAlloca(gen, genlType(gen, var->vtype), &var->namesym->namestr);
        genlRootNote(gen, var->llvmvar, var->vtype);
        if (first)
            LLVMBuildStore(gen->builder, LLVMGetParam(gen->fn, 0), var->llvmvar);
        genlDropFlagBegin(gen, var, first ? DropFlagWhole : DropFlagEmpty);
        first = 0;
    }

    // The method's body, in a block of its own that no path reaches but a loop
    // leading back to it
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "body");
    LLVMPositionBuilderAtEnd(gen->builder, body);
    genlBlock(gen, (BlockNode *)fnnode->value);
    if (gen->resumeblk == NULL)
        errorUnreachable((INode *)node, "a second half whose seam its method's body never generated");
    else {
        LLVMPositionBuilderAtEnd(gen->builder, entry);
        genlSeamEntry(gen, node, seam);
    }

    genlRootFrame(gen);
    if (LLVMGetInstructionParent(allocaPoint))
        LLVMInstructionEraseFromParent(allocaPoint);
    LLVMDisposeBuilder(gen->builder);

    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fnblock = svfnblock;
    gen->fndcl = svfndcl;
    gen->exitzero = svexitzero;
    gen->tempcnt = gen->tempbase;
    gen->tempbase = svtempbase;
    genlRootsRestore(gen, &svroots);
    gen->seams = svseams;
    gen->resumeat = svresumeat;
    gen->resumeblk = NULL;
    gen->flightcnt = gen->flightbase;
    gen->flightbase = svflightbase;
}

// ---- A message's reply -------------------------------------------------------

// A seam's resume function, '<method>'<n>.resume(state, [id,] data)', which
// the dispatch calls when the reply to its request arrives, with where the
// value returned is in the reply's node: the record taken out of the pending
// table at id, its block freed, and the second half called with the record
// and the value, both moved. Where the method returns a value, the second
// half's is its answer: unless a later seam parked the method again, it goes
// back in the reply the Answer slot holds (answerTo), or is dropped where the
// message was sent with no 'await'
static void genlSeamResume(GenState *gen, FnDclNode *fnnode, AwaitNode *node) {
    GenSeam *seam = node->genseam;
    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    FnDclNode *svfndcl = gen->fndcl;
    uint32_t svtempbase = gen->tempbase;
    gen->tempbase = gen->tempcnt;
    GenRoots svroots;
    genlRootsSave(gen, &svroots);

    gen->fn = seam->resume;
    gen->fndcl = fnnode;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "entry");
    gen->builder = LLVMCreateBuilderInContext(gen->context);
    LLVMPositionBuilderAtEnd(gen->builder, entry);
    LLVMValueRef allocaPoint = LLVMBuildAlloca(gen->builder, LLVMInt32TypeInContext(gen->context), "alloca_point");
    gen->allocaPoint = allocaPoint;
    if (!gen->opt->release)
        LLVMSetCurrentDebugLocation2(gen->builder,
            LLVMDIBuilderCreateDebugLocation(gen->context, node->linenbr, 0, LLVMGetSubprogram(gen->fn), NULL));

    ActorInfo *info = genlSplitActor(fnnode);
    LLVMValueRef st = LLVMGetParam(gen->fn, 0);
    LLVMValueRef data = LLVMGetParam(gen->fn, seam->record ? 2 : 1);
    LLVMValueRef args[3];
    unsigned nargs = 0;
    args[nargs++] = st;
    if (seam->record) {
        LLVMValueRef uargs[2] = {genlStateField(gen, info, st, info->pending), LLVMGetParam(gen->fn, 1)};
        LLVMValueRef block = genlCallFn(gen, actorRuntime[ActorRtUnpark], uargs, 2, "block");
        args[nargs++] = LLVMBuildLoad2(gen->builder, seam->record, block, "record");
        genlCallFn(gen, actorRuntime[ActorRtRecordFree], &block, 1, "");
    }
    if (genlSeamHasResult(node))
        args[nargs++] = LLVMBuildLoad2(gen->builder, genlType(gen, node->vtype), data, "returned");
    LLVMValueRef ret = LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(seam->half), seam->half, args, nargs, "");

    if (genlSplitAnswers(fnnode)) {
        INode *rettype = ((FnSigNode *)itypeGetTypeDcl(fnnode->vtype))->rettype;
        LLVMValueRef slot = genlStateField(gen, info, st, info->answer);
        LLVMValueRef to = genlCallFn(gen, actorRuntime[ActorRtAnswerTo], &slot, 1, "answerto");
        LLVMTypeRef usize = genlUsize(gen);
        LLVMValueRef toint = LLVMBuildPtrToInt(gen->builder, to, usize, "");
        LLVMBasicBlockRef answer = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "answer");
        LLVMBasicBlockRef discard = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "discard");
        LLVMBasicBlockRef reply = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "reply");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "done");
        // Parked again by a later seam: no value
        LLVMBuildCondBr(gen->builder, LLVMBuildICmp(gen->builder, LLVMIntEQ, toint, LLVMConstInt(usize, 0, 0), ""), done, answer);
        LLVMPositionBuilderAtEnd(gen->builder, answer);
        LLVMBuildCondBr(gen->builder, LLVMBuildICmp(gen->builder, LLVMIntEQ, toint, LLVMConstInt(usize, 1, 0), ""), discard, reply);
        // Nobody awaits it: it is dropped
        LLVMPositionBuilderAtEnd(gen->builder, discard);
        LLVMValueRef kept = genlAlloca(gen, genlType(gen, rettype), "answer");
        LLVMBuildStore(gen->builder, ret, kept);
        genlFinalizeAt(gen, kept, rettype);
        genlCallFn(gen, actorRuntime[ActorRtAnswered], &slot, 1, "");
        LLVMBuildBr(gen->builder, done);
        // It goes back in the reply, moved
        LLVMPositionBuilderAtEnd(gen->builder, reply);
        LLVMBuildStore(gen->builder, ret, to);
        genlCallFn(gen, actorRuntime[ActorRtAnswered], &slot, 1, "");
        LLVMBuildBr(gen->builder, done);
        LLVMPositionBuilderAtEnd(gen->builder, done);
    }
    LLVMBuildRetVoid(gen->builder);

    genlRootFrame(gen);
    if (LLVMGetInstructionParent(allocaPoint))
        LLVMInstructionEraseFromParent(allocaPoint);
    LLVMDisposeBuilder(gen->builder);
    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->fndcl = svfndcl;
    gen->tempcnt = gen->tempbase;
    gen->tempbase = svtempbase;
    genlRootsRestore(gen, &svroots);
}

void genlSplitHalves(GenState *gen, FnDclNode *fnnode) {
    Nodes *seams = awaitSplitOf(fnnode);
    INode **nodesp;
    uint32_t cnt;
    // A seam the first half did not generate stands where this build generates
    // nothing (an arm a constant of the build rules out, genlIf): no half
    // reaches it, and it has none
    for (nodesFor(seams, cnt, nodesp)) {
        AwaitNode *node = (AwaitNode *)*nodesp;
        if (node->genseam == NULL)
            continue;
        genlSplitHalf(gen, fnnode, node);
        if (node->genseam->resume)
            genlSeamResume(gen, fnnode, node);
    }
}
