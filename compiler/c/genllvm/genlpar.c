/** Generation of a parallel each
 * @file
 *
 * A 'parallel each' is built by pareach.c as the statements of its own block
 * (pareach.h): the source made ready, then the range [lo, hi) of passes, the
 * index 'k' that counts through it, and the loop. The statements before 'k' are
 * generated where the loop is written. 'k' and the loop are the PIECE: they are
 * generated into a function of their own,
 *
 *   void piece(ptr record, usize lo, usize hi)
 *
 * which counts k from lo and leaves its loop at hi, and the loop's place gets a
 * call of the actors package's parallelEach over [lo, hi) with that function
 * and a record. parallelEach splits the range on demand and runs the function
 * on pieces of it on the workers, the caller working on pieces meanwhile; it
 * returns when every pass has run.
 *
 * No closures exist, so the piece finds what the loop uses from outside it --
 * the source held in its hidden variable, and every variable of the function
 * the loop is written in that the body names -- through the record: an array of
 * pointers to those variables, which stay where they are in the caller's frame
 * for as long as the loop runs. The body is not told, and the generator finds
 * out as it goes: the first time a use of a variable from outside is
 * generated in the piece (genlParCapture), the pointer is taken from the
 * record at the function's entry and stands for the variable from then on; the
 * record is made after the piece is, from the variables so found. A loop inside
 * a piece's body is a piece of a piece: its record is made in the outer piece,
 * from what it found in the outer piece's own.
 *
 * Nothing in the body may write what it finds there (pareach.c checks it), so
 * the pieces only read the caller's frame.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../shared/error.h"
#include "../coneopts.h"
#include "../ir/nametbl.h"
#include "genllvm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// The variable of this name among a block's statements
static VarDclNode *genlParFind(BlockNode *blk, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        if ((*nodesp)->tag == VarDclTag && ((VarDclNode*)*nodesp)->namesym == name)
            return (VarDclNode*)*nodesp;
    }
    return NULL;
}

// A number range's bounds as unsigned or signed, widened to usize's width
static LLVMValueRef genlParWiden(GenState *gen, VarDclNode *bound, int issigned) {
    LLVMTypeRef usize = genlUsize(gen);
    LLVMValueRef val = LLVMBuildLoad2(gen->builder, genlType(gen, bound->vtype), bound->llvmvar, "bound");
    if (LLVMGetIntTypeWidth(LLVMTypeOf(val)) < LLVMGetIntTypeWidth(usize))
        val = issigned ? LLVMBuildSExt(gen->builder, val, usize, "bound") : LLVMBuildZExt(gen->builder, val, usize, "bound");
    return val;
}

// The number of passes of a range, worked out from its bounds where the loop
// is written, and put in 'hi': how many numbers there are from the first bound
// up to the last (excluded, or included for '<='), none where the last is
// below the first. The difference of two numbers of one type always fits an
// unsigned word of usize's width, which is how it is taken.
static void genlParCount(GenState *gen, BlockNode *blk, VarDclNode *first, VarDclNode *last, VarDclNode *hi) {
    LLVMTypeRef usize = genlUsize(gen);
    INode *type = itypeGetTypeDcl(first->vtype);
    int issigned = type->tag == IntNbrTag;
    int inclusive = (blk->flags & FlagParIncl) != 0;
    LLVMValueRef a = genlParWiden(gen, first, issigned);
    LLVMValueRef b = genlParWiden(gen, last, issigned);
    LLVMIntPredicate pred = inclusive ? (issigned ? LLVMIntSLE : LLVMIntULE) : (issigned ? LLVMIntSLT : LLVMIntULT);
    LLVMValueRef nonempty = LLVMBuildICmp(gen->builder, pred, a, b, "nonempty");
    LLVMValueRef count = LLVMBuildSub(gen->builder, b, a, "count");
    if (inclusive)
        count = LLVMBuildAdd(gen->builder, count, LLVMConstInt(usize, 1, 0), "count");
    LLVMValueRef n = LLVMBuildSelect(gen->builder, nonempty, count, LLVMConstInt(usize, 0, 0), "passes");
    LLVMBuildStore(gen->builder, n, hi->llvmvar);
}

void genlParCapture(GenState *gen, VarDclNode *var) {
    GenParBody *pb = gen->parbody;
    LLVMValueRef home = var->llvmvar;
    if (home == NULL || !LLVMIsAInstruction(home))
        return;
    // A variable of the piece itself, or of one it is written in that was found already
    if (LLVMGetBasicBlockParent(LLVMGetInstructionParent(home)) == gen->fn)
        return;

    if (pb->ncaps == pb->maxcaps) {
        uint32_t newmax = pb->maxcaps ? pb->maxcaps * 2 : 8;
        GenCapture *caps = (GenCapture *)memAllocBlk(newmax * sizeof(GenCapture));
        if (pb->ncaps)
            memcpy(caps, pb->caps, pb->ncaps * sizeof(GenCapture));
        pb->caps = caps;
        pb->maxcaps = newmax;
    }
    uint32_t index = pb->ncaps++;
    pb->caps[index].var = var;
    pb->caps[index].home = home;

    // Its pointer is taken from the record at the function's entry, ahead of
    // the allocas, where it is found for every use after
    LLVMContextRef context = gen->context;
    LLVMTypeRef ptr = LLVMPointerTypeInContext(context, 0);
    LLVMBasicBlockRef current = LLVMGetInsertBlock(gen->builder);
    LLVMMetadataRef debugloc = LLVMGetCurrentDebugLocation2(gen->builder);
    LLVMPositionBuilderBefore(gen->builder, gen->allocaPoint);
    LLVMValueRef at = LLVMConstInt(LLVMInt32TypeInContext(context), index, 0);
    LLVMValueRef slot = LLVMBuildInBoundsGEP2(gen->builder, ptr, pb->record, &at, 1, "capslot");
    var->llvmvar = LLVMBuildLoad2(gen->builder, ptr, slot, &var->namesym->namestr);
    LLVMPositionBuilderAtEnd(gen->builder, current);
    LLVMSetCurrentDebugLocation2(gen->builder, debugloc);
}

INode *genlParallelRun(GenState *gen, BlockNode *blk, INode *kdcl) {
    LLVMContextRef context = gen->context;
    LLVMTypeRef usize = genlUsize(gen);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(context, 0);

    VarDclNode *lo = genlParFind(blk, parLoName);
    VarDclNode *hi = genlParFind(blk, parHiName);
    INode *loop = NULL;
    uint32_t kat = 0;
    for (uint32_t i = 0; i < blk->stmts->used; ++i) {
        INode *stmt = nodesGet(blk->stmts, i);
        if (stmt == kdcl)
            kat = i;
        else if (kat != 0) {
            // The loop is the block's last statement, which type check made its value
            INode *cand = stmt->tag == BlockRetTag ? ((BreakRetNode*)stmt)->exp : stmt;
            if (cand->tag == BlockTag && (cand->flags & FlagLoop)) {
                loop = cand;
                break;
            }
        }
    }
    assert(lo && hi && loop && "a parallel each is built with its range, its index and its loop");

    // A range's count is not known until its bounds are
    if (blk->flags & FlagParRange)
        genlParCount(gen, blk, genlParFind(blk, parFirstName), genlParFind(blk, parLastName), hi);
    LLVMValueRef rlo = LLVMBuildLoad2(gen->builder, usize, lo->llvmvar, "lo");
    LLVMValueRef rhi = LLVMBuildLoad2(gen->builder, usize, hi->llvmvar, "hi");

    // ---- The piece, a function of its own ----
    LLVMTypeRef partypes[3] = { ptr, usize, usize };
    LLVMTypeRef fntype = LLVMFunctionType(LLVMVoidTypeInContext(context), partypes, 3, 0);
    char name[256];
    size_t outerlen;
    const char *outername = LLVMGetValueName2(gen->fn, &outerlen);
    snprintf(name, sizeof(name), "%.*s.parallel", (int)(outerlen > 200 ? 200 : outerlen), outername);
    LLVMValueRef fn = LLVMAddFunction(gen->module, name, fntype);
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    if (!gen->opt->release) {
        LLVMMetadataRef ditype = LLVMDIBuilderCreateSubroutineType(gen->dibuilder, gen->difile, NULL, 0, 0);
        LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(gen->dibuilder, gen->difile,
            name, strlen(name), name, strlen(name), gen->difile, loop->linenbr, ditype, 1, 1, loop->linenbr,
            LLVMDIFlagPrivate, 0);
        LLVMSetSubprogram(fn, sp);
    }

    // What the function being generated is, set aside as genlFn does
    LLVMValueRef svfn = gen->fn;
    LLVMBuilderRef svbuilder = gen->builder;
    LLVMValueRef svallocaPoint = gen->allocaPoint;
    int svexitzero = gen->exitzero;
    GenRoots svroots;
    genlRootsSave(gen, &svroots);
    uint32_t svtempbase = gen->tempbase;
    gen->tempbase = gen->tempcnt;
    Nodes *svseams = gen->seams;
    AwaitNode *svresumeat = gen->resumeat;
    uint32_t svflightbase = gen->flightbase;
    gen->seams = NULL;
    gen->resumeat = NULL;
    gen->flightbase = gen->flightcnt;
    GenParBody pb;
    memset(&pb, 0, sizeof(pb));
    pb.enclosing = gen->parbody;
    pb.fn = fn;
    pb.record = LLVMGetParam(fn, 0);
    gen->parbody = &pb;
    gen->fn = fn;
    gen->exitzero = 0;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, fn, "entry");
    gen->builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(gen->builder, entry);
    if (!gen->opt->release) {
        unsigned col = loop->srcp && loop->linep ? (unsigned)(loop->srcp - loop->linep) : 0;
        LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(context, loop->linenbr, col, LLVMGetSubprogram(fn), NULL);
        LLVMSetCurrentDebugLocation2(gen->builder, loc);
    }
    gen->allocaPoint = LLVMBuildAlloca(gen->builder, LLVMInt32TypeInContext(context), "alloca_point");

    // The range this piece runs is its own: lo and hi stand for the piece's
    LLVMValueRef svlo = lo->llvmvar;
    LLVMValueRef svhi = hi->llvmvar;
    lo->llvmvar = genlAlloca(gen, usize, "lo");
    LLVMBuildStore(gen->builder, LLVMGetParam(fn, 1), lo->llvmvar);
    hi->llvmvar = genlAlloca(gen, usize, "hi");
    LLVMBuildStore(gen->builder, LLVMGetParam(fn, 2), hi->llvmvar);

    // The index and the loop
    for (uint32_t i = kat; i < blk->stmts->used; ++i) {
        INode *stmt = nodesGet(blk->stmts, i);
        if (stmt->tag == BlockRetTag)
            stmt = ((BreakRetNode*)stmt)->exp;
        uint32_t tempmark = gen->tempcnt;
        genlExpr(gen, stmt);
        genlTempsEnd(gen, tempmark);
        if (stmt == loop)
            break;
    }
    LLVMBuildRetVoid(gen->builder);

    genlRootFrame(gen);
    if (LLVMGetInstructionParent(gen->allocaPoint))
        LLVMInstructionEraseFromParent(gen->allocaPoint);
    LLVMDisposeBuilder(gen->builder);

    // Back in the function the loop is written in
    gen->builder = svbuilder;
    gen->fn = svfn;
    gen->allocaPoint = svallocaPoint;
    gen->exitzero = svexitzero;
    gen->tempcnt = gen->tempbase;
    gen->tempbase = svtempbase;
    genlRootsRestore(gen, &svroots);
    gen->seams = svseams;
    gen->resumeat = svresumeat;
    gen->flightcnt = gen->flightbase;
    gen->flightbase = svflightbase;
    gen->parbody = pb.enclosing;
    lo->llvmvar = svlo;
    hi->llvmvar = svhi;

    // ---- A loop cut at its end ----
    // A behaviour's parallel each is followed by the seam that cuts the behaviour
    // there (pareach.c). It does not call the loop: the seam hands it to the
    // runtime as the behaviour returns (genlawait.c). What the pieces read of the
    // behaviour's variables must outlive its frame, so it is copied now into a
    // block of its own, and the record points into that
    AwaitNode *cut = NULL;
    for (uint32_t i = 0; i < blk->stmts->used; ++i) {
        INode *stmt = nodesGet(blk->stmts, i);
        // (the block's last statement is its value, a 'blockret')
        if (stmt->tag == BlockRetTag)
            stmt = ((BreakRetNode*)stmt)->exp;
        if (stmt->tag == AwaitTag && ((AwaitNode*)stmt)->par)
            cut = (AwaitNode*)stmt;
    }
    if (cut != NULL) {
        GenParSeam *seam = (GenParSeam *)memAllocBlk(sizeof(GenParSeam));
        seam->lo = rlo;
        seam->hi = rhi;
        seam->piece = fn;
        seam->caps = NULL;
        if (pb.ncaps > 0) {
            // { [n x ptr] pointers, copy 0, copy 1, ... }
            LLVMTypeRef *ftypes = (LLVMTypeRef *)memAllocBlk((pb.ncaps + 1) * sizeof(LLVMTypeRef));
            ftypes[0] = LLVMArrayType2(ptr, pb.ncaps);
            for (uint32_t i = 0; i < pb.ncaps; ++i) {
                LLVMValueRef home = pb.caps[i].home;
                ftypes[i + 1] = LLVMIsAAllocaInst(home) ? LLVMGetAllocatedType(home) : genlType(gen, pb.caps[i].var->vtype);
            }
            LLVMTypeRef blocktype = LLVMStructTypeInContext(context, ftypes, pb.ncaps + 1, 0);
            if (actorRuntime[ActorRtParBlock]->llvmvar == NULL)
                genlGloFnName(gen, actorRuntime[ActorRtParBlock]);
            LLVMValueRef size = LLVMConstInt(usize, LLVMABISizeOfType(gen->datalayout, blocktype), 0);
            LLVMValueRef block = genlFnDclCall(gen, actorRuntime[ActorRtParBlock], actorRuntime[ActorRtParBlock]->llvmvar, &size, 1);
            LLVMValueRef pointers = LLVMBuildStructGEP2(gen->builder, blocktype, block, 0, "pointers");
            for (uint32_t i = 0; i < pb.ncaps; ++i) {
                VarDclNode *var = pb.caps[i].var;
                LLVMValueRef copy = LLVMBuildStructGEP2(gen->builder, blocktype, block, i + 1, "copy");
                LLVMValueRef value = LLVMBuildLoad2(gen->builder, ftypes[i + 1], pb.caps[i].home, "tocopy");
                LLVMBuildStore(gen->builder, value, copy);
                LLVMValueRef at = LLVMConstInt(LLVMInt32TypeInContext(context), i, 0);
                LLVMValueRef slot = LLVMBuildInBoundsGEP2(gen->builder, ptr, pointers, &at, 1, "recslot");
                LLVMBuildStore(gen->builder, copy, slot);
                var->llvmvar = pb.caps[i].home;
            }
            seam->caps = block;
        }
        cut->genpar = seam;
        return loop;
    }

    // ---- The call ----
    // The record: a pointer to each variable the piece found outside it. In a
    // piece of a piece, those must be the outer piece's own pointers, which the
    // outer one takes from its record in turn (genlParCapture)
    LLVMValueRef record;
    if (pb.ncaps == 0)
        record = LLVMConstNull(ptr);
    else {
        record = genlAlloca(gen, LLVMArrayType2(ptr, pb.ncaps), "record");
        for (uint32_t i = 0; i < pb.ncaps; ++i) {
            VarDclNode *var = pb.caps[i].var;
            var->llvmvar = pb.caps[i].home;
            if (gen->parbody != NULL)
                genlParCapture(gen, var);
            LLVMValueRef at = LLVMConstInt(LLVMInt32TypeInContext(context), i, 0);
            LLVMValueRef slot = LLVMBuildInBoundsGEP2(gen->builder, ptr, record, &at, 1, "recslot");
            LLVMBuildStore(gen->builder, var->llvmvar, slot);
        }
    }
    if (parallelEachFn->llvmvar == NULL)
        genlGloFnName(gen, parallelEachFn);
    LLVMValueRef args[4] = { rlo, rhi, fn, record };
    genlFnDclCall(gen, parallelEachFn, parallelEachFn->llvmvar, args, 4);
    return loop;
}
