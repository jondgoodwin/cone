/** Generation of a split method: an actor's message holding an 'await'
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
 * Nothing waits for a reply yet (that is the envelope and the reply dispatch,
 * not built), so a method is split only under '--await-direct', which is for
 * tests: each seam hands its record straight to its second half, with the
 * value it awaited, before it returns. compiler/c/doc/phases/generation.md,
 * "A split method", is the note.
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
    default:
        return gen->flights[gen->flightbase + field->at].slot;
    }
}

// Lay a seam out, the first time a half generates it: what its record holds,
// in the order the values would die, and its second half, declared
static GenSeam *genlSeamLayout(GenState *gen, AwaitNode *node) {
    GenSeam *seam = (GenSeam *)memAllocBlk(sizeof(GenSeam));
    GenSeamField *fields = NULL;
    uint32_t cnt = 0, max = 0;
    GenSeamField field;
    memset(&field, 0, sizeof(field));

    // The values in flight across it, the newest first
    seam->nflights = gen->flightcnt - gen->flightbase;
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
    seam->half = LLVMAddFunction(gen->module, symbol, halftype);
    LLVMSetLinkage(seam->half, LLVMInternalLinkage);
    genlComdat(gen, seam->half);
    if (!gen->opt->release) {
        char *fnname = &fnnode->namesym->namestr;
        LLVMMetadataRef fntype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder, gen->difile, NULL, 0, 0);
        LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
            fnname, strlen(fnname), symbol, strlen(symbol),
            gen->difile, node->linenbr, fntype, 1, 1, node->linenbr, LLVMDIFlagPrivate, 0);
        LLVMSetSubprogram(seam->half, sp);
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
        LLVMValueRef val = LLVMBuildLoad2(gen->builder, genlSeamFieldType(gen, field), genlSeamFieldAt(gen, field), "tosave");
        record = LLVMBuildInsertValue(gen->builder, record, val, i, "record");
    }
    return record;
}

LLVMValueRef genlAwait(GenState *gen, AwaitNode *node) {
    // What is awaited runs before the seam
    LLVMValueRef value = genlExpr(gen, node->exp);
    if (gen->seams == NULL || node->seamno == 0 || node->seamno == UINT32_MAX) {
        errorUnreachable((INode *)node, "an 'await' generated in a method that is not split");
        return NULL;
    }
    GenSeam *seam = node->genseam;
    if (seam == NULL)
        seam = node->genseam = genlSeamLayout(gen, node);
    else if (seam->ntemps != gen->tempcnt - gen->tempbase || seam->nflights != gen->flightcnt - gen->flightbase) {
        errorUnreachable((INode *)node, "a seam generated in two halves with different temporaries or values in flight");
        return NULL;
    }
    int hasresult = genlSeamHasResult(node);

    // 1, 2: every borrow ends, a lock's guard giving its lock back; what the
    // ended borrows froze is free (nothing at run time)
    genlSeamGiveBack(gen, node);
    // 3: the record, every value still held moved in. 4: what is left is
    // nothing that does anything as it dies, so nothing is dropped
    LLVMValueRef record = seam->record ? genlSeamRecord(gen, seam) : NULL;
    // 5: the return to the dispatcher. Under '--await-direct', the only way a
    // split is generated yet, the record is first handed straight to the
    // second half, with what was awaited as the result
    VarDclNode *self = genlSplitSelf(gen->fndcl);
    LLVMValueRef args[3];
    unsigned nargs = 0;
    args[nargs++] = LLVMBuildLoad2(gen->builder, genlType(gen, self->vtype), self->llvmvar, "self");
    if (record)
        args[nargs++] = record;
    if (hasresult)
        args[nargs++] = value;
    LLVMBuildCall2(gen->builder, LLVMGlobalGetValueType(seam->half), seam->half, args, nargs, "");
    INode *rettype = ((FnSigNode *)itypeGetTypeDcl(gen->fndcl->vtype))->rettype;
    if (itypeGetTypeDcl(rettype)->tag != VoidTag) {
        errorUnreachable((INode *)node, "a split method that returns a value");
        return NULL;
    }
    genlFnDclReturn(gen, gen->fndcl, LLVMGetUndef(genlType(gen, rettype)));

    // What follows the seam: reached only in its second half, which enters here
    LLVMBasicBlockRef resume = genlInsertBlock(gen, "resume");
    LLVMPositionBuilderAtEnd(gen->builder, resume);
    if (gen->resumeat != node)
        return hasresult ? LLVMGetPoison(genlType(gen, node->vtype)) : LLVMGetUndef(gen->emptyStructType);
    gen->resumeblk = resume;
    gen->resumedest = (LLVMValueRef *)memAllocBlk((seam->nfields ? seam->nfields : 1) * sizeof(LLVMValueRef));
    for (uint32_t i = 0; i < seam->nfields; ++i)
        gen->resumedest[i] = genlSeamFieldAt(gen, &seam->fields[i]);
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
    for (uint32_t i = 0; i < seam->nfields; ++i)
        LLVMBuildStore(gen->builder, LLVMBuildExtractValue(gen->builder, record, i, "saved"), gen->resumedest[i]);
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

void genlSplitHalves(GenState *gen, FnDclNode *fnnode) {
    Nodes *seams = awaitSplitOf(fnnode);
    INode **nodesp;
    uint32_t cnt;
    // A seam the first half did not generate stands where this build generates
    // nothing (an arm a constant of the build rules out, genlIf): no half
    // reaches it, and it has none
    for (nodesFor(seams, cnt, nodesp)) {
        if (((AwaitNode *)*nodesp)->genseam)
            genlSplitHalf(gen, fnnode, (AwaitNode *)*nodesp);
    }
}
