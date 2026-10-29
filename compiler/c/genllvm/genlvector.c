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
    if (lit->args->used == 3) {
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
