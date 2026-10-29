/** Generation of SIMD vectors, 'Vector[T, N]': LLVM's <N x T>
 *
 * A vector is an LLVM vector value, so the operators and lane methods the
 * number types have are LLVM's own instructions on the whole vector: one
 * 'fadd <4 x float>' where a scalar has one 'fadd float'. What differs from
 * the scalar is here: an integer division checks every lane before it divides,
 * a shift is defined past the width lane by lane, the reductions and mask
 * tests are LLVM's vector reductions, and the conversions between vectors and
 * arrays, and from a number into every lane, are made lane by lane or through
 * memory.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "genllvm.h"

#include <string.h>

// Call one of LLVM's intrinsic functions, overloaded on 'ntypes' types
static LLVMValueRef genlVecCall(GenState *gen, char *name, LLVMTypeRef *types, unsigned ntypes,
        LLVMValueRef *args, unsigned nargs) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef fn = LLVMGetIntrinsicDeclaration(gen->module, id, types, ntypes);
    return LLVMBuildCall2(gen->builder, LLVMIntrinsicGetType(gen->context, id, types, ntypes), fn, args, nargs, "");
}

static int genlLaneIsFloat(LLVMTypeRef lane) {
    LLVMTypeKind kind = LLVMGetTypeKind(lane);
    return kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind;
}

// A value put in every lane of a vector of 'lanes'
static LLVMValueRef genlSplat(GenState *gen, LLVMValueRef scalar, unsigned lanes) {
    LLVMTypeRef vectype = LLVMVectorType(LLVMTypeOf(scalar), lanes);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    LLVMValueRef first = LLVMBuildInsertElement(gen->builder, LLVMGetPoison(vectype), scalar, LLVMConstInt(i32, 0, 0), "");
    return LLVMBuildShuffleVector(gen->builder, first, LLVMGetPoison(vectype), LLVMConstNull(LLVMVectorType(i32, lanes)), "splat");
}

// Panic unless no lane of an integer division's divisor is zero and, for a
// signed one, no lane divides the type's minimum by -1: the quotient does not
// fit, and the hardware traps on it. The lanes are compared together, reduced
// to one flag and tested once before the divide (a vector of N lanes costs the
// compare, the reduction and a branch, not N tests).
static void genlDivideCheck(GenState *gen, INode *site, int16_t kind, LLVMValueRef a, LLVMValueRef b) {
    LLVMTypeRef vectype = LLVMTypeOf(b);
    LLVMTypeRef lane = LLVMGetElementType(vectype);
    unsigned lanes = LLVMGetVectorSize(vectype);
    unsigned width = LLVMGetIntTypeWidth(lane);
    LLVMValueRef iszero = LLVMBuildICmp(gen->builder, LLVMIntEQ, b, LLVMConstNull(vectype), "divzero");
    LLVMTypeRef masktype = LLVMTypeOf(iszero);
    LLVMValueRef anyzero = genlVecCall(gen, "llvm.vector.reduce.or", &masktype, 1, &iszero, 1);
    LLVMValueRef bad = anyzero;
    if (kind == SDivIntrinsic || kind == SRemIntrinsic) {
        LLVMValueRef minval = genlSplat(gen, LLVMConstInt(lane, 1ULL << (width - 1), 0), lanes);
        LLVMValueRef negone = genlSplat(gen, LLVMConstInt(lane, ~0ULL, 1), lanes);
        LLVMValueRef overflow = LLVMBuildAnd(gen->builder,
            LLVMBuildICmp(gen->builder, LLVMIntEQ, a, minval, ""),
            LLVMBuildICmp(gen->builder, LLVMIntEQ, b, negone, ""), "divoverflow");
        LLVMValueRef anyoverflow = genlVecCall(gen, "llvm.vector.reduce.or", &masktype, 1, &overflow, 1);
        bad = LLVMBuildOr(gen->builder, anyzero, anyoverflow, "divbad");
    }
    LLVMBasicBlockRef panicblk = genlInsertBlock(gen, "panic");
    LLVMBasicBlockRef okblk = genlInsertBlock(gen, "divok");
    LLVMBuildCondBr(gen->builder, bad, panicblk, okblk);
    LLVMPositionBuilderAtEnd(gen->builder, panicblk);
    LLVMTypeRef usize = genlUsize(gen);
    LLVMValueRef what = LLVMBuildSelect(gen->builder, anyzero, LLVMConstInt(usize, 0, 0), LLVMConstInt(usize, 1, 0), "");
    genlPanic(gen, site, PanicDivide, &what);
    LLVMPositionBuilderAtEnd(gen->builder, okblk);
}

// '<<' and '>>' on integer lanes, defined for every amount as a scalar's are
// (genlShift): an amount of the lane's width or more leaves 0, or for '>>' on
// signed lanes the sign in every bit, never poison
static LLVMValueRef genlVectorShift(GenState *gen, int16_t op, LLVMValueRef x, LLVMValueRef n) {
    LLVMTypeRef vectype = LLVMTypeOf(x);
    LLVMTypeRef lane = LLVMGetElementType(vectype);
    unsigned lanes = LLVMGetVectorSize(vectype);
    unsigned width = LLVMGetIntTypeWidth(lane);
    LLVMValueRef widths = genlSplat(gen, LLVMConstInt(lane, width, 0), lanes);
    LLVMValueRef inrange = LLVMBuildICmp(gen->builder, LLVMIntULT, n, widths, "shiftinrange");
    if (op == SShrIntrinsic) {
        LLVMValueRef limit = genlSplat(gen, LLVMConstInt(lane, width - 1, 0), lanes);
        LLVMValueRef amount = LLVMBuildSelect(gen->builder, inrange, n, limit, "");
        return LLVMBuildAShr(gen->builder, x, amount, "");
    }
    LLVMValueRef shifted = op == ShlIntrinsic ? LLVMBuildShl(gen->builder, x, n, "") : LLVMBuildLShr(gen->builder, x, n, "");
    return LLVMBuildSelect(gen->builder, inrange, shifted, LLVMConstNull(vectype), "");
}

// The alignment a lane has in memory, which is all a slice promises its lanes:
// a load or store of the lanes together is unaligned for the vector
static unsigned genlLaneAlign(LLVMTypeRef lane) {
    switch (LLVMGetTypeKind(lane)) {
    case LLVMHalfTypeKind: return 2;
    case LLVMFloatTypeKind: return 4;
    case LLVMDoubleTypeKind: return 8;
    default: return LLVMGetIntTypeWidth(lane) / 8;
    }
}

// The address of the first of 'n' lanes at s[at] of the slice, once all of them
// are within it: at + n <= the count, checked once (not at + n - 1 < count per
// lane), else the index panic with the last lane's index and the count
static LLVMValueRef genlSliceSpan(GenState *gen, INode *site, LLVMValueRef slice, LLVMValueRef at, unsigned n, LLVMTypeRef lane) {
    LLVMBuilderRef bld = gen->builder;
    LLVMTypeRef usize = genlUsize(gen);
    LLVMValueRef ptr = LLVMBuildExtractValue(bld, slice, 0, "sliceptr");
    LLVMValueRef count = LLVMBuildExtractValue(bld, slice, 1, "count");
    LLVMValueRef lanes = LLVMConstInt(usize, n, 0);
    // The slice holds n lanes at all, and 'at' is no further than that leaves room
    // for. Two branches to one panic, not one branch on both: the first depends on
    // the slice alone, so a loop over the slice has it loop-invariant and the
    // optimizer takes it out of the loop, leaving the one comparison of 'at'.
    LLVMBasicBlockRef panicblk = genlInsertBlock(gen, "panic");
    LLVMBasicBlockRef roomblk = genlInsertBlock(gen, "spanroom");
    LLVMBasicBlockRef okblk = genlInsertBlock(gen, "spanin");
    LLVMValueRef fits = LLVMBuildICmp(bld, LLVMIntUGE, count, lanes, "spanfits");
    LLVMBuildCondBr(bld, fits, roomblk, panicblk);
    LLVMPositionBuilderAtEnd(bld, roomblk);
    LLVMValueRef room = LLVMBuildSub(bld, count, lanes, "");
    LLVMValueRef inrange = LLVMBuildICmp(bld, LLVMIntULE, at, room, "spanok");
    LLVMBuildCondBr(bld, inrange, okblk, panicblk);
    LLVMPositionBuilderAtEnd(bld, panicblk);
    LLVMValueRef sat[2] = {at, LLVMConstInt(usize, n - 1, 0)};
    LLVMValueRef last = genlVecCall(gen, "llvm.uadd.sat", &usize, 1, sat, 2);
    LLVMValueRef vals[2] = {last, count};
    genlPanic(gen, site, PanicIndex, vals);
    LLVMPositionBuilderAtEnd(bld, okblk);
    return LLVMBuildGEP2(bld, lane, ptr, &at, 1, "lanes");
}

// 'Vector[T, N].load(s, at)': the N lanes of the slice from 'at' as a vector
LLVMValueRef genlVectorLoad(GenState *gen, INode *site, INode *vectype, LLVMValueRef *fnargs) {
    LLVMTypeRef llvmvec = genlType(gen, vectype);
    LLVMTypeRef lane = LLVMGetElementType(llvmvec);
    LLVMValueRef slot = genlSliceSpan(gen, site, fnargs[0], fnargs[1], (unsigned)vectorLanes(vectype), lane);
    LLVMValueRef load = LLVMBuildLoad2(gen->builder, llvmvec, slot, "lanes");
    LLVMSetAlignment(load, genlLaneAlign(lane));
    return load;
}

// 'm[i]': a mask's lane, a bool. The mask is a register value (or, through a
// reference, loaded), and its lane is taken at a checked index.
LLVMValueRef genlMaskLane(GenState *gen, FnCallNode *fncall) {
    INode *objtype = iexpGetTypeDcl(fncall->objfn);
    LLVMValueRef mask = genlExpr(gen, fncall->objfn);
    if (objtype->tag == RefTag) {
        INode *masktype = itypeGetTypeDcl(((RefNode*)objtype)->vtexp);
        mask = LLVMBuildLoad2(gen->builder, genlType(gen, masktype), mask, "");
        objtype = masktype;
    }
    LLVMValueRef index = genlExpr(gen, nodesGet(fncall->args, 0));
    genlBoundsCheck(gen, (INode*)fncall, index, LLVMConstInt(genlUsize(gen), vectorLanes(objtype), 0));
    return LLVMBuildExtractElement(gen->builder, mask, index, "lane");
}

// 'a.join(b)' and 'v.(3, 2, 1, 0)': one shufflevector. A pick of a join is that
// join's two vectors picked from directly, not the joined vector picked from.
// One position is the lane itself, extracted.
static LLVMValueRef genlVectorShuffle(GenState *gen, FnCallNode *lit, VectorLitKind kind) {
    LLVMBuilderRef bld = gen->builder;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
    if (kind == VecLitJoin) {
        LLVMValueRef a = genlExpr(gen, nodesGet(lit->args, 0));
        LLVMValueRef b = genlExpr(gen, nodesGet(lit->args, 1));
        unsigned total = LLVMGetVectorSize(LLVMTypeOf(a)) * 2;
        LLVMValueRef mask[VectorMaxLanes];
        for (unsigned i = 0; i < total; ++i)
            mask[i] = LLVMConstInt(i32, i, 0);
        return LLVMBuildShuffleVector(bld, a, b, LLVMConstVector(mask, total), "join");
    }
    INode *src = nodesGet(lit->args, 0);
    unsigned count = lit->args->used - 1;
    if (count == 1) {
        LLVMValueRef v = genlExpr(gen, src);
        uint64_t pos = ((ULitNode*)nodesGet(lit->args, 1))->uintlit;
        return LLVMBuildExtractElement(bld, v, LLVMConstInt(i32, pos, 0), "lane");
    }
    LLVMValueRef a, b;
    if (src->tag == TypeLitTag && ((FnCallNode*)src)->args->used >= 2 && vectorLitKind((FnCallNode*)src) == VecLitJoin) {
        a = genlExpr(gen, nodesGet(((FnCallNode*)src)->args, 0));
        b = genlExpr(gen, nodesGet(((FnCallNode*)src)->args, 1));
    }
    else {
        a = genlExpr(gen, src);
        b = LLVMGetPoison(LLVMTypeOf(a));
    }
    LLVMValueRef mask[VectorMaxLanes];
    for (unsigned i = 0; i < count; ++i)
        mask[i] = LLVMConstInt(i32, ((ULitNode*)nodesGet(lit->args, i + 1))->uintlit, 0);
    return LLVMBuildShuffleVector(bld, a, b, LLVMConstVector(mask, count), "pick");
}

// An operator, method or reduction of a vector, over its first argument's lanes
LLVMValueRef genlVectorIntrinsic(GenState *gen, INode *site, int16_t kind, LLVMValueRef *fnargs, uint32_t fnargcnt) {
    (void)fnargcnt;
    LLVMValueRef a = fnargs[0];
    LLVMValueRef b = fnargs[1];
    LLVMTypeRef vectype = LLVMTypeOf(a);
    LLVMTypeRef lane = LLVMGetElementType(vectype);
    int isfloat = genlLaneIsFloat(lane);
    LLVMBuilderRef bld = gen->builder;
    switch (kind) {
    case NegIntrinsic: return isfloat ? LLVMBuildFNeg(bld, a, "") : LLVMBuildNeg(bld, a, "");
    case NotIntrinsic: return LLVMBuildNot(bld, a, "");
    case AddIntrinsic: return isfloat ? LLVMBuildFAdd(bld, a, b, "") : LLVMBuildAdd(bld, a, b, "");
    case SubIntrinsic: return isfloat ? LLVMBuildFSub(bld, a, b, "") : LLVMBuildSub(bld, a, b, "");
    case MulIntrinsic: return isfloat ? LLVMBuildFMul(bld, a, b, "") : LLVMBuildMul(bld, a, b, "");
    case DivIntrinsic:
        if (isfloat)
            return LLVMBuildFDiv(bld, a, b, "");
        genlDivideCheck(gen, site, kind, a, b);
        return LLVMBuildUDiv(bld, a, b, "");
    case SDivIntrinsic:
        genlDivideCheck(gen, site, kind, a, b);
        return LLVMBuildSDiv(bld, a, b, "");
    case RemIntrinsic:
        if (isfloat)
            return LLVMBuildFRem(bld, a, b, "");
        genlDivideCheck(gen, site, kind, a, b);
        return LLVMBuildURem(bld, a, b, "");
    case SRemIntrinsic:
        genlDivideCheck(gen, site, kind, a, b);
        return LLVMBuildSRem(bld, a, b, "");
    case AndIntrinsic: return LLVMBuildAnd(bld, a, b, "");
    case OrIntrinsic: return LLVMBuildOr(bld, a, b, "");
    case XorIntrinsic: return LLVMBuildXor(bld, a, b, "");
    case ShlIntrinsic:
    case ShrIntrinsic:
    case SShrIntrinsic:
        return genlVectorShift(gen, kind, a, b);

    // Comparisons give a mask: a vector of i1, which is what the compare is
    case EqIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealOEQ, a, b, "") : LLVMBuildICmp(bld, LLVMIntEQ, a, b, "");
    // Unordered, true when either side is a NaN, as the scalar's
    case NeIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealUNE, a, b, "") : LLVMBuildICmp(bld, LLVMIntNE, a, b, "");
    case LtIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealOLT, a, b, "") : LLVMBuildICmp(bld, LLVMIntULT, a, b, "");
    case LeIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealOLE, a, b, "") : LLVMBuildICmp(bld, LLVMIntULE, a, b, "");
    case GtIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealOGT, a, b, "") : LLVMBuildICmp(bld, LLVMIntUGT, a, b, "");
    case GeIntrinsic: return isfloat ? LLVMBuildFCmp(bld, LLVMRealOGE, a, b, "") : LLVMBuildICmp(bld, LLVMIntUGE, a, b, "");
    case SLtIntrinsic: return LLVMBuildICmp(bld, LLVMIntSLT, a, b, "");
    case SLeIntrinsic: return LLVMBuildICmp(bld, LLVMIntSLE, a, b, "");
    case SGtIntrinsic: return LLVMBuildICmp(bld, LLVMIntSGT, a, b, "");
    case SGeIntrinsic: return LLVMBuildICmp(bld, LLVMIntSGE, a, b, "");

    // Math, over the vector (LLVM's vector forms of the scalar intrinsics)
    case SqrtIntrinsic: return genlVecCall(gen, "llvm.sqrt", &vectype, 1, &a, 1);
    case SinIntrinsic: return genlVecCall(gen, "llvm.sin", &vectype, 1, &a, 1);
    case CosIntrinsic: return genlVecCall(gen, "llvm.cos", &vectype, 1, &a, 1);

    // Reductions. A float sum is the ordered one -- lane 0 plus lane 1, plus
    // lane 2, and so on -- since nothing marks a float operation as
    // reassociable, started at -0.0, which adds to lane 0 and changes nothing,
    // not even a -0.0 there. The product likewise, from 1.0. A float minimum
    // or maximum takes the number over a NaN, as LLVM's minnum and maxnum do.
    case VecSumIntrinsic:
        if (isfloat) {
            LLVMValueRef args[2] = {LLVMConstReal(lane, -0.0), a};
            return genlVecCall(gen, "llvm.vector.reduce.fadd", &vectype, 1, args, 2);
        }
        return genlVecCall(gen, "llvm.vector.reduce.add", &vectype, 1, &a, 1);
    case VecProductIntrinsic:
        if (isfloat) {
            LLVMValueRef args[2] = {LLVMConstReal(lane, 1.0), a};
            return genlVecCall(gen, "llvm.vector.reduce.fmul", &vectype, 1, args, 2);
        }
        return genlVecCall(gen, "llvm.vector.reduce.mul", &vectype, 1, &a, 1);
    case VecMinIntrinsic:
        return genlVecCall(gen, isfloat ? "llvm.vector.reduce.fmin" : "llvm.vector.reduce.umin", &vectype, 1, &a, 1);
    case VecSMinIntrinsic:
        return genlVecCall(gen, "llvm.vector.reduce.smin", &vectype, 1, &a, 1);
    case VecMaxIntrinsic:
        return genlVecCall(gen, isfloat ? "llvm.vector.reduce.fmax" : "llvm.vector.reduce.umax", &vectype, 1, &a, 1);
    case VecSMaxIntrinsic:
        return genlVecCall(gen, "llvm.vector.reduce.smax", &vectype, 1, &a, 1);
    case VecAnyIntrinsic:
        return genlVecCall(gen, "llvm.vector.reduce.or", &vectype, 1, &a, 1);
    case VecAllIntrinsic:
        return genlVecCall(gen, "llvm.vector.reduce.and", &vectype, 1, &a, 1);
    case VecStoreIntrinsic: {
        // 'v.store(s, at)': a is the vector, then the slice and the position
        LLVMValueRef slot = genlSliceSpan(gen, site, fnargs[1], fnargs[2], (unsigned)LLVMGetVectorSize(vectype), lane);
        LLVMValueRef store = LLVMBuildStore(bld, a, slot);
        LLVMSetAlignment(store, genlLaneAlign(lane));
        return store;
    }
    }
    errorUnreachable(site, "a vector operation generation has no case for");
    return NULL;
}

// ---- Conversions -----------------------------------------------------------

// One lane type's value as another, over whole vectors: widened, narrowed,
// or converted as a scalar's 'from' converts. A float into an integer
// saturates (a value below the integer's range gives its minimum, above it
// its maximum, NaN gives 0), never poison [Jon 28 Oct 2026], LLVM's
// fptosi.sat and fptoui.sat; a number into bool asks whether it is not zero.
static LLVMValueRef genlVectorLanes(GenState *gen, LLVMValueRef val, INode *fromlane, INode *tolane, LLVMTypeRef totype) {
    LLVMBuilderRef bld = gen->builder;
    LLVMTypeRef fromtype = LLVMTypeOf(val);
    LLVMTypeRef llvmlane = LLVMGetElementType(totype);
    int tobool = tolane == (INode*)boolType;
    int frombool = fromlane == (INode*)boolType;
    if (tobool) {
        if (frombool)
            return val;
        if (fromlane->tag == FloatNbrTag)
            return LLVMBuildFCmp(bld, LLVMRealUNE, val, LLVMConstNull(fromtype), "");
        return LLVMBuildICmp(bld, LLVMIntNE, val, LLVMConstNull(fromtype), "");
    }
    unsigned frombits = frombool ? 1 : ((NbrNode*)fromlane)->bits;
    unsigned tobits = ((NbrNode*)tolane)->bits;
    if (tolane->tag == FloatNbrTag) {
        if (fromlane->tag == FloatNbrTag)
            return tobits < frombits ? LLVMBuildFPTrunc(bld, val, totype, "")
                : tobits > frombits ? LLVMBuildFPExt(bld, val, totype, "") : val;
        return fromlane->tag == IntNbrTag ? LLVMBuildSIToFP(bld, val, totype, "") : LLVMBuildUIToFP(bld, val, totype, "");
    }
    if (fromlane->tag == FloatNbrTag) {
        LLVMTypeRef types[2] = {totype, fromtype};
        return genlVecCall(gen, tolane->tag == IntNbrTag ? "llvm.fptosi.sat" : "llvm.fptoui.sat", types, 2, &val, 1);
    }
    (void)llvmlane;
    if (tobits < frombits)
        return LLVMBuildTrunc(bld, val, totype, "");
    if (tobits > frombits)
        return fromlane->tag == IntNbrTag ? LLVMBuildSExt(bld, val, totype, "") : LLVMBuildZExt(bld, val, totype, "");
    return val;
}

// A vector's value as an array of its lanes, and an array's as the vector, in
// memory: both are the lanes one after another, so a store of one and a load
// of the other is the whole conversion, which the optimizer sees through. A
// mask's lanes are single bits in a vector and a byte each in an array, so
// those are moved lane by lane.
static LLVMValueRef genlVectorArray(GenState *gen, LLVMValueRef val, LLVMTypeRef totype, int tomask, int toarray) {
    LLVMBuilderRef bld = gen->builder;
    LLVMTypeRef fromtype = LLVMTypeOf(val);
    if (tomask) {
        unsigned lanes = toarray ? (unsigned)LLVMGetArrayLength2(totype) : LLVMGetVectorSize(totype);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(gen->context);
        LLVMValueRef out = LLVMGetPoison(totype);
        for (unsigned i = 0; i < lanes; ++i) {
            if (toarray) {
                LLVMValueRef lane = LLVMBuildExtractElement(bld, val, LLVMConstInt(i32, i, 0), "");
                out = LLVMBuildInsertValue(bld, out, lane, i, "");
            }
            else {
                LLVMValueRef lane = LLVMBuildExtractValue(bld, val, i, "");
                out = LLVMBuildInsertElement(bld, out, lane, LLVMConstInt(i32, i, 0), "");
            }
        }
        return out;
    }
    // The memory is the vector's, the larger and the more aligned of the two
    LLVMTypeRef vectype = toarray ? fromtype : totype;
    LLVMValueRef slot = genlAlloca(gen, vectype, "vecarray");
    LLVMBuildStore(bld, val, slot);
    return LLVMBuildLoad2(bld, totype, slot, "");
}

// A vector made of a value: 'Vector[T, N].from(x)'. x is an array of the
// vector's lanes, free; another vector of the same number of lanes, converted
// lane by lane; or a number, put in every lane. The array the vector's contents
// are written as comes here the same way. A vector also converts to an array.
// With three arguments it is a mask's select, a where the mask holds, else b.
LLVMValueRef genlVectorConvert(GenState *gen, FnCallNode *lit) {
    INode *totype = itypeGetTypeDcl(lit->vtype);
    LLVMBuilderRef bld = gen->builder;
    VectorLitKind kind = vectorLitKind(lit);
    if (kind == VecLitJoin || kind == VecLitPick)
        return genlVectorShuffle(gen, lit, kind);
    if (kind == VecLitSelect) {
        LLVMValueRef mask = genlExpr(gen, nodesGet(lit->args, 0));
        LLVMValueRef a = genlExpr(gen, nodesGet(lit->args, 1));
        LLVMValueRef b = genlExpr(gen, nodesGet(lit->args, 2));
        return LLVMBuildSelect(bld, mask, a, b, "select");
    }
    INode *arg = nodesGet(lit->args, 0);
    INode *fromtype = iexpGetTypeDcl(arg);
    LLVMTypeRef totypeLlvm = genlType(gen, totype);
    LLVMValueRef val = genlExpr(gen, arg);
    if (totype->tag == ArrayTag) {
        INode *lane = itypeGetTypeDcl(arrayElemType(totype));
        return genlVectorArray(gen, val, totypeLlvm, lane == (INode*)boolType, 1);
    }
    if (fromtype->tag == ArrayTag) {
        INode *lane = itypeGetTypeDcl(vectorElemType(totype));
        return genlVectorArray(gen, val, totypeLlvm, lane == (INode*)boolType, 0);
    }
    if (fromtype->tag == VectorTag)
        return genlVectorLanes(gen, val, itypeGetTypeDcl(vectorElemType(fromtype)),
            itypeGetTypeDcl(vectorElemType(totype)), totypeLlvm);
    // A number in every lane
    return genlSplat(gen, val, (unsigned)vectorLanes(totype));
}

// The address of lane 'index' of the vector in memory at 'vecp', once the index
// is within the lane count. A vector in memory is its lanes one after another,
// so the lane is a scalar there, read and written as any element is.
LLVMValueRef genlVectorLane(GenState *gen, INode *site, INode *vectype, LLVMValueRef vecp, LLVMValueRef index) {
    LLVMValueRef count = LLVMConstInt(genlUsize(gen), vectorLanes(vectype), 0);
    genlBoundsCheck(gen, site, index, count);
    LLVMTypeRef lane = genlType(gen, vectorElemType(vectype));
    return LLVMBuildGEP2(gen->builder, lane, vecp, &index, 1, "lane");
}
