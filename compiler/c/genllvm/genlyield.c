/** Generation of a generator: a function declared 'yields'
 *
 * A generator (parser/parsegen.c) is a struct holding its parameters, with a
 * method 'next' whose body is the author's. Calling 'next' runs the body from
 * where the last call left it up to its next 'yield', or its end, and gives
 * 'Some(value)' or 'None'. The body is generated as one function, entered
 * through a switch on the struct's state field:
 *
 *   state 0         the body from its start (the generator was just made)
 *   state k         the body just after its k-th 'yield', in the order written
 *   GenDone         the body has ended: 'None' again, running nothing
 *
 * A 'yield' stores its number in the state and returns the 'Some(value)' it
 * built; its seam adds a case to the entry switch that goes to the block after
 * it. That is the split 'await' makes (genlawait.c) without its halves: nothing
 * is moved into a record. The locals that live across a seam -- those the loan
 * walk noted as used after one or as doing something as they die, at any seam
 * (yield.h, SeamFlags) -- are the generator's frame, hidden storage that
 * follows its parameters in the struct's layout (genlGenFrameType). A frame
 * local is generated where it is declared, but its storage is the frame's,
 * found at the function's entry, so that every path that resumes the body
 * reaches it. So are the drop flags of the frame's locals; the flag of any
 * other local in scope at a seam says it holds nothing where the body resumes,
 * which the case for the seam stores.
 *
 * A generator that holds a generator of its own kind, as a recursive walk does
 * ('yield each walk(t.left)' keeps the sub-generator in a local), would be of
 * unknowable size, so that local, wherever it makes the struct hold itself, is
 * kept on the heap: the frame holds its address, made when the body first runs
 * and freed when the generator dies.
 *
 * A generator dying with its body suspended (genlGenDrop) finalizes what its
 * frame holds at the seam it was left at, newest first, and what the body ended
 * with has been finalized by its own scopes ending; its parameters are fields
 * and die after, with the struct's own.
 *
 * compiler/c/doc/phases/generation.md, "A generator", is the note.
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

// ---- The frame's plan --------------------------------------------------------

static int genlGenIndex(GenInfo *info, VarDclNode *var) {
    for (uint32_t i = 0; i < info->nframe; ++i) {
        if (info->frame[i] == var)
            return (int)i;
    }
    return -1;
}

static GenInfo *genlGenOfType(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    return dcl->tag == StructTag ? yieldGenOfStruct(dcl) : NULL;
}

static void genlGenPlan(GenState *gen, GenInfo *info);

// Does the generator 'outer' hold the generator 'target' inline in its frame,
// itself or by way of another it holds? Asked of a generator whose frame is planned
static int genlGenHolds(GenInfo *outer, StructNode *target) {
    for (uint32_t i = 0; i < outer->nframe; ++i) {
        if (outer->kind[i] != GenFrameInline)
            continue;
        GenInfo *sub = genlGenOfType(outer->frame[i]->vtype);
        if (sub == NULL)
            continue;
        if (sub->gen == target || (sub->framed == 2 && genlGenHolds(sub, target)))
            return 1;
    }
    return 0;
}

// Is the local 'var' of the generator 'info' of a kind that would make the
// generator hold itself: its own, or one that holds it in its frame?
static int genlGenSelfHolding(GenState *gen, GenInfo *info, VarDclNode *var) {
    GenInfo *sub = genlGenOfType(var->vtype);
    if (sub == NULL)
        return 0;
    if (sub == info)
        return 1;
    genlGenPlan(gen, sub);
    return sub->framed == 1 || genlGenHolds(sub, info->gen);
}

// What a seam keeps of its variables: those that hold their value and are used
// after it or do something as they die. Planned once, from every seam
static void genlGenPlan(GenState *gen, GenInfo *info) {
    if (info->framed)
        return;
    info->framed = 1;

    // The variables, each once, in the order first met
    Nodes *vars = newNodes(8);
    Nodes *flagonly = newNodes(4);
    if (info->yields) {
        INode **yp;
        uint32_t ycnt;
        for (nodesFor(info->yields, ycnt, yp)) {
            YieldNode *yield = (YieldNode *)*yp;
            for (uint32_t i = 0; i < yield->nseamvars; ++i) {
                SeamVar *sv = &yield->seamvars[i];
                if (sv->flags & (SeamTemp | SeamGuard | SeamParm))
                    continue;
                if (!(sv->flags & SeamOpen) || !(sv->flags & (SeamLive | SeamDies)))
                    continue;
                VarDclNode *var = sv->var;
                // A match's binding in the matched value's own storage has
                // none of its own: the variable that owns the value keeps it
                if (flowMatchInPlace(var)) {
                    VarDclNode *owner = flowDropOwner(var);
                    int have = 0;
                    INode **np;
                    uint32_t ncnt;
                    for (nodesFor(vars, ncnt, np))
                        have |= *np == (INode *)owner;
                    if (!have)
                        nodesAdd(&vars, (INode *)owner);
                    if (var->flowtempflags & VarDropFlag) {
                        have = 0;
                        for (nodesFor(flagonly, ncnt, np))
                            have |= *np == (INode *)var;
                        if (!have)
                            nodesAdd(&flagonly, (INode *)var);
                    }
                    continue;
                }
                int have = 0;
                INode **np;
                uint32_t ncnt;
                for (nodesFor(vars, ncnt, np))
                    have |= *np == (INode *)var;
                if (!have)
                    nodesAdd(&vars, (INode *)var);
            }
        }
    }

    uint32_t n = vars->used + flagonly->used;
    info->frame = (VarDclNode **)memAllocBlk((n ? n : 1) * sizeof(VarDclNode *));
    info->kind = (uint8_t *)memAllocBlk(n ? n : 1);
    info->valueat = (int32_t *)memAllocBlk((n ? n : 1) * sizeof(int32_t));
    info->flagat = (int32_t *)memAllocBlk((n ? n : 1) * sizeof(int32_t));
    info->nframe = 0;
    info->nelems = 0;
    INode **np;
    uint32_t ncnt;
    for (nodesFor(vars, ncnt, np)) {
        VarDclNode *var = (VarDclNode *)*np;
        uint32_t k = info->nframe++;
        info->frame[k] = var;
        info->kind[k] = genlGenSelfHolding(gen, info, var) ? GenFrameBoxed : GenFrameInline;
        info->valueat[k] = (int32_t)info->nelems++;
        info->flagat[k] = (var->flowtempflags & VarDropFlag) ? (int32_t)info->nelems++ : -1;
    }
    for (nodesFor(flagonly, ncnt, np)) {
        VarDclNode *var = (VarDclNode *)*np;
        uint32_t k = info->nframe++;
        info->frame[k] = var;
        info->kind[k] = GenFrameFlag;
        info->valueat[k] = -1;
        info->flagat[k] = (int32_t)info->nelems++;
    }
    info->framed = 2;
}

LLVMTypeRef genlGenFrameType(GenState *gen, StructNode *strnode) {
    if (!yieldAny())
        return NULL;
    GenInfo *info = yieldGenOfStruct((INode *)strnode);
    if (info == NULL)
        return NULL;
    if (info->framellvm)
        return (LLVMTypeRef)info->framellvm;
    genlGenPlan(gen, info);
    if (info->nelems == 0)
        return NULL;
    LLVMTypeRef *elems = (LLVMTypeRef *)memAllocBlk(info->nelems * sizeof(LLVMTypeRef));
    for (uint32_t k = 0; k < info->nframe; ++k) {
        if (info->kind[k] == GenFrameInline)
            elems[info->valueat[k]] = genlType(gen, info->frame[k]->vtype);
        else if (info->kind[k] == GenFrameBoxed)
            elems[info->valueat[k]] = LLVMPointerTypeInContext(gen->context, 0);
        if (info->flagat[k] >= 0)
            elems[info->flagat[k]] = LLVMInt8TypeInContext(gen->context);
    }
    info->framellvm = LLVMStructTypeInContext(gen->context, elems, info->nelems, 0);
    return (LLVMTypeRef)info->framellvm;
}

int genlGenFrameVar(GenState *gen, VarDclNode *var) {
    if (gen->genstep == NULL)
        return 0;
    int k = genlGenIndex(gen->genstep, var);
    return k >= 0 && gen->genstep->kind[k] != GenFrameFlag;
}

// A local whose drop flag lives in the frame: found at the entry, and given
// its starting state where the local is declared
int genlGenFrameFlag(GenState *gen, VarDclNode *var) {
    if (gen->genstep == NULL)
        return 0;
    int k = genlGenIndex(gen->genstep, var);
    return k >= 0 && gen->genstep->flagat[k] >= 0;
}

// ---- The function ------------------------------------------------------------

static void genlGenDebugAt(GenState *gen, FnDclNode *fnnode) {
    if (!gen->opt->release)
        LLVMSetCurrentDebugLocation2(gen->builder,
            LLVMDIBuilderCreateDebugLocation(gen->context, fnnode->linenbr, 0, LLVMGetSubprogram(gen->fn), NULL));
}

void genlGenBegin(GenState *gen, FnDclNode *fnnode) {
    GenInfo *info = yieldAny() ? yieldGenOf(fnnode) : NULL;
    gen->genstep = info;
    gen->genself = NULL;
    gen->genswitch = NULL;
    gen->gendone = NULL;
    if (info == NULL) {
        // The generator's 'final', which the generator's death runs before its
        // fields' (the parameters) are finalized: what the frame holds dies in it
        if (yieldAny() && fnnode->namesym == finalName) {
            INode *owner = (INode *)fnnode->dclinfo.owner;
            if (owner && owner->tag == StructTag && yieldGenOfStruct(owner))
                genlGenDrop(gen, (StructNode *)owner, LLVMGetParam(gen->fn, 0));
        }
        return;
    }
    genlGenPlan(gen, info);
    StructNode *strnode = info->gen;
    LLVMTypeRef structllvm = genlType(gen, (INode *)strnode);
    LLVMContextRef context = gen->context;
    LLVMBuilderRef builder = gen->builder;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    gen->genself = LLVMGetParam(gen->fn, 0);
    genlGenDebugAt(gen, fnnode);

    // The frame's storage, found once, here, where it dominates every path
    uint32_t nboxed = 0;
    LLVMValueRef *boxslot = (LLVMValueRef *)memAllocBlk((info->nframe ? info->nframe : 1) * sizeof(LLVMValueRef));
    if (info->framellvm) {
        LLVMValueRef framep = LLVMBuildStructGEP2(builder, structllvm, gen->genself, strnode->fields.used, "frame");
        for (uint32_t k = 0; k < info->nframe; ++k) {
            VarDclNode *var = info->frame[k];
            if (info->kind[k] != GenFrameFlag) {
                LLVMValueRef slot = LLVMBuildStructGEP2(builder, (LLVMTypeRef)info->framellvm, framep, info->valueat[k], &var->namesym->namestr);
                if (info->kind[k] == GenFrameInline)
                    var->llvmvar = slot;
                else {
                    boxslot[k] = slot;
                    ++nboxed;
                }
            }
            if (info->flagat[k] >= 0)
                var->llvmflag = LLVMBuildStructGEP2(builder, (LLVMTypeRef)info->framellvm, framep, info->flagat[k], "held");
        }
    }

    LLVMValueRef statep = LLVMBuildStructGEP2(builder, structllvm, gen->genself, info->state->index, "state");
    LLVMValueRef state = LLVMBuildLoad2(builder, i32, statep, "resumeat");

    // The first call makes the boxes
    if (nboxed) {
        LLVMValueRef first = LLVMBuildICmp(builder, LLVMIntEQ, state, LLVMConstInt(i32, 0, 0), "first");
        LLVMBasicBlockRef allocblk = genlInsertBlock(gen, "genbox");
        LLVMBasicBlockRef joinblk = genlInsertBlock(gen, "genboxed");
        LLVMMoveBasicBlockBefore(allocblk, joinblk);
        LLVMBuildCondBr(builder, first, allocblk, joinblk);
        LLVMPositionBuilderAtEnd(builder, allocblk);
        for (uint32_t k = 0; k < info->nframe; ++k) {
            if (info->kind[k] != GenFrameBoxed)
                continue;
            LLVMValueRef box = LLVMBuildMalloc(builder, genlType(gen, info->frame[k]->vtype), "box");
            LLVMBuildStore(builder, box, boxslot[k]);
        }
        LLVMBuildBr(builder, joinblk);
        LLVMPositionBuilderAtEnd(builder, joinblk);
        for (uint32_t k = 0; k < info->nframe; ++k) {
            if (info->kind[k] != GenFrameBoxed)
                continue;
            info->frame[k]->llvmvar = LLVMBuildLoad2(builder, LLVMPointerTypeInContext(context, 0), boxslot[k], "boxed");
        }
    }

    // The entry: the body from its start, or after the seam it was left at, or done
    LLVMBasicBlockRef firstblk = genlInsertBlock(gen, "body");
    gen->gendone = LLVMAppendBasicBlockInContext(context, gen->fn, "done");
    gen->genswitch = LLVMBuildSwitch(builder, state, firstblk, 4);
    LLVMAddCase(gen->genswitch, LLVMConstInt(i32, GenDone, 0), gen->gendone);
    LLVMPositionBuilderAtEnd(builder, firstblk);
}

void genlGenEnd(GenState *gen) {
    GenInfo *info = gen->genstep;
    if (info == NULL)
        return;
    LLVMBuilderRef builder = gen->builder;
    LLVMPositionBuilderAtEnd(builder, gen->gendone);
    FnDclNode *none = info->none;
    if (none->llvmvar == NULL)
        genlGloFnName(gen, none);
    LLVMValueRef result = genlFnDclCall(gen, none, none->llvmvar, NULL, 0);
    genlFnDclReturn(gen, gen->fndcl, result);
}

void genlGenReturn(GenState *gen) {
    GenInfo *info = gen->genstep;
    if (info == NULL)
        return;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    LLVMTypeRef structllvm = genlType(gen, (INode *)info->gen);
    LLVMValueRef statep = LLVMBuildStructGEP2(gen->builder, structllvm, gen->genself, info->state->index, "state");
    LLVMBuildStore(gen->builder, LLVMConstInt(i32, GenDone, 0), statep);
}

// ---- The seam ----------------------------------------------------------------

LLVMValueRef genlYield(GenState *gen, YieldNode *node) {
    GenInfo *info = gen->genstep;
    if (info == NULL || node->yieldno == 0) {
        errorUnreachable((INode *)node, "a 'yield' generated where no generator is");
        return NULL;
    }
    if (gen->tempcnt != gen->tempbase) {
        errorMsgNode((INode *)node, ErrorGenFrame,
            "A 'yield' here would leave a temporary of an enclosing statement still to be dropped across the seam, which the generator's frame does not keep. Bind the value that made the temporary to a variable before the statement.");
        return LLVMGetUndef(gen->emptyStructType);
    }
    LLVMContextRef context = gen->context;
    LLVMBuilderRef builder = gen->builder;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);

    // The result 'next' gives, made; its statement's temporaries end, since
    // the result holds what it needs of them
    uint32_t mark = gen->tempcnt;
    LLVMValueRef result = genlExpr(gen, node->exp);
    genlTempsEnd(gen, mark);

    // The state says where the next call resumes, and the call returns
    LLVMTypeRef structllvm = genlType(gen, (INode *)info->gen);
    LLVMValueRef statep = LLVMBuildStructGEP2(builder, structllvm, gen->genself, info->state->index, "state");
    LLVMBuildStore(builder, LLVMConstInt(i32, node->yieldno, 0), statep);
    genlFnDclReturn(gen, gen->fndcl, result);

    // What follows the seam: reached by the entry's switch, through a stub that
    // says the flags of the locals the frame does not keep hold nothing
    LLVMBasicBlockRef resume = genlInsertBlock(gen, "resume");
    LLVMBasicBlockRef stub = LLVMAppendBasicBlockInContext(context, gen->fn, "resumeat");
    LLVMBuilderRef sb = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(sb, stub);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    for (uint32_t i = 0; i < node->nseamvars; ++i) {
        VarDclNode *var = node->seamvars[i].var;
        int k = genlGenIndex(info, var);
        if (var->llvmflag == NULL || (k >= 0 && info->flagat[k] >= 0) || (node->seamvars[i].flags & SeamParm))
            continue;
        LLVMBuildStore(sb, LLVMConstInt(i8, DropFlagEmpty, 0), var->llvmflag);
    }
    LLVMBuildBr(sb, resume);
    LLVMDisposeBuilder(sb);
    LLVMAddCase(gen->genswitch, LLVMConstInt(i32, node->yieldno, 0), stub);
    LLVMPositionBuilderAtEnd(builder, resume);
    return LLVMGetUndef(gen->emptyStructType);
}

// ---- Death -------------------------------------------------------------------

// Finalize what the frame holds of the local 'k', in place
static void genlGenDropVar(GenState *gen, GenInfo *info, uint32_t k, LLVMTypeRef structllvm, LLVMValueRef framep) {
    VarDclNode *var = info->frame[k];
    LLVMBuilderRef builder = gen->builder;
    LLVMTypeRef framellvm = (LLVMTypeRef)info->framellvm;
    LLVMValueRef at = LLVMBuildStructGEP2(builder, framellvm, framep, info->valueat[k], "dropping");
    if (info->kind[k] == GenFrameBoxed)
        at = LLVMBuildLoad2(builder, LLVMPointerTypeInContext(gen->context, 0), at, "boxed");
    LLVMBasicBlockRef endblk = NULL;
    if (info->flagat[k] >= 0) {
        LLVMTypeRef i8 = LLVMInt8TypeInContext(gen->context);
        LLVMValueRef flagat = LLVMBuildStructGEP2(builder, framellvm, framep, info->flagat[k], "flag");
        LLVMValueRef held = LLVMBuildLoad2(builder, i8, flagat, "held");
        LLVMValueRef whole = LLVMBuildICmp(builder, LLVMIntEQ, held, LLVMConstInt(i8, DropFlagWhole, 0), "whole");
        LLVMBasicBlockRef doblk = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "drop");
        endblk = LLVMAppendBasicBlockInContext(gen->context, gen->fn, "dropped");
        LLVMBuildCondBr(builder, whole, doblk, endblk);
        LLVMPositionBuilderAtEnd(builder, doblk);
    }
    genlFinalizeAt(gen, at, var->vtype);
    if (endblk) {
        LLVMBuildBr(builder, endblk);
        LLVMPositionBuilderAtEnd(builder, endblk);
    }
}

void genlGenDrop(GenState *gen, StructNode *strnode, LLVMValueRef self) {
    if (!yieldAny())
        return;
    GenInfo *info = yieldGenOfStruct((INode *)strnode);
    if (info == NULL || info->state == NULL)
        return;
    genlGenPlan(gen, info);
    if (info->framellvm == NULL)
        return;
    genlGenDebugAt(gen, gen->fndcl);
    LLVMContextRef context = gen->context;
    LLVMBuilderRef builder = gen->builder;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef structllvm = genlType(gen, (INode *)strnode);
    LLVMValueRef framep = LLVMBuildStructGEP2(builder, structllvm, self, strnode->fields.used, "frame");
    LLVMValueRef statep = LLVMBuildStructGEP2(builder, structllvm, self, info->state->index, "state");
    LLVMValueRef state = LLVMBuildLoad2(builder, i32, statep, "resumeat");

    // A generator not yet run holds nothing in its frame, boxes included
    LLVMBasicBlockRef endblk = LLVMAppendBasicBlockInContext(context, gen->fn, "gendropped");
    LLVMBasicBlockRef runblk = LLVMAppendBasicBlockInContext(context, gen->fn, "genran");
    LLVMValueRef notrun = LLVMBuildICmp(builder, LLVMIntEQ, state, LLVMConstInt(i32, 0, 0), "notrun");
    LLVMBuildCondBr(builder, notrun, endblk, runblk);
    LLVMPositionBuilderAtEnd(builder, runblk);

    // Left at a seam: what it holds there, newest first
    LLVMBasicBlockRef freeblk = LLVMAppendBasicBlockInContext(context, gen->fn, "genfree");
    LLVMValueRef sw = LLVMBuildSwitch(builder, state, freeblk, 4);
    if (info->yields) {
        INode **yp;
        uint32_t ycnt;
        for (nodesFor(info->yields, ycnt, yp)) {
            YieldNode *yield = (YieldNode *)*yp;
            int any = 0;
            for (uint32_t i = 0; i < yield->nseamvars; ++i) {
                SeamVar *sv = &yield->seamvars[i];
                any |= (sv->flags & SeamOpen) && (sv->flags & SeamDies) && !(sv->flags & (SeamTemp | SeamGuard | SeamParm));
            }
            if (!any)
                continue;
            LLVMBasicBlockRef blk = LLVMAppendBasicBlockInContext(context, gen->fn, "genseam");
            LLVMAddCase(sw, LLVMConstInt(i32, yield->yieldno, 0), blk);
            LLVMPositionBuilderAtEnd(builder, blk);
            for (uint32_t i = yield->nseamvars; i > 0; --i) {
                SeamVar *sv = &yield->seamvars[i - 1];
                if (!(sv->flags & SeamOpen) || !(sv->flags & SeamDies) || (sv->flags & (SeamTemp | SeamGuard | SeamParm)))
                    continue;
                int k = genlGenIndex(info, flowDropOwner(sv->var));
                if (flowMatchInPlace(sv->var) || k < 0 || info->kind[k] == GenFrameFlag)
                    continue;
                genlGenDropVar(gen, info, (uint32_t)k, structllvm, framep);
            }
            LLVMBuildBr(builder, freeblk);
        }
    }

    // The boxes, if any, however it ended
    LLVMPositionBuilderAtEnd(builder, freeblk);
    for (uint32_t k = 0; k < info->nframe; ++k) {
        if (info->kind[k] != GenFrameBoxed)
            continue;
        LLVMValueRef slot = LLVMBuildStructGEP2(builder, (LLVMTypeRef)info->framellvm, framep, info->valueat[k], "boxslot");
        LLVMValueRef box = LLVMBuildLoad2(builder, LLVMPointerTypeInContext(context, 0), slot, "box");
        LLVMBuildFree(builder, box);
    }
    LLVMBuildBr(builder, endblk);
    LLVMPositionBuilderAtEnd(builder, endblk);
}
